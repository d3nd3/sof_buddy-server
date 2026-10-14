#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "windows.h"
#include "buddy_import.h"
#include "log.h"

namespace fake {
std::int64_t qpc = 0;
constexpr std::int64_t kHz = 1000000;
void AdvanceMs(double ms) { if (ms > 0) qpc += static_cast<std::int64_t>(ms * 1000.0 + 0.5); }

struct Cvar {
    char* name; char* string; char* latched; int flags; int unknown; int modified;
    float value; void* next;
};
static_assert(sizeof(Cvar) == 0x20, "cvar_t stub");
std::vector<Cvar*> cvars;
Cvar* Find(const char* name) {
    for (Cvar* c : cvars)
        if (std::strcmp(c->name, name) == 0) return c;
    return nullptr;
}
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    out->QuadPart = fake::qpc;
    return 1;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = fake::kHz;
    return 1;
}

extern "C" void* Buddy_GetEngineCvar(const char* name, const char* value, int flags, void*) {
    if (fake::Cvar* e = fake::Find(name)) return e;
    auto* c = static_cast<fake::Cvar*>(std::calloc(1, sizeof(fake::Cvar)));
    c->name = strdup(name);
    c->string = strdup(value);
    c->flags = flags;
    c->value = static_cast<float>(std::atof(value));
    fake::cvars.push_back(c);
    return c;
}
extern "C" float Buddy_ReadCvarValue(void* cv, float def) {
    if (!cv) return def;
    return *reinterpret_cast<float*>(static_cast<char*>(cv) + 0x18);
}

void cmdcost_RegisterAddonsCmd() {}
void cmdcost_RegisterEventsCmd() {}
void cmdcost_RegisterSofplusCmds() {}

#include "../../../src/features/cpu_optimizations/cmd_cost/cvar.cpp"
#include "../../../src/features/cpu_optimizations/cmd_cost/cmd_cost.cpp"

int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } while (0)

static void Run(const char* line, double ms) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", line);
    char* p = buf;
    cmdcost_CmdPre(p);
    fake::AdvanceMs(ms);
    cmdcost_CmdPost(p);
}

int main() {
    cmdcost_OnGameDllLoaded(nullptr);
    if (fake::Cvar* on = fake::Find("_sofbuddy_cmdcost"))
        on->value = 1.f;
    CHECK(cmdcost::g.ready, "clock not ready");
    CHECK(cmdcost::PredictMs("sp_sc_cvar_math_add") == 0.f, "predicted before samples");

    for (int i = 0; i < 4; ++i)
        Run("sp_sc_cvar_math_add wcnt 1", 2.0);
    float p = cmdcost::PredictMs("sp_sc_cvar_math_add extra");
    CHECK(p > 1.5f && p < 2.5f, "ema %.3f not ~2ms", p);
    CHECK(cmdcost::g.maxMs > 1.9 && cmdcost::g.maxMs < 2.1, "max %.3f", cmdcost::g.maxMs);

    Run("wait", 8.0);
    CHECK(cmdcost::g.maxMs > 7.5, "wait did not become worst (%.3f)", cmdcost::g.maxMs);
    CHECK(cmdcost::PredictMs("nope") == 0.f, "unknown predicted");

    cmdcost::Dump();
    cmdcost::Reset();
    CHECK(cmdcost::g.maxMs == 0, "reset max");
    CHECK(cmdcost::PredictMs("wait") == 0.f, "reset wait ema");

    static int g_spins = 0;
    g_spins = 0;
    cmdcost::InstallOrig("sp_sc_cvar_math_add", []() {
        ++g_spins;
        fake::AdvanceMs(0.001);
    });
    cmdcost::Spin("sp_sc_cvar_math_add", 0);
    CHECK(g_spins >= 1000, "adaptive spin too few (%d)", g_spins);
    CHECK(cmdcost::g.maxMs > 0.0005, "spin per-call low %.6f", cmdcost::g.maxMs);
    CHECK(cmdcost::g.maxMs < 0.002, "spin per-call high %.6f", cmdcost::g.maxMs);

    if (g_failures == 0)
        std::printf("All cmd_cost tests passed.\n");
    else
        std::printf("%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
