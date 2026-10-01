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
    CHECK(std::strstr(c.text, "tc 1 ") != nullptr, "gameplay color");
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
    test_render();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
