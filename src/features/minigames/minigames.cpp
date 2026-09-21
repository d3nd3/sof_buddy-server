// minigames: platform for 2D minigames (tictactoe now, chess later).
//
// Owns the three tricky pieces so games stay simple rules + drawing:
//   1. ClientCommand export hook — replaces ge+0x34 with our handler so
//      client console words ("ttt") reach registered games. (Inline DetourXS
//      on ClientCommand itself fails: retail prologue is SEH/fs:0 setup.)
//   2. Layout visibility — re-asserts ps.stats[STAT_LAYOUTS] every server
//      frame for visible slots (stock G_SetStats rewrites stats each frame).
//   3. Slot/edict/argv plumbing with bounds checks on every pointer.
//
// Offsets (verified, no magic):
//   game_export_t.ClientCommand .. ge+0x34  (retail gamex86.dll GetGameAPI
//      stores it at export+0x34; the function reads ent->client at +0x74
//      and calls gi.argv — both match)
//   game_export_t.edicts ........ ge+0x60, edict_size ge+0x64 (= 0x464)
//   edict_t.client .............. ent+0x74 (same as stufftext/ctf_spawn)
//   gclient_t.ps ................ client+0 (first field, game source)
//   player_state_t.stats ........ ps+164, short elems (game source)
//   STAT_LAYOUTS ................ stats[9] (game + engine agree)

#include "cvar.h"
#include "minigames_api.h"

#include "DetourXS/detourxs.h"
#include "buddy_import.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"
#include "tictactoe/cvar.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

extern "C" HMODULE Buddy_GetGameDllHandle(void);

namespace {

constexpr unsigned kExportClientCommandOfs = 0x34;
constexpr unsigned kExportEdictsOfs = 0x60;
constexpr unsigned kExportEdictSizeOfs = 0x64;
constexpr unsigned kRvaGEdicts = 0x15CCA0;
constexpr unsigned kEdictStrideDefault = 0x464;
constexpr unsigned kEdictClientOfs = 0x74;
constexpr unsigned kEdictInuseOfs = 0x78;
constexpr unsigned kClientStatsOfs = 164;
constexpr unsigned kClientShowscoresOfs = 0x470;  // gclient_t.showscores
constexpr unsigned kClientShowinventoryOfs = 0x474;
constexpr unsigned kClientShowhelpTimeOfs = 0x568;  // gclient_t.showhelp_time (float)
constexpr unsigned kEdictEnemyOfs = 0x804;
constexpr unsigned kRvaDeathmatchClass = 0x15C4D8;
constexpr unsigned kRvaCmdScoreF = 0xF6710;  // cmd_score_f @ gamex86+ (sofree hooks here)
constexpr unsigned kRvaLevelIntermissiontime = 0x15D1C8;  // level.intermissiontime @ G_SetStats
constexpr unsigned kRvaScoreboardPulseMask = 0xF9E2F;  // ClientEndServerFrame: test level.framenum, imm8
constexpr unsigned char kMgScoreboardPulseMask = 0x1F;   // stock &31 (~3.2s); not SoFree's &0
constexpr int kPmDead = 3;                               // pmtype_t PM_DEAD
constexpr unsigned kClientPmTypeOfs = 0;                 // gclient.ps.pmove.pm_type
constexpr unsigned kClientPersHealthOfs = 0x2FC;         // gclient.pers.health @ G_SetStats+764
constexpr int kStatLayouts = 9;
constexpr int kSvcLayout = 2;  // svc_layout → client layout_string (Com_sprintf replace)
constexpr unsigned short kDmLayoutReset = 0x002b;  // dm_generic LAYOUT_RESET ("*"), always on clients

constexpr unsigned kRvaSvClients = 0x396EEC;
constexpr unsigned kClientStride = 0xD2AC;
constexpr unsigned kClientEdictOfs = 0x298;
constexpr int kCsSpawned = 3;

constexpr int kMaxGames = 8;

using clientcmd_fn = void(__cdecl*)(void*);
using cmd_score_fn = void(__cdecl*)(void*);

clientcmd_fn g_originalClientCommand = nullptr;
void* g_geClientCommandSlot = nullptr;
void* g_scoreTrampoline = nullptr;
char* g_edicts = nullptr;
int g_edictSize = 0;
void* g_cvMaxClients = nullptr;

const MgGameOps* g_games[kMaxGames] = {};
bool g_visible[kMgMaxSlots + 1] = {};
char g_layoutCache[kMgMaxSlots + 1][kMgLayoutCap] = {};

enum class MgScorePage : unsigned char { Off = 0, Scoreboard = 1, Minigame = 2 };
MgScorePage g_page[kMgMaxSlots + 1] = {};
bool g_layoutDirty[kMgMaxSlots + 1] = {};
char g_placeholderLayout[kMgLayoutCap] = {};
int g_placeholderBg = -1;

struct PrevUserCmd {
    short forward = 0;
    short side = 0;
    unsigned char buttons = 0;
};
PrevUserCmd g_prevCmd[kMgMaxSlots + 1] = {};

constexpr int kMgPanelCTile = 256;
constexpr int kMgIdleBannerH = 128;

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

inline bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !Readable(h, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const char* base = reinterpret_cast<const char*>(h);
    if (!Readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const unsigned img = nt->OptionalHeader.SizeOfImage;
    return rva < img && rva + size <= img;
}

HMODULE GameMod() {
    if (HMODULE shim = GetModuleHandleA("gamex86.dll")) {
        using handle_fn = HMODULE (*)();
        auto fn = reinterpret_cast<handle_fn>(GetProcAddress(shim, "Buddy_GetGameDllHandle"));
        if (fn) {
            if (HMODULE stock = fn())
                return stock;
        }
    }
    if (HMODULE h = GetModuleHandleA("oldgamex86.dll"))
        return h;
    return GetModuleHandleA("OLDGAMEX86.DLL");
}

char* GameEdicts() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaGEdicts, sizeof(void*)))
        return nullptr;
    char* ed = *reinterpret_cast<char**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
    if (!ed || !Readable(ed, kEdictInuseOfs + sizeof(int)))
        return nullptr;
    return ed;
}

int EdictStride() {
    return g_edictSize > 0 ? g_edictSize : static_cast<int>(kEdictStrideDefault);
}

char* EdictTable() {
    if (char* ed = GameEdicts())
        return ed;
    return g_edicts;
}

void* ClientForEnt(void* ent) {
    if (!ent || !Readable(static_cast<char*>(ent) + kEdictClientOfs, sizeof(void*)))
        return nullptr;
    void* client = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClientOfs);
    if (!client || !Readable(client, kClientStatsOfs + (kStatLayouts + 1) * 2u))
        return nullptr;
    return client;
}

