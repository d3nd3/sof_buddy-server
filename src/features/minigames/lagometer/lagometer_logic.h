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
    bool lag_debug = false;
    float wall_total_ms = 0.0f;
    float gap_ms = 0.0f;
    float wall_rp_ms = 0.0f;
    float wall_move_ms = 0.0f;
    float wall_run_ms = 0.0f;
    float cpu_rp_ms = 0.0f;
    float rp_sched_ms = 0.0f;
    float shell_wall_ms = 0.0f;
    float shell_sched_ms = 0.0f;
    int rp_calls = 0;
    int move_calls = 0;
    int pkt_in = 0;
};

// Live HUD must not publish the default zero-filled snapshot (Sim 0.0…).
// A committed tick always has some wall time; raw_total_ms == 0 is not showable.
inline bool LagLiveSamplePublishable(bool valid, float raw_total_ms) {
    return valid && raw_total_ms > 0.0f;
}

// Warmup skips busiest-rank bookkeeping only; live still commits every eligible tick.
inline bool LagWarmupBlocksBusiest(int map_warmup_frames) {
    return map_warmup_frames > 0;
}

// Fills out for live mode. Returns false → caller must not LagRender/push
// (keeps the previous canvas instead of painting Sim 0.0).
inline bool LagFillLiveView(bool valid, float raw_total_ms, const LagSnapshot& sample,
                            LagSnapshot& out) {
    if (!LagLiveSamplePublishable(valid, raw_total_ms))
        return false;
    out = sample;
    out.live = true;
    out.rank = 0;
    return true;
}

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

inline float LagSnapshotEngMs(const LagSnapshot& s) {
    return s.engine_ms + s.send_ms;
}

inline void LagRefreshSpare(LagSnapshot& s) {
    float sim = s.game_ms;
    float eng = LagSnapshotEngMs(s);
    float buf = s.cmd_ms;
    float move = s.clcmove_ms;
    LagNormalizeBreakdown(sim, eng, buf, move, s.spare_ms);
    s.raw_total_ms = s.game_ms + s.engine_ms + s.send_ms + s.cmd_ms + s.clcmove_ms;
}

// %.1f rounds |x| < 0.05 to "0.0". Live Move/Eng often land there (empty
// ReadPackets quantum / send shorter than a QPC tick) while Sim is real.
constexpr float kLagDisplayEpsMs = 0.05f;
// Bridge a few missed quanta — sustained zeros (disconnect) must still show.
constexpr int kLagLiveKeyHoldMax = 3;

// prev is the last *published* live view. Re-reads of the same server_frame
// must be idempotent (PushLag + maintain both call ReadView).
inline void LagStabilizeLiveKeys(const LagSnapshot& prev, LagSnapshot& next, int& hold_move,
                                 int& hold_eng) {
    if (next.server_frame >= 0 && next.server_frame == prev.server_frame) {
        next = prev;
        return;
    }
    if (next.game_ms <= 0.0f) {
        hold_move = 0;
        hold_eng = 0;
        return;
    }
    if (next.clcmove_ms < kLagDisplayEpsMs && prev.clcmove_ms >= kLagDisplayEpsMs &&
        hold_move < kLagLiveKeyHoldMax) {
        next.clcmove_ms = prev.clcmove_ms;
        ++hold_move;
    } else if (next.clcmove_ms >= kLagDisplayEpsMs) {
        hold_move = 0;
    } else {
        hold_move = kLagLiveKeyHoldMax;
    }
    if (LagSnapshotEngMs(next) < kLagDisplayEpsMs && LagSnapshotEngMs(prev) >= kLagDisplayEpsMs &&
        hold_eng < kLagLiveKeyHoldMax) {
        next.engine_ms = prev.engine_ms;
        next.send_ms = prev.send_ms;
        ++hold_eng;
    } else if (LagSnapshotEngMs(next) >= kLagDisplayEpsMs) {
        hold_eng = 0;
    } else {
        hold_eng = kLagLiveKeyHoldMax;
    }
    LagRefreshSpare(next);
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
    const char* color;
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
        MgCanvasPText(c, x, y, keys[i].color, keys[i].text);
        x += LagTextWidthPx(keys[i].text) + kGap;
    }
}

inline bool LagEmitBarRun(MgCanvas& c, int& x, int y, const char* color, char ch, int count) {
    if (count <= 0)
        return true;
    char run[48];
    LagFillRun(run, sizeof(run), ch, count);
    if (!MgCanvasPText(c, x, y, color, run))
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
    LagEmitBarRun(c, x, y, kMgPGreen, kLagBarFill, a);
    LagEmitBarRun(c, x, y, kMgPRed, kLagBarFill, b);
    LagEmitBarRun(c, x, y, kMgPYellow, kLagBarFill, d);
    LagEmitBarRun(c, x, y, kMgPWhite, kLagBarFill, f);
    LagEmitBarRun(c, x, y, kMgPBlack, kLagBarFree, free);
}

