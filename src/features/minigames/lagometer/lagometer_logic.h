#pragma once

// Pure lagometer layout (no engine). Unit-testable on Linux.

#include "../minigames_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

constexpr float kLagTickMs = 100.0f;
constexpr int kLagBarChars = 34;
constexpr int kLagBarScaleMs = 100;  // full bar = one server tick budget

struct LagSnapshot {
    float drainLastMs = 0.0f;
    float drainMaxMs = 0.0f;
    float lateAvgMs = 0.0f;
    long long highclamps = 0;
    long long lowclamps = 0;
    int clampLastMs = 0;
    long long clampLostMs = 0;
    float clampAvgMs = 0.0f;
    int lowclampWorstMs = 0;
    long long tickpaceSaved = 0;
    int parkBytes = 0;
    long long parkDefers = 0;
    int cbufBytes = 0;
    int cbufFillMax = 0;
    float cmdcostMaxMs = 0.0f;
    int dedicated = 0;
    int cpuopt = 1;
    int tickpace = 1;
    int tickpaceSettle = 1;
    int spinMs = 0;
    int cmdpark = 1;
    int cmdparkStrict = 1;
    int reserveMs = 0;
    int qpc = 1;
};

inline int LagMsColor(float ms) {
    if (ms < 25.0f)
        return kMgColGreen;
    if (ms < 60.0f)
        return kMgColYellow;
    return kMgColRed;
}

inline void LagFormatBar(char* out, int cap, float ms) {
    if (cap <= 0)
        return;
    int filled = static_cast<int>(std::floor(ms * static_cast<float>(kLagBarChars) /
                                             static_cast<float>(kLagBarScaleMs)));
    if (filled < 0)
        filled = 0;
    if (filled > kLagBarChars)
        filled = kLagBarChars;
    int i = 0;
    for (; i < filled && i + 1 < cap; ++i)
        out[i] = '=';
    for (; i < kLagBarChars && i + 1 < cap; ++i)
        out[i] = '-';
    out[i] = '\0';
}

inline const char* LagOnOff(int v) { return v ? "ON" : "off"; }

inline void LagRender(const LagSnapshot& s, MgCanvas& c) {
    MgCanvasClear(c);
    MgCanvasPic(c, 0, 0, kMgSbMgBg);
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 320, 36, "SERVER LAGOMETER");
    MgCanvasTc(c, kMgColWhite);

    char line[96];
    char bar[kLagBarChars + 4];

    LagFormatBar(bar, sizeof(bar), s.drainLastMs);
    std::snprintf(line, sizeof(line), "drain last %5.1fms", s.drainLastMs);
    MgCanvasText(c, 24, 68, line);
    MgCanvasTc(c, LagMsColor(s.drainLastMs));
    MgCanvasText(c, 200, 68, bar);
    MgCanvasTc(c, kMgColWhite);

    LagFormatBar(bar, sizeof(bar), s.drainMaxMs);
    std::snprintf(line, sizeof(line), "drain max  %5.1fms", s.drainMaxMs);
    MgCanvasText(c, 24, 88, line);
    MgCanvasTc(c, LagMsColor(s.drainMaxMs));
    MgCanvasText(c, 200, 88, bar);
    MgCanvasTc(c, kMgColWhite);

    std::snprintf(line, sizeof(line), "tick late avg %.1fms  saved=%lld", s.lateAvgMs,
                  static_cast<long long>(s.tickpaceSaved));
    MgCanvasText(c, 24, 112, line);

    std::snprintf(line, sizeof(line), "hi=%lld lo=%lld last=%d lost=%lld avg=%.0f",
                  s.highclamps, s.lowclamps, s.clampLastMs, s.clampLostMs, s.clampAvgMs);
    MgCanvasText(c, 24, 132, line);
    if (s.lowclampWorstMs > 0) {
        std::snprintf(line, sizeof(line), "lowclamp worst %dms", s.lowclampWorstMs);
        MgCanvasText(c, 24, 152, line);
    } else {
        MgCanvasText(c, 24, 152, "lowclamp worst 0ms");
    }

    std::snprintf(line, sizeof(line), "park %dB def=%lld cbuf=%d peak=%d", s.parkBytes,
                  s.parkDefers, s.cbufBytes, s.cbufFillMax);
    MgCanvasText(c, 24, 176, line);
    if (s.cmdcostMaxMs > 0.0f) {
        std::snprintf(line, sizeof(line), "cmdcost worst %.2fms", s.cmdcostMaxMs);
        MgCanvasText(c, 24, 196, line);
    }

    MgCanvasTc(c, kMgColYellow);
    MgCanvasText(c, 24, 224, "OPTIONS (live cvars)");
    MgCanvasTc(c, kMgColWhite);
    std::snprintf(line, sizeof(line), "ded=%s cpu=%s tp=%s settle=%s spin=%d",
                  LagOnOff(s.dedicated), LagOnOff(s.cpuopt), LagOnOff(s.tickpace),
                  LagOnOff(s.tickpaceSettle), s.spinMs);
    MgCanvasText(c, 24, 244, line);
    std::snprintf(line, sizeof(line), "park=%s strict=%s rsv=%dms qpc=%s",
                  LagOnOff(s.cmdpark), LagOnOff(s.cmdparkStrict), s.reserveMs,
                  LagOnOff(s.qpc));
    MgCanvasText(c, 24, 264, line);
    MgCanvasTc(c, kMgColGreen);
    MgCanvasCenter(c, 320, 300, "type lag to close  |  +use+score");
    MgCanvasTc(c, kMgColWhite);
}
