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
    CHECK(MgClientCmdMatches(".mg_lag", "mg_lag"), "dot mg_lag");
    CHECK(MgClientCmdIsMinigameChatWord(".mg"), "mg menu");
    CHECK(MgClientCmdIsMinigameChatWord(".mg_lag"), "mg_lag chat word");
    CHECK(MgClientCmdIsMinigameChatWord(".mg_ttt"), "mg_ttt chat word");
    CHECK(MgClientCmdIsMinigameChatWord(".mg_cvars"), "mg_cvars chat word");
    char buf[64];
    const char* words[4];
    int n = MgSplitCmdWords(".mg_lag next", buf, 64, words, 4);
    CHECK(n == 2 && std::strcmp(words[0], ".mg_lag") == 0 && std::strcmp(words[1], "next") == 0,
          "say-box mg_lag next");
    n = MgSplitCmdWords("\".mg_cvars clamp 2\"", buf, 64, words, 4);
    CHECK(n == 3 && std::strcmp(words[0], ".mg_cvars") == 0 && std::strcmp(words[2], "2") == 0,
          "quoted say-box cvars");
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
