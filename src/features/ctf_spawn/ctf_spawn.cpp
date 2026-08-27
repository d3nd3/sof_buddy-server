// CTF team spawn override: see README.md. Uses SOF_EP_* from generated_engine_pointers.h.
// Gated by cvar _sofbuddy_custom_respawn (0 = stock, 1 = custom).

#include "cvar.h"
#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "log.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <windows.h>

extern "C" HMODULE Buddy_GetGameDllHandle(void);

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
constexpr int kMaxFindIterations = 4096;

inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool IsSafeMemoryBlock(const void* ptr, size_t size) {
    if (!IsValidUserPointer(ptr) || size == 0)
        return false;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr + size < addr || addr + size > 0x7FFFFFFF)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;

    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + size <= regionEnd);
}

inline bool IsExecutableCodeAddress(const void* ptr) {
    if (!IsValidUserPointer(ptr))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

HMODULE GameMod() {
    HMODULE h = Buddy_GetGameDllHandle ? Buddy_GetGameDllHandle() : nullptr;
    if (h) return h;
    h = GetModuleHandleA("oldgamex86.dll");
    if (!h) h = GetModuleHandleA("OldGamex86.dll");
    if (!h) h = GetModuleHandleA("Oldgamex86.dll");
    if (!h) h = GetModuleHandleA("OLDGAMEX86.DLL");
    if (!h) h = GetModuleHandleA("oldgamex86");
    return h;
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000000) return false;
    if (!IsSafeMemoryBlock(reinterpret_cast<const char*>(dos) + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (rva >= nt->OptionalHeader.SizeOfImage) return false;
    if (rva + size > nt->OptionalHeader.SizeOfImage || rva + size < rva) return false;
    return true;
}

int CvarIntMode(unsigned rvaCvarSlot) {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, rvaCvarSlot, sizeof(void*))) return -1;
    void* cv = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + rvaCvarSlot);
    if (!IsSafeMemoryBlock(cv, kCvarValueOfs + sizeof(float))) return -1;

    const char* pVal = static_cast<const char*>(cv) + kCvarValueOfs;
    return static_cast<int>(*reinterpret_cast<const float*>(pVal));
}

int GameDeathmatchMode() { return CvarIntMode(kRvaDeathmatchCvar); }

int GameMaxClients() {
    int v = CvarIntMode(kRvaMaxclientsCvar);
    if (v < 1) return 0;
    if (v > 64) return 64;
    return v;
}

void* GameEdicts() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaGEdicts, sizeof(void*))) return nullptr;
    void* edicts = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
    if (!IsValidUserPointer(edicts)) return nullptr;
    return edicts;
}

float VecDist(const void* a, const void* b) {
    if (!IsSafeMemoryBlock(a, kEdictOrigin + 12) || !IsSafeMemoryBlock(b, kEdictOrigin + 12))
        return 0.0f;
    const auto* pa = static_cast<const char*>(a);
    const auto* pb = static_cast<const char*>(b);
    float dx = *reinterpret_cast<const float*>(pa + kEdictOrigin) -
               *reinterpret_cast<const float*>(pb + kEdictOrigin);
    float dy = *reinterpret_cast<const float*>(pa + kEdictOrigin + 4) -
               *reinterpret_cast<const float*>(pb + kEdictOrigin + 4);
    float dz = *reinterpret_cast<const float*>(pa + kEdictOrigin + 8) -
               *reinterpret_cast<const float*>(pb + kEdictOrigin + 8);
    float distSq = dx * dx + dy * dy + dz * dz;
    return (distSq > 0.0f) ? std::sqrt(distSq) : 0.0f;
}