inline void LagRender(const LagSnapshot& snapshot, MgCanvas& canvas, int selectedAction = 0,
                      bool showTips = true) {
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

    if (raw_total > kLagTickBudgetMs + 0.5f)
        MgCanvasPCenter(canvas, 320, 88, kMgPGreen, "SERVER TICK (bar = 100 ms scale)");
    else
        MgCanvasPCenter(canvas, 320, 88, kMgPGreen, "SERVER TICK (100 ms)");

    LagRenderBar(canvas, 168, snapshot);

    char line[80];
    if (raw_total > kLagTickBudgetMs + 0.05f)
        std::snprintf(line, sizeof(line), "wall %.1f ms  (%.1f ms free in bar)", raw_total, spare);
    else
        std::snprintf(line, sizeof(line), "%.1f ms free", spare);
    MgCanvasPCenter(canvas, 320, 196,
                    spare >= 15.0f ? kMgPGreen : (spare >= 5.0f ? kMgPYellow : kMgPRed), line);

    if (snapshot.live && snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "live  |  sv.framenum %d", snapshot.server_frame);
        MgCanvasPCenter(canvas, 320, 220, kMgPWhite, line);
    } else if (snapshot.rank > 0 && snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "#%d / %d  |  sv.framenum %d", snapshot.rank,
                      snapshot.rank_count, snapshot.server_frame);
        MgCanvasPCenter(canvas, 320, 220, kMgPWhite, line);
    } else if (snapshot.server_frame >= 0) {
        std::snprintf(line, sizeof(line), "busiest tick  |  sv.framenum %d",
                      snapshot.server_frame);
        MgCanvasPCenter(canvas, 320, 220, kMgPWhite, line);
    } else {
        MgCanvasPCenter(canvas, 320, 220, kMgPWhite, "busiest moment this map");
    }

    int y = 244;
    if (snapshot.highclamp_lost_ms > 0) {
        std::snprintf(line, sizeof(line), "highclamp  dropped %d ms", snapshot.highclamp_lost_ms);
        MgCanvasPCenter(canvas, 320, y, kMgPRed, line);
        y += 24;
    }

    if (snapshot.event_tag[0])
        std::snprintf(line, sizeof(line), "game event  |  %s", snapshot.event_tag);
    else
        std::snprintf(line, sizeof(line), "game event  |  (none tagged)");
    MgCanvasPCenter(canvas, 320, y, kMgPWhite, line);
    y += 24;

    char sim_s[24], eng_s[24], move_s[24], buf_s[24], free_s[24];
    std::snprintf(sim_s, sizeof(sim_s), "Sim %.1f", raw_sim);
    std::snprintf(eng_s, sizeof(eng_s), "Eng %.1f", raw_eng + raw_send);
    std::snprintf(move_s, sizeof(move_s), "Move %.1f", raw_move);
    std::snprintf(buf_s, sizeof(buf_s), "Buf %.1f", raw_buf);
    std::snprintf(free_s, sizeof(free_s), "Free %.1f", spare);
    const LagKey raw[] = {{sim_s, kMgPGreen},
                          {eng_s, kMgPRed},
                          {move_s, kMgPWhite},
                          {buf_s, kMgPYellow},
                          {free_s, kMgPBlack}};
    LagRenderKeyRow(canvas, 320, y, raw, 5);
    y += 24;

    if (snapshot.lag_debug) {
        std::snprintf(line, sizeof(line), "cpu %.1f  wall %.1f  gap %.1f",
                      raw_total, snapshot.wall_total_ms, snapshot.gap_ms);
        MgCanvasPCenter(canvas, 320, y, kMgPYellow, line);
        y += 20;
        std::snprintf(line, sizeof(line), "rpC %.1f  rpW %.1f  rpG %.1f",
                      snapshot.cpu_rp_ms, snapshot.wall_rp_ms, snapshot.rp_sched_ms);
        MgCanvasPCenter(canvas, 320, y, kMgPWhite, line);
        y += 20;
        std::snprintf(line, sizeof(line), "rpN %d  pkt %d  move %d  shellG %.1f",
                      snapshot.rp_calls, snapshot.pkt_in, snapshot.move_calls,
                      snapshot.shell_sched_ms);
        MgCanvasPCenter(canvas, 320, y, kMgPWhite, line);
        y += 20;
        std::snprintf(line, sizeof(line), "shell=ReadPackets-Move  pings lag if rpG/shellG high");
        MgCanvasPCenter(canvas, 320, y, kMgPRed, line);
        y += 20;
    }

    y += 4;
    const char* actions[] = {"previous page", "next page"};
    for (int i = 0; i < 2; ++i) {
        char action[32];
        std::snprintf(action, sizeof(action), "%s%s", selectedAction == i ? "> " : "",
                      actions[i]);
        MgCanvasPCenter(canvas, 320, y,
                        selectedAction == i ? kMgMenuSelectedColor : kMgMenuNormalColor, action);
        y += 18;
    }
    if (showTips)
        MgCanvasPCenter(canvas, 320, y + 6, kMgPGreen,
                        "tap +use select | hold +use forward/back | hold +use +moveleft back");
}
