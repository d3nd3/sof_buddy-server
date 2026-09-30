// Suppress spurious skin/teamname userinfo side effects:
// - before stock runs, point teamname at the new skin when it still names the old one
// - block the in-function teamplay suicide, but leave AssignTeam's +0x380
//   flag so ClientThink still respawns on a team_red_blue team change.
//   Profiles lets AssignTeam run for a wrapped guid and for the packed
//   integer used once a client integerizes the cvar (team in the low bit).

#include "skinteam_quiet_guard.h"
#include "skinteam_quiet_logic.h"

#include "buddy_import.h"
#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace {

constexpr unsigned kExportClientUserinfoOfs = 0x2C;
constexpr unsigned kExportEdictsOfs = 0x60;
constexpr unsigned kEdictStride = 0x464;
constexpr unsigned kEdictClient = 0x74;
constexpr unsigned kClNetname = 0x2CC;
constexpr unsigned kClTeam = 0x324;
constexpr int kCsPlayerskins = 0x587;
constexpr unsigned kEdictFlags = 0x1A0;
constexpr unsigned kEdictHealth = 0x2EC;
constexpr unsigned kEdictDeadflag = 0x2F8;
constexpr unsigned kInfoStringMax = 512;
using UserinfoFn = void(__cdecl*)(void*, char*, int);

UserinfoFn g_userinfoChain = nullptr;
void** g_userinfoSlot = nullptr;
char* g_edicts = nullptr;
void* g_gameExport = nullptr;

struct UserinfoScope {
    int depth = 0;
    void* ent = nullptr;
    int savedHealth = 0;
    int savedDeadflag = 0;
    int savedFlags = 0;
    bool blockedSuicide = false;
};

UserinfoScope g_scope;

inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return addr >= 0x10000 && addr <= 0x7FFFFFFF;
}

inline bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

