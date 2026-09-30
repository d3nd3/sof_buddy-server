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
    float sim = 30.0f, eng = 10.0f, buf = 20.0f, think = 10.0f, spare = 0.0f;
    LagNormalizeBreakdown(sim, eng, buf, think, spare);
    CHECK(spare == 30.0f, "spare from parts");
    sim = 50.0f;
    eng = 50.0f;
    buf = 50.0f;
    think = 50.0f;
    LagNormalizeBreakdown(sim, eng, buf, think, spare);
    CHECK(sim + eng + buf + think <= 100.0f, "scaled to budget");
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
    snapshot.shell_ms = 5.0f;
    snapshot.server_frame = 42;
    LagNormalizeBreakdown(snapshot.game_ms, snapshot.engine_ms, snapshot.cmd_ms, snapshot.shell_ms,
                          snapshot.spare_ms);
    MgCanvas c;
    LagRender(snapshot, c);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "SERVER TICK") != nullptr, "title");
    CHECK(std::strstr(c.text, "tc 1 ") != nullptr, "gameplay color");
    CHECK(std::strstr(c.text, "Sim") != nullptr, "legend");
    CHECK(std::strstr(c.text, "Engine") != nullptr, "legend engine");
    CHECK(std::strstr(c.text, "sv.framenum 42") != nullptr, "frame label");
}

int main() {
    test_normalize();
    test_bar_chars();
    test_render();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
