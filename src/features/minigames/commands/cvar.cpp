#include "cvar.h"

#include "buddy_import.h"

namespace {

void* g_cvEnable = nullptr;

}  // namespace

bool Cmds_Enabled() {
    if (!g_cvEnable)
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_cmds_enable", "1", 1, nullptr);
    return Buddy_ReadCvarValue(g_cvEnable, 1.0f) != 0.0f;
}