bool LevelIntermission() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaLevelIntermissiontime, sizeof(float)))
        return false;
    const float* t =
        reinterpret_cast<const float*>(reinterpret_cast<char*>(h) + kRvaLevelIntermissiontime);
    return *t != 0.0f;
}

// Stock auto-scoreboard: death (player_die→Cmd_Score_f), intermission, or dead corpse.
bool ClientWantsStockScoreboard(void* ent) {
    if (LevelIntermission())
        return true;
    void* client = ClientForEnt(ent);
    if (!client)
        return false;
    char* c = static_cast<char*>(client);
    if (Readable(c + kClientPmTypeOfs, sizeof(int)) &&
        *reinterpret_cast<int*>(c + kClientPmTypeOfs) == kPmDead)
        return true;
    if (Readable(c + kClientPersHealthOfs, sizeof(int)) &&
        *reinterpret_cast<int*>(c + kClientPersHealthOfs) <= 0)
        return true;
    return false;
}

void ApplyLayoutClient(void* ent, bool on);
void ApplyScorePage(void* ent, int slot, MgScorePage page);

void EnsureScoreboardPage(void* ent, int slot) {
    if (slot < 1 || slot > kMgMaxSlots)
        return;
    if (g_page[slot] != MgScorePage::Scoreboard)
        ApplyScorePage(ent, slot, MgScorePage::Scoreboard);
    else
        ApplyLayoutClient(ent, true);
}

bool SameNoCase(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

void PokeLayoutBit(void* client, bool on) {
    auto* stats = reinterpret_cast<short*>(static_cast<char*>(client) + kClientStatsOfs);
    if (on)
        stats[kStatLayouts] = static_cast<short>(stats[kStatLayouts] | 1);  // DM: 2|1 → draw
    else
        stats[kStatLayouts] = static_cast<short>(stats[kStatLayouts] & ~1);
}

// Block stock scoreboard/help layout refresh for this frame (p_view.cpp).
void SuppressStockLayoutRefresh(void* client) {
    if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
        *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 0;
    if (Readable(static_cast<char*>(client) + kClientShowhelpTimeOfs, sizeof(float)))
        *reinterpret_cast<float*>(static_cast<char*>(client) + kClientShowhelpTimeOfs) = 0.0f;
}

void PushLayoutPayload(void* ent, const char* layout);

void SendMinigameLayout(void* ent, int slot, const char* layout) {
    if (!ent || slot < 1 || !layout || !layout[0])
        return;
    PushLayoutPayload(ent, layout);
    g_layoutDirty[slot] = false;
}

// Stock ClientEndServerFrame gates scoreboard on level.framenum & imm8 (@ gamex86+0xF9E2F).
// SoFree patches imm8 to 0 (every tick). We keep &31 for the scoreboard tab only.
void EnsureScoreboardPulse() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaScoreboardPulseMask, 1))
        return;
    char* p = reinterpret_cast<char*>(h) + kRvaScoreboardPulseMask;
    if (!Readable(p, 1))
        return;
    if (static_cast<unsigned char>(*p) == kMgScoreboardPulseMask)
        return;
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_READWRITE, &old))
        return;
    *p = static_cast<char>(kMgScoreboardPulseMask);
    VirtualProtect(p, 1, old, &old);
    PrintOut(PRINT_LOG, "[minigames] scoreboard pulse set to &%u (stock rate)\n", kMgScoreboardPulseMask);
}

void MgCanvasBackground(MgCanvas& c) {
    if (MgBgUseBlack640()) {
        MgCanvasPic(c, 0, 0, kMgSbMgBg);
        return;
    }
    const int px = (kMgVirtW - kMgPanelCTile * 2) / 2;
    const int py = (kMgVirtH - kMgPanelCTile) / 2;
    MgCanvasPic(c, px, py, kMgSbMgPn);
    MgCanvasPic(c, px + kMgPanelCTile, py, kMgSbMgPn);
}

void EnsurePlaceholderLayout() {
    const int bg = MgBgUseBlack640() ? 1 : 0;
    if (g_placeholderLayout[0] && g_placeholderBg == bg)
        return;
    g_placeholderBg = bg;
    MgCanvas c;
    MgCanvasClear(c);
    MgCanvasBackground(c);
    MgCanvasPic(c, 0, (kMgVirtH - kMgIdleBannerH) / 2, kMgSbMgId);
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 320, 240, "SOF BUDDY");
    MgCanvasTc(c, kMgColGreen);
    MgCanvasCenter(c, 320, 268, "No minigame active");
    std::strncpy(g_placeholderLayout, c.text, kMgLayoutCap);
    g_placeholderLayout[kMgLayoutCap - 1] = '\0';
}

const char* MinigameLayoutForSlot(int slot) {
    if (slot >= 1 && slot <= kMgMaxSlots && g_visible[slot] && g_layoutCache[slot][0])
        return g_layoutCache[slot];
    EnsurePlaceholderLayout();
    return g_placeholderLayout;
}

void ApplyLayoutClient(void* ent, bool on) {
    void* client = ClientForEnt(ent);
    if (!client)
        return;
    PokeLayoutBit(client, on);
}

void* DeathmatchClass() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaDeathmatchClass, sizeof(void*)))
        return nullptr;
    void* dm = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaDeathmatchClass);
    return Readable(dm, sizeof(void*)) ? dm : nullptr;
}

void PaintScoreboard(void* ent) {
    void* dm = DeathmatchClass();
    if (!dm || !ent || !Readable(static_cast<char*>(ent) + kEdictEnemyOfs, sizeof(void*)))
        return;
    void* killer = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictEnemyOfs);
    auto vtbl = *reinterpret_cast<void***>(dm);
    if (!vtbl || !Readable(vtbl, 200))
        return;
    using fn_t = void(__thiscall*)(void*, void*, void*, int);
    auto fn = reinterpret_cast<fn_t>(vtbl[49]);  // clientScoreboardMessage @ vtbl+0xC4
    if (!fn)
        return;
    fn(dm, ent, killer, 0);
}

