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
    float cmd_ms = 0.0f;
    float shell_ms = 0.0f;
    float spare_ms = 100.0f;
};

inline void LagNormalizeBreakdown(float& game_ms, float& cmd_ms, float& shell_ms, float& spare_ms) {
    float used = game_ms + cmd_ms + shell_ms;
    if (used > kLagTickBudgetMs && used > 0.0f) {
        const float scale = kLagTickBudgetMs / used;
        game_ms *= scale;
        cmd_ms *= scale;
        shell_ms *= scale;
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
    float game = s.game_ms;
    float cmd = s.cmd_ms;
    float shell = s.shell_ms;
    float spare = s.spare_ms;
    LagNormalizeBreakdown(game, cmd, shell, spare);

    int g = LagMsToBarChars(game);
    int d = LagMsToBarChars(cmd);
    int f = LagMsToBarChars(shell);
    int used = g + d + f;
    int free = kLagBarCharacters - used;
    if (free < 0)
        free = 0;

    int x = 160;
    LagEmitBarRun(c, x, y, kMgColGreen, kLagBarFill, g);
    LagEmitBarRun(c, x, y, kMgColYellow, kLagBarFill, d);
    LagEmitBarRun(c, x, y, kMgColWhite, kLagBarFill, f);
    LagEmitBarRun(c, x, y, kMgColBlack, kLagBarFree, free);
}

inline void LagRender(const LagSnapshot& snapshot, MgCanvas& canvas) {
    MgCanvasClear(canvas);
    float game = snapshot.game_ms;
    float cmd = snapshot.cmd_ms;
    float shell = snapshot.shell_ms;
    float spare = snapshot.spare_ms;
    LagNormalizeBreakdown(game, cmd, shell, spare);

    MgCanvasTc(canvas, kMgColYellow);
    MgCanvasCenter(canvas, 320, 88, "SERVER TICK (100 ms)");

    LagRenderBar(canvas, 168, snapshot);

    char line[64];
    std::snprintf(line, sizeof(line), "%.0f ms free", spare);
    MgCanvasTc(canvas, spare >= 15.0f ? kMgColGreen : (spare >= 5.0f ? kMgColYellow : kMgColRed));
    MgCanvasCenter(canvas, 320, 196, line);

    MgCanvasTc(canvas, kMgColWhite);
    MgCanvasCenter(canvas, 320, 228, "busiest moment this map");

    std::snprintf(line, sizeof(line), "Play %.0f  Console %.0f  Overhead %.0f", game, cmd, shell);
    MgCanvasCenter(canvas, 320, 252, line);

    int lx = 88;
    int ly = 276;
    MgCanvasTc(canvas, kMgColGreen);
    MgCanvasText(canvas, lx, ly, "Play");
    lx += 48;
    MgCanvasTc(canvas, kMgColYellow);
    MgCanvasText(canvas, lx, ly, "Console");
    lx += 72;
    MgCanvasTc(canvas, kMgColWhite);
    MgCanvasText(canvas, lx, ly, "Overhead");
    lx += 80;
    MgCanvasTc(canvas, kMgColBlack);
    MgCanvasText(canvas, lx, ly, "Free");

    MgCanvasTc(canvas, kMgColGreen);
    MgCanvasCenter(canvas, 320, 304, "score cycles  |  +use+score normal");
}
