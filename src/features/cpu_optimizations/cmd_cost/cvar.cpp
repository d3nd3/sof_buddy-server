#include "cvar.h"
#include "cmd_cost.h"
#include "../cpuopt.h"
#include "buddy_import.h"
#include <cstddef>
#include <cstdio>
#include <windows.h>

namespace cmdcost {

constexpr unsigned kStr = 0x04, kVal = 0x18;
constexpr int kNoSet = 8, kArchive = 1;

struct Out {
    void* cv = nullptr;
    char* original = nullptr;
    char buf[2][32] = {};
    int live = 0;
    void Bind(const char* name) {
        if (cv) return;
        cv = Buddy_GetEngineCvar(name, "0", kNoSet, nullptr);
        if (cv) original = *reinterpret_cast<char**>(static_cast<char*>(cv) + kStr);
    }
    void Pub(float v, const char* text) {
        if (!cv) return;
        char* base = static_cast<char*>(cv);
        *reinterpret_cast<volatile float*>(base + kVal) = v;
        char* s = buf[1 - live];
        std::size_t i = 0;
        for (; text[i] && i + 1 < sizeof(buf[0]); ++i) s[i] = text[i];
        s[i] = '\0';
        *reinterpret_cast<char* volatile*>(base + kStr) = s;
        live = 1 - live;
    }
    void Restore() {
        if (cv && original)
            *reinterpret_cast<char* volatile*>(static_cast<char*>(cv) + kStr) = original;
        cv = nullptr;
    }
};

Out g_max, g_name, g_ema, g_emaName, g_n;
void* g_on = nullptr;

void InitCvars() {
    g_max.Bind("_sofbuddy_cmdcost_max");
    g_name.Bind("_sofbuddy_cmdcost_name");
    g_ema.Bind("_sofbuddy_cmdcost_ema");
    g_emaName.Bind("_sofbuddy_cmdcost_ema_name");
    g_n.Bind("_sofbuddy_cmdcost_n");
    if (!g_on)
        g_on = Buddy_GetEngineCvar("_sofbuddy_cmdcost", "1", kArchive, nullptr);
}

bool Enabled() {
    return CpuOpt_Enabled() && Buddy_ReadCvarValue(g_on, 0.0f) != 0.0f;
}

void Publish(float maxMs, const char* maxName, float emaMs, const char* emaName, int n) {
    char t[32];
    std::snprintf(t, sizeof(t), "%.3f", static_cast<double>(maxMs));
    g_max.Pub(maxMs, t);
    g_name.Pub(maxMs, maxName ? maxName : "?");
    std::snprintf(t, sizeof(t), "%.3f", static_cast<double>(emaMs));
    g_ema.Pub(emaMs, t);
    g_emaName.Pub(emaMs, emaName ? emaName : "?");
    std::snprintf(t, sizeof(t), "%d", n);
    g_n.Pub(static_cast<float>(n), t);
}

}  // namespace cmdcost

extern "C" void CmdCost_Shutdown() {
    cmdcost::UnwrapAll();
    cmdcost::g_max.Restore();
    cmdcost::g_name.Restore();
    cmdcost::g_ema.Restore();
    cmdcost::g_emaName.Restore();
    cmdcost::g_n.Restore();
}