void ApplyScorePage(void* ent, int slot, MgScorePage page) {
    void* client = ClientForEnt(ent);
    if (!client || slot < 1 || slot > kMgMaxSlots)
        return;
    g_page[slot] = page;
    switch (page) {
    case MgScorePage::Off:
        if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 0;
        ApplyLayoutClient(ent, false);
        PushLayoutPayload(ent, "");
        break;
    case MgScorePage::Scoreboard:
        if (Readable(static_cast<char*>(client) + kClientShowinventoryOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowinventoryOfs) = 0;
        if (Readable(static_cast<char*>(client) + kClientShowhelpTimeOfs, sizeof(float)))
            *reinterpret_cast<float*>(static_cast<char*>(client) + kClientShowhelpTimeOfs) = 0.0f;
        if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 1;
        ApplyLayoutClient(ent, true);
        PaintScoreboard(ent);
        break;
    case MgScorePage::Minigame:
        if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 0;
        ApplyLayoutClient(ent, true);
        SendMinigameLayout(ent, slot, MinigameLayoutForSlot(slot));
        break;
    }
}

void CycleScorePage(void* ent, int slot) {
    if (slot < 1)
        return;
    const unsigned next =
        (static_cast<unsigned>(g_page[slot]) + 1u) % (static_cast<unsigned>(MgScorePage::Minigame) + 1u);
    ApplyScorePage(ent, slot, static_cast<MgScorePage>(next));
}

void __cdecl HkCmd_Score_f(void* ent) {
    if (MgPlatformEnabled() && ent) {
        const int slot = MgSlotForEdict(ent);
        if (slot >= 1) {
            if (ClientWantsStockScoreboard(ent))
                ApplyScorePage(ent, slot, MgScorePage::Scoreboard);
            else
                CycleScorePage(ent, slot);
            return;
        }
    }
    if (auto original = reinterpret_cast<cmd_score_fn>(g_scoreTrampoline))
        original(ent);
}

void UnicastLayout(void* ent, const char* layout) {
    if (!ent || !layout || !layout[0] || !Buddy_GetGameImport())
        return;
    Buddy_WriteByte(kSvcLayout);
    Buddy_WriteString(layout);
    Buddy_Unicast(ent, 1);
}

void PushLayoutPayload(void* ent, const char* layout) {
    if (!ent || !Buddy_GetGameImport())
        return;
    // Stock scoreboard path (dm.cpp): SP_Print(LAYOUT_RESET) then layout tokens.
    Buddy_SP_Print(ent, kDmLayoutReset);
    UnicastLayout(ent, layout);
}

void __cdecl HkClientCommand(void* ent) {
    if (MgPlatformEnabled() && MgEnabled() && ent) {
        const char* cmd = MgArgv(0);
        if (cmd && cmd[0]) {
            for (const MgGameOps* g : g_games) {
                if (g && g->command && g->onClientCmd && SameNoCase(cmd, g->command)) {
                    const int slot = MgSlotForEdict(ent);
                    if (slot >= 1)
                        g->onClientCmd(slot);
                    else
                        Buddy_ClientPrintf(ent, 2,
                                           "[minigames] cannot resolve player slot for '%s'\n",
                                           g->command);
                    return;
                }
            }
        }
    }
    if (g_originalClientCommand)
        g_originalClientCommand(ent);
}

void InstallClientCommandHook(void* gameExport) {
    if (g_originalClientCommand)
        return;
    if (!gameExport) {
        PrintOut(PRINT_BAD, "[minigames] ClientCommand hook skipped: no game export\n");
        return;
    }
    if (!Readable(static_cast<char*>(gameExport) + kExportClientCommandOfs, sizeof(void*)) ||
        !Readable(static_cast<char*>(gameExport) + kExportEdictsOfs, sizeof(void*)) ||
        !Readable(static_cast<char*>(gameExport) + kExportEdictSizeOfs, sizeof(int))) {
        PrintOut(PRINT_BAD, "[minigames] ClientCommand hook skipped: unreadable ge table\n");
        return;
    }
    void** slot =
        reinterpret_cast<void**>(static_cast<char*>(gameExport) + kExportClientCommandOfs);
    void* target = *slot;
    char* edicts = *reinterpret_cast<char**>(static_cast<char*>(gameExport) + kExportEdictsOfs);
    const int edictSize =
        *reinterpret_cast<int*>(static_cast<char*>(gameExport) + kExportEdictSizeOfs);
    if (!target || !edicts || edictSize <= 0) {
        PrintOut(PRINT_BAD, "[minigames] ClientCommand hook skipped: bad target/edicts\n");
        return;
    }
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(target, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        PrintOut(PRINT_BAD, "[minigames] ClientCommand hook skipped: target not committed\n");
        return;
    }
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & exec)) {
        PrintOut(PRINT_BAD, "[minigames] ClientCommand hook skipped: target not executable\n");
        return;
    }
    g_edicts = edicts;
    g_edictSize = edictSize;
    g_originalClientCommand = reinterpret_cast<clientcmd_fn>(target);
    g_geClientCommandSlot = slot;
    *slot = reinterpret_cast<void*>(&HkClientCommand);
    PrintOut(PRINT_LOG, "[minigames] ClientCommand hooked (ge+0x34)\n");
}

void InstallCmdScoreHook() {
    if (g_scoreTrampoline)
        return;
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaCmdScoreF, 8))
        return;
    void* target = reinterpret_cast<char*>(h) + kRvaCmdScoreF;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(target, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & exec))
        return;
    g_scoreTrampoline = DetourCreate(target, reinterpret_cast<void*>(&HkCmd_Score_f), DETOUR_TYPE_JMP, 6);
    if (g_scoreTrampoline)
        PrintOut(PRINT_LOG, "[minigames] Cmd_Score_f hooked (3-page score cycle)\n");
    else
        PrintOut(PRINT_BAD, "[minigames] Cmd_Score_f hook failed\n");
}

}  // namespace

// ---- platform API ----

bool MgRegisterGame(const MgGameOps* ops) {
    if (!ops || !ops->command || !ops->command[0] || !ops->onClientCmd)
        return false;
    for (const MgGameOps* g : g_games) {
        if (g && g->command && SameNoCase(g->command, ops->command))
            return false;
    }
    for (MgGameOps const*& slot : g_games) {
        if (!slot) {
            slot = ops;
            return true;
        }
    }
    return false;
}

bool MgRegisterConsoleCommand(const char* name, void* fn) {
    if (!name || !fn || !detour_Cmd_AddCommand::oCmd_AddCommand)
        return false;
    SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
    return true;
}

int MgRegisterImage(const char* name) {
    if (!name || !name[0])
        return 0;
    return Buddy_ImageIndex(name);
}

// sv.configstrings[0] @ SoF.exe+0x3A2374 (configstrings_CS_NAME; gi.configstring
// and PF_Configstring write here). NOT unk_203A23B4 (+0x3A23B4) — that is
// SV_FindIndex's search scratch, one slot earlier. CS_GHOULFILES = 1497.
constexpr unsigned kRvaSvConfigstrings = 0x3A2374;
constexpr int kCsGhoulFiles = 1497;
constexpr int kCsMapChecksum = 6;
constexpr int kMaxGhoulFiles = 2048;
constexpr int kMaxQpath = 64;

char* SvConfigstrings() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    char* base = reinterpret_cast<char*>(h) + kRvaSvConfigstrings;
    if (!Readable(base, kMaxQpath))
        return nullptr;
    return base;
}

