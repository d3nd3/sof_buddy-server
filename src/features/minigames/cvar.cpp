#include "cvar.h"

#include "buddy_import.h"

namespace {

void* g_cvEnable = nullptr;
void* g_cvBg = nullptr;

}  // namespace

bool MgPlatformEnabled() {
    if (!g_cvEnable)
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_minigames_enable", "1", 1, nullptr);
    return Buddy_ReadCvarValue(g_cvEnable, 1.0f) != 0.0f;
}

bool MgBgUseBlack640() {
    if (!g_cvBg)
        g_cvBg = Buddy_GetEngineCvar("_sofbuddy_minigames_bg", "0", 1, nullptr);
    return Buddy_ReadCvarValue(g_cvBg, 0.0f) != 0.0f;
}
