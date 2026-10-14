#include "print_guard_logic.h"

#include <cstdio>
#include <cstring>

static int fails = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL: %s\n", msg); \
            ++fails; \
        } \
    } while (0)

// Mirrors Pg_SafeCbufExecute line scan + cap (host test).
static int LineLen(const char* buf, int total) {
    int q = 0;
    int len = 0;
    while (len < total) {
        const unsigned char c = static_cast<unsigned char>(buf[len]);
        if (c == '"')
            ++q;
        if ((q & 1) == 0 && (c == ';' || c == '\n'))
            break;
        ++len;
    }
    return len < kPgClientBuf - 1 ? len : kPgClientBuf - 1;
}

static void test_long_line_cap() {
    char buf[2000];
    std::memset(buf, 'a', sizeof(buf));
    buf[1999] = '\0';
    CHECK(LineLen(buf, 1999) == kPgClientBuf - 1, "line capped at 1023");
}

int main() {
    test_long_line_cap();
    if (fails) {
        std::printf("%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