int MgRegisterGhoulFile(const char* path) {
    if (!path || !path[0])
        return 0;
    const int len = static_cast<int>(std::strlen(path));
    if (len >= kMaxQpath || std::strstr(path, "..") != nullptr ||
        path[0] == '/' || path[0] == '\\')
        return 0;
    char* strings = SvConfigstrings();
    if (!strings)
        return 0;
    int freeIdx = 0;
    for (int i = 1; i < kMaxGhoulFiles; ++i) {
        char* slot = strings + static_cast<unsigned>(kCsGhoulFiles + i) * kMaxQpath;
        if (!Readable(slot, kMaxQpath))
            return 0;
        if (slot[0] == '\0') {
            if (freeIdx == 0)
                freeIdx = i;
            continue;
        }
        // Length-bounded: slots are NUL-padded, the caller's bytes after
        // its NUL are not — a fixed 64-byte compare never matches.
        if (std::strncmp(slot, path, static_cast<std::size_t>(len) + 1) == 0)
            return kCsGhoulFiles + i;  // already registered
    }
    if (freeIdx == 0)
        return 0;
    if (!Buddy_Configstring(kCsGhoulFiles + freeIdx, path))
        return 0;
    return kCsGhoulFiles + freeIdx;
}

const char* MgMapChecksum() {
    char* strings = SvConfigstrings();
    if (!strings)
        return "";
    char* cs = strings + static_cast<unsigned>(kCsMapChecksum) * kMaxQpath;
    return Readable(cs, kMaxQpath) ? cs : "";
}

constexpr char kMgSbMgBgGhoul[] = "sb/mg/bg.m32";
constexpr char kMgSbMgPnGhoul[] = "sb/mg/pn.m32";
constexpr char kMgSbMgIdGhoul[] = "sb/mg/id.m32";

bool MgIsBuddyGhoulPath(const char* path) {
    return path && (std::strncmp(path, "sb/mg/", 6) == 0 || std::strncmp(path, "sb/tt/", 6) == 0);
}

void MgStripBuddyGhoulFiles() {
    char* strings = SvConfigstrings();
    if (!strings)
        return;
    struct Entry {
        int idx;
        char path[kMaxQpath + 1];
    };
    Entry list[32];
    int n = 0;
    for (int i = 1; i < kMaxGhoulFiles && n < static_cast<int>(sizeof(list) / sizeof(list[0])); ++i) {
        char* slot = strings + static_cast<unsigned>(kCsGhoulFiles + i) * kMaxQpath;
        if (!Readable(slot, kMaxQpath) || slot[0] == '\0')
            continue;
        if (!MgIsBuddyGhoulPath(slot))
            continue;
        list[n].idx = kCsGhoulFiles + i;
        std::memcpy(list[n].path, slot, kMaxQpath);
        list[n].path[kMaxQpath] = '\0';
        ++n;
    }
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            if (list[b].idx > list[a].idx) {
                Entry t = list[a];
                list[a] = list[b];
                list[b] = t;
            }
        }
    }
    for (int i = 0; i < n; ++i)
        (void)Buddy_RemoveIndex(list[i].path, kCsGhoulFiles, kMaxGhoulFiles);
}

void MgRegisterPlatformGhoulFiles() {
    if (!MgPlatformEnabled())
        return;
    const int black = MgRegisterGhoulFile(kMgSbMgBgGhoul);
    const int panel = MgRegisterGhoulFile(kMgSbMgPnGhoul);
    const int idle = MgRegisterGhoulFile(kMgSbMgIdGhoul);
    (void)MgRegisterImage(kMgSbMgBg);
    (void)MgRegisterImage(kMgSbMgPn);
    (void)MgRegisterImage(kMgSbMgId);
    if (black && panel && idle)
        Buddy_DebugPrintf("[minigames] platform ghoul mg bg=%d pn=%d id=%d\n", black, panel, idle);
    else
        Buddy_DebugPrintf("[minigames] platform ghoul register failed\n");
}

extern void TttRegisterGhoulFiles();
extern void TttOnEnabled();

void MgSyncGhoulFiles() {
    char* strings = SvConfigstrings();
    if (!strings)
        return;
    char* cs = strings + static_cast<unsigned>(kCsMapChecksum) * kMaxQpath;
    if (!Readable(cs, kMaxQpath) || cs[0] == '\0')
        return;
    static char lastMap[kMaxQpath + 1] = {};
    if (std::strncmp(lastMap, cs, kMaxQpath) == 0)
        return;
    std::memcpy(lastMap, cs, kMaxQpath);
    lastMap[kMaxQpath] = '\0';
    MgStripBuddyGhoulFiles();
    MgRegisterPlatformGhoulFiles();
    TttRegisterGhoulFiles();
    Buddy_DebugPrintf("[minigames] ghoul table synced (map %s)\n", lastMap);
}

// Mid-map cvar 0→1: append missing ghoul/image entries only (MgRegisterGhoulFile
// is idempotent). Disable/switch never removes — map change compacts via sync.
void MgAppendGhoulFilesOnEnable() {
    static int prevPlatform = -1;
    static int prevTtt = -1;
    const bool plat = MgPlatformEnabled();
    const bool ttt = Ttt_Enabled();
    if (prevPlatform < 0) {
        prevPlatform = plat ? 1 : 0;
        prevTtt = ttt ? 1 : 0;
        return;
    }
    if (plat && !prevPlatform)
        MgRegisterPlatformGhoulFiles();
    if (ttt && !prevTtt)
        TttOnEnabled();
    prevPlatform = plat ? 1 : 0;
    prevTtt = ttt ? 1 : 0;
}

int MgMaxClients() {
    if (!g_cvMaxClients)
        g_cvMaxClients = Buddy_GetEngineCvar("maxclients", "4", 0, nullptr);
    int n = static_cast<int>(Buddy_ReadCvarValue(g_cvMaxClients, 4.0f));
    if (n < 1)
        return 1;
    if (n > kMgMaxSlots)
        return kMgMaxSlots;
    return n;
}

int MgUserToInternal(int slot0) {
    if (slot0 < 0 || slot0 >= MgMaxClients())
        return 0;
    return slot0 + 1;
}

int MgInternalToUser(int slot1) {
    if (slot1 < 1 || slot1 > MgMaxClients())
        return -1;
    return slot1 - 1;
}

bool MgSlotSpawned(int slot1) {
    if (slot1 < 1 || slot1 > MgMaxClients())
        return false;
    HMODULE h = ExeMod();
    if (!h || !Readable(reinterpret_cast<char*>(h) + kRvaSvClients, sizeof(void*)))
        return false;
    char* clients = *reinterpret_cast<char**>(reinterpret_cast<char*>(h) + kRvaSvClients);
    if (!clients)
        return false;
    char* cl = clients + static_cast<unsigned>(slot1 - 1) * kClientStride;
    if (!Readable(cl, sizeof(int)))
        return false;
    return *reinterpret_cast<int*>(cl) == kCsSpawned;
}

