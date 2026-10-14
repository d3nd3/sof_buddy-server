#include "macro_guard.h"
#include "print_guard_logic.h"

#include <cstdio>
#include <cstring>

extern "C" void* Buddy_GetEngineCvar(const char*, const char*, int, void*) {
    return nullptr;
}
extern "C" float Buddy_ReadCvarValue(void*, float def) {
    return def;
}

static int fails = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL: %s\n", msg); \
            ++fails; \
        } \
    } while (0)

static void test_no_macros() {
    char line[] = "echo hello";
    CHECK(Pg_SafeMacroExpand(line) == line, "plain line unchanged");
}

int main() {
    test_no_macros();
    if (fails) {
        std::printf("%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
