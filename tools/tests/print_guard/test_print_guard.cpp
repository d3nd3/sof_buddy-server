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

static void fill(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, kPgClientBuf, fmt, ap);
    va_end(ap);
}

static void test_truncates() {
    char buf[kPgClientBuf];
    char in[2000];
    std::memset(in, 'a', sizeof(in) - 1);
    in[sizeof(in) - 1] = '\0';
    fill(buf, "%s", in);
    CHECK(std::strlen(buf) == static_cast<size_t>(kPgClientBuf - 1), "1024 buf truncates");
    CHECK(buf[kPgClientBuf - 1] == '\0', "nul-terminated");
}

int main() {
    test_truncates();
    if (fails) {
        std::printf("%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