void* MgEdictForSlot(int slot1) {
    char* edicts = EdictTable();
    const int stride = EdictStride();
    if (!edicts || stride <= 0 || slot1 < 1 || slot1 > MgMaxClients())
        return nullptr;
    char* ent = edicts + static_cast<unsigned>(slot1) * static_cast<unsigned>(stride);
    if (!Readable(ent, kEdictInuseOfs + sizeof(int)))
        return nullptr;
    if (*reinterpret_cast<int*>(ent + kEdictInuseOfs) == 0)
        return nullptr;
    if (!Readable(ent + kEdictClientOfs, sizeof(void*)))
        return nullptr;
    void* client = *reinterpret_cast<void**>(ent + kEdictClientOfs);
    if (!client || !Readable(client, kClientStatsOfs + (kStatLayouts + 1) * 2u))
        return nullptr;
    return ent;
}

int MgSlotForEdict(void* ent) {
    char* edicts = EdictTable();
    const int stride = EdictStride();
    if (!ent || !edicts || stride <= 0)
        return 0;
    const auto diff = static_cast<char*>(ent) - edicts;
    if (diff > 0 && diff % stride == 0) {
        const int slot = static_cast<int>(diff / stride);
        if (slot >= 1 && slot <= MgMaxClients())
            return slot;
    }
    const int n = MgMaxClients();
    for (int slot = 1; slot <= n; ++slot) {
        if (MgEdictForSlot(slot) == ent)
            return slot;
    }
    return 0;
}

int MgSlotFromClient(void* client) {
    if (!client || !Readable(client, kClientEdictOfs + sizeof(void*)))
        return 0;
    HMODULE h = ExeMod();
    if (!h || !Readable(reinterpret_cast<char*>(h) + kRvaSvClients, sizeof(void*)))
        return 0;
    char* clients = *reinterpret_cast<char**>(reinterpret_cast<char*>(h) + kRvaSvClients);
    if (!clients)
        return 0;
    const auto off = static_cast<char*>(client) - clients;
    if (off < 0 || off % static_cast<ptrdiff_t>(kClientStride) != 0)
        return 0;
    const int slot = static_cast<int>(off / static_cast<ptrdiff_t>(kClientStride)) + 1;
    if (slot < 1 || slot > MgMaxClients() || !MgSlotSpawned(slot))
        return 0;
    return slot;
}

void MgParseUserCmd(int slot1, void* cmd, MgUserCmdInput* in) {
    if (!in || slot1 < 1 || slot1 > kMgMaxSlots || !cmd ||
        !Readable(cmd, kMgUcmdOffUp + sizeof(short)))
        return;
    auto* u = static_cast<char*>(cmd);
    const short fwd = *reinterpret_cast<short*>(u + kMgUcmdOffForward);
    const short side = *reinterpret_cast<short*>(u + kMgUcmdOffSide);
    const unsigned char buttons = static_cast<unsigned char>(u[kMgUcmdOffButtons]);
    PrevUserCmd& prev = g_prevCmd[slot1];

    in->forward = fwd > kMgMoveDeadzone;
    in->back = fwd < -kMgMoveDeadzone;
    in->left = side < -kMgMoveDeadzone;
    in->right = side > kMgMoveDeadzone;
    in->forwardEdge = in->forward && !(prev.forward > kMgMoveDeadzone);
    in->backEdge = in->back && !(prev.forward < -kMgMoveDeadzone);
    in->leftEdge = in->left && !(prev.side < -kMgMoveDeadzone);
    in->rightEdge = in->right && !(prev.side > kMgMoveDeadzone);
    in->attack = (buttons & kMgBtnAttack) != 0;
    in->attackEdge = in->attack && !(prev.buttons & kMgBtnAttack);
    in->use = (buttons & kMgBtnUse) != 0;
    in->useEdge = in->use && !(prev.buttons & kMgBtnUse);

    prev.forward = fwd;
    prev.side = side;
    prev.buttons = buttons;
}

void MgStripUserCmd(void* cmd) {
    if (!cmd || !Readable(cmd, kMgUcmdOffUp + sizeof(short)))
        return;
    auto* u = static_cast<char*>(cmd);
    *reinterpret_cast<short*>(u + kMgUcmdOffForward) = 0;
    *reinterpret_cast<short*>(u + kMgUcmdOffSide) = 0;
    *reinterpret_cast<short*>(u + kMgUcmdOffUp) = 0;
    u[kMgUcmdOffButtons] =
        static_cast<char>(static_cast<unsigned char>(u[kMgUcmdOffButtons]) & ~(kMgBtnAttack | kMgBtnUse));
}

int MgArgc() {
    return Buddy_ClientArgc();
}

const char* MgArgv(int n) {
    return Buddy_ClientArgv(n);
}

bool MgEnabled() {
    return MgPlatformEnabled();
}

void MgShowLayout(int slot1, bool on) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        g_visible[slot1] = on;
    void* ent = MgEdictForSlot(slot1);
    if (!ent)
        return;
    if (on && g_layoutCache[slot1][0])
        ApplyScorePage(ent, slot1, MgScorePage::Minigame);
    else if (!on)
        ApplyScorePage(ent, slot1, MgScorePage::Off);
}

void MgPushLayout(int slot1, const MgCanvas& canvas) {
    void* ent = MgEdictForSlot(slot1);
    if (!ent) {
        Buddy_DebugPrintf("[minigames] push layout: slot %d has no edict\n", slot1);
        return;
    }
    if (slot1 >= 1 && slot1 <= kMgMaxSlots) {
        std::strncpy(g_layoutCache[slot1], canvas.text, kMgLayoutCap);
        g_layoutCache[slot1][kMgLayoutCap - 1] = '\0';
        g_layoutDirty[slot1] = true;
    }
    if (!canvas.text[0])
        return;
    if (g_visible[slot1] && g_page[slot1] == MgScorePage::Minigame)
        SendMinigameLayout(ent, slot1, canvas.text);
    else if (g_visible[slot1] && g_page[slot1] == MgScorePage::Off)
        ApplyScorePage(ent, slot1, MgScorePage::Minigame);
    else if (!g_visible[slot1])
        PushLayoutPayload(ent, canvas.text);
}

void MgClearLayout(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots) {
        g_layoutCache[slot1][0] = '\0';
        g_layoutDirty[slot1] = false;
        g_visible[slot1] = false;
        g_page[slot1] = MgScorePage::Off;
    }
    if (void* ent = MgEdictForSlot(slot1))
        ApplyScorePage(ent, slot1, MgScorePage::Off);
}

