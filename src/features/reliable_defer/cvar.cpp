#include "cvar.h"
#include "buddy_import.h"

RelDefPolicy RelDef_ReadPolicy() {
    RelDefPolicy p;
    p.reserveBytes = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_reserve", "256", 1, nullptr), 256.0f));
    p.frameReserveBytes = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_frame_reserve", "0", 1, nullptr), 0.0f));
    p.onePerTick = Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_one_per_tick", "0", 1, nullptr), 0.0f) != 0.0f;
    p.maxQueue = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_max_queue", "32", 1, nullptr), 32.0f));
    p.maxQueueBytes = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_max_queue_bytes", "262144", 1, nullptr),
        262144.0f));
    p.frameFirst = Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_frame_first", "1", 1, nullptr), 1.0f) != 0.0f;
    p.maxDripWaitTicks = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_max_drip_wait", "10", 1, nullptr), 10.0f));
    if (p.maxDripWaitTicks < 0)
        p.maxDripWaitTicks = 0;
    return p;
}
