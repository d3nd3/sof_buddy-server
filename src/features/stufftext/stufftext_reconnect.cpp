// stufftext_reconnect: chained reconnect with a welcome line.
//
//   stufftext_reconnect <slot>
//   stufftext_reconnect cancel
//
// Chain of events for one client slot:
//   1. save the engine's attractloop flag, set it, and READ IT BACK: the
//      chain refuses to start unless the flag verifiably reads set before
//      anything is stuffed. Then neuter the connect-refusal check for the
//      chain window (see the patch block below): the flag reads set, but
//      nobody - target included - is refused while it runs.
//   2. stuff "reconnect" to the target slot.
//   3. once the target is observed server-side as cs_connected (reconnecting)
//      - or after the wait timeout (see StuffReconnectConfig) - stuff the
//      welcome line to the target. The slot exists because the refusal is
//      neutered; without the patch this step could never fire (see below).
//   4. restore the refusal byte, then the attractloop flag, then stuff
//      "reconnect" to the target when still present plus every other slot
//      seen connecting that is still present.
//
// Why the refusal must be neutered: stock SVC_DirectConnect refuses *every*
// remote connect while attractloop is set - including the target's own (only
// a true NA_LOOPBACK client bypasses; a UDP client, even on 127.0.0.1, is
// refused with "Remote connect in attract loop. Ignored."). Unpatched, the
// target parks in the getchallenge/connect retry loop, its slot goes
// zombie/free, and no later unicast has anyone to reach. The 1-byte patch
// cannot discriminate target from strangers, so during the window exclusion
// is traded for a working resync: whoever arrives is tracked (seen set) and
// re-stuffed in the final round.
//
// The chain is driven from an SV_Frame Post hook (engine thread, same context
// stock game code calls gi.unicast from), so there are no threads and no
// races: console command and ticker never run concurrently. At most one chain
// runs at a time; a second invocation is rejected until the first finishes,
// times out, or is cancelled. Detach (StuffText_Shutdown) restores the flag
// and parks the machine without sending.
//
// Engine addresses below are the single place to update for a new server
// binary. All were verified in IDA and cross-checked against both shipped
// engines (SoF.exe and SoF-spsv.exe share .text byte-for-byte):
//   sv.attractloop : BYTE  at engine base + 0x3A1F24 (neighbours sv.state
//                    @0x3A1F20 / sv.time @0x3A1F28 - see clamp_monitor)
//   svs.clients    : client_t* at engine base + 0x396EEC, stride 0xD2AC,
//                    state DWORD at +0x0 (0 free / 1 zombie / 2 connected /
//                    3 spawned - stock Quake 2 client_state_t order)
//   refusal check  : in SVC_DirectConnect at engine base + 0x5E808
//                    (VA 0x2005E808): `74 53 / je allow` over the attractloop
//                    refusal. Patching the opcode byte 0x74->0xEB turns it
//                    into `EB 53 / jmp allow`, neutering the refusal while the
//                    flag itself stays set. Restored at chain end.

#include "stufftext_reconnect.h"

#include "stufftext_reconnect_logic.h"
#include "cvar.h"
#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace {

// ---------------------------------------------------------------------------
// Engine layout (see file header for the IDA trail).
// ---------------------------------------------------------------------------
constexpr unsigned kRvaAttractLoop = 0x3A1F24;  // BYTE sv.attractloop
constexpr unsigned kRvaSvsClients = 0x396EEC;   // client_t* svs.clients
constexpr unsigned kClientStride = 0xD2AC;      // sizeof(client_t)
constexpr unsigned kClientStateOfs = 0x0;       // client_state_t state

// Refusal-check patch (see file header): VA 0x2005E808 holds `74 53`
// (je allow). Writing 0xEB makes it `EB 53` (jmp allow).
constexpr unsigned kRvaRefusalCheck = 0x5E808;
constexpr uint8_t kRefusalOrigByte = 0x74;
constexpr uint8_t kRefusalNextByte = 0x53;
constexpr uint8_t kRefusalPatchedByte = 0xEB;

constexpr int kCsFree = 0;
constexpr int kCsZombie = 1;
constexpr int kCsConnected = 2;
constexpr int kCsSpawned = 3;