inline bool IsExecutableCodeAddress(const void* ptr) {
    if (!IsValidUserPointer(ptr))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

bool BodyNameReady() {
    return detour_PB_GetActualSkinName::oPB_GetActualSkinName &&
           detour_PB_GetActualTeamName::oPB_GetActualTeamName &&
           !IsBadReadPtr(reinterpret_cast<void*>(detour_PB_GetActualSkinName::oPB_GetActualSkinName),
                         1) &&
           !IsBadReadPtr(reinterpret_cast<void*>(detour_PB_GetActualTeamName::oPB_GetActualTeamName),
                         1);
}

void SaveVitals(void* ent) {
    char* e = static_cast<char*>(ent);
    g_scope.ent = ent;
    g_scope.savedHealth = *reinterpret_cast<int*>(e + kEdictHealth);
    g_scope.savedDeadflag = *reinterpret_cast<int*>(e + kEdictDeadflag);
    g_scope.savedFlags = *reinterpret_cast<int*>(e + kEdictFlags);
    g_scope.blockedSuicide = false;
}

void RestoreVitalsIfNeeded(void* ent) {
    if (!g_scope.blockedSuicide || !ent || ent != g_scope.ent)
        return;
    char* e = static_cast<char*>(ent);
    *reinterpret_cast<int*>(e + kEdictHealth) = g_scope.savedHealth;
    *reinterpret_cast<int*>(e + kEdictDeadflag) = g_scope.savedDeadflag;
    *reinterpret_cast<int*>(e + kEdictFlags) = g_scope.savedFlags;
    Buddy_DebugPrintf("[skinteam_quiet] restored vitals after blocked userinfo suicide\n");
    g_scope.blockedSuicide = false;
}

bool ReadBodyNames(void* ent, char* actualSkin, char* actualTeam) {
    if (!ent || !actualSkin || !actualTeam || !BodyNameReady())
        return false;
    detour_PB_GetActualSkinName::oPB_GetActualSkinName(ent, actualSkin);
    detour_PB_GetActualTeamName::oPB_GetActualTeamName(ent, actualTeam);
    return true;
}

// In-place \teamname\ replace. Stock already copied nothing yet; this runs
// before ClientUserinfoChanged snapshots skin/teamname for the star check.
bool SetInfoValue(char* info, const char* key, const char* value) {
    if (!info || !key || !key[0] || !value || !Readable(info, 1))
        return false;
    char pat[72];
    std::snprintf(pat, sizeof(pat), "\\%s\\", key);
    char* hit = std::strstr(info, pat);
    if (!hit) {
        std::size_t n = std::strlen(info);
        std::size_t add = std::strlen(pat) + std::strlen(value);
        if (n + add >= kInfoStringMax)
            return false;
        std::memcpy(info + n, pat, std::strlen(pat));
        std::memcpy(info + n + std::strlen(pat), value, std::strlen(value) + 1);
        return true;
    }
    char* val = hit + std::strlen(pat);
    char* end = std::strchr(val, '\\');
    if (!end)
        end = val + std::strlen(val);
    std::size_t oldLen = static_cast<std::size_t>(end - val);
    std::size_t newLen = std::strlen(value);
    std::size_t tail = std::strlen(end);
    if (static_cast<std::size_t>(val - info) + newLen + tail >= kInfoStringMax)
        return false;
    std::memmove(val + newLen, end, tail + 1);
    std::memcpy(val, value, newLen);
    return true;
}

void InfoValue(const char* info, const char* key, char* out, int cap) {
    out[0] = 0;
    if (!info || !key || !key[0] || !out || cap < 2)
        return;
    char pat[72];
    std::snprintf(pat, sizeof(pat), "\\%s\\", key);
    const char* hit = std::strstr(info, pat);
    if (!hit)
        return;
    hit += std::strlen(pat);
    int i = 0;
    while (hit[i] && hit[i] != '\\' && i < cap - 1) {
        out[i] = hit[i];
        ++i;
    }
    out[i] = 0;
}

void CacheEdicts(void* gameExport) {
    if (gameExport)
        g_gameExport = gameExport;
    void* ge = gameExport ? gameExport : g_gameExport;
    if (!ge || !Readable(static_cast<char*>(ge) + kExportEdictsOfs, sizeof(void*)))
        return;
    if (char* ed = *reinterpret_cast<char**>(static_cast<char*>(ge) + kExportEdictsOfs))
        g_edicts = ed;
}

// Stock's false path is what puts '*' on the team field. Write the same
// playerskin without it and let the caller skip that path.
bool WriteUnstarredPlayerskin(void* ent, const char* skin, const char* team) {
    CacheEdicts(nullptr);
    if (!g_edicts || !ent || !skin || !skin[0] || !team || !team[0])
        return false;
    auto diff = static_cast<char*>(ent) - g_edicts;
    if (diff <= 0 || diff % static_cast<ptrdiff_t>(kEdictStride) != 0)
        return false;
    int slot = static_cast<int>(diff / static_cast<ptrdiff_t>(kEdictStride)) - 1;
    if (slot < 0 || slot >= 32)
        return false;
    if (!Readable(static_cast<char*>(ent) + kEdictClient, sizeof(void*)))
        return false;
    char* cl = *reinterpret_cast<char**>(static_cast<char*>(ent) + kEdictClient);
    if (!cl || !Readable(cl + kClNetname, 16) || !Readable(cl + kClTeam, sizeof(int)))
        return false;
    char name[16];
    std::memcpy(name, cl + kClNetname, 15);
    name[15] = 0;
    int bit = *reinterpret_cast<int*>(cl + kClTeam) - 1;
    char cs[160];
    std::snprintf(cs, sizeof(cs), "%s\\%s\\%s\\%d", name, team, skin, bit);
    return Buddy_Configstring(kCsPlayerskins + slot, cs);
}

void AlignTeamNameToSkin(void* ent, char* userinfo) {
    (void)ent;
    if (!userinfo || !IsValidUserPointer(userinfo) || !userinfo[0])
        return;
    char wantSkin[64];
    char wantTeam[64];
    InfoValue(userinfo, "skin", wantSkin, static_cast<int>(sizeof(wantSkin)));
    InfoValue(userinfo, "teamname", wantTeam, static_cast<int>(sizeof(wantTeam)));
    if (!skinteam_quiet::TeamNameFollowsSkin(wantTeam, wantSkin, nullptr, nullptr))
        return;
    SetInfoValue(userinfo, "teamname", wantSkin);
}

void __cdecl HkClientUserinfoChanged(void* ent, char* userinfo, int notFirst) {
    AlignTeamNameToSkin(ent, userinfo);
    skinteam_UserinfoScopeBegin(ent);
    if (g_userinfoChain)
        g_userinfoChain(ent, userinfo, notFirst);
    skinteam_UserinfoScopeEnd(ent);
}

bool InstallUserinfoWrapper(void* gameExport) {
    if (!gameExport || g_userinfoSlot)
        return false;
    if (!Readable(static_cast<char*>(gameExport) + kExportClientUserinfoOfs, sizeof(void*)))
        return false;
    void** slot = reinterpret_cast<void**>(static_cast<char*>(gameExport) + kExportClientUserinfoOfs);
    void* target = *slot;
    if (!target || !IsExecutableCodeAddress(target)) {
        PrintOut(PRINT_BAD, "[skinteam_quiet] ClientUserinfoChanged hook skipped: bad target\n");
        return false;
    }
    if (target == reinterpret_cast<void*>(&HkClientUserinfoChanged))
        return true;
    g_userinfoChain = reinterpret_cast<UserinfoFn>(target);
    g_userinfoSlot = slot;
    *slot = reinterpret_cast<void*>(&HkClientUserinfoChanged);
    PrintOut(PRINT_LOG, "[skinteam_quiet] ClientUserinfoChanged wrapper installed\n");
    return true;
}

}  // namespace

