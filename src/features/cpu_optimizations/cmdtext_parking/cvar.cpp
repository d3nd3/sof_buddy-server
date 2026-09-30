#include "cvar.h"
#include "../cpuopt.h"
#include "engine.h"

#include "buddy_import.h"
#include "log.h"

#include <cstddef>
#include <cstdio>
#include <windows.h>

namespace cmdpark {
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

OutputCvar g_outCbufLast;
OutputCvar g_outCbufMax;
OutputCvar g_outDefers;
OutputCvar g_outCursize;
OutputCvar g_outFillMax;

void* g_cvEnabled    = nullptr;
void* g_cvReserveMs  = nullptr;

}  // namespace

void InitCvars() {
    g_outCbufLast.Bind("_sofbuddy_cmdpark_cbuf_last");
    g_outCbufMax.Bind("_sofbuddy_cmdpark_cbuf_max");
    g_outDefers.Bind("_sofbuddy_cmdpark_defers");
    g_outCursize.Bind("_sofbuddy_cmdpark_cbuf_cursize");
    g_outFillMax.Bind("_sofbuddy_cmdpark_cbuf_fill_max");

    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_cmdpark", "1", kCvarFlagArchive, nullptr);
    if (!g_cvReserveMs)
        g_cvReserveMs = Buddy_GetEngineCvar("_sofbuddy_cmdpark_reserve_ms", "0", kCvarFlagArchive, nullptr);
    CpuOpt_MasterCvar();
    CpuOpt_StrictCvar();
}

Config ReadConfig() {
    Config c;
    c.enabled    = CpuOpt_Enabled() && Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
    c.strict     = CpuOpt_Strict();
    c.reserveMs  = Buddy_ReadCvarValue(g_cvReserveMs, 0.0f);
    if (!(c.reserveMs > 0.0f)) c.reserveMs = 0.0f;
    if (c.reserveMs > 50.0f)   c.reserveMs = 50.0f;
    c.dedicated = IsDedicated();
    return c;
}

void SetOutputs(float cbufLastMs, float cbufMaxMs, long long defers, int cursize, int fillMax) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(cbufLastMs));
    g_outCbufLast.Publish(cbufLastMs, text);
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(cbufMaxMs));
    g_outCbufMax.Publish(cbufMaxMs, text);
    FormatI64(text, sizeof(text), defers);
    g_outDefers.Publish(static_cast<float>(defers), text);
    FormatI64(text, sizeof(text), cursize);
    g_outCursize.Publish(static_cast<float>(cursize), text);
    FormatI64(text, sizeof(text), fillMax);
    g_outFillMax.Publish(static_cast<float>(fillMax), text);
}

void RestoreOutputs() {
    g_outCbufLast.Restore();
    g_outCbufMax.Restore();
    g_outDefers.Restore();
    g_outCursize.Restore();
    g_outFillMax.Restore();
}

}  // namespace cmdpark