// Game DLL layout (same verified constants as stufftext.cpp / ctf_spawn).
constexpr unsigned kEdictStride = 0x464;
constexpr unsigned kEdictClient = 0x74;
constexpr unsigned kEdictInuse = 0x78;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr unsigned kRvaMaxclientsCvar = 0x15D9B4;
constexpr unsigned kRvaGEdicts = 0x15CCA0;

// Chain payloads. Trailing \n is part of the framing svc_stufftext expects.
constexpr char kReconnectText[] = "reconnect\n";
constexpr char kWelcomeText[] = "echo Welcome to SoF Buddy - enjoy your stay!\n";

StuffReconnectConfig g_cfg;  // default timeouts from the logic header
StuffReconnectState g_chain;
bool g_patchApplied = false;  // refusal check currently neutered by us

// ---------------------------------------------------------------------------
// Safety helpers (same pattern as stufftext.cpp).
// ---------------------------------------------------------------------------
inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool IsSafeMemoryBlock(const void* ptr, size_t size) {
    if (!IsValidUserPointer(ptr) || size == 0)
        return false;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr + size < addr || addr + size > 0x7FFFFFFF)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + size <= regionEnd);
}

inline bool IsExecutableCodeAddress(const void* ptr) {
    if (!IsValidUserPointer(ptr))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

HMODULE GameMod() {
    HMODULE h = nullptr;
    if (HMODULE shim = GetModuleHandleA("gamex86.dll")) {
        using handle_fn = HMODULE (*)();
        auto fn = reinterpret_cast<handle_fn>(GetProcAddress(shim, "Buddy_GetGameDllHandle"));
        if (fn) {
            HMODULE stock = fn();
            if (stock)
                return stock;
        }
    }
    h = GetModuleHandleA("oldgamex86.dll");
    if (!h) h = GetModuleHandleA("OldGamex86.dll");
    if (!h) h = GetModuleHandleA("OLDGAMEX86.DLL");
    return h;
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000000) return false;
    if (!IsSafeMemoryBlock(reinterpret_cast<const char*>(dos) + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (rva >= nt->OptionalHeader.SizeOfImage) return false;
    if (rva + size > nt->OptionalHeader.SizeOfImage || rva + size < rva) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Engine state access.
// ---------------------------------------------------------------------------
volatile uint8_t* AttractFlagPtr() {
    HMODULE h = ExeMod();
    if (!h || !IsValidModuleRva(h, kRvaAttractLoop, sizeof(uint8_t)))
        return nullptr;
    volatile uint8_t* p =
        reinterpret_cast<volatile uint8_t*>(reinterpret_cast<char*>(h) + kRvaAttractLoop);
    if (!IsSafeMemoryBlock(const_cast<uint8_t*>(p), sizeof(uint8_t)))
        return nullptr;
    return p;
}

bool AttractGet(bool& on) {
    volatile uint8_t* p = AttractFlagPtr();
    if (!p)
        return false;
    on = (*p != 0);
    return true;
}

bool AttractSet(bool on) {
    volatile uint8_t* p = AttractFlagPtr();
    if (!p)
        return false;
    *p = on ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    return true;
}

void* SvsClientsPtr() {
    HMODULE h = ExeMod();
    if (!h || !IsValidModuleRva(h, kRvaSvsClients, sizeof(void*)))
        return nullptr;
    void* clients = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaSvsClients);
    if (!IsValidUserPointer(clients))
        return nullptr;
    return clients;
}

// ---------------------------------------------------------------------------
// Refusal-check patch. Writes/mends one opcode byte in engine .text with
// full verification: expected original bytes, VirtualProtect round-trip,
// instruction-cache flush, and a read-back of the written value. Returns
// false (leaving everything as found) on any mismatch.
// ---------------------------------------------------------------------------
uint8_t* RefusalCheckPtr() {
    HMODULE h = ExeMod();
    if (!h || !IsValidModuleRva(h, kRvaRefusalCheck, 2))
        return nullptr;
    uint8_t* p = reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(h) + kRvaRefusalCheck);
    if (!IsSafeMemoryBlock(p, 2))
        return nullptr;
    return p;
}

bool WriteCodeByte(uint8_t* p, uint8_t value) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    *p = value;
    DWORD ignored = 0;
    VirtualProtect(p, 1, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return *p == value;
}

// Neuter the refusal (`74`->`EB`). Fails closed: any unexpected byte means
// this is not the engine revision the patch was verified against.
bool PatchRefusalApply() {
    if (g_patchApplied)
        return true;
    uint8_t* p = RefusalCheckPtr();
    if (!p) {
        Buddy_DebugPrintf("stufftext_reconnect: refusal check not resolvable on this engine binary\n");
        return false;
    }
    if (p[0] != kRefusalOrigByte || p[1] != kRefusalNextByte) {
        Buddy_DebugPrintf(
            "stufftext_reconnect: refusing to patch: expected 74 53 at 0x5E808, found %02X %02X\n",
            p[0], p[1]);
        return false;
    }
    if (!WriteCodeByte(p, kRefusalPatchedByte)) {
        Buddy_DebugPrintf("stufftext_reconnect: refusal patch write failed (read-back mismatch)\n");
        return false;
    }
    g_patchApplied = true;
    PrintOut(PRINT_LOG, "[stufftext_reconnect] refusal check neutered (74->EB at 0x5E808)\n");
    return true;
}

void PatchRefusalRestore() {
    if (!g_patchApplied)
        return;
    uint8_t* p = RefusalCheckPtr();
    if (!p) {
        Buddy_DebugPrintf("stufftext_reconnect: WARNING: engine image gone, refusal patch NOT restored\n");
        return;
    }
    if (p[0] == kRefusalOrigByte && p[1] == kRefusalNextByte) {
        g_patchApplied = false;
        PrintOut(PRINT_LOG, "[stufftext_reconnect] refusal check already stock, nothing to restore\n");
        return;
    }
    if (p[0] != kRefusalPatchedByte) {
        g_patchApplied = false;
        Buddy_DebugPrintf(
            "stufftext_reconnect: WARNING: refusal byte is %02X (not ours), leaving it alone\n",
            p[0]);
        return;
    }
    if (!WriteCodeByte(p, kRefusalOrigByte)) {
        Buddy_DebugPrintf("stufftext_reconnect: WARNING: refusal restore failed (read-back mismatch), will retry on next unlock path\n");
        return;
    }
    g_patchApplied = false;
    PrintOut(PRINT_LOG, "[stufftext_reconnect] refusal check restored (EB->74 at 0x5E808)\n");
}

// Server-side client state for one slot, or -1 when unreadable.
int EngineClientState(void* clients, int slot) {
    if (!clients || slot < 0 || slot >= StuffReconnectState::kMaxSlots)
        return -1;
    const void* cell = static_cast<const char*>(clients) +
                       static_cast<unsigned>(slot) * kClientStride + kClientStateOfs;
    if (!IsSafeMemoryBlock(cell, sizeof(int)))
        return -1;
    const int st = *static_cast<const int*>(cell);
    if (st < kCsFree || st > kCsSpawned)
        return -1;
    return st;
}

int GameMaxClients() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaMaxclientsCvar, sizeof(void*))) return 0;
    void* cv = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaMaxclientsCvar);
    if (!IsSafeMemoryBlock(cv, kCvarValueOfs + sizeof(float))) return 0;
    int v = static_cast<int>(*reinterpret_cast<float*>(static_cast<char*>(cv) + kCvarValueOfs));
    if (v < 1) return 0;
    if (v > StuffReconnectState::kMaxSlots) return StuffReconnectState::kMaxSlots;
    return v;
}

