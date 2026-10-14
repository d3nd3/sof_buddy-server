#include "lagometer_logic.h"

#include <cstdio>
#include <cstring>

static int fails = 0;

#define CHECK(cond, msg)                     \
    do {                                     \
        if (!(cond)) {                       \
            std::printf("FAIL: %s\n", msg);  \
            ++fails;                         \
        }                                    \
    } while (0)

static void test_normalize() {
    float sim = 30.0f, eng = 10.0f, buf = 20.0f, move = 10.0f, spare = 0.0f;
    LagNormalizeBreakdown(sim, eng, buf, move, spare);
    CHECK(spare == 30.0f, "spare from parts");
    sim = 50.0f;
    eng = 50.0f;
    buf = 50.0f;
    move = 50.0f;
    LagNormalizeBreakdown(sim, eng, buf, move, spare);
    CHECK(sim + eng + buf + move <= 100.0f, "scaled to budget");
}

static void test_event_tags() {
    char buf[kLagEventTagCap] = {};
    LagAppendEventTag(buf, sizeof(buf), "connect");
    LagAppendEventTag(buf, sizeof(buf), "putclient");
    CHECK(std::strcmp(buf, "connect+putclient") == 0, "append tags");
    LagAppendEventTag(buf, sizeof(buf), "mapspawn");
    CHECK(std::strcmp(buf, "connect+putclient+mapspawn") == 0, "append third");
}

static void test_bar_chars() {
    CHECK(LagMsToBarChars(0.0f) == 0, "zero ms");
    CHECK(LagMsToBarChars(100.0f) == kLagBarCharacters, "full bar");
    CHECK(LagMsToBarChars(2.5f) >= 1, "tiny slice");
}

static LagSnapshot LiveSample(int frame, float sim, float eng, float send, float move, float buf) {
    LagSnapshot s;
    s.live = true;
    s.server_frame = frame;
    s.game_ms = sim;
    s.engine_ms = eng;
    s.send_ms = send;
    s.clcmove_ms = move;
    s.cmd_ms = buf;
    LagRefreshSpare(s);
    return s;
}

static void test_live_move_eng_no_flicker() {
    LagSnapshot prev = LiveSample(10, 2.0f, 0.5f, 0.8f, 1.2f, 0.3f);
    LagSnapshot next = LiveSample(11, 2.1f, 0.0f, 0.0f, 0.0f, 0.2f);

    MgCanvas c;
    LagRender(next, c);
    CHECK(std::strstr(c.text, "Eng 0.0") != nullptr, "raw next paints Eng 0.0");
    CHECK(std::strstr(c.text, "Move 0.0") != nullptr, "raw next paints Move 0.0");

    int hold_m = 0, hold_e = 0;
    LagStabilizeLiveKeys(prev, next, hold_m, hold_e);
    LagRender(next, c);
    CHECK(std::strstr(c.text, "Eng 1.3") != nullptr, "stabilized Eng kept");
    CHECK(std::strstr(c.text, "Move 1.2") != nullptr, "stabilized Move kept");
    CHECK(std::strstr(c.text, "Sim 2.1") != nullptr, "sim still updates");

    // Same framenum re-read (PushLag then maintain) must not burn the hold.
    LagSnapshot reread = LiveSample(11, 2.1f, 0.0f, 0.0f, 0.0f, 0.2f);
    LagStabilizeLiveKeys(next, reread, hold_m, hold_e);
    LagRender(reread, c);
    CHECK(std::strstr(c.text, "Move 1.2") != nullptr, "same-frame reread keeps Move");
    CHECK(std::strstr(c.text, "Eng 1.3") != nullptr, "same-frame reread keeps Eng");
    CHECK(hold_m == 1, "same-frame reread does not burn move hold");
    CHECK(hold_e == 1, "same-frame reread does not burn eng hold");

    // Hold bridges a few zero frames (max 3), then shows sustained zeros.
    LagSnapshot pub = reread;
    for (int f = 12; f <= 13; ++f) {
        LagSnapshot z = LiveSample(f, 2.1f, 0.0f, 0.0f, 0.0f, 0.2f);
        LagStabilizeLiveKeys(pub, z, hold_m, hold_e);
        pub = z;
        LagRender(pub, c);
        CHECK(std::strstr(c.text, "Move 0.0") == nullptr, "held across zero frames");
    }
    LagSnapshot done = LiveSample(14, 2.1f, 0.0f, 0.0f, 0.0f, 0.2f);
    LagStabilizeLiveKeys(pub, done, hold_m, hold_e);
    LagRender(done, c);
    CHECK(std::strstr(c.text, "Move 0.0") != nullptr, "sustained Move zero shown");
    CHECK(std::strstr(c.text, "Eng 0.0") != nullptr, "sustained Eng zero shown");
}

