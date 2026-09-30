#pragma once

#include "../minigames_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

constexpr float kLagTickBudgetMs = 100.0f;
constexpr int kLagBarCharacters = 40;

struct LagSnapshot {
    float spare_headroom_ms = 100.0f;
};

inline float LagSpareHeadroomMs(float worst_cmd_drain_ms, float worst_tick_svframe_ms) {
    const float busy = worst_cmd_drain_ms + worst_tick_svframe_ms;
    float spare = kLagTickBudgetMs - busy;
    if (spare < 0.0f)
        spare = 0.0f;
    if (spare > kLagTickBudgetMs)
        spare = kLagTickBudgetMs;
    return spare;
}

inline int LagHeadroomColor(float spare_ms) {
    if (spare_ms >= 40.0f)
        return kMgColGreen;
    if (spare_ms >= 15.0f)
        return kMgColYellow;
    return kMgColRed;
}

inline void LagFormatHeadroomBar(char* out, int capacity, float spare_ms) {
    if (capacity <= 0)
        return;
    int filled = static_cast<int>(
        std::floor(spare_ms * static_cast<float>(kLagBarCharacters) / kLagTickBudgetMs));
    if (filled < 0)
        filled = 0;
    if (filled > kLagBarCharacters)
        filled = kLagBarCharacters;
    int i = 0;
    for (; i < filled && i + 1 < capacity; ++i)
        out[i] = '=';
    for (; i < kLagBarCharacters && i + 1 < capacity; ++i)
        out[i] = '-';
    out[i] = '\0';
}

inline void LagRender(const LagSnapshot& snapshot, MgCanvas& canvas) {
    MgCanvasClear(canvas);
    const float spare = snapshot.spare_headroom_ms;
    const int color = LagHeadroomColor(spare);

    MgCanvasTc(canvas, kMgColYellow);
    MgCanvasCenter(canvas, 320, 100, "TICK HEADROOM");
    MgCanvasTc(canvas, color);
    char value[32];
    std::snprintf(value, sizeof(value), "%.0f ms", spare);
    MgCanvasCenter(canvas, 320, 150, value);

    char bar[kLagBarCharacters + 4];
    LagFormatHeadroomBar(bar, sizeof(bar), spare);
    MgCanvasTc(canvas, color);
    MgCanvasCenter(canvas, 320, 200, bar);

    MgCanvasTc(canvas, kMgColWhite);
    MgCanvasCenter(canvas, 320, 248, "worst spare on this map");
    MgCanvasTc(canvas, kMgColGreen);
    MgCanvasCenter(canvas, 320, 280, "lag hide  |  +use+score toggle");
    MgCanvasTc(canvas, kMgColWhite);
}
