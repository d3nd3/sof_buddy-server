#include "cvar.h"

#include "buddy_import.h"

namespace {

void* g_cvEnable = nullptr;

}  // namespace

bool Ttt_Enabled() {
    if (!g_cvEnable)
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_ttt_enable", "0", 1, nullptr);
    return Buddy_ReadCvarValue(g_cvEnable, 0.0f) != 0.0f;
}
