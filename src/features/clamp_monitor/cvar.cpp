#include "cvar.h"
#include "buddy_import.h"

#include <cstddef>
#include <cstdio>

namespace {

// Verified cvar_t layout (IDA, Cvar_Set2 @ 0x20021d70 in SoF.exe):
//   +0x00 name   +0x04 string   +0x08 latched_string   +0x0C flags
//   +0x14 modified   +0x18 value   +0x1C next
// Note this is NOT stock Quake 2's layout - SoF's `value` sits at +0x18, not
// +0x14. Engine cvar flags: 1 = ARCHIVE, 8 = NOSET.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs  = 0x18;

constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet   = 8;

/** One output cvar, published by writing cvar_t directly.
 *
 *  Why not gi.cvar_setvalue: engine cvar API calls are documented to crash on
 *  some Wine builds when made from inside frame-path hooks (see
 *  buddy_import.h), and these are published from the G_RunFrame hook.
 *
 *  Both fields must be written. The engine's console print of a cvar reads
 *  cvar_t.string, not .value, so poking only the float leaves the console
 *  showing "0" forever.
 *
 *  `string` is double-buffered: the live buffer is never rewritten in place,
 *  we format into the spare one and then swap the pointer (a single aligned
 *  32-bit store), so a reader can never observe a half-written string.
 *
 *  `original` is the engine-allocated string we displaced. It must be put
 *  back on detach: the engine owns the cvar_t for the life of the process but
 *  spsv FreeLibrary/reloads this DLL, which would leave cvar_t.string dangling
 *  into an unmapped image - and Cvar_Set2 does `Z_Free(var->string)`, so a
 *  forced set would also hand our static buffer to the engine's allocator. */
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

    /** Publishes `text` as the cvar's string and `value` as its float. */
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

/** 64-bit decimal, hand-rolled on purpose: this DLL builds against msvcrt,
 *  whose printf does not reliably understand the "ll" length modifier
 *  (%I64d is its own spelling), and the counters below must never come out as
 *  garbage in the console. Everything else here is plain %d / %.2f, which
 *  msvcrt does handle. */
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

OutputCvar g_outHighClamps;  // _sofbuddy_highclamps
OutputCvar g_outAvg;         // _sofbuddy_clamp_avg
OutputCvar g_outLast;        // _sofbuddy_clamp_last
OutputCvar g_outLost;        // _sofbuddy_clamp_lost_ms
OutputCvar g_outLowClamps;   // _sofbuddy_lowclamps
OutputCvar g_outLowChecks;   // _sofbuddy_lowclamp_checks
OutputCvar g_outGained;      // _sofbuddy_lowclamp_gained_ms
OutputCvar g_outWorstLow;    // _sofbuddy_lowclamp_worst

void* g_cvNotify = nullptr;             // _sofbuddy_clamp_notify_ms
void* g_cvWindow = nullptr;             // _sofbuddy_clamp_window (seconds)
void* g_cvBroadcastMs = nullptr;        // _sofbuddy_clamp_broadcast_ms (0 = off)
void* g_cvBroadcastInterval = nullptr;  // _sofbuddy_clamp_broadcast_interval (seconds)

}  // namespace

void ClampMonitor_InitCvars() {
    g_outHighClamps.Bind("_sofbuddy_highclamps");
    g_outAvg.Bind("_sofbuddy_clamp_avg");
    g_outLast.Bind("_sofbuddy_clamp_last");
    g_outLost.Bind("_sofbuddy_clamp_lost_ms");
    g_outLowClamps.Bind("_sofbuddy_lowclamps");
    g_outLowChecks.Bind("_sofbuddy_lowclamp_checks");
    g_outGained.Bind("_sofbuddy_lowclamp_gained_ms");
    g_outWorstLow.Bind("_sofbuddy_lowclamp_worst");

    if (!g_cvNotify)
        g_cvNotify = Buddy_GetEngineCvar("_sofbuddy_clamp_notify_ms", "5", kCvarFlagArchive, nullptr);
    if (!g_cvWindow)
        g_cvWindow = Buddy_GetEngineCvar("_sofbuddy_clamp_window", "2", kCvarFlagArchive, nullptr);
    if (!g_cvBroadcastMs)
        // Off by default: broadcasting is player-visible, unlike the shim-log
        // notify threshold above - opt in explicitly per server.
        g_cvBroadcastMs = Buddy_GetEngineCvar("_sofbuddy_clamp_broadcast_ms", "0", kCvarFlagArchive, nullptr);
    if (!g_cvBroadcastInterval)
        g_cvBroadcastInterval = Buddy_GetEngineCvar("_sofbuddy_clamp_broadcast_interval", "30", kCvarFlagArchive, nullptr);
}

void* ClampMonitor_NotifyCvar() { return g_cvNotify; }
void* ClampMonitor_WindowCvar() { return g_cvWindow; }
void* ClampMonitor_BroadcastThresholdCvar() { return g_cvBroadcastMs; }
void* ClampMonitor_BroadcastIntervalCvar() { return g_cvBroadcastInterval; }

void ClampMonitor_SetOutputs(long long highclamps, float avg_ms, int last_ms, long long lost_ms) {
    char text[32];

    // Counters print as exact integers: the old "%g" turned anything past
    // ~1e6 into "1.23457e+06" in the console and in sofplus string compares.
    FormatI64(text, sizeof(text), highclamps);
    g_outHighClamps.Publish(static_cast<float>(highclamps), text);

    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(avg_ms));
    g_outAvg.Publish(avg_ms, text);

    std::snprintf(text, sizeof(text), "%d", last_ms);
    g_outLast.Publish(static_cast<float>(last_ms), text);

    FormatI64(text, sizeof(text), lost_ms);
    g_outLost.Publish(static_cast<float>(lost_ms), text);
}

void ClampMonitor_SetLowOutputs(long long lowclamps, long long checks,
                                long long gained_ms, int worst_ms) {
    char text[32];

    FormatI64(text, sizeof(text), lowclamps);
    g_outLowClamps.Publish(static_cast<float>(lowclamps), text);

    FormatI64(text, sizeof(text), checks);
    g_outLowChecks.Publish(static_cast<float>(checks), text);

    FormatI64(text, sizeof(text), gained_ms);
    g_outGained.Publish(static_cast<float>(gained_ms), text);

    std::snprintf(text, sizeof(text), "%d", worst_ms);
    g_outWorstLow.Publish(static_cast<float>(worst_ms), text);
}

void ClampMonitor_Shutdown() {
    g_outHighClamps.Restore();
    g_outLowClamps.Restore();
    g_outLowChecks.Restore();
    g_outGained.Restore();
    g_outWorstLow.Restore();
    g_outAvg.Restore();
    g_outLast.Restore();
    g_outLost.Restore();
}
