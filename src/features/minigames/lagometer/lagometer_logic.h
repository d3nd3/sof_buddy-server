#pragma once

#include "../minigames_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

constexpr float kLagTickBudgetMs = 100.0f;
constexpr int kLagBarCharacters = 40;
constexpr char kLagBarFill = '#';
constexpr char kLagBarFree = '-';

struct LagSnapshot {
    float game_ms = 0.0f;
    float engine_ms = 0.0f;
    float cmd_ms = 0.0f;
    float shell_ms = 0.0f;
    float spare_ms = 100.0f;
    int server_frame = -1;
};

inline void LagNormalizeBreakdown(float& sim_ms, float& engine_ms, float& buffer_ms,
                                  float& think_ms, float& spare_ms) {
    float used = sim_ms + engine_ms + buffer_ms + think_ms;
    if (used > kLagTickBudgetMs && used > 0.0f) {
        const float scale = kLagTickBudgetMs / used;
        sim_ms *= scale;
        engine_ms *= scale;
        buffer_ms *= scale;
        think_ms *= scale;
        used = kLagTickBudgetMs;
    }
    spare_ms = kLagTickBudgetMs - used;
    if (spare_ms < 0.0f)
        spare_ms = 0.0f;
}

inline int LagMsToBarChars(float ms) {
    if (ms <= 0.0f)
        return 0;
    int n = static_cast<int>(std::floor(ms * static_cast<float>(kLagBarCharacters) / kLagTickBudgetMs));
    if (n < 1 && ms > 0.0f)
        n = 1;
    if (n > kLagBarCharacters)
        n = kLagBarCharacters;
    return n;
}

inline void LagFillRun(char* out, int capacity, char ch, int count) {
    if (capacity <= 0 || count <= 0) {
        if (capacity > 0)
            out[0] = '\0';
        return;
    }
    if (count >= capacity)
        count = capacity - 1;
    for (int i = 0; i < count; ++i)
        out[i] = ch;
    out[count] = '\0';
}

inline int LagTextWidthPx(const char* s) {
    return s ? static_cast<int>(std::strlen(s)) * 8 : 0;
}

inline void LagRenderLegend(MgCanvas& c, int centerX, int y) {
    struct Key {
        const char* text;
        int tc;
    };
    static const Key keys[] = {{"Sim", kMgColGreen},
                               {"Engine", kMgColRed},
                               {"ClientThink", kMgColWhite},
                               {"Buffer", kMgColYellow},
                               {"Free", kMgColBlack}};
    constexpr int kGap = 8;
    constexpr int n = 5;
    int total = 0;
    for (int i = 0; i < n; ++i) {
        total += LagTextWidthPx(keys[i].text);
        if (i + 1 < n)
            total += kGap;
    }
    int x = centerX - total / 2;
    for (int i = 0; i < n; ++i) {
        MgCanvasTc(c, keys[i].tc);
        MgCanvasText(c, x, y, keys[i].text);
        x += LagTextWidthPx(keys[i].text) + kGap;
    }
}

inline bool LagEmitBarRun(MgCanvas& c, int& x, int y, int tc, char ch, int count) {
    if (count <= 0)
        return true;
    char run[48];
    LagFillRun(run, sizeof(run), ch, count);
    if (!MgCanvasTc(c, tc))
        return false;
    if (!MgCanvasText(c, x, y, run))
        return false;
    x += count * 8;
    return true;
}

inline void LagRenderBar(MgCanvas& c, int y, const LagSnapshot& s) {
    float sim = s.game_ms;
    float eng = s.engine_ms;
    float buf = s.cmd_ms;
    float think = s.shell_ms;
    float spare = s.spare_ms;
    LagNormalizeBreakdown(sim, eng, buf, think, spare);

    int a = LagMsToBarChars(sim);
    int b = LagMsToBarChars(eng);
    int d = LagMsToBarChars(buf);
    int f = LagMsToBarChars(think);
    int used = a + b + d + f;
    int free = kLagBarCharacters - used;
    if (free < 0)
        free = 0;

    int x = 160;
    LagEmitBarRun(c, x, y, kMgColGreen, kLagBarFill, a);
    LagEmitBarRun(c, x, y, kMgColRed, kLagBarFill, b);
    LagEmitBarRun(c, x, y, kMgColYellow, kLagBarFill, d);
    LagEmitBarRun(c, x, y, kMgColWhite, kLagBarFill, f);
    LagEmitBarRun(c, x, y, kMgColBlack, kLagBarFree, free);
}

inline void LagRender(const LagSnapshot& snapshot, MgCanvas& canvas) {
    MgCanvasClear(canvas);
    float sim = snapshot.game_ms;
    float eng = snapshot.engine_ms;
    float buf = snapshot.cmd_ms;
    float think = snapshot.shell_ms;
    float spare = snapshot.spare_ms;
    LagNormalizeBreakdown(sim, eng, buf, think, spare);

    MgCanvasTc(canvas, kMgColYellow);
    MgCanvasCenter(canvas, 320, 88, "SERVER TICK (100 ms)");

    LagRenderBar(canvas, 168, snapshot);

    char line[64];
    std::snprintf(line, sizeof(line), "%.0f ms free", spare);
    MgCanvasTc(canvas, spare >= 15.0f ? kMgColGreen : (spare >= 5.0f ? kMgColYellow : kMgColRed));
    MgCanvasCenter(canvas, 320, 196, line);

    MgCanvasTc(canvas, kMgColWhite);
    if (snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "busiest tick  |  sv.framenum %d",
                      snapshot.server_frame);
        MgCanvasCenter(canvas, 320, 228, line);
    } else {
        MgCanvasCenter(canvas, 320, 228, "busiest moment this map");
    }

    std::snprintf(line, sizeof(line), "Sim %.0f  Engine %.0f  Think %.0f  Buf %.0f", sim, eng,
                  think, buf);
    MgCanvasCenter(canvas, 320, 252, line);

    LagRenderLegend(canvas, 320, 276);

    MgCanvasTc(canvas, kMgColGreen);
    MgCanvasCenter(canvas, 320, 304, "score cycles  |  +use+score normal");
}