float MinEnemyDist(const void* spot, const void* self, const char* edictBase, int maxClients) {
    float best = 1e7f;
    if (!IsValidUserPointer(edictBase) || maxClients < 1 || maxClients > 64 ||
        !IsValidUserPointer(spot) || !IsValidUserPointer(self))
        return best;
    if (!detour_OnSameTeam::oOnSameTeam || !IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_OnSameTeam::oOnSameTeam)))
        return best;

    if (!IsSafeMemoryBlock(self, kEdictClient + sizeof(void*)))
        return best;
    void* selfCl = *reinterpret_cast<void* const*>(static_cast<const char*>(self) + kEdictClient);
    if (!IsSafeMemoryBlock(selfCl, kClTeam + sizeof(int)))
        return best;

    for (int n = 1; n <= maxClients; n++) {
        const char* p = edictBase + n * kEdictStride;
        if (!IsSafeMemoryBlock(p, kEdictHealth + sizeof(int))) continue;
        if (!*reinterpret_cast<const int*>(p + kEdictInuse)) continue;
        if (*reinterpret_cast<const int*>(p + kEdictHealth) <= 0) continue;
        void* cl = *reinterpret_cast<void* const*>(p + kEdictClient);
        if (!IsSafeMemoryBlock(cl, kClTeam + sizeof(int))) continue;
        if (p == self) continue;
        if (SOF_EP_OnSameTeam(const_cast<void*>(self), const_cast<char*>(p))) continue;
        float d = VecDist(spot, p);
        if (d < best) best = d;
    }
    return best;
}

bool AllyBlocks(const void* spot, const void* self, const char* edictBase, int maxClients) {
    if (!IsValidUserPointer(edictBase) || maxClients < 1 || maxClients > 64 ||
        !IsValidUserPointer(spot) || !IsValidUserPointer(self))
        return false;
    if (!detour_OnSameTeam::oOnSameTeam || !IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_OnSameTeam::oOnSameTeam)))
        return false;

    if (!IsSafeMemoryBlock(self, kEdictClient + sizeof(void*)))
        return false;
    void* selfCl = *reinterpret_cast<void* const*>(static_cast<const char*>(self) + kEdictClient);
    if (!IsSafeMemoryBlock(selfCl, kClTeam + sizeof(int)))
        return false;

    for (int n = 1; n <= maxClients; n++) {
        const char* p = edictBase + n * kEdictStride;
        if (!IsSafeMemoryBlock(p, kEdictHealth + sizeof(int))) continue;
        if (!*reinterpret_cast<const int*>(p + kEdictInuse)) continue;
        if (*reinterpret_cast<const int*>(p + kEdictHealth) <= 0) continue;
        void* cl = *reinterpret_cast<void* const*>(p + kEdictClient);
        if (!IsSafeMemoryBlock(cl, kClTeam + sizeof(int))) continue;
        if (p == self) continue;
        if (!SOF_EP_OnSameTeam(const_cast<void*>(self), const_cast<char*>(p))) continue;
        if (VecDist(spot, p) < kAllyBlockRadius) return true;
    }
    return false;
}

const char* TeamSpawnClass(void* ent, int* team12) {
    if (!IsSafeMemoryBlock(ent, kEdictClient + sizeof(void*)) || !team12)
        return nullptr;
    void* cl = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClient);
    if (!IsSafeMemoryBlock(cl, kClTeam + sizeof(int)))
        return nullptr;

    int t = *reinterpret_cast<int*>(static_cast<char*>(cl) + kClTeam);
    if (t == 1) {
        *team12 = 1;
        return "info_player_team1";
    }
    if (t == 2) {
        *team12 = 2;
        return "info_player_team2";
    }
    if (!detour_Info_ValueForKey::oInfo_ValueForKey ||
        !IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Info_ValueForKey::oInfo_ValueForKey)))
        return nullptr;

    const char* u = static_cast<const char*>(cl) + kClUserinfo;
    if (!IsSafeMemoryBlock(u, 64)) return nullptr;

    const char* tn = SOF_EP_Info_ValueForKey(u, "teamname");
    if (!tn || !IsValidUserPointer(tn) || !IsSafeMemoryBlock(tn, 1)) return nullptr;
    if (!_stricmp(tn, "blue") || !_stricmp(tn, "1") || !_stricmp(tn, "blue_team")) {
        *team12 = 1;
        return "info_player_team1";
    }
    if (!_stricmp(tn, "red") || !_stricmp(tn, "2") || !_stricmp(tn, "red_team")) {
        *team12 = 2;
        return "info_player_team2";
    }
    return nullptr;
}

