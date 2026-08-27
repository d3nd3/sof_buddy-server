#include "cvar.h"

#include "buddy_import.h"
#include "engine.h"
#include "log.h"

#include <cstddef>
#include <cstdio>
#include <windows.h>

namespace tickpace {
namespace {

// Verified cvar_t layout (IDA, Cvar_Set2 @ 0x20021d70 in SoF.exe):
//   +0x00 name  +0x04 string  +0x08 latched_string  +0x0C flags
//   +0x14 modified  +0x18 value  +0x1C next
// Not stock Quake 2's layout - SoF's `value` sits at +0x18, not +0x14.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs  = 0x18;

constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet   = 8;

/** One output cvar, published by writing cvar_t directly.
 *
 *  Why not gi.cvar_setvalue: engine cvar API calls are documented to crash on
 *  some Wine builds when made from inside frame-path hooks (see
 *  buddy_import.h), and these are published from the SV_Frame hook.
 *
 *  Both fields must be written - the engine's console print of a cvar reads
 *  cvar_t.string, not .value. `string` is double-buffered so a reader can
 *  never observe a half-written string, and the displaced engine pointer is
 *  put back on detach (Cvar_Set2 does Z_Free(var->string), so a forced set
 *  would otherwise hand our static buffer to the engine's allocator).
 *
 *  This mirrors clamp_monitor/cvar.cpp deliberately rather than sharing code:
 *  features in this tree are self-contained folders (see NEW_FEATURE.md), and
 *  one feature's detach must not be able to strand another's cvar strings. */
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

/** 64-bit decimal, hand-rolled: this DLL builds against msvcrt, whose printf
 *  does not reliably understand the "ll" length modifier, and "%g" turns
 *  anything past ~1e6 into "1.23457e+06" in the console and in sofplus string
 *  compares. */
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

OutputCvar g_outLateAvg;   // _sofbuddy_tickpace_late_avg
OutputCvar g_outLateMax;   // _sofbuddy_tickpace_late_max
OutputCvar g_outSaved;     // _sofbuddy_tickpace_saved
OutputCvar g_outCbufMax;   // _sofbuddy_tickpace_cbuf_max
OutputCvar g_outDefers;    // _sofbuddy_tickpace_defers

void* g_cvEnabled    = nullptr;
void* g_cvSpinMs     = nullptr;
void* g_cvReserveMs  = nullptr;
void* g_cvDeferMaxMs = nullptr;

}  // namespace

void InitCvars() {
    g_outLateAvg.Bind("_sofbuddy_tickpace_late_avg");
    g_outLateMax.Bind("_sofbuddy_tickpace_late_max");
    g_outSaved.Bind("_sofbuddy_tickpace_saved");
    g_outCbufMax.Bind("_sofbuddy_tickpace_cbuf_max");
    g_outDefers.Bind("_sofbuddy_tickpace_defers");

    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_tickpace", "1", kCvarFlagArchive, nullptr);
    if (!g_cvSpinMs)
        // Off by default: a spin burns spin_ms of a core out of every 100ms
        // tick. Opt in once the late_avg output says the loop granularity is
        // actually costing something.
        g_cvSpinMs = Buddy_GetEngineCvar("_sofbuddy_tickpace_spin_ms", "0", kCvarFlagArchive, nullptr);
    if (!g_cvReserveMs)
        // On by default. This is the only knob here that changes *when*
        // console commands run: it is a margin on top of the measured worst
        // recent drain, and a drain is never split, so it has to cover a whole
        // drain. Set it to 0 to put command scheduling back exactly as the
        // engine has it.
        g_cvReserveMs = Buddy_GetEngineCvar("_sofbuddy_tickpace_reserve_ms", "3", kCvarFlagArchive, nullptr);
    if (!g_cvDeferMaxMs)
        g_cvDeferMaxMs = Buddy_GetEngineCvar("_sofbuddy_tickpace_defer_max_ms", "200", kCvarFlagArchive, nullptr);
}

Config ReadConfig() {
    Config c;
    c.enabled    = Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
    c.spinMs     = Buddy_ReadCvarValue(g_cvSpinMs, 0.0f);
    c.reserveMs  = Buddy_ReadCvarValue(g_cvReserveMs, 3.0f);
    c.deferMaxMs = Buddy_ReadCvarValue(g_cvDeferMaxMs, 200.0f);

    // Clamp the knobs to sane ranges here, once, so the hot path can trust
    // them. A spin longer than a tick would mean 100% CPU forever; a defer
    // window that swallows a whole tick would stall the console.
    if (!(c.spinMs > 0.0f)) c.spinMs = 0.0f;
    if (c.spinMs > 20.0f)   c.spinMs = 20.0f;
    if (!(c.reserveMs > 0.0f)) c.reserveMs = 0.0f;
    if (c.reserveMs > 50.0f)   c.reserveMs = 50.0f;
    if (!(c.deferMaxMs > 0.0f)) c.deferMaxMs = 200.0f;
    if (c.deferMaxMs > 1000.0f) c.deferMaxMs = 1000.0f;

    // Spin and command scheduling are dedicated-server behaviours (the user-visible cost of
    // both is paid by whatever else shares this thread; on a listen server
    // that is the client). The settle correction is safe either way.
    c.dedicated = IsDedicated();
    return c;
}

void SetOutputs(float lateAvgMs, float lateMaxMs, long long saved,
                float cbufMaxMs, long long defers) {
    char text[32];

    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(lateAvgMs));
    g_outLateAvg.Publish(lateAvgMs, text);

    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(lateMaxMs));
    g_outLateMax.Publish(lateMaxMs, text);

    FormatI64(text, sizeof(text), saved);
    g_outSaved.Publish(static_cast<float>(saved), text);

    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(cbufMaxMs));
    g_outCbufMax.Publish(cbufMaxMs, text);

    FormatI64(text, sizeof(text), defers);
    g_outDefers.Publish(static_cast<float>(defers), text);
}

}  // namespace tickpace

extern "C" void TickPacing_Shutdown() {
    tickpace::g_outLateAvg.Restore();
    tickpace::g_outLateMax.Restore();
    tickpace::g_outSaved.Restore();
    tickpace::g_outCbufMax.Restore();
    tickpace::g_outDefers.Restore();
}