char* GameEdicts() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaGEdicts, sizeof(void*))) return nullptr;
    void* edicts = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
    if (!IsValidUserPointer(edicts)) return nullptr;
    return static_cast<char*>(edicts);
}

bool EdictInUse(const void* edict) {
    if (!IsSafeMemoryBlock(edict, kEdictInuse + sizeof(int)))
        return false;
    if (!*reinterpret_cast<const int*>(static_cast<const char*>(edict) + kEdictInuse))
        return false;
    if (!IsSafeMemoryBlock(static_cast<const char*>(edict) + kEdictClient, sizeof(void*)))
        return false;
    void* cl = *reinterpret_cast<void* const*>(static_cast<const char*>(edict) + kEdictClient);
    return IsValidUserPointer(cl);
}

bool EngineCmdReady() {
    return detour_Cmd_Argc::oCmd_Argc && detour_Cmd_Argv::oCmd_Argv &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argc::oCmd_Argc)) &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argv::oCmd_Argv));
}

bool ChainActive() {
    return g_chain.stage != STUFF_RECONNECT_IDLE;
}

// Stuff one line to a slot. Presence is the game edict *or* the engine
// connecting state: a cs_connected edict is not game-inuse yet (ClientBegin
// has not run), but unicast only needs the slot index.
bool ChainSendToSlot(int slot, const char* text, int maxClients, char* edicts) {
    if (!text || slot < 0 || slot >= maxClients || !IsValidUserPointer(edicts))
        return false;
    char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
    if (!EdictInUse(edict)) {
        void* clients = SvsClientsPtr();
        if (EngineClientState(clients, slot) != kCsConnected)
            return false;
    }
    return Buddy_StuffText(edict, text);
}