void MgShowIdleLayout(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_layoutCache[slot1][0] = '\0';
    g_visible[slot1] = true;
    g_page[slot1] = MgScorePage::Minigame;
    if (void* ent = MgEdictForSlot(slot1))
        ApplyScorePage(ent, slot1, MgScorePage::Minigame);
}

// ---- engine entry points (wired via hooks/callbacks json) ----

// Splits "slot rest-of-line" out of Cmd_Args-style text.
bool SplitSlotAndRest(const char* args, int& slot, const char*& rest) {
    slot = 0;
    rest = "";
    if (!args)
        return false;
    while (*args == ' ' || *args == '\t')
        ++args;
    if (!*args)
        return false;
    slot = std::atoi(args);
    while (*args && *args != ' ' && *args != '\t')
        ++args;
    while (*args == ' ' || *args == '\t')
        ++args;
    rest = args;
    return slot >= 0 && slot < MgMaxClients();
}

bool TargetIsAll(const char* s) {
    return s && (_stricmp(s, "all") == 0 || _stricmp(s, "*") == 0);
}

// 0 = all spawned slots, 1..N = one internal slot, -1 = bad target.
int ParseTargetSlot(const char* s) {
    if (!s || !*s)
        return -1;
    if (TargetIsAll(s))
        return 0;
    const int internal = MgUserToInternal(std::atoi(s));
    return internal ? internal : -1;
}

void JoinArgvFrom(int first, char* out, int cap) {
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    const int argc = Buddy_ClientArgc();
    for (int i = first; i < argc; ++i) {
        const char* a = Buddy_ClientArgv(i);
        if (!a || !*a)
            continue;
        if (out[0]) {
            if (static_cast<int>(std::strlen(out) + 1) >= cap)
                return;
            std::strcat(out, " ");
        }
        if (static_cast<int>(std::strlen(out) + std::strlen(a)) >= static_cast<unsigned>(cap))
            return;
        std::strcat(out, a);
    }
}

constexpr int kSvcCountdown = 31;

void SendCountdown(int slot1, int secs) {
    void* ent = MgEdictForSlot(slot1);
    if (!ent)
        return;
    Buddy_WriteByte(kSvcCountdown);
    Buddy_WriteByte(secs & 0xFF);
    Buddy_Unicast(ent, 1);
}

// Engine xcommand_t: void (__cdecl*)(void).
extern "C" void __cdecl mg_Push_f();
extern "C" void __cdecl mg_Show_f();
extern "C" void __cdecl mg_Clear_f();
extern "C" void __cdecl mg_Idle_f();
extern "C" void __cdecl mg_GhoulList_f();
extern "C" void __cdecl mg_GhoulRegister_f();
extern "C" void __cdecl mg_SpRegister_f();
extern "C" void __cdecl mg_Print_f();
extern "C" void __cdecl mg_Center_f();
extern "C" void __cdecl mg_Caption_f();
extern "C" void __cdecl mg_Cin_f();
extern "C" void __cdecl mg_Welcome_f();
extern "C" void __cdecl mg_Name_f();
extern "C" void __cdecl mg_Countdown_f();
extern "C" void __cdecl mg_Test_f();

extern "C" void __cdecl mg_Push_f() {
    if (!MgPlatformEnabled())
        return;
    int slot = 0;
    const char* rest = nullptr;
    if (!SplitSlotAndRest(Buddy_ClientArgs(), slot, rest) || !*rest) {
        Buddy_DebugPrintf("usage: mg_push <slot 0-based> <layout tokens...>\n");
        return;
    }
    const int internal = MgUserToInternal(slot);
    if (!internal || !MgSlotSpawned(internal)) {
        Buddy_DebugPrintf("[minigames] mg_push: slot %d not spawned\n", slot);
        return;
    }
    MgCanvas c;
    MgCanvasClear(c);
    if (!MgCanvasRaw(c, rest)) {
        Buddy_DebugPrintf("[minigames] mg_push: layout too long (1024 cap)\n");
        return;
    }
    MgPushLayout(internal, c);
    MgShowLayout(internal, true);
}

extern "C" void __cdecl mg_Show_f() {
    if (!MgPlatformEnabled())
        return;
    const int slot = std::atoi(Buddy_ClientArgv(1));
    const int on = std::atoi(Buddy_ClientArgv(2));
    const int internal = MgUserToInternal(slot);
    if (!internal || Buddy_ClientArgc() < 3) {
        Buddy_DebugPrintf("usage: mg_show <slot 0-based> <0|1>\n");
        return;
    }
    MgShowLayout(internal, on != 0);
}

extern "C" void __cdecl mg_Clear_f() {
    if (!MgPlatformEnabled())
        return;
    const int slot = std::atoi(Buddy_ClientArgv(1));
    const int internal = MgUserToInternal(slot);
    if (!internal) {
        Buddy_DebugPrintf("usage: mg_clear <slot 0-based>\n");
        return;
    }
    MgClearLayout(internal);
}

extern "C" void __cdecl mg_Idle_f() {
    if (!MgPlatformEnabled())
        return;
    const int internal = MgUserToInternal(std::atoi(Buddy_ClientArgv(1)));
    if (!internal) {
        Buddy_DebugPrintf("usage: mg_idle <slot 0-based>\n");
        return;
    }
    MgShowIdleLayout(internal);
}

// Lists non-empty CS_GHOULFILES slots (sofree sf_sv_ghoul_list equivalent).
// Empty slots END the client's download walk for the whole range, so gaps
// matter: everything must be contiguous from 1497+1.
extern "C" void __cdecl mg_GhoulList_f() {
    char* strings = SvConfigstrings();
    if (!strings) {
        Buddy_DebugPrintf("[minigames] mg_ghoul_list: configstrings unreadable\n");
        return;
    }
    const char* search = Buddy_ClientArgc() >= 2 ? Buddy_ClientArgv(1) : "";
    if (!search)
        search = "";
    int count = 0;
    for (int i = 1; i < kMaxGhoulFiles; ++i) {
        char* slot = strings + static_cast<unsigned>(kCsGhoulFiles + i) * kMaxQpath;
        if (!Readable(slot, kMaxQpath))
            break;
        if (slot[0] == '\0')
            continue;
        char name[kMaxQpath + 1];
        std::memcpy(name, slot, kMaxQpath);
        name[kMaxQpath] = '\0';
        if (search[0] != '\0' && std::strstr(name, search) == nullptr)
            continue;
        Buddy_DebugPrintf("[minigames] ghoul %d: %s\n", kCsGhoulFiles + i, name);
        ++count;
    }
    if (count == 0) {
        char probe[kMaxQpath + 1];
        char* cs0 = strings;
        std::memcpy(probe, cs0, kMaxQpath);
        probe[kMaxQpath] = '\0';
        Buddy_DebugPrintf("[minigames] cs[0]=%s (empty ghoul table)\n", probe);
    }
    Buddy_DebugPrintf("[minigames] %d ghoul entries\n", count);
}

