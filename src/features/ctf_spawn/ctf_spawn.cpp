// CTF team spawn override: see README.md. Uses SOF_EP_* from generated_engine_pointers.h.

#include "generated_engine_pointers.h"
#include "generated_registrations.h"

// #include <cstdarg>  // with CtfDPrintf
// #include <cstddef>  // ptrdiff_t in EdictIdx
// #include <cstdio>   // with CtfDPrintf
#include <cmath>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace {

constexpr int kDmCtf = 4;
constexpr unsigned kFofsClassname = 0x1B4;
constexpr unsigned kEdictStride = 0x464;
constexpr unsigned kEdictClient = 0x74;
constexpr unsigned kEdictInuse = 0x78;
constexpr unsigned kEdictHealth = 0x2EC;
constexpr unsigned kEdictOrigin = 4;
constexpr unsigned kClUserinfo = 0xCC;
constexpr unsigned kClTeam = 0x324;
constexpr unsigned kSpawnWpId = 0x318;
constexpr unsigned kEntPred458 = 0x458;
constexpr unsigned kEntPsPtr = 0x45C;
constexpr unsigned kPsWpIndex = 0x408;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr unsigned kRvaDeathmatchCvar = 0x15C8C8;
constexpr unsigned kRvaMaxclientsCvar = 0x15D9B4;
constexpr unsigned kRvaGEdicts = 0x15CCA0;
constexpr unsigned kRvaWpManager = 0x159A60;
constexpr float kAllyBlockRadius = 56.0f;

HMODULE GameMod() { return GetModuleHandleA("oldgamex86.dll"); }

int CvarIntMode(unsigned rvaCvarSlot) {
    HMODULE h = GameMod();
    if (!h) return -1;
    void* cv = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + rvaCvarSlot);
    if (!cv) return -1;
    return static_cast<int>(*reinterpret_cast<float*>(static_cast<char*>(cv) + kCvarValueOfs));
}

int GameDeathmatchMode() { return CvarIntMode(kRvaDeathmatchCvar); }

int GameMaxClients() {
    int v = CvarIntMode(kRvaMaxclientsCvar);
    return v < 0 ? 0 : v;
}

void* GameEdicts() {
    HMODULE h = GameMod();
    if (!h) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
}

// Debug: restore cstdarg/cstddef/cstdio includes and uncomment EdictIdx / CtfDPrintf + all CtfDPrintf(...) calls.
// int EdictIdx(void* e) {
//     void* b = GameEdicts();
//     if (!b || !e) return -1;
//     ptrdiff_t d = static_cast<char*>(e) - static_cast<char*>(b);
//     if (d < 0 || d % static_cast<ptrdiff_t>(kEdictStride) != 0) return -1;
//     return static_cast<int>(d / static_cast<ptrdiff_t>(kEdictStride));
// }
//
// void CtfDPrintf(const char* fmt, ...) {
//     if (!detour_Com_DPrintf::oCom_DPrintf) return;
//     va_list ap;
//     va_start(ap, fmt);
//     char buf[512];
//     std::vsnprintf(buf, sizeof(buf), fmt, ap);
//     va_end(ap);
//     SOF_EP_Com_DPrintf("%s", buf);
// }

float VecDist(const void* a, const void* b) {
    const auto* pa = static_cast<const char*>(a);
    const auto* pb = static_cast<const char*>(b);
    float x = *reinterpret_cast<const float*>(pa + kEdictOrigin) -
              *reinterpret_cast<const float*>(pb + kEdictOrigin);
    float y = *reinterpret_cast<const float*>(pa + kEdictOrigin + 4) -
              *reinterpret_cast<const float*>(pb + kEdictOrigin + 4);
    float z = *reinterpret_cast<const float*>(pa + kEdictOrigin + 8) -
              *reinterpret_cast<const float*>(pb + kEdictOrigin + 8);
    return std::sqrt(x * x + y * y + z * z);
}

float MinEnemyDist(void* spot, void* self, const char* edictBase, int maxClients) {
    float best = 1e7f;
    if (!edictBase || maxClients < 1) return best;
    for (int n = 1; n <= maxClients; n++) {
        char* p = const_cast<char*>(edictBase) + n * kEdictStride;
        if (!*reinterpret_cast<int*>(p + kEdictInuse)) continue;
        if (*reinterpret_cast<int*>(p + kEdictHealth) <= 0) continue;
        if (!*reinterpret_cast<void**>(p + kEdictClient)) continue;
        if (p == self) continue;
        if (SOF_EP_OnSameTeam(self, p)) continue;
        float d = VecDist(spot, p);
        if (d < best) best = d;
    }
    return best;
}

bool AllyBlocks(void* spot, void* self, const char* edictBase, int maxClients) {
    if (!edictBase || maxClients < 1) return false;
    for (int n = 1; n <= maxClients; n++) {
        char* p = const_cast<char*>(edictBase) + n * kEdictStride;
        if (!*reinterpret_cast<int*>(p + kEdictInuse)) continue;
        if (*reinterpret_cast<int*>(p + kEdictHealth) <= 0) continue;
        if (!*reinterpret_cast<void**>(p + kEdictClient)) continue;
        if (p == self) continue;
        if (!SOF_EP_OnSameTeam(self, p)) continue;
        if (VecDist(spot, p) < kAllyBlockRadius) return true;
    }
    return false;
}

