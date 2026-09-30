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

static void test_spare() {
    CHECK(LagSpareHeadroomMs(0.0f, 0.0f) == 100.0f, "idle spare");
    CHECK(LagSpareHeadroomMs(30.0f, 20.0f) == 50.0f, "busy spare");
    CHECK(LagSpareHeadroomMs(80.0f, 50.0f) == 0.0f, "clamped spare");
}

static void test_bar() {
    char bar[48];
    LagFormatHeadroomBar(bar, sizeof(bar), 100.0f);
    CHECK(bar[0] == '=' && bar[kLagBarCharacters - 1] == '=' && bar[kLagBarCharacters] == '\0',
          "full bar");
    LagFormatHeadroomBar(bar, sizeof(bar), 0.0f);
    CHECK(bar[0] == '-' && bar[kLagBarCharacters - 1] == '-', "empty bar");
}

static void test_render() {
    LagSnapshot snapshot;
    snapshot.spare_headroom_ms = 55.0f;
    MgCanvas c;
    LagRender(snapshot, c);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "TICK HEADROOM") != nullptr, "title");
    CHECK(std::strstr(c.text, "55 ms") != nullptr, "value");
}

int main() {
    test_spare();
    test_bar();
    test_render();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
