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
    LagFormatBar(bar, sizeof(bar), 0.0f);
    CHECK(std::strcmp(bar, "----------------------------------") == 0, "empty bar");
    LagFormatBar(bar, sizeof(bar), 100.0f);
    CHECK(bar[0] == '=' && bar[kLagBarChars - 1] == '=' && bar[kLagBarChars] == '\0',
          "full bar");
    LagFormatBar(bar, sizeof(bar), 250.0f);
    CHECK(bar[kLagBarChars - 1] == '=' && bar[kLagBarChars] == '\0', "clamped bar");
}

static void test_render_fits() {
    LagSnapshot s;
    s.drainLastMs = 7.5f;
    s.drainMaxMs = 42.0f;
    s.highclamps = 3;
    s.cmdparkStrict = 1;
    MgCanvas c;
    LagRender(s, c);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "drain last") != nullptr, "drain label");
    CHECK(std::strstr(c.text, "strict=ON") != nullptr, "strict on");
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