void ApplyWp(void* ent, void* spot, int team12) {
    if (!IsSafeMemoryBlock(ent, kEntPsPtr + sizeof(void*)) ||
        !IsSafeMemoryBlock(spot, kSpawnWpId + sizeof(int)))
        return;

    HMODULE hMod = GameMod();
    if (!hMod || !IsValidModuleRva(hMod, kRvaWpManager, sizeof(void*))) return;

    int sid = *reinterpret_cast<int*>(static_cast<char*>(spot) + kSpawnWpId);
    void* wpm = reinterpret_cast<char*>(hMod) + kRvaWpManager;
    if (!IsSafeMemoryBlock(wpm, sizeof(void*))) return;

    short ax = 0;
    if (team12 == 1 && detour_WP_Team1SpawnIndex::oWP_Team1SpawnIndex &&
        IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_WP_Team1SpawnIndex::oWP_Team1SpawnIndex)))
        ax = SOF_EP_WP_Team1SpawnIndex(wpm, sid);
    else if (team12 == 2 && detour_WP_Team2SpawnIndex::oWP_Team2SpawnIndex &&
             IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_WP_Team2SpawnIndex::oWP_Team2SpawnIndex)))
        ax = SOF_EP_WP_Team2SpawnIndex(wpm, sid);

    void* ps = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEntPsPtr);
    if (IsSafeMemoryBlock(ps, kPsWpIndex + sizeof(std::uint16_t))) {
        *reinterpret_cast<std::uint16_t*>(static_cast<char*>(ps) + kPsWpIndex) =
            static_cast<std::uint16_t>(ax);
    }
}

}  // namespace

void* ctfspawn_SelectTeamSpawn(void* ent,
                                detour_SelectTeamDeathmatchSpawnPoint::tSelectTeamDeathmatchSpawnPoint original) {
    if (!original || !IsExecutableCodeAddress(reinterpret_cast<const void*>(original)))
        return nullptr;

    if (!IsValidUserPointer(ent) || !CtfSpawn_CustomRespawnEnabled())
        return original(ent);

    // Guard against potential re-entrancy loops when falling back to random spawn
    static thread_local bool s_inCtfSpawn = false;
    if (s_inCtfSpawn)
        return original(ent);

    struct RecursionGuard {
        bool& flag;
        RecursionGuard(bool& f) : flag(f) { flag = true; }
        ~RecursionGuard() { flag = false; }
    } guard(s_inCtfSpawn);

    const int dm = GameDeathmatchMode();
    if (dm != kDmCtf) {
        return original(ent);
    }

    int team12 = 0;
    const char* cname = TeamSpawnClass(ent, &team12);
    if (!cname) {
        return original(ent);
    }

    if (!detour_G_Find::oG_Find || !IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_G_Find::oG_Find))) {
        return original(ent);
    }

    const char* const edictBase = static_cast<const char*>(GameEdicts());
    const int maxCl = GameMaxClients();
    if (!IsValidUserPointer(edictBase) || maxCl < 1) {
        return original(ent);
    }

    void* best = nullptr;
    void* fallback = nullptr;
    float bd = -1.f;
    float fd = -1.f;
    int findIterations = 0;
    for (void* s = nullptr;;) {
        s = SOF_EP_G_Find(s, static_cast<int>(kFofsClassname), const_cast<char*>(cname), 0);
        if (!s) break;
        if (++findIterations > kMaxFindIterations) break;
        if (!IsValidUserPointer(s)) break;

        const float d = MinEnemyDist(s, ent, edictBase, maxCl);
        const bool blocked = AllyBlocks(s, ent, edictBase, maxCl);
        if (blocked) {
            if (d > fd) {
                fd = d;
                fallback = s;
            }
        } else if (d > bd) {
            bd = d;
            best = s;
        }
    }

    void* pick = best ? best : fallback;
    if (!pick) {
        if (detour_SelectRandomDeathmatchSpawnPoint::oSelectRandomDeathmatchSpawnPoint &&
            IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_SelectRandomDeathmatchSpawnPoint::oSelectRandomDeathmatchSpawnPoint))) {
            void* rnd = SOF_EP_SelectRandomDeathmatchSpawnPoint();
            if (IsValidUserPointer(rnd)) return rnd;
        }
        return original(ent);
    }

    ApplyWp(ent, pick, team12);
    return pick;
}

void ctfspawn_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    /* Create cvars during bootstrap only - see cvar.cpp. */
    CtfSpawn_InitCvars();
}
