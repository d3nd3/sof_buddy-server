#include "cvar.h"
#include "../cpuopt.h"
#include "buddy_import.h"

namespace {

constexpr int kCvarFlagArchive = 1;

void* g_cvEnable = nullptr;  // _sofbuddy_hashmap

}  // namespace

void HashLookup_InitCvars() {
    if (!g_cvEnable)
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_hashmap", "1", kCvarFlagArchive, nullptr);
}

bool HashLookup_Enabled() {
    return CpuOpt_Enabled() && Buddy_ReadCvarValue(g_cvEnable, 1.0f) != 0.0f;
}
