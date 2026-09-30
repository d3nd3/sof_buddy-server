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

static void test_bar() {
    char bar[40];
    LagFormatDrainBar(bar, sizeof(bar), 0.0f);
    CHECK(std::strcmp(bar, "----------------------------------") == 0, "empty bar");
    LagFormatDrainBar(bar, sizeof(bar), 100.0f);
    CHECK(bar[0] == '=' && bar[kLagBarCharacters - 1] == '=' && bar[kLagBarCharacters] == '\0',
          "full bar");
    LagFormatDrainBar(bar, sizeof(bar), 250.0f);
    CHECK(bar[kLagBarCharacters - 1] == '=' && bar[kLagBarCharacters] == '\0', "clamped bar");
}

static void test_render_fits() {
    LagSnapshot snapshot;
    snapshot.command_buffer_drain_last_ms = 7.5f;
    snapshot.command_buffer_drain_peak_ms = 42.0f;
    snapshot.timer_clamp_high_count = 3;
    snapshot.command_parking_strict = true;
    MgCanvas c;
    LagRender(snapshot, c);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "Last tick spent on commands") != nullptr, "drain label");
    CHECK(std::strstr(c.text, "strict") != nullptr, "strict mode");
}

int main() {
    test_bar();
    test_render_fits();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
