#pragma once

// Pure lagometer layout (no engine). Unit-testable on Linux.

#include "../minigames_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

constexpr int kLagBarCharacters = 34;
constexpr int kLagBarFullScaleMilliseconds = 100;  // one server tick budget

struct LagSnapshot {
    float command_buffer_drain_last_ms = 0.0f;
    float command_buffer_drain_peak_ms = 0.0f;
    float tick_late_average_ms = 0.0f;
    long long timer_clamp_high_count = 0;
    long long timer_clamp_low_count = 0;
    int timer_clamp_last_shift_ms = 0;
    long long timer_clamp_total_lost_ms = 0;
    int timer_clamp_low_worst_ms = 0;
    int command_buffer_bytes = 0;
    int command_buffer_peak_bytes = 0;
    int command_park_queued_bytes = 0;
    float slowest_command_ms = 0.0f;
    bool cpu_optimizations_enabled = true;
    bool tick_pacing_enabled = true;
    bool command_parking_enabled = true;
    bool command_parking_strict = true;
};

inline int LagMillisecondsColor(float milliseconds) {
    if (milliseconds < 25.0f)
        return kMgColGreen;
    if (milliseconds < 60.0f)
        return kMgColYellow;
    return kMgColRed;
}

inline void LagFormatDrainBar(char* out, int capacity, float milliseconds) {
    if (capacity <= 0)
        return;
    int filled = static_cast<int>(
        std::floor(milliseconds * static_cast<float>(kLagBarCharacters) /
                   static_cast<float>(kLagBarFullScaleMilliseconds)));
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
    MgCanvasTc(canvas, kMgColYellow);
    MgCanvasCenter(canvas, 320, 36, "SERVER TICK HEALTH");
    MgCanvasTc(canvas, kMgColWhite);

    char line[112];
    char bar[kLagBarCharacters + 4];

    std::snprintf(line, sizeof(line), "Cmd-buffer work this tick: %5.1f ms",
                  snapshot.command_buffer_drain_last_ms);
    MgCanvasText(canvas, 24, 68, line);
    LagFormatDrainBar(bar, sizeof(bar), snapshot.command_buffer_drain_last_ms);
    MgCanvasTc(canvas, LagMillisecondsColor(snapshot.command_buffer_drain_last_ms));
    MgCanvasText(canvas, 200, 68, bar);
    MgCanvasTc(canvas, kMgColWhite);

    std::snprintf(line, sizeof(line), "Cmd-buffer work worst:    %5.1f ms",
                  snapshot.command_buffer_drain_peak_ms);
    MgCanvasText(canvas, 24, 92, line);

    std::snprintf(line, sizeof(line), "Tick pacing late (avg):   %5.1f ms",
                  snapshot.tick_late_average_ms);
    MgCanvasText(canvas, 24, 116, line);

    std::snprintf(
        line, sizeof(line),
        "Timer clamps: %lld high, %lld low  last shift %+d ms  total lost %lld ms",
        snapshot.timer_clamp_high_count, snapshot.timer_clamp_low_count,
        snapshot.timer_clamp_last_shift_ms, snapshot.timer_clamp_total_lost_ms);
    MgCanvasText(canvas, 24, 140, line);
    if (snapshot.timer_clamp_low_worst_ms > 0) {
        std::snprintf(line, sizeof(line), "Worst low-clamp stretch:  %d ms",
                      snapshot.timer_clamp_low_worst_ms);
        MgCanvasText(canvas, 24, 160, line);
    }

    std::snprintf(line, sizeof(line), "Engine cmd buffer: %d B (peak %d B)",
                  snapshot.command_buffer_bytes, snapshot.command_buffer_peak_bytes);
    MgCanvasText(canvas, 24, 184, line);
    if (snapshot.command_park_queued_bytes > 0) {
        std::snprintf(line, sizeof(line), "Deferred cmd text parked: %d B",
                      snapshot.command_park_queued_bytes);
        MgCanvasText(canvas, 24, 204, line);
    }
    if (snapshot.slowest_command_ms > 0.0f) {
        std::snprintf(line, sizeof(line), "Slowest handler (recent): %.2f ms",
                      snapshot.slowest_command_ms);
        MgCanvasText(canvas, 24, 224, line);
    }

    MgCanvasTc(canvas, kMgColYellow);
    MgCanvasText(canvas, 24, 252, "Buddy features (server)");
    MgCanvasTc(canvas, kMgColWhite);
    std::snprintf(line, sizeof(line), "CPU opts %s  tick pace %s  cmd park %s%s",
                  snapshot.cpu_optimizations_enabled ? "on" : "off",
                  snapshot.tick_pacing_enabled ? "on" : "off",
                  snapshot.command_parking_enabled ? "on" : "off",
                  snapshot.command_parking_strict ? " strict" : "");
    MgCanvasText(canvas, 24, 272, line);

    MgCanvasTc(canvas, kMgColGreen);
    MgCanvasCenter(canvas, 320, 304, "type lag to hide  |  score closes tab");
    MgCanvasTc(canvas, kMgColWhite);
}
