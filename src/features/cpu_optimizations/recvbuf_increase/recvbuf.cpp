// recvbuf_increase: raise the engine UDP sockets' SO_RCVBUF so burst
// clc_move traffic survives main-thread stalls instead of being dropped by
// the kernel.
//
// IDA (SoF.exe, base 0x20000000):
//   setupIpUdpSocket @0x2004ECB0 is Quake 2's UDP_OpenSocket: socket(AF_INET,
//   SOCK_DGRAM) -> ioctlsocket(FIONBIO) -> setsockopt(SO_BROADCAST) ->
//   bind(port). It returns the SOCKET, or 0 on failure (INVALID_SOCKET
//   never escapes: every error path returns 0). Single caller: NET_OpenIP
//   @0x2004EE30, which stores the result in ip_sockets[] and then calls
//   getsockname on it.
//   The engine never sets SO_RCVBUF anywhere (only SO_BROADCAST), so every
//   socket keeps the OS default (~106KB real payload on a stock Linux
//   rmem_default of 212992, which getsockopt reports doubled).
//
// Why this matters: SV_ReadPackets drains the socket once per SV_Frame, but
// Cbuf_Execute / G_RunFrame / sofplus timers all run on the same thread.
// While the server is busy scripting, client packets queue in the kernel.
// At 143fps x N players a 100ms+ stall is tens of packets; once the queue
// exceeds SO_RCVBUF the kernel silently drops the oldest datagrams and
// Netchan_Process reports "Dropped %i packets" (or "Out of order" for
// reordered leftovers), which surfaces as micro-stutter. A bigger buffer
// converts that loss into (briefly) delayed delivery, which the existing
// net_drop compensation replays.
//
// How: a Post hook on setupIpUdpSocket applies setsockopt(SO_RCVBUF) to
// every newly opened socket, plus a sweep of the already-open ip_sockets[]
// at GameDllLoaded (engine NET_Init runs before the game DLL loads, so the
// hook alone would miss the live sockets). The actual size is read back
// with getsockopt (the kernel clamps to net.core.rmem_max) and published
// so over-asking is observable instead of silent.

#include <winsock2.h>

#include "cvar.h"
#include "../cpuopt.h"

#include "buddy_import.h"
#include "log.h"

#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace recvbuf {
namespace {

// ip_sockets[2] base, from NET_OpenIP @0x2004EE30 / NET_GetPacket @0x2004E630:
// base+0 = client slot, base+4 = server slot (0x2039011C). Read via the
// process image (SoF.exe or SoF-spsv.exe) the same way every other module
// resolves engine globals: unnamed handle first, named handles for the
// debugger.
constexpr unsigned kRvaIpSockets = 0x390118;
constexpr int kIpSocketSlots = 2;

HMODULE EngineModule() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

/** Apply the requested buffer to one socket. Returns true when the socket
 *  was valid and setsockopt succeeded. */
bool ApplyToSocket(std::uintptr_t sock) {
    if (sock == 0 || sock == static_cast<std::uintptr_t>(0xFFFFFFFF))
        return false;
    const int want = RequestedBytes();
    if (want <= 0)
        return false;
    int size = want;
    if (setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
        return false;
    int actual = 0;
    int len = sizeof(actual);
    if (getsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<char*>(&actual), &len) == 0 && actual > 0)
        SetOutputs(actual, 1);
    else
        SetOutputs(want, 1);
    return true;
}

int SweepOpenSockets() {
    const int want = RequestedBytes();
    if (want <= 0)
        return 0;
    HMODULE exe = EngineModule();
    if (!exe)
        return 0;
    volatile int* slots =
        reinterpret_cast<volatile int*>(reinterpret_cast<char*>(exe) + kRvaIpSockets);
    int touched = 0;
    int lastActual = 0;
    for (int i = 0; i < kIpSocketSlots; ++i) {
        const std::uintptr_t sock =
            static_cast<std::uintptr_t>(static_cast<unsigned int>(slots[i]));
        if (sock == 0 || sock == static_cast<std::uintptr_t>(0xFFFFFFFF))
            continue;
        int size = want;
        if (setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char*>(&size), sizeof(size)) != 0) {
            PrintOut(PRINT_BAD, "[recvbuf] setsockopt(SO_RCVBUF=%d) failed on socket[%d]: %d\n",
                     want, i, WSAGetLastError());
            continue;
        }
        int actual = 0;
        int len = sizeof(actual);
        if (getsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<char*>(&actual), &len) == 0 && actual > 0)
            lastActual = actual;
        ++touched;
        PrintOut(PRINT_LOG, "[recvbuf] socket[%d]=%u SO_RCVBUF %d requested, %d actual\n",
                 i, static_cast<unsigned>(sock), want, lastActual);
    }
    if (touched > 0)
        SetOutputs(lastActual > 0 ? lastActual : want, touched);
    return touched;
}

}  // namespace
}  // namespace recvbuf

/** Post hook on setupIpUdpSocket @0x2004ECB0 (UDP_OpenSocket). The engine
 *  returns 0 on every failure path, so anything else is a live SOCKET. */
int recvbuf_SetupIpUdpSocketPost(int result, char* net_interface, int port) {
    (void)net_interface;
    (void)port;
    using namespace recvbuf;
    if (result == 0 || result == -1)
        return result;
    if (ApplyToSocket(static_cast<std::uintptr_t>(static_cast<unsigned int>(result))))
        PrintOut(PRINT_LOG, "[recvbuf] new socket=%u SO_RCVBUF applied\n",
                 static_cast<unsigned>(result));
    return result;
}

void recvbuf_OnGameDllLoaded(void* game_export) {
    using namespace recvbuf;
    (void)game_export;
    InitCvars();
    // Engine NET_Init (and its sockets) predates the game DLL, so the Post
    // hook alone would never touch the live server socket. Sweep now.
    const int n = SweepOpenSockets();
    if (n == 0)
        PrintOut(PRINT_LOG, "[recvbuf] no open sockets yet (hook will catch NET_OpenIP)\n");
}
