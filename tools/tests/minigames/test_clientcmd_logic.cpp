#include "minigames_clientcmd_logic.h"
#include "minigames_api.h"

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
    CHECK(MgKeepMinigameOnDeath(true, false, true), "keep menu through death");
    CHECK(!MgKeepMinigameOnDeath(true, true, true), "intermission still stock");
    CHECK(!MgKeepMinigameOnDeath(false, false, true), "stock view stays stock");
    CHECK(MgClientCmdArgvBase("lag", nullptr) == 0, "direct lag");
    CHECK(MgClientCmdArgvBase("say", ".lag") == 1, "say .lag");
    CHECK(MgClientCmdArgvBase("say_team", "ttt") == 1, "say_team ttt");
    CHECK(MgClientCmdArgvBase("say", "") == 0, "say empty");
    CHECK(MgClientCmdMatches(".lag", "lag"), "dot lag");
    CHECK(MgClientCmdMatches("lag", "sofbuddy_lag") == 0, "distinct names");
    CHECK(MgClientCmdMatches(".mg_lag", "mg_lag"), "dot mg_lag");
    CHECK(MgClientCmdIsMinigameChatWord(".mg"), "mg menu");
    CHECK(!MgClientCmdIsMinigameChatWord(".mg_lag"), "mg_lag is menu-only");
    CHECK(!MgClientCmdIsMinigameChatWord(".mg_ttt"), "mg_ttt is menu-only");
    CHECK(!MgClientCmdIsMinigameChatWord(".mg_cvars"), "mg_cvars is menu-only");
    char buf[64];
    const char* words[4];
    int n = MgSplitCmdWords(".mg list", buf, 64, words, 4);
    CHECK(n == 2 && std::strcmp(words[0], ".mg") == 0 && std::strcmp(words[1], "list") == 0,
          "say-box mg list");
    n = MgSplitCmdWords("\".mg list\"", buf, 64, words, 4);
    CHECK(n == 2 && std::strcmp(words[0], ".mg") == 0 && std::strcmp(words[1], "list") == 0,
          "quoted say-box mg list");
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