extern "C" void __cdecl mg_GhoulRegister_f() {
    const char* path = Buddy_ClientArgv(1);
    if (!path || !*path) {
        Buddy_DebugPrintf("usage: mg_ghoul_register <path>  (e.g. sb/tt/x.m32)\n");
        return;
    }
    const int idx = MgRegisterGhoulFile(path);
    if (idx)
        Buddy_DebugPrintf("[minigames] ghoul registered cs=%d path=%s\n", idx, path);
    else
        Buddy_DebugPrintf("[minigames] mg_ghoul_register failed: %s\n", path);
}

extern "C" void __cdecl mg_SpRegister_f() {
    const char* pkg = Buddy_ClientArgv(1);
    if (!pkg || !*pkg) {
        Buddy_DebugPrintf("usage: mg_sp_register <package>  (reference name, no .sp suffix)\n");
        return;
    }
    if (Buddy_SP_Register(pkg))
        Buddy_DebugPrintf("[minigames] SP_Register: %s\n", pkg);
    else
        Buddy_DebugPrintf("[minigames] mg_sp_register failed: %s\n", pkg);
}

extern "C" void __cdecl mg_Print_f() {
    const int target = ParseTargetSlot(Buddy_ClientArgv(1));
    const int level = std::atoi(Buddy_ClientArgv(2));
    char text[1024];
    JoinArgvFrom(3, text, sizeof(text));
    if (target < 0 || !text[0]) {
        Buddy_DebugPrintf("usage: mg_print <slot|all> <level> <text...>\n");
        return;
    }
    if (target == 0) {
        Buddy_BroadcastPrintf(level, "%s", text);
        return;
    }
    if (void* ent = MgEdictForSlot(target))
        Buddy_ClientPrintf(ent, level, "%s", text);
}

extern "C" void __cdecl mg_Center_f() {
    const int target = ParseTargetSlot(Buddy_ClientArgv(1));
    char text[1024];
    JoinArgvFrom(2, text, sizeof(text));
    if (target < 0 || !text[0]) {
        Buddy_DebugPrintf("usage: mg_center <slot|all> <text...>\n");
        return;
    }
    if (target == 0) {
        const int n = MgMaxClients();
        for (int slot = 1; slot <= n; ++slot) {
            if (void* ent = MgEdictForSlot(slot))
                Buddy_CenterPrintf(ent, "%s", text);
        }
        return;
    }
    if (void* ent = MgEdictForSlot(target))
        Buddy_CenterPrintf(ent, "%s", text);
}

extern "C" void __cdecl mg_Caption_f() {
    const int target = ParseTargetSlot(Buddy_ClientArgv(1));
    const unsigned short id = static_cast<unsigned short>(std::atoi(Buddy_ClientArgv(2)));
    if (target < 0 || Buddy_ClientArgc() < 3) {
        Buddy_DebugPrintf("usage: mg_caption <slot|all> <sp_string_id>\n");
        return;
    }
    if (target == 0) {
        Buddy_BroadcastCaption(2, id);
        return;
    }
    if (void* ent = MgEdictForSlot(target))
        Buddy_CaptionPrintf(ent, id);
}

extern "C" void __cdecl mg_Cin_f() {
    const int target = ParseTargetSlot(Buddy_ClientArgv(1));
    const int x = std::atoi(Buddy_ClientArgv(2));
    const int y = std::atoi(Buddy_ClientArgv(3));
    const int speed = std::atoi(Buddy_ClientArgv(4));
    char text[1024];
    JoinArgvFrom(5, text, sizeof(text));
    if (target < 0 || !text[0] || Buddy_ClientArgc() < 6) {
        Buddy_DebugPrintf("usage: mg_cin <slot|all> <x> <y> <speed> <text...>\n");
        return;
    }
    const int n = MgMaxClients();
    const int lo = target ? target : 1;
    const int hi = target ? target : n;
    for (int slot = lo; slot <= hi; ++slot) {
        if (!MgSlotSpawned(slot))
            continue;
        if (void* ent = MgEdictForSlot(slot))
            Buddy_CinPrintf(ent, x, y, speed, text);
    }
}

extern "C" void __cdecl mg_Welcome_f() {
    const int target = ParseTargetSlot(Buddy_ClientArgv(1));
    if (target < 0) {
        Buddy_DebugPrintf("usage: mg_welcome <slot|all>\n");
        return;
    }
    const int n = MgMaxClients();
    const int lo = target ? target : 1;
    const int hi = target ? target : n;
    for (int slot = lo; slot <= hi; ++slot) {
        if (!MgSlotSpawned(slot))
            continue;
        if (void* ent = MgEdictForSlot(slot))
            Buddy_WelcomePrintf(ent);
    }
}

extern "C" void __cdecl mg_Name_f() {
    const int slot = std::atoi(Buddy_ClientArgv(1));
    const int from = std::atoi(Buddy_ClientArgv(2));
    const int color = std::atoi(Buddy_ClientArgv(3));
    char text[1024];
    JoinArgvFrom(4, text, sizeof(text));
    const int internal = MgUserToInternal(slot);
    const int fromInternal = MgUserToInternal(from);
    if (!internal || !fromInternal || !text[0]) {
        Buddy_DebugPrintf("usage: mg_name <slot 0-based> <from_slot 0-based> <color> <text...>\n");
        return;
    }
    void* ent = MgEdictForSlot(internal);
    void* fromEnt = MgEdictForSlot(fromInternal);
    if (ent && fromEnt)
        Buddy_NamePrintf(ent, fromEnt, color, "%s", text);
}

extern "C" void __cdecl mg_Countdown_f() {
    const int target = ParseTargetSlot(Buddy_ClientArgv(1));
    const int secs = std::atoi(Buddy_ClientArgv(2));
    if (target < 0 || Buddy_ClientArgc() < 3) {
        Buddy_DebugPrintf("usage: mg_countdown <slot|all> <seconds>\n");
        return;
    }
    const int n = MgMaxClients();
    const int lo = target ? target : 1;
    const int hi = target ? target : n;
    for (int slot = lo; slot <= hi; ++slot) {
        if (MgSlotSpawned(slot))
            SendCountdown(slot, secs);
    }
}

extern "C" void __cdecl mg_Test_f() {
    if (!MgPlatformEnabled())
        return;
    const int internal = MgUserToInternal(std::atoi(Buddy_ClientArgv(1)));
    if (!internal || !MgSlotSpawned(internal)) {
        Buddy_DebugPrintf("usage: mg_test <slot 0-based>  (bg panel + label; see _sofbuddy_minigames_bg)\n");
        return;
    }
    MgCanvas c;
    MgCanvasClear(c);
    MgCanvasBackground(c);
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 320, 240, "MINIGAME TEST");
    MgPushLayout(internal, c);
    MgShowLayout(internal, true);
}