void ChainNote(bool ok, const char* what, int slot) {
    StuffText_NoteDelivery(ok);
    if (ok)
        PrintOut(PRINT_LOG, "[stufftext_reconnect] slot %d: stuffed %s", slot, what);
    else
        Buddy_DebugPrintf("stufftext_reconnect: delivery of %s to slot %d failed\n", what, slot);
}

// Final round: refusal byte and flag already restored by the caller. The
// target is listed when still present (seen or not); every other slot seen
// connecting that is still present follows. A target that left for good is
// simply absent from the list - no loud failure for the normal path.
void ChainFinish() {
    const int target = g_chain.targetSlot;
    const int maxClients = GameMaxClients();
    char* edicts = GameEdicts();
    void* clients = SvsClientsPtr();

    bool present[StuffReconnectState::kMaxSlots] = {};
    int slots = 0;
    if (maxClients > 0 && clients) {
        slots = maxClients < StuffReconnectState::kMaxSlots ? maxClients
                                                            : StuffReconnectState::kMaxSlots;
        for (int i = 0; i < slots; ++i) {
            const int st = EngineClientState(clients, i);
            present[i] = (st == kCsConnected || st == kCsSpawned);
        }
    }

    int list[StuffReconnectState::kMaxSlots] = {};
    const int n = StuffReconnect_FinalList(g_chain, present, slots, list,
                                           StuffReconnectState::kMaxSlots);
    int delivered = 0;
    for (int k = 0; k < n; ++k) {
        const int slot = list[k];
        const bool ok = (IsValidUserPointer(edicts) && maxClients > 0)
                            ? ChainSendToSlot(slot, kReconnectText, maxClients, edicts)
                            : false;
        StuffText_NoteDelivery(ok);
        if (ok)
            ++delivered;
        else
            Buddy_DebugPrintf("stufftext_reconnect: final reconnect to slot %d failed\n", slot);
    }

    PrintOut(PRINT_LOG,
             "[stufftext_reconnect] chain for slot %d finished%s: final reconnect to %d slot(s)\n",
             target, g_chain.targetLeft ? " (target passed through, back or on its way)" : "",
             delivered);
    StuffReconnect_Reset(g_chain);
}

