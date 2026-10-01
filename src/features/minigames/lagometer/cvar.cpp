#include "cvar.h"

#include "buddy_import.h"

namespace {

void* g_cvEnable = nullptr;
void* g_cvWarmupFrames = nullptr;

}  // namespace

bool Lag_Enabled() {
    if (!g_cvEnable)
        g_cvEnable = Buddy_GetEngineCvar("_sofbuddy_lagometer_enable", "1", 1, nullptr);
    return Buddy_ReadCvarValue(g_cvEnable, 1.0f) != 0.0f;
}

int Lag_MapWarmupFrames() {
    if (!g_cvWarmupFrames)
        g_cvWarmupFrames =
            Buddy_GetEngineCvar("_sofbuddy_lagometer_warmup_frames", "200", 1, nullptr);
    const float v = Buddy_ReadCvarValue(g_cvWarmupFrames, 200.0f);
    if (v <= 0.0f)
        return 0;
    if (v > 100000.0f)
        return 100000;
    return static_cast<int>(v);
}