const char* TeamSpawnClass(void* ent, int* team12) {
    void* cl = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClient);
    if (!cl) return nullptr;
    int t = *reinterpret_cast<int*>(static_cast<char*>(cl) + kClTeam);
    if (t == 1) {
        *team12 = 1;
        return "info_player_team1";
    }
    if (t == 2) {
        *team12 = 2;
        return "info_player_team2";
    }
    const char* u = static_cast<const char*>(cl) + kClUserinfo;
    const char* tn = SOF_EP_Info_ValueForKey(u, "teamname");
    if (!tn) return nullptr;
    if (!_stricmp(tn, "blue")) {
        *team12 = 1;
        return "info_player_team1";
    }
    if (!_stricmp(tn, "red")) {
        *team12 = 2;
        return "info_player_team2";
    }
    return nullptr;
}

void ApplyWp(void* ent, void* spot, int team12) {
    void* pred = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEntPred458);
    if (!pred) {
        // CtfDPrintf("[ctf_spawn] ApplyWp: skip (ent+0x458 null) ent#%i spot#%i\n", EdictIdx(ent),
        //            EdictIdx(spot));
        return;
    }
    int sid = *reinterpret_cast<int*>(static_cast<char*>(spot) + kSpawnWpId);
    void* wpm = reinterpret_cast<char*>(GameMod()) + kRvaWpManager;
    short ax = 0;
    if (team12 == 1)
        ax = SOF_EP_WP_Team1SpawnIndex(wpm, sid);
    else if (team12 == 2)
        ax = SOF_EP_WP_Team2SpawnIndex(wpm, sid);
    void* ps = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEntPsPtr);
    if (ps)
        *reinterpret_cast<std::uint16_t*>(static_cast<char*>(ps) + kPsWpIndex) =
            static_cast<std::uint16_t>(ax);
    // CtfDPrintf("[ctf_spawn] ApplyWp: ent#%i team%i spawn_id=%i wp_idx=%i ps=%p\n", EdictIdx(ent), team12,
    //            sid, static_cast<int>(ax), ps);
}

}  // namespace

void* ctfspawn_SelectTeamSpawn(void* ent,
                                detour_SelectTeamDeathmatchSpawnPoint::tSelectTeamDeathmatchSpawnPoint original) {
    const int dm = GameDeathmatchMode();
    // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i p=%p deathmatch_mode=%i (CTF=%i)\n", who, ent, dm,
    //            kDmCtf);

    if (dm != kDmCtf) {
        // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i not CTF -> stock path\n", who);
        return original(ent);
    }

    int team12 = 0;
    const char* cname = TeamSpawnClass(ent, &team12);
    if (!cname) {
        // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i no team class -> stock path\n", who);
        return original(ent);
    }

    const char* const edictBase = static_cast<const char*>(GameEdicts());
    const int maxCl = GameMaxClients();
    // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i CTF team=%i classname=%s maxclients=%i g_edicts=%p\n",
    //            who, team12, cname, maxCl, edictBase);

    void* best = nullptr;
    void* fallback = nullptr;
    float bd = -1.f;
    float fd = -1.f;
    for (void* s = nullptr;;) {
        s = SOF_EP_G_Find(s, static_cast<int>(kFofsClassname), const_cast<char*>(cname), 0);
        if (!s) break;
        const float d = MinEnemyDist(s, ent, edictBase, maxCl);
        const bool blocked = AllyBlocks(s, ent, edictBase, maxCl);
        // CtfDPrintf(
        //     "[ctf_spawn]   spot#%i p=%p min_enemy_dist=%.1f ally_blocks=%i origin=(%.1f %.1f %.1f)\n",
        //     EdictIdx(s), s, d, blocked ? 1 : 0,
        //     *reinterpret_cast<float*>(static_cast<char*>(s) + kEdictOrigin),
        //     *reinterpret_cast<float*>(static_cast<char*>(s) + kEdictOrigin + 4),
        //     *reinterpret_cast<float*>(static_cast<char*>(s) + kEdictOrigin + 8));
        if (blocked) {
            if (d > fd) {
                fd = d;
                fallback = s;
                // CtfDPrintf("[ctf_spawn]   -> new best blocked fallback dist=%.1f\n", fd);
            }
        } else if (d > bd) {
            bd = d;
            best = s;
            // CtfDPrintf("[ctf_spawn]   -> new best clear spawn dist=%.1f\n", bd);
        }
    }

    // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i scanned %i spots best_clear=%p (bd=%.1f) "
    //            "fallback_blk=%p (fd=%.1f)\n",
    //            who, nspot, best, bd, fallback, fd);

    void* pick = best ? best : fallback;
    if (!pick) {
        // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i no team spawns -> SelectRandomDeathmatchSpawnPoint\n",
        //            who);
        void* r = SOF_EP_SelectRandomDeathmatchSpawnPoint();
        // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i random dm spot p=%p\n", who, r);
        return r;
    }

    // CtfDPrintf("[ctf_spawn] SelectTeamSpawn: ent#%i PICK p=%p (#%i) %s\n", who, pick, EdictIdx(pick),
    //            best ? "clearest_vs_enemy" : "all_blocked_use_farthest_enemy_anyway");
    ApplyWp(ent, pick, team12);
    return pick;
}
