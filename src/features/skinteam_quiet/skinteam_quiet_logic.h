#pragma once

#include <cstdint>
#include <cstring>

namespace skinteam_quiet {

// True when userinfo skin/teamname match what the client already carries.
inline bool SkinTeamUnchanged(const char* haveSkin, const char* haveTeam,
                              const char* wantSkin, const char* wantTeam) {
    if (!haveSkin || !haveTeam || !wantSkin || !wantTeam)
        return false;
#if defined(_WIN32)
    return _stricmp(haveSkin, wantSkin) == 0 && _stricmp(haveTeam, wantTeam) == 0;
#else
    return strcasecmp(haveSkin, wantSkin) == 0 && strcasecmp(haveTeam, wantTeam) == 0;
#endif
}

constexpr int kUserinfoSuicideDamage = 100000;

inline bool IsUserinfoSuicidePattern(void* self, void* inflictor, void* attacker, int damage,
                                     int userinfoDepth) {
    return userinfoDepth > 0 && self && self == inflictor && self == attacker &&
           damage == kUserinfoSuicideDamage;
}

inline bool SkinNameMatches(const char* actualSkin, const char* wantSkin) {
    if (!actualSkin || !wantSkin || !wantSkin[0])
        return false;
#if defined(_WIN32)
    return _stricmp(actualSkin, wantSkin) == 0;
#else
    return strcasecmp(actualSkin, wantSkin) == 0;
#endif
}

// Stock PB_InitBody returns false when the .gpm team is "Noteam" but userinfo
// teamname equals the skin (common in DM). Actual body/skin are already fine.
inline bool RelaxedTeamMatches(const char* actualTeam, const char* wantTeam,
                               const char* wantSkin) {
    if (!wantTeam || !wantTeam[0])
        return true;
    if (SkinNameMatches(actualTeam, wantTeam))
        return true;
    return wantSkin && wantSkin[0] && SkinNameMatches(wantTeam, wantSkin);
}

inline bool BodyMatchesUserinfo(const char* actualSkin, const char* actualTeam,
                                const char* wantSkin, const char* wantTeam) {
    if (!SkinNameMatches(actualSkin, wantSkin))
        return false;
    return RelaxedTeamMatches(actualTeam, wantTeam, wantSkin);
}

// PB_InitBody always copies userinfo skin/team into client->oldSkinRequest /
// oldTeamnameRequest before returning, even when it reports false (Noteam).
inline bool StoredSkinTeamMatches(const char* storedSkin, const char* storedTeam,
                                  const char* wantSkin, const char* wantTeam) {
    return BodyMatchesUserinfo(storedSkin, storedTeam, wantSkin, wantTeam);
}

// skin is a userinfo cvar and does not update teamname. Set teamname to
// skin before stock compares, or a Noteam .gpm team (TokMan2) is starred.
inline bool TeamNameFollowsSkin(const char* wantTeam, const char* wantSkin,
                                const char* haveSkin, const char* haveTeam) {
    (void)haveSkin;
    (void)haveTeam;
    if (!wantSkin || !wantSkin[0])
        return false;
    return !SkinNameMatches(wantTeam, wantSkin);
}

// Noteam .gpm: loaded team is the skin (TokMan2 vs userinfo skin tokman2).
// The skin userinfo cvar changed; teamname did not. Stock would star teamname.
inline bool SuppressNoteamTeamStar(const char* actualSkin, const char* actualTeam,
                                   const char* wantSkin) {
    if (!SkinNameMatches(actualSkin, wantSkin))
        return false;
    return SkinNameMatches(actualTeam, actualSkin) || SkinNameMatches(actualTeam, wantSkin);
}

// CS_PLAYERSKINS is name\team\skin\teamBit. A leading * on team is
// TEXT_INVALID_TEAM on the client. Drop it. A starred skin is left alone.
inline bool StripSameSkinTeamStar(const char* in, char* out, int cap) {
    if (!in || !out || cap < 2)
        return false;
    const char* s1 = std::strchr(in, '\\');
    if (!s1)
        return false;
    const char* s2 = std::strchr(s1 + 1, '\\');
    if (!s2 || s1[1] != '*')
        return false;
    const char* teamBody = s1 + 2;
    int teamLen = static_cast<int>(s2 - teamBody);
    int nameLen = static_cast<int>(s1 - in) + 1;
    int restLen = static_cast<int>(std::strlen(s2));
    if (nameLen + teamLen + restLen + 1 > cap)
        return false;
    std::memcpy(out, in, static_cast<std::size_t>(nameLen));
    std::memcpy(out + nameLen, teamBody, static_cast<std::size_t>(teamLen));
    std::memcpy(out + nameLen + teamLen, s2, static_cast<std::size_t>(restLen + 1));
    return true;
}

}  // namespace skinteam_quiet