void mg_OnGameDllLoaded(void* gameExport) {
    if (char* ed = GameEdicts())
        g_edicts = ed;
    else if (gameExport && !g_edicts &&
             Readable(static_cast<char*>(gameExport) + kExportEdictsOfs, sizeof(void*)) &&
             Readable(static_cast<char*>(gameExport) + kExportEdictSizeOfs, sizeof(int))) {
        g_edicts = *reinterpret_cast<char**>(static_cast<char*>(gameExport) + kExportEdictsOfs);
        g_edictSize = *reinterpret_cast<int*>(static_cast<char*>(gameExport) + kExportEdictSizeOfs);
    }
    if (g_edictSize <= 0)
        g_edictSize = static_cast<int>(kEdictStrideDefault);
    InstallClientCommandHook(gameExport);
    InstallCmdScoreHook();
    EnsureScoreboardPulse();
    MgRegisterConsoleCommand("mg_push", reinterpret_cast<void*>(&mg_Push_f));
    MgRegisterConsoleCommand("mg_show", reinterpret_cast<void*>(&mg_Show_f));
    MgRegisterConsoleCommand("mg_clear", reinterpret_cast<void*>(&mg_Clear_f));
    MgRegisterConsoleCommand("mg_idle", reinterpret_cast<void*>(&mg_Idle_f));
    MgRegisterConsoleCommand("mg_ghoul_list", reinterpret_cast<void*>(&mg_GhoulList_f));
    MgRegisterConsoleCommand("mg_ghoul_register", reinterpret_cast<void*>(&mg_GhoulRegister_f));
    MgRegisterConsoleCommand("mg_sp_register", reinterpret_cast<void*>(&mg_SpRegister_f));
    MgRegisterConsoleCommand("mg_print", reinterpret_cast<void*>(&mg_Print_f));
    MgRegisterConsoleCommand("mg_center", reinterpret_cast<void*>(&mg_Center_f));
    MgRegisterConsoleCommand("mg_caption", reinterpret_cast<void*>(&mg_Caption_f));
    MgRegisterConsoleCommand("mg_cin", reinterpret_cast<void*>(&mg_Cin_f));
    MgRegisterConsoleCommand("mg_welcome", reinterpret_cast<void*>(&mg_Welcome_f));
    MgRegisterConsoleCommand("mg_name", reinterpret_cast<void*>(&mg_Name_f));
    MgRegisterConsoleCommand("mg_countdown", reinterpret_cast<void*>(&mg_Countdown_f));
    MgRegisterConsoleCommand("mg_test", reinterpret_cast<void*>(&mg_Test_f));
    PrintOut(PRINT_LOG, "[minigames] server commands registered\n");
}

void MaintainLayoutClient(void* ent, int slot) {
    if (!ent || slot < 1)
        return;
    if (LevelIntermission() || ClientWantsStockScoreboard(ent)) {
        EnsureScoreboardPage(ent, slot);
        return;
    }
    if (g_page[slot] == MgScorePage::Minigame) {
        if (void* client = ClientForEnt(ent))
            SuppressStockLayoutRefresh(client);
        ApplyLayoutClient(ent, true);
        // svc_layout only when the canvas changed — not every server tick.
        if (g_layoutDirty[slot])
            SendMinigameLayout(ent, slot, MinigameLayoutForSlot(slot));
        return;
    }
    if (g_page[slot] == MgScorePage::Scoreboard)
        ApplyLayoutClient(ent, true);  // stock ClientEndServerFrame refreshes at &31
}

void mg_ClientEndServerFramePre(void*& ent) {
    if (!MgPlatformEnabled() || !ent)
        return;
    const int slot = MgSlotForEdict(ent);
    if (slot < 1)
        return;
    if (LevelIntermission() || ClientWantsStockScoreboard(ent)) {
        EnsureScoreboardPage(ent, slot);
        return;
    }
    if (g_page[slot] != MgScorePage::Minigame)
        return;
    if (void* client = ClientForEnt(ent))
        SuppressStockLayoutRefresh(client);
}

void mg_ClientEndServerFramePost(void* ent) {
    if (!MgPlatformEnabled() || !ent)
        return;
    const int slot = MgSlotForEdict(ent);
    if (slot < 1 || g_page[slot] == MgScorePage::Off)
        return;
    if (!MgSlotSpawned(slot)) {
        g_visible[slot] = false;
        g_layoutCache[slot][0] = '\0';
        g_layoutDirty[slot] = false;
        g_page[slot] = MgScorePage::Off;
        return;
    }
    MaintainLayoutClient(ent, slot);
}

void mg_CL_SendClientMessagesPre() {
    if (!MgPlatformEnabled())
        return;
    const int n = MgMaxClients();
    for (int slot = 1; slot <= n; ++slot) {
        if (g_page[slot] == MgScorePage::Off)
            continue;
        if (void* ent = MgEdictForSlot(slot)) {
            if (LevelIntermission() || ClientWantsStockScoreboard(ent))
                EnsureScoreboardPage(ent, slot);
            else
                ApplyLayoutClient(ent, true);
        }
    }
}

void mg_SvClientThinkPre(void*& client, void*& cmd) {
    if (!MgPlatformEnabled() || !client || !cmd)
        return;
    const int slot = MgSlotFromClient(client);
    if (slot < 1)
        return;
    MgUserCmdInput in = {};
    MgParseUserCmd(slot, cmd, &in);
    for (const MgGameOps* g : g_games) {
        if (!g || !g->onUserCmd)
            continue;
        g->onUserCmd(slot, &in);
    }
    if (in.consumed)
        MgStripUserCmd(cmd);
}

void mg_SvFramePost(int msec) {
    (void)msec;
    MgSyncGhoulFiles();
    MgAppendGhoulFilesOnEnable();
}

extern "C" void Minigames_Shutdown() {
    if (g_originalClientCommand && g_geClientCommandSlot &&
        Readable(g_geClientCommandSlot, sizeof(void*))) {
        *reinterpret_cast<void**>(g_geClientCommandSlot) =
            reinterpret_cast<void*>(g_originalClientCommand);
    }
    g_originalClientCommand = nullptr;
    g_geClientCommandSlot = nullptr;
    if (g_scoreTrampoline) {
        DetourRemove(&g_scoreTrampoline);
        g_scoreTrampoline = nullptr;
    }
    g_edicts = nullptr;
    g_edictSize = 0;
    for (bool& v : g_visible)
        v = false;
    for (char* row : g_layoutCache)
        row[0] = '\0';
    for (bool& d : g_layoutDirty)
        d = false;
    for (MgScorePage& p : g_page)
        p = MgScorePage::Off;
    g_placeholderLayout[0] = '\0';
    g_placeholderBg = -1;
}