static void test_live_no_zero_flash() {
    CHECK(!LagLiveSamplePublishable(false, 0.0f), "invalid live not publishable");
    CHECK(!LagLiveSamplePublishable(true, 0.0f), "zero-total live not publishable");
    CHECK(LagLiveSamplePublishable(true, 0.1f), "positive live publishable");
    CHECK(LagWarmupBlocksBusiest(1), "warmup blocks busiest");
    CHECK(!LagWarmupBlocksBusiest(0), "warmup done allows busiest");

    // Empty LagRender is how the zero flash looks — must not be published.
    MgCanvas empty_c;
    LagRender(LagSnapshot{}, empty_c);
    CHECK(std::strstr(empty_c.text, "Sim 0.0") != nullptr, "empty render paints Sim 0.0");

    LagSnapshot view;
    MgCanvas c;
    CHECK(!LagFillLiveView(false, 0.0f, LagSnapshot{}, view), "reject invalid live");
    // No LagRender on reject → canvas stays clear (no Sim 0.0 flash).
    CHECK(c.len == 0, "rejected live leaves canvas untouched");

    LagSnapshot good;
    good.game_ms = 2.5f;
    good.engine_ms = 1.0f;
    good.raw_total_ms = 3.5f;
    good.server_frame = 99;
    CHECK(LagFillLiveView(true, good.raw_total_ms, good, view), "accept valid live");
    LagRender(view, c);
    CHECK(std::strstr(c.text, "Sim 2.5") != nullptr, "valid live shows sim");
    CHECK(std::strstr(c.text, "live  |  sv.framenum 99") != nullptr, "valid live label");
    CHECK(std::strstr(c.text, "Sim 0.0") == nullptr, "valid live has no Sim 0.0");
}

static void test_render() {
    LagSnapshot snapshot;
    snapshot.game_ms = 25.0f;
    snapshot.engine_ms = 8.0f;
    snapshot.cmd_ms = 10.0f;
    snapshot.clcmove_ms = 5.0f;
    snapshot.server_frame = 42;
    LagNormalizeBreakdown(snapshot.game_ms, snapshot.engine_ms, snapshot.cmd_ms, snapshot.clcmove_ms,
                          snapshot.spare_ms);
    MgCanvas c;
    LagRender(snapshot, c);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "SERVER TICK") != nullptr, "title");
    CHECK(std::strstr(c.text, "\003Sim") != nullptr, "gameplay color");
    CHECK(std::strstr(c.text, "Sim") != nullptr, "sim key");
    CHECK(std::strstr(c.text, "Eng") != nullptr, "eng key");
    CHECK(std::strstr(c.text, "sv.framenum 42") != nullptr, "frame label");
    snapshot.rank = 1;
    snapshot.rank_count = 10;
    LagRender(snapshot, c);
    CHECK(std::strstr(c.text, "#1 / 10") != nullptr, "rank index");
    snapshot.live = true;
    snapshot.rank = 0;
    LagRender(snapshot, c);
    CHECK(std::strstr(c.text, "live  |  sv.framenum 42") != nullptr, "live frame");
    snapshot.live = false;
    snapshot.rank = 1;
    CHECK(std::strstr(c.text, "highclamp") == nullptr, "no clamp line without clamp");
    CHECK(std::strstr(c.text, "raw ms") == nullptr, "no raw ms label");
    CHECK(std::strstr(c.text, "ClientThink") == nullptr, "no key legend");

    snapshot.highclamp_lost_ms = 37;
    LagRender(snapshot, c);
    CHECK(std::strstr(c.text, "highclamp  dropped 37 ms") != nullptr, "clamp line");

    LagSnapshot tenths;
    tenths.game_ms = 1.3f;
    tenths.engine_ms = 2.0f;
    tenths.send_ms = 1.3f;
    tenths.clcmove_ms = 0.4f;
    tenths.cmd_ms = 0.2f;
    LagRender(tenths, c);
    CHECK(std::strstr(c.text, "Sim 1.3") != nullptr, "sim tenths");
    CHECK(std::strstr(c.text, "Eng 3.3") != nullptr, "send folded into red engine");
    CHECK(std::strstr(c.text, "Move 0.4") != nullptr, "move tenths");
    CHECK(std::strstr(c.text, "Buf 0.2") != nullptr, "buf tenths");
    CHECK(std::strstr(c.text, "Free 94.8") != nullptr, "free tenths");
    CHECK(std::strstr(c.text, "Send") == nullptr, "no separate send key");
}

int main() {
    test_normalize();
    test_event_tags();
    test_bar_chars();
    test_live_no_zero_flash();
    test_live_move_eng_no_flicker();
    test_render();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