void skinteam_UserinfoScopeBegin(void* ent) {
    if (g_scope.depth++ == 0 && ent && IsValidUserPointer(ent))
        SaveVitals(ent);
}

void skinteam_UserinfoScopeEnd(void* ent) {
    if (g_scope.depth <= 0)
        return;
    if (--g_scope.depth == 0)
        RestoreVitalsIfNeeded(ent);
}

bool skinteam_ShouldBlockUserinfoSuicide(void* self, void* inflictor, void* attacker, int damage) {
    return skinteam_quiet::IsUserinfoSuicidePattern(self, inflictor, attacker, damage,
                                                    g_scope.depth);
}

bool skinteam_PbInitBody(void* ent, char* userinfo, detour_PB_InitBody::tPB_InitBody original) {
    if (!original)
        return false;
    if (original(ent, userinfo))
        return true;
    // skin is a userinfo cvar; setting it does not change teamname. Noteam
    // .gpm team is the skin, so a stale teamname is the only mismatch.
    char wantSkin[64] = {};
    if (userinfo)
        InfoValue(userinfo, "skin", wantSkin, static_cast<int>(sizeof(wantSkin)));
    char actualSkin[128] = {};
    char actualTeam[128] = {};
    if (!ReadBodyNames(ent, actualSkin, actualTeam) ||
        !skinteam_quiet::SuppressNoteamTeamStar(actualSkin, actualTeam, wantSkin))
        return false;
    if (!WriteUnstarredPlayerskin(ent, wantSkin, actualTeam))
        return false;
    SetInfoValue(userinfo, "teamname", actualTeam);
    return true;
}

void skinteam_Configstring(int num, char* s, detour_configstring::tconfigstring original) {
    // CS_PLAYERSKINS .. +MAX_CLIENTS. *team that is just the skin is the
    // "doesn't recognise teamname TokMan2 for skin tokman2" line.
    constexpr int kCsPlayerskins = 0x587;
    constexpr int kMaxClients = 32;
    char quiet[256];
    if (original && s && num >= kCsPlayerskins && num < kCsPlayerskins + kMaxClients &&
        skinteam_quiet::StripSameSkinTeamStar(s, quiet, static_cast<int>(sizeof(quiet)))) {
        original(num, quiet);
        return;
    }
    if (original)
        original(num, s);
}

void skinteam_PlayerDie(void* self, void* inflictor, void* attacker, int damage, float* point,
                        detour_player_die::tplayer_die original) {
    if (skinteam_ShouldBlockUserinfoSuicide(self, inflictor, attacker, damage)) {
        g_scope.blockedSuicide = true;
        Buddy_DebugPrintf("[skinteam_quiet] blocked userinfo skin/team suicide\n");
        return;
    }
    if (original)
        original(self, inflictor, attacker, damage, point);
}

void skinteam_OnGameDllLoaded(void* gameExport) {
    CacheEdicts(gameExport);
    InstallUserinfoWrapper(gameExport);
    PrintOut(PRINT_LOG, "[skinteam_quiet] PB_InitBody filter + userinfo suicide block active\n");
}