// One ticker step. Observes presence, latches targetLeft, and advances the
// wait: welcome on observed connecting, finish after the welcome delay.
void ChainTick(uint32_t nowMs) {
    if (!ChainActive())
        return;
    void* clients = SvsClientsPtr();
    const int maxClients = GameMaxClients();

    bool targetConnecting = false;
    bool targetPresent = false;
    if (clients && maxClients > 0) {
        const int n = maxClients < StuffReconnectState::kMaxSlots ? maxClients
                                                                  : StuffReconnectState::kMaxSlots;
        for (int i = 0; i < n; ++i) {
            const int st = EngineClientState(clients, i);
            if (st == kCsConnected) {
                StuffReconnect_MarkSeen(g_chain, i);
                if (i == g_chain.targetSlot)
                    targetConnecting = true;
            }
            if (i == g_chain.targetSlot && (st == kCsConnected || st == kCsSpawned))
                targetPresent = true;
        }
    }
    if (!targetPresent)
        g_chain.targetLeft = true;

    const StuffReconnectAction act =
        StuffReconnect_Poll(g_chain, g_cfg, nowMs, targetConnecting);
    if (act == STUFF_RECONNECT_SEND_WELCOME) {
        const int slot = g_chain.targetSlot;
        const int mc = GameMaxClients();
        char* ed = GameEdicts();
        ChainNote(ChainSendToSlot(slot, kWelcomeText, mc, ed), "welcome", slot);
    } else if (act == STUFF_RECONNECT_FINISH) {
        // The unlock runs before any final stuff and restores both the
        // refusal byte and the flag, so a failed send can never leave the
        // server neutered or refusing connects.
        PatchRefusalRestore();
        const bool restoreTo = g_chain.priorAttract;
        if (!AttractSet(restoreTo))
            Buddy_DebugPrintf("stufftext_reconnect: WARNING: could not restore attractloop flag\n");
        else {
            bool check = false;
            AttractGet(check);
            PrintOut(PRINT_LOG, "[stufftext_reconnect] attractloop restored to %d (reads %d)\n",
                     restoreTo ? 1 : 0, check ? 1 : 0);
        }
        ChainFinish();
    }
}

// Roll back a half-armed chain start: un-neuter, restore the flag, park.
void ChainStartRollback(bool priorAttract) {
    PatchRefusalRestore();
    AttractSet(priorAttract);
    StuffReconnect_Reset(g_chain);
}

void ChainAbort(const char* why) {
    if (!ChainActive()) {
        Buddy_DebugPrintf("stufftext_reconnect: no chain active\n");
        return;
    }
    const int target = g_chain.targetSlot;
    PatchRefusalRestore();
    if (!AttractSet(g_chain.priorAttract))
        Buddy_DebugPrintf("stufftext_reconnect: WARNING: could not restore attractloop flag\n");
    else
        PrintOut(PRINT_LOG, "[stufftext_reconnect] attractloop restored to %d (%s)\n",
                 g_chain.priorAttract ? 1 : 0, why ? why : "aborted");
    StuffReconnect_Reset(g_chain);
    PrintOut(PRINT_LOG, "[stufftext_reconnect] chain for slot %d aborted (%s)\n",
             target, why ? why : "aborted");
}

}  // namespace

void StuffReconnect_OnDetach() {
    // Best effort: never leave the server neutered or refusing connects
    // because this DLL went away mid-chain. No sends on this path.
    if (!ChainActive())
        return;
    PatchRefusalRestore();
    AttractSet(g_chain.priorAttract);
    StuffReconnect_Reset(g_chain);
}

// SV_Frame Post: drives the chain at loop rate. Near-zero cost when idle.
void stuffreconnect_SvFramePost(int msec) {
    (void)msec;
    if (!ChainActive())
        return;
    ChainTick(GetTickCount());
}

