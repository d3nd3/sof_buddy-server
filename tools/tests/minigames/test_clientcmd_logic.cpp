#include "minigames_clientcmd_logic.h"

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

int main() {
    CHECK(MgClientCmdArgvBase("lag", nullptr) == 0, "direct lag");
    CHECK(MgClientCmdArgvBase("say", ".lag") == 1, "say .lag");
    CHECK(MgClientCmdArgvBase("say_team", "ttt") == 1, "say_team ttt");
    CHECK(MgClientCmdArgvBase("say", "") == 0, "say empty");
    CHECK(MgClientCmdMatches(".lag", "lag"), "dot lag");
    CHECK(MgClientCmdMatches("lag", "sofbuddy_lag") == 0, "distinct names");
    CHECK(MgClientCmdIsMinigameChatWord(".ttt"), "ttt dot");
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
