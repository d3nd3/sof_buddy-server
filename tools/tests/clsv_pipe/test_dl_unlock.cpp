// Host tests for src/features/clsv_pipe/dl_unlock.h (pure allowlist matcher).
#include <cstdio>

#include "dl_unlock.h"

static int g_fails = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                 \
            ++g_fails;                                                     \
        }                                                                  \
    } while (0)

int main() {
    using dlunlock::AllowPath;
    // Exact path, case-insensitive, both separators.
    CHECK(AllowPath("sofplus/addons/pipe.func"));
    CHECK(AllowPath("SOFPLUS/ADDONS/PIPE.FUNC"));
    CHECK(AllowPath("SoFplus/Addons/Pipe.Func"));
    CHECK(AllowPath("sofplus\\addons\\pipe.func"));
    CHECK(AllowPath("sofplus/addons\\pipe.func"));
    // Everything else fails closed.
    CHECK(!AllowPath(nullptr));
    CHECK(!AllowPath(""));
    CHECK(!AllowPath("sofplus/addons/pipe.func "));
    CHECK(!AllowPath(" sofplus/addons/pipe.func"));
    CHECK(!AllowPath("sofplus/addons/pipe.func2"));
    CHECK(!AllowPath("sofplus/addons/pipe.fun"));
    CHECK(!AllowPath("sofplus/addons/pipecfg"));
    CHECK(!AllowPath("sofplus/addons/"));
    CHECK(!AllowPath("sofplus/addons"));
    CHECK(!AllowPath("sofplus/"));
    CHECK(!AllowPath("sofplus"));
    CHECK(!AllowPath("xsofplus/addons/pipe.func"));
    CHECK(!AllowPath("sofplus/addons/pipe.func/x"));
    CHECK(!AllowPath("sofplus/../sofplus/addons/pipe.func"));
    CHECK(!AllowPath("sofplus/addons/../addons/pipe.func"));
    CHECK(!AllowPath("base/sofplus/addons/pipe.func"));
    CHECK(!AllowPath("sofplus/addons/pipe.func\n"));
    if (g_fails == 0)
        std::printf("dl_unlock: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
