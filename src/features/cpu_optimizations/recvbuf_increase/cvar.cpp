#include "cvar.h"
#include "../cpuopt.h"

#include "buddy_import.h"

#include <cstddef>
#include <cstdio>

namespace recvbuf {
namespace {

// Verified cvar_t layout (IDA, Cvar_Set2 @ 0x20021d70 in SoF.exe):
//   +0x00 name   +0x04 string   +0x08 latched_string   +0x0C flags
//   +0x14 modified   +0x18 value   +0x1C next
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs  = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet   = 8;

/** Output cvar published by writing cvar_t directly - same shape as
 *  clamp_monitor/cvar.cpp, deliberately duplicated rather than shared:
 *  features here are self-contained folders, and one feature's detach must
 *  not strand another's strings. */
struct OutputCvar {
    void* cv = nullptr;
    char* original = nullptr;
    char buf[2][32] = {};
    int live = 0;

    void Bind(const char* name) {
        if (cv)
            return;
        cv = Buddy_GetEngineCvar(name, "0", kCvarFlagNoSet, nullptr);
        if (!cv)
            return;
        original = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    }

    void Publish(float value, const char* text) {
        if (!cv)
            return;
        char* base = static_cast<char*>(cv);
        *reinterpret_cast<volatile float*>(base + kCvarValueOfs) = value;
        char* spare = buf[1 - live];
        std::size_t i = 0;
        for (; text[i] && i + 1 < sizeof(buf[0]); ++i)
            spare[i] = text[i];
        spare[i] = '\0';
        *reinterpret_cast<char* volatile*>(base + kCvarStringOfs) = spare;
        live = 1 - live;
    }

    void Restore() {
        if (!cv || !original)
            return;
        *reinterpret_cast<char* volatile*>(static_cast<char*>(cv) + kCvarStringOfs) = original;
        cv = nullptr;
    }
};

void FormatI64(char* out, std::size_t n, long long v) {
    if (n == 0)
        return;
    char digits[24];
    std::size_t d = 0;
    unsigned long long mag = v < 0 ? 0ull - static_cast<unsigned long long>(v)
                                   : static_cast<unsigned long long>(v);
    do {
        digits[d++] = static_cast<char>('0' + (mag % 10ull));
        mag /= 10ull;
    } while (mag != 0ull && d < sizeof(digits));
    std::size_t i = 0;
    if (v < 0 && i + 1 < n)
        out[i++] = '-';
    while (d > 0 && i + 1 < n)
        out[i++] = digits[--d];
    out[i] = '\0';
}

OutputCvar g_outApplied;  // _sofbuddy_recvbuf_applied (actual bytes, post-clamp)
OutputCvar g_outSockets;  // _sofbuddy_recvbuf_sockets (sockets touched)

void* g_cvKb = nullptr;  // _sofbuddy_recvbuf_kb

}  // namespace

void InitCvars() {
    g_outApplied.Bind("_sofbuddy_recvbuf_applied");
    g_outSockets.Bind("_sofbuddy_recvbuf_sockets");

    if (!g_cvKb)
        g_cvKb = Buddy_GetEngineCvar("_sofbuddy_recvbuf_kb", "3107", kCvarFlagArchive, nullptr);
}

int RequestedBytes() {
    if (!CpuOpt_Enabled())
        return 0;
    const float kb = Buddy_ReadCvarValue(g_cvKb, 3107.0f);
    if (kb <= 0.0f)
        return 0;
    // Clamp to something the kernel will plausibly allow: below needs
    // net.core.rmem_max raised (Linux) and is silently clamped otherwise.
    // getsockopt reports back the actual, so over-asking is observable.
    long long bytes = static_cast<long long>(kb * 1024.0f);
    if (bytes < 64 * 1024)
        bytes = 64 * 1024;
    if (bytes > 4 * 1024 * 1024)
        bytes = 4 * 1024 * 1024;
    return static_cast<int>(bytes);
}

void SetOutputs(int actualBytes, int sockets) {
    char text[32];
    FormatI64(text, sizeof(text), actualBytes);
    g_outApplied.Publish(static_cast<float>(actualBytes), text);
    FormatI64(text, sizeof(text), sockets);
    g_outSockets.Publish(static_cast<float>(sockets), text);
}

}  // namespace recvbuf

extern "C" void Recvbuf_Shutdown() {
    recvbuf::g_outApplied.Restore();
    recvbuf::g_outSockets.Restore();
}
