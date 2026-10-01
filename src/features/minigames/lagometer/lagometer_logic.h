#pragma once

#include "../minigames_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

constexpr float kLagTickBudgetMs = 100.0f;
constexpr std::size_t kLagEventTagCap = 48;

inline void LagAppendEventTag(char* buf, std::size_t cap, const char* tag) {
    if (!buf || cap == 0 || !tag || !tag[0])
        return;
    const std::size_t used = std::strlen(buf);
    if (used == 0) {
        std::strncpy(buf, tag, cap - 1);
        buf[cap - 1] = '\0';
        return;
    }
    if (used + 1 + std::strlen(tag) >= cap)
        return;
    std::strncat(buf, "+", cap - used - 1);
    std::strncat(buf, tag, cap - std::strlen(buf) - 1);
}
constexpr int kLagBarCharacters = 40;
constexpr char kLagBarFill = '#';
constexpr char kLagBarFree = '-';

struct LagSnapshot {
    float game_ms = 0.0f;
    float engine_ms = 0.0f;
    float send_ms = 0.0f;
    float cmd_ms = 0.0f;
    float clcmove_ms = 0.0f;
    float spare_ms = 100.0f;
    float raw_total_ms = 0.0f;
    int highclamp_lost_ms = 0;
    char event_tag[kLagEventTagCap] = {};
    int server_frame = -1;
    int rank = 0;
    int rank_count = 0;
    bool live = false;
};

inline void LagNormalizeBreakdown(float& sim_ms, float& engine_ms, float& buffer_ms,
                                  float& move_ms, float& spare_ms) {
    float used = sim_ms + engine_ms + buffer_ms + move_ms;
    if (used > kLagTickBudgetMs && used > 0.0f) {
        const float scale = kLagTickBudgetMs / used;
        sim_ms *= scale;
        engine_ms *= scale;
        buffer_ms *= scale;
        move_ms *= scale;
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

struct LagKey {
    const char* text;
    int tc;
};

inline void LagRenderKeyRow(MgCanvas& c, int centerX, int y, const LagKey* keys, int n) {
    constexpr int kGap = 8;
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
    float eng = s.engine_ms + s.send_ms;
    float buf = s.cmd_ms;
    float move = s.clcmove_ms;
    float spare = s.spare_ms;
    LagNormalizeBreakdown(sim, eng, buf, move, spare);

    int a = LagMsToBarChars(sim);
    int b = LagMsToBarChars(eng);
    int d = LagMsToBarChars(buf);
    int f = LagMsToBarChars(move);
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

    const float raw_total =
        snapshot.raw_total_ms > 0.0f
            ? snapshot.raw_total_ms
            : snapshot.game_ms + snapshot.engine_ms + snapshot.send_ms + snapshot.cmd_ms +
                  snapshot.clcmove_ms;
    const float raw_sim = snapshot.game_ms;
    const float raw_eng = snapshot.engine_ms;
    const float raw_send = snapshot.send_ms;
    const float raw_move = snapshot.clcmove_ms;
    const float raw_buf = snapshot.cmd_ms;

    float sim = raw_sim;
    float eng = raw_eng + raw_send;
    float buf = raw_buf;
    float move = raw_move;
    float spare = snapshot.spare_ms;
    LagNormalizeBreakdown(sim, eng, buf, move, spare);

    MgCanvasTc(canvas, kMgColYellow);
    if (raw_total > kLagTickBudgetMs + 0.5f)
        MgCanvasCenter(canvas, 320, 88, "SERVER TICK (bar = 100 ms scale)");
    else
        MgCanvasCenter(canvas, 320, 88, "SERVER TICK (100 ms)");

    LagRenderBar(canvas, 168, snapshot);

    char line[80];
    if (raw_total > kLagTickBudgetMs + 0.05f)
        std::snprintf(line, sizeof(line), "wall %.1f ms  (%.1f ms free in bar)", raw_total, spare);
    else
        std::snprintf(line, sizeof(line), "%.1f ms free", spare);
    MgCanvasTc(canvas, spare >= 15.0f ? kMgColGreen : (spare >= 5.0f ? kMgColYellow : kMgColRed));
    MgCanvasCenter(canvas, 320, 196, line);

    MgCanvasTc(canvas, kMgColWhite);
    if (snapshot.live && snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "live  |  sv.framenum %d", snapshot.server_frame);
        MgCanvasCenter(canvas, 320, 220, line);
    } else if (snapshot.rank > 0 && snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "#%d / %d  |  sv.framenum %d", snapshot.rank,
                      snapshot.rank_count, snapshot.server_frame);
        MgCanvasCenter(canvas, 320, 220, line);
    } else if (snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "busiest tick  |  sv.framenum %d",
                      snapshot.server_frame);
        MgCanvasCenter(canvas, 320, 220, line);
    } else {
        MgCanvasCenter(canvas, 320, 220, "busiest moment this map");
    }

    int y = 244;
    if (snapshot.highclamp_lost_ms > 0) {
        std::snprintf(line, sizeof(line), "highclamp  dropped %d ms", snapshot.highclamp_lost_ms);
        MgCanvasTc(canvas, kMgColRed);
        MgCanvasCenter(canvas, 320, y, line);
        y += 24;
    }

    if (snapshot.event_tag[0])
        std::snprintf(line, sizeof(line), "game event  |  %s", snapshot.event_tag);
    else
        std::snprintf(line, sizeof(line), "game event  |  (none tagged)");
    MgCanvasTc(canvas, kMgColWhite);
    MgCanvasCenter(canvas, 320, y, line);
    y += 24;

    char sim_s[24], eng_s[24], move_s[24], buf_s[24], free_s[24];
    std::snprintf(sim_s, sizeof(sim_s), "Sim %.1f", raw_sim);
    std::snprintf(eng_s, sizeof(eng_s), "Eng %.1f", raw_eng + raw_send);
    std::snprintf(move_s, sizeof(move_s), "Move %.1f", raw_move);
    std::snprintf(buf_s, sizeof(buf_s), "Buf %.1f", raw_buf);
    std::snprintf(free_s, sizeof(free_s), "Free %.1f", spare);
    const LagKey raw[] = {{sim_s, kMgColGreen},
                          {eng_s, kMgColRed},
                          {move_s, kMgColWhite},
                          {buf_s, kMgColYellow},
                          {free_s, kMgColBlack}};
    LagRenderKeyRow(canvas, 320, y, raw, 5);
    y += 24;

    MgCanvasTc(canvas, kMgColGreen);
    MgCanvasCenter(canvas, 320, y, "score cycles  |  +use+score normal");
}
