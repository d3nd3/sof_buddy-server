#include "cvar.h"
#include "buddy_import.h"

namespace {

void* g_cvCustomRespawn = nullptr;

}  // namespace

void CtfSpawn_InitCvars() {
    if (!g_cvCustomRespawn) {
        g_cvCustomRespawn = Buddy_GetEngineCvar("_sofbuddy_custom_respawn", "1", 0, nullptr);
    }
}

bool CtfSpawn_CustomRespawnEnabled() {
    /* Never call the engine cvar API from gameplay hooks: query once at
     * bootstrap (GameDllLoaded), afterwards only read the cached cvar_t. */
    if (!g_cvCustomRespawn) {
        return true;
    }
    return Buddy_ReadCvarValue(g_cvCustomRespawn, 1.0f) != 0.0f;
}
