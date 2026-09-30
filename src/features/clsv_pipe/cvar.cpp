#include "cvar.h"
#include "buddy_import.h"

#include <cstddef>
#include <cstdio>

namespace {

constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet = 8;

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

OutputCvar g_outSent;    // _sofbuddy_clsv_sent
OutputCvar g_outJobs;    // _sofbuddy_clsv_jobs
OutputCvar g_outErrors;  // _sofbuddy_clsv_errors
OutputCvar g_outDlAllowed;  // _sofbuddy_pipe_dl_allowed

void* g_cvEnabled = nullptr;   // _sofbuddy_clsv
void* g_cvLanes = nullptr;     // _sofbuddy_clsv_lanes
void* g_cvPerTick = nullptr;   // _sofbuddy_clsv_per_tick
void* g_cvInterval = nullptr;  // _sofbuddy_clsv_interval
void* g_cvChunk = nullptr;     // _sofbuddy_clsv_chunk
void* g_cvAuto = nullptr;      // _sofbuddy_clsv_auto
void* g_cvAckWait = nullptr;   // _sofbuddy_clsv_ack_wait_ms
void* g_cvPipeDl = nullptr;    // _sofbuddy_pipe_dl

float ReadLive(void*& cv, const char* name, const char* def, float fallback) {
    if (!cv)
        cv = Buddy_GetEngineCvar(name, def, kCvarFlagArchive, nullptr);
    return Buddy_ReadCvarValue(cv, fallback);
}

long LiveInt(void*& cv, const char* name, const char* def, long fallback) {
    float v = ReadLive(cv, name, def, static_cast<float>(fallback));
    if (v < -2000000000.0f || v > 2000000000.0f)
        return fallback;
    return static_cast<long>(v);
}

}  // namespace

void Clsv_InitCvars() {
    g_outSent.Bind("_sofbuddy_clsv_sent");
    g_outJobs.Bind("_sofbuddy_clsv_jobs");
    g_outErrors.Bind("_sofbuddy_clsv_errors");
    g_outDlAllowed.Bind("_sofbuddy_pipe_dl_allowed");
    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_clsv", "1", kCvarFlagArchive, nullptr);
    if (!g_cvLanes)
        g_cvLanes = Buddy_GetEngineCvar("_sofbuddy_clsv_lanes", "4", kCvarFlagArchive, nullptr);
    if (!g_cvPerTick)
        g_cvPerTick = Buddy_GetEngineCvar("_sofbuddy_clsv_per_tick", "2", kCvarFlagArchive, nullptr);
    if (!g_cvInterval)
        g_cvInterval = Buddy_GetEngineCvar("_sofbuddy_clsv_interval", "1", kCvarFlagArchive, nullptr);
    if (!g_cvChunk)
        g_cvChunk = Buddy_GetEngineCvar("_sofbuddy_clsv_chunk", "200", kCvarFlagArchive, nullptr);
    if (!g_cvAuto)
        g_cvAuto = Buddy_GetEngineCvar("_sofbuddy_clsv_auto", "1", kCvarFlagArchive, nullptr);
    if (!g_cvAckWait)
        g_cvAckWait =
            Buddy_GetEngineCvar("_sofbuddy_clsv_ack_wait_ms", "10000", kCvarFlagArchive, nullptr);
    if (!g_cvPipeDl)
        g_cvPipeDl = Buddy_GetEngineCvar("_sofbuddy_pipe_dl", "0", kCvarFlagArchive, nullptr);
}

bool Clsv_IsEnabled() {
    if (!g_cvEnabled)
        return true;
    return Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
}

int Clsv_Lanes() {
    return static_cast<int>(LiveInt(g_cvLanes, "_sofbuddy_clsv_lanes", "4", 4));
}

int Clsv_PerTick() {
    return static_cast<int>(LiveInt(g_cvPerTick, "_sofbuddy_clsv_per_tick", "2", 2));
}

int Clsv_Interval() {
    return static_cast<int>(LiveInt(g_cvInterval, "_sofbuddy_clsv_interval", "1", 1));
}

int Clsv_MaxEsc() {
    return static_cast<int>(LiveInt(g_cvChunk, "_sofbuddy_clsv_chunk", "200", 200));
}

bool Clsv_AutoSend() {
    if (!g_cvAuto)
        g_cvAuto = Buddy_GetEngineCvar("_sofbuddy_clsv_auto", "1", kCvarFlagArchive, nullptr);
    return Buddy_ReadCvarValue(g_cvAuto, 1.0f) != 0.0f;
}

unsigned Clsv_AckWaitMs() {
    long v = LiveInt(g_cvAckWait, "_sofbuddy_clsv_ack_wait_ms", "10000", 10000);
    if (v < 0)
        v = 0;
    return static_cast<unsigned>(v);
}

bool DlUnlock_Enabled() {
    if (!g_cvPipeDl)
        g_cvPipeDl = Buddy_GetEngineCvar("_sofbuddy_pipe_dl", "0", kCvarFlagArchive, nullptr);
    return Buddy_ReadCvarValue(g_cvPipeDl, 0.0f) != 0.0f;
}

void Clsv_SetDlAllowed(long long allowed) {
    char text[32];
    FormatI64(text, sizeof(text), allowed);
    g_outDlAllowed.Publish(static_cast<float>(allowed), text);
}

void Clsv_SetOutputs(long long sent, long long jobs, long long errors) {    char text[32];
    FormatI64(text, sizeof(text), sent);
    g_outSent.Publish(static_cast<float>(sent), text);
    FormatI64(text, sizeof(text), jobs);
    g_outJobs.Publish(static_cast<float>(jobs), text);
    FormatI64(text, sizeof(text), errors);
    g_outErrors.Publish(static_cast<float>(errors), text);
}

void Clsv_ShutdownCvars() {
    g_outSent.Restore();
    g_outJobs.Restore();
    g_outErrors.Restore();
    g_outDlAllowed.Restore();
}
