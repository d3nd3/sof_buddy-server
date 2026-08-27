#include "cvar.h"
#include "buddy_import.h"

namespace {

constexpr int kCvarFlagArchive = 1;

void* g_cvEnable = nullptr;  // _sofbuddy_hashmap

}  // namespace

void HashLookup_InitCvars() {
    if (!g_cvEnable) {
        // Off by default. The patches rewrite engine code, so turning them on
        // is an explicit decision per server: set it on the command line
        // (+set _sofbuddy_hashmap 1) or in a config that runs before the map
        // loads, since it is read once when the game DLL comes up.
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_hashmap", "0", kCvarFlagArchive, nullptr);
    }
}

bool HashLookup_Enabled() {
    return Buddy_ReadCvarValue(g_cvEnable, 0.0f) != 0.0f;
}
