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
    float g = 30.0f, d = 20.0f, s = 10.0f, spare = 0.0f;
    LagNormalizeBreakdown(g, d, s, spare);
    CHECK(spare == 40.0f, "spare from parts");
    g = 80.0f;
    d = 50.0f;
    s = 0.0f;
    LagNormalizeBreakdown(g, d, s, spare);
    CHECK(g + d + s <= 100.0f, "scaled to budget");
}

static void test_bar_chars() {
    CHECK(LagMsToBarChars(0.0f) == 0, "zero ms");
    CHECK(LagMsToBarChars(100.0f) == kLagBarCharacters, "full bar");
    CHECK(LagMsToBarChars(2.5f) >= 1, "tiny slice");
}

static void test_render() {
    LagSnapshot snapshot;
    snapshot.game_ms = 25.0f;
    snapshot.cmd_ms = 10.0f;
    snapshot.shell_ms = 5.0f;
    LagNormalizeBreakdown(snapshot.game_ms, snapshot.cmd_ms, snapshot.shell_ms, snapshot.spare_ms);
    MgCanvas c;
    LagRender(snapshot, c);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "TICK BUDGET") != nullptr, "title");
    CHECK(std::strstr(c.text, "tc 1 ") != nullptr, "game color");
    CHECK(std::strstr(c.text, "Game") != nullptr, "legend");
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
