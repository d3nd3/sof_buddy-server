#include "skinteam_quiet_logic.h"

#include <cstdint>
#include <cstring>
#include <iostream>

static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

int main() {
    using namespace skinteam_quiet;
    CHECK(SkinTeamUnchanged("abc", "red", "abc", "red"));
    CHECK(SkinTeamUnchanged("ABC", "Red", "abc", "red"));
    CHECK(!SkinTeamUnchanged("abc", "red", "xyz", "red"));
    CHECK(!SkinTeamUnchanged("abc", "red", "abc", "blue"));
    CHECK(!SkinTeamUnchanged(nullptr, "red", "abc", "red"));
    CHECK(SkinNameMatches("tokman2", "tokman2"));
    CHECK(SkinNameMatches("TOKMAN2", "tokman2"));
    CHECK(!SkinNameMatches("dekker", "tokman2"));
    CHECK(!SkinNameMatches("tokman2", nullptr));
    CHECK(SkinNameMatches("red", "RED"));
    CHECK(BodyMatchesUserinfo("tokman2", "tokman2", "tokman2", "tokman2"));
    CHECK(BodyMatchesUserinfo("tokman2", "Noteam", "tokman2", "tokman2"));
    CHECK(StoredSkinTeamMatches("tokman2", "tokman2", "tokman2", "tokman2"));
    CHECK(StoredSkinTeamMatches("tokman2", "Noteam", "tokman2", "tokman2"));
    CHECK(!BodyMatchesUserinfo("dekker", "dekker", "tokman2", "tokman2"));
    CHECK(!BodyMatchesUserinfo("tokman2", "red", "tokman2", "blue"));
    CHECK(TeamNameFollowsSkin("dekker", "tokman2", "dekker", "dekker"));
    CHECK(TeamNameFollowsSkin("Dekker", "tokman2", "dekker", "TokMan2"));
    CHECK(TeamNameFollowsSkin("", "tokman2", "dekker", "dekker"));
    CHECK(!TeamNameFollowsSkin("tokman2", "tokman2", "dekker", "dekker"));
    CHECK(!TeamNameFollowsSkin("TokMan2", "tokman2", "dekker", "dekker"));
    CHECK(TeamNameFollowsSkin("blue", "tokman2", "dekker", "dekker"));
    CHECK(TeamNameFollowsSkin("Dekker", "tokman2", "mullins", "mullins"));
    CHECK(SuppressNoteamTeamStar("TokMan2", "TokMan2", "tokman2"));
    CHECK(!SuppressNoteamTeamStar("mullins", "mullins", "tokman2"));
    CHECK(!SuppressNoteamTeamStar("tokman2", "blue", "tokman2"));
    char buf[128];
    CHECK(StripSameSkinTeamStar("p\\*TokMan2\\tokman2\\0", buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "p\\TokMan2\\tokman2\\0") == 0);
    CHECK(StripSameSkinTeamStar("p\\*tokman2\\tokman2\\1", buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "p\\tokman2\\tokman2\\1") == 0);
    CHECK(StripSameSkinTeamStar("p\\*blue\\tokman2\\0", buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "p\\blue\\tokman2\\0") == 0);
    CHECK(!StripSameSkinTeamStar("p\\TokMan2\\tokman2\\0", buf, sizeof(buf)));
    void* ent = reinterpret_cast<void*>(0x10000);
    CHECK(IsUserinfoSuicidePattern(ent, ent, ent, kUserinfoSuicideDamage, 1));
    CHECK(!IsUserinfoSuicidePattern(ent, ent, ent, kUserinfoSuicideDamage, 0));
    CHECK(!IsUserinfoSuicidePattern(ent, ent, ent, 99999, 1));
    CHECK(!IsUserinfoSuicidePattern(nullptr, nullptr, nullptr, kUserinfoSuicideDamage, 1));
    std::cout << "skinteam_quiet_logic: all " << g_checks << " checks passed\n";
    return 0;
}