// Engine xcommand_t: void (__cdecl*)(void). Same context as stufftext.
extern "C" void __cdecl stuffreconnect_Command_f() {
    if (!StuffText_IsEnabled()) {
        Buddy_DebugPrintf("stufftext_reconnect is disabled (_sofbuddy_stufftext 0)\n");
        StuffText_NoteDelivery(false);
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("stufftext_reconnect: engine arg accessors unavailable\n");
        StuffText_NoteDelivery(false);
        return;
    }

    const int argc = SOF_EP_Cmd_Argc();
    if (argc != 2) {
        Buddy_DebugPrintf("usage: stufftext_reconnect <slot|cancel>\n");
        StuffText_NoteDelivery(false);
        return;
    }

    const char* arg = SOF_EP_Cmd_Argv(1);
    if (!arg || !IsValidUserPointer(arg) || !IsSafeMemoryBlock(arg, 1)) {
        StuffText_NoteDelivery(false);
        return;
    }

    if (_stricmp(arg, "cancel") == 0) {
        ChainAbort("cancelled by operator");
        return;
    }

    if (ChainActive()) {
        Buddy_DebugPrintf("stufftext_reconnect: chain already active for slot %d (use 'stufftext_reconnect cancel' first)\n",
                          g_chain.targetSlot);
        StuffText_NoteDelivery(false);
        return;
    }

    // Strict numeric slot only: no "all", no name matching - a reconnect
    // chain must never fan out beyond one deliberately chosen slot (plus, at
    // the end, whoever genuinely entered connecting under the lock).
    if (_stricmp(arg, "all") == 0) {
        Buddy_DebugPrintf("stufftext_reconnect: refusing 'all' - give one numeric slot\n");
        StuffText_NoteDelivery(false);
        return;
    }
    char* end = nullptr;
    const long v = std::strtol(arg, &end, 10);
    if (!end || *end != '\0' || v < 0) {
        Buddy_DebugPrintf("usage: stufftext_reconnect <slot|cancel>  (slot is 0-based)\n");
        StuffText_NoteDelivery(false);
        return;
    }

    const int maxClients = GameMaxClients();
    char* edicts = GameEdicts();
    if (!IsValidUserPointer(edicts) || maxClients < 1) {
        Buddy_DebugPrintf("stufftext_reconnect: server not ready\n");
        StuffText_NoteDelivery(false);
        return;
    }
    int slot = static_cast<int>(v);
    if (slot == maxClients)  // same 1-based alias for the last slot as stufftext
        slot = maxClients - 1;
    if (slot < 0 || slot >= maxClients) {
        Buddy_DebugPrintf("stufftext_reconnect: slot out of range (0-%d)\n", maxClients - 1);
        StuffText_NoteDelivery(false);
        return;
    }

    char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
    if (!EdictInUse(edict)) {
        void* clients = SvsClientsPtr();
        const int st = EngineClientState(clients, slot);
        if (st != kCsConnected && st != kCsSpawned) {
            Buddy_DebugPrintf("stufftext_reconnect: slot %d is not in use\n", slot);
            StuffText_NoteDelivery(false);
            return;
        }
    }

    bool priorAttract = false;
    if (!AttractGet(priorAttract)) {
        Buddy_DebugPrintf("stufftext_reconnect: attractloop flag not resolvable on this engine binary\n");
        StuffText_NoteDelivery(false);
        return;
    }

    if (g_patchApplied) {
        // Should never happen (single-flight + detach cleanup), but never
        // layer a second patch over a leaked one: mend first, then proceed.
        Buddy_DebugPrintf("stufftext_reconnect: WARNING: refusal patch already applied, mending first\n");
        PatchRefusalRestore();
    }

    StuffReconnect_Begin(g_chain, slot, GetTickCount(), priorAttract);
    if (priorAttract)
        PrintOut(PRINT_LOG, "[stufftext_reconnect] note: attractloop was already on; it will be left on\n");

    // Arm order is load-bearing: the flag must verifiably read set BEFORE
    // the reconnect is stuffed, and the refusal must be neutered before the
    // target's retry arrives. Every step is read back; any failure rolls the
    // earlier steps back and refuses to start.
    if (!AttractSet(true)) {
        StuffReconnect_Reset(g_chain);
        Buddy_DebugPrintf("stufftext_reconnect: could not set attractloop flag\n");
        StuffText_NoteDelivery(false);
        return;
    }
    bool flagReads = false;
    if (!AttractGet(flagReads) || !flagReads) {
        ChainStartRollback(priorAttract);
        Buddy_DebugPrintf("stufftext_reconnect: attractloop flag did not read back set - refusing to start\n");
        StuffText_NoteDelivery(false);
        return;
    }
    if (!PatchRefusalApply()) {
        ChainStartRollback(priorAttract);
        Buddy_DebugPrintf("stufftext_reconnect: refusal patch failed - chain not started\n");
        StuffText_NoteDelivery(false);
        return;
    }

    if (!ChainSendToSlot(slot, kReconnectText, maxClients, edicts)) {
        ChainStartRollback(priorAttract);
        Buddy_DebugPrintf("stufftext_reconnect: delivery of first reconnect to slot %d failed\n", slot);
        StuffText_NoteDelivery(false);
        return;
    }
    StuffText_NoteDelivery(true);
    PrintOut(PRINT_LOG,
             "[stufftext_reconnect] chain started for slot %d (attractloop reads 1, refusal neutered, reconnect sent)\n",
             slot);
}

