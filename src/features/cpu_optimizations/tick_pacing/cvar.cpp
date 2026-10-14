#include "cvar.h"
#include "../cpuopt.h"
#include "engine.h"
#include "sleep_gate.h"

#include "buddy_import.h"
#include "log.h"

#include <cstddef>
#include <cstdio>
#include <windows.h>

namespace tickpace {
namespace {

constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs  = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet   = 8;

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

OutputCvar g_outLateAvg;
OutputCvar g_outSaved;

void* g_cvEnabled = nullptr;
void* g_cvSpinMs  = nullptr;
void* g_cvSettle  = nullptr;

}  // namespace

void InitCvars() {
    g_outLateAvg.Bind("_sofbuddy_tickpace_late_avg");
    g_outSaved.Bind("_sofbuddy_tickpace_saved");
    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_tickpace", "1", kCvarFlagArchive, nullptr);
    if (!g_cvSpinMs)
        g_cvSpinMs = Buddy_GetEngineCvar("_sofbuddy_tickpace_spin_ms", "2", kCvarFlagArchive, nullptr);
    if (!g_cvSettle)
        g_cvSettle = Buddy_GetEngineCvar("_sofbuddy_tickpace_settle", "1", kCvarFlagArchive, nullptr);
    CpuOpt_MasterCvar();
    CpuOpt_StrictCvar();
}

float SpinWindowMs() {
    float spin = Buddy_ReadCvarValue(g_cvSpinMs, 2.0f);
    if (!(spin > 0.0f))
        return 0.0f;
    if (spin > 20.0f)
        return 20.0f;
    return spin;
}

Config ReadConfig() {
    Config c;
    c.enabled   = CpuOpt_Enabled() && Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
    c.strict    = CpuOpt_Strict();
    c.spinMs    = Buddy_ReadCvarValue(g_cvSpinMs, 2.0f);
    if (!(c.spinMs > 0.0f)) c.spinMs = 0.0f;
    if (c.spinMs > 20.0f)   c.spinMs = 20.0f;
    c.settle    = c.enabled && Buddy_ReadCvarValue(g_cvSettle, 1.0f) != 0.0f;
    c.dedicated = IsDedicated();
    return c;
}

void SetOutputs(float lateAvgMs, long long saved) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(lateAvgMs));
    g_outLateAvg.Publish(lateAvgMs, text);
    FormatI64(text, sizeof(text), saved);
    g_outSaved.Publish(static_cast<float>(saved), text);
}

}  // namespace tickpace

extern "C" void TickPacing_Shutdown() {
    tickpace::SleepGate_Remove();
    if (CpuOpt_Strict())
        TickPace_LogSessionSummary();
    tickpace::g_outLateAvg.Restore();
    tickpace::g_outSaved.Restore();
}
