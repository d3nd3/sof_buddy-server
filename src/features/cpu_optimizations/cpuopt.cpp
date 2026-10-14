#include "cpuopt.h"

#include <cstdio>

namespace {

void* g_masterCv = nullptr;
void* g_strictCv = nullptr;
CpuOptFatalFn g_onFatal = nullptr;
int g_cbufHist[kCpuOptCbufHist];
int g_cbufHead = 0;
int g_cbufN = 0;

}  // namespace

void* CpuOpt_MasterCvar() {
    if (!g_masterCv)
        g_masterCv = Buddy_GetEngineCvar("_sofbuddy_cpuopt", "1", 1, nullptr);
    return g_masterCv;
}

void* CpuOpt_StrictCvar() {
    if (!g_strictCv)
        g_strictCv = Buddy_GetEngineCvar("_sofbuddy_cmdpark_strict", "1", 1, nullptr);
    return g_strictCv;
}

bool CpuOpt_Enabled() {
    return Buddy_ReadCvarValue(CpuOpt_MasterCvar(), 1.0f) != 0.0f;
}

bool CpuOpt_Strict() {
    return CpuOpt_Enabled() && Buddy_ReadCvarValue(CpuOpt_StrictCvar(), 1.0f) != 0.0f;
}

CpuOptFatalFn& CpuOpt_OnFatal() {
    return g_onFatal;
}

void CpuOpt_ResetCbufHist() {
    g_cbufHead = 0;
    g_cbufN = 0;
}

void CpuOpt_NoteCbuf(int bytes) {
    if (bytes < 0)
        bytes = 0;
    g_cbufHist[g_cbufHead] = bytes;
    g_cbufHead = (g_cbufHead + 1) % kCpuOptCbufHist;
    if (g_cbufN < kCpuOptCbufHist)
        ++g_cbufN;
}

void CpuOpt_Fatal(const char* why) {
    PrintOut(PRINT_BAD, "[cpuopt] fatal: %s\n", why ? why : "");
    const int n = g_cbufN;
    if (n <= 0) {
        PrintOut(PRINT_BAD, "[cpuopt] cmd_text cursize last 5 ticks: (none yet)\n");
    } else {
        const int start = (n < kCpuOptCbufHist) ? 0 : g_cbufHead;
        char line[96];
        int w = std::snprintf(line, sizeof(line),
                              "[cpuopt] cmd_text cursize last %d ticks (oldest first):", n);
        for (int i = 0; i < n && w > 0 && w < static_cast<int>(sizeof(line) - 8); ++i) {
            const int b = g_cbufHist[(start + i) % kCpuOptCbufHist];
            w += std::snprintf(line + w, sizeof(line) - static_cast<std::size_t>(w), " %d", b);
        }
        PrintOut(PRINT_BAD, "%s\n", line);
    }
    if (CpuOpt_Strict()) {
        ClampMonitor_LogSessionSummary();
        TickPace_LogSessionSummary();
    }
    if (g_onFatal) {
        g_onFatal(why);
        return;
    }
    ExitProcess(1);
}
