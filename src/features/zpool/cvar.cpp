#include "cvar.h"
#include "buddy_import.h"

namespace {

constexpr int kCvarFlagArchive = 1;

void* g_cvEnable = nullptr;  // _sofbuddy_zpool

}  // namespace

void ZPool_InitCvars() {
    if (!g_cvEnable) {
        // Off by default, like hash_lookup: this replaces the engine's
        // allocator entry points, so switching it on is a deliberate per-server
        // decision. Set it before the first map loads.
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_zpool", "0", kCvarFlagArchive, nullptr);
    }
}

bool ZPool_Enabled() {
    return Buddy_ReadCvarValue(g_cvEnable, 0.0f) != 0.0f;
}
