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
#include "commands/commands.h"
#include "cvarview/cvarview.h"
#include "../profiles/cvar.h"
#include "../profiles/gamepark/gamepark.h"
#include "../profiles/stats/stats.h"
#include "lagometer/lagometer.h"
#include "menu/menu.h"
#include "minigames_api.h"
#include "minigames_clientcmd_logic.h"
#include "settings/settings.h"

extern void ttt_EnsureRegistered();

#include "DetourXS/detourxs.h"
#include "buddy_import.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"

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
constexpr unsigned kEdictViewsideOfs = 0x308;  // edict_t.viewside (game ClientThink)
constexpr unsigned kClientStatsOfs = 164;
constexpr unsigned kClientShowscoresOfs = 0x470;  // gclient_t.showscores
constexpr unsigned kClientButtonsOfs = 0x47C;     // gclient_t.buttons (ClientThink)
constexpr unsigned kClientShowinventoryOfs = 0x474;
constexpr unsigned kClientShowhelpTimeOfs = 0x568;  // gclient_t.showhelp_time (float)
constexpr unsigned kEdictEnemyOfs = 0x804;
constexpr unsigned kRvaDeathmatchClass = 0x15C4D8;
constexpr unsigned kRvaCmdScoreF = 0xF6710;  // cmd_score_f @ gamex86+
constexpr unsigned kRvaDmctfScoreboard = 0x71D70;  // dmctf_c::clientScoreboardMessage
constexpr unsigned kRvaLevelIntermissiontime = 0x15D1C8;  // level.intermissiontime @ G_SetStats
constexpr unsigned kRvaLevelFramenum = 0x15CCD8;          // level.framenum (p_view scoreboard cadence)
constexpr int kLayoutRefreshMask = 31;                    // !(framenum & 31) — stock dm scoreboard
constexpr int kPmDead = 3;                               // pmtype_t PM_DEAD
constexpr unsigned kClientPmTypeOfs = 0;                 // gclient.ps.pmove.pm_type
constexpr unsigned kClientPersHealthOfs = 0x2FC;         // gclient.pers.health @ G_SetStats+764
constexpr int kStatLayouts = 9;
constexpr int kSvcLayout = 2;  // svc_layout → client layout_string (Com_sprintf replace)
constexpr unsigned short kDmLayoutReset = 0x002b;  // dm_generic LAYOUT_RESET ("*"), always on clients
constexpr unsigned short kSobuddyLayoutRaw = 0x0700;  // index 0 — SP_FLAG_LAYOUT "%s"
constexpr unsigned short kSobuddyCreditRaw = 0x0701;  // index 1 — SP_FLAG_CREDIT "%s" (future)

constexpr unsigned kRvaSvClients = 0x396EEC;
constexpr unsigned kClientStride = 0xD2AC;
constexpr unsigned kClientEdictOfs = 0x298;
constexpr int kCsSpawned = 3;

constexpr int kMaxGames = 16;
constexpr int kMgGameIdLen = 16;
constexpr int kMgNavDepth = 8;
constexpr unsigned char kMaxPendingNavPresses = 8;
constexpr char kMgScriptGameId[] = "mgscript";

using clientcmd_fn = void(__cdecl*)(void*);
using cmd_score_fn = void(__cdecl*)(void*);
using dmctf_scoreboard_fn = void(__thiscall*)(void*, void*, void*, int);

clientcmd_fn g_originalClientCommand = nullptr;
void* g_geClientCommandSlot = nullptr;
void* g_scoreTrampoline = nullptr;
void* g_dmctfScoreTrampoline = nullptr;
void* g_spsvCmdTrampoline = nullptr;
char* g_edicts = nullptr;
int g_edictSize = 0;
void* g_cvMaxClients = nullptr;

const MgGameOps* g_games[kMaxGames] = {};
bool g_visible[kMgMaxSlots + 1] = {};
char g_layoutCache[kMgMaxSlots + 1][kMgLayoutCap] = {};
char g_displayOwner[kMgMaxSlots + 1][kMgGameIdLen] = {};
struct MgNavEntry {
    char game[kMgGameIdLen] = {};
    int selected = -1;  // menu cursor the page had when it was pushed
};
MgNavEntry g_navStack[kMgMaxSlots + 1][kMgNavDepth] = {};
unsigned char g_navDepth[kMgMaxSlots + 1] = {};

enum class MgView : unsigned char { Off = 0, Minigame = 1, StockScoreboard = 2 };
MgView g_page[kMgMaxSlots + 1] = {};
bool g_minigameDeathView[kMgMaxSlots + 1] = {};
bool g_layoutDirty[kMgMaxSlots + 1] = {};
bool g_layoutPrimed[kMgMaxSlots + 1] = {};
unsigned char g_menuInputLatch[kMgMaxSlots + 1] = {};
int g_clientCmdArgvBase = 0;
bool g_argOverlay = false;
int g_argOverlayArgc = 0;
const char* g_argOverlayArgv[8] = {};
char g_ctfSbHintLayout[kMgLayoutCap] = {};
bool g_sobuddySpReady = false;
char g_sobuddyRegisterName[64] = {};
unsigned long g_sobuddyFailMs = 0;
constexpr unsigned long kSobuddyRetryMs = 5000;
char* g_svClients = nullptr;

struct PendingInput {
    MgUserCmdInput value = {};
    // Several clc_move packets may arrive before the next server-frame hook.
    // Keep every press so +use can stay held across repeated +forward/-forward.
    unsigned char forwardPresses = 0;
    unsigned char backPresses = 0;
    unsigned char menuBackPresses = 0;
    bool valid = false;
};
PendingInput g_pendingInput[kMgMaxSlots + 1] = {};
bool g_useNavigation[kMgMaxSlots + 1] = {};
bool g_menuBackHeld[kMgMaxSlots + 1] = {};
bool g_menuBackEdge[kMgMaxSlots + 1] = {};

struct ThinkContext {
    void* client = nullptr;
    void* cmd = nullptr;
    int slot = 0;
    bool active = false;
};
ThinkContext g_think = {};

struct PrevUserCmd {
    short forward = 0;
    short side = 0;
    signed char lean = 0;
    unsigned char buttons = 0;
};
PrevUserCmd g_prevCmd[kMgMaxSlots + 1] = {};

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

char* SvClients() {
    if (g_svClients)
        return g_svClients;
    HMODULE h = ExeMod();
    if (!h || !Readable(reinterpret_cast<char*>(h) + kRvaSvClients, sizeof(void*)))
        return nullptr;
    char* clients = *reinterpret_cast<char**>(reinterpret_cast<char*>(h) + kRvaSvClients);
    if (!clients)
        return nullptr;
    g_svClients = clients;
    return clients;
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
    if (g_edicts)
        return g_edicts;
    if (char* ed = GameEdicts())
        g_edicts = ed;
    if (g_edicts)
        return g_edicts;
    return nullptr;
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
void ApplyView(void* ent, int slot, MgView view);
void SuppressStockLayoutRefresh(void* client);
bool KeepMinigameOnDeath(void* ent, int slot);

bool ClientUseHeld(void* ent) {
    void* client = ClientForEnt(ent);
    if (!client || !Readable(static_cast<char*>(client) + kClientButtonsOfs, sizeof(int)))
        return false;
    return (*reinterpret_cast<int*>(static_cast<char*>(client) + kClientButtonsOfs) & kMgBtnUse) != 0;
}

bool ClientShowscoresOpen(void* ent) {
    void* client = ClientForEnt(ent);
    if (!client || !Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
        return false;
    return *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) != 0;
}

void CancelMenuUseOnScoreToggle(void* ent, int slot) {
    if (slot >= 1 && ClientUseHeld(ent) && g_page[slot] == MgView::Minigame)
        g_useNavigation[slot] = true;
}

void EnsureScoreboardPage(void* ent, int slot) {
    if (slot < 1 || slot > kMgMaxSlots)
        return;
    if (KeepMinigameOnDeath(ent, slot))
        return;
    if (g_page[slot] != MgView::StockScoreboard)
        ApplyView(ent, slot, MgView::StockScoreboard);
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

bool KeepMinigameOnDeath(void* ent, int slot) {
    if (slot < 1 || slot > kMgMaxSlots)
        return false;
    const bool keep = MgKeepMinigameOnDeath(
        g_page[slot] == MgView::Minigame, LevelIntermission(),
        ClientWantsStockScoreboard(ent));
    if (!keep) {
        g_minigameDeathView[slot] = false;
        return false;
    }
    if (!g_minigameDeathView[slot]) {
        g_minigameDeathView[slot] = true;
        ApplyView(ent, slot, MgView::Minigame);
    } else {
        if (void* client = ClientForEnt(ent))
            SuppressStockLayoutRefresh(client);
        ApplyLayoutClient(ent, true);
    }
    return true;
}

void PushLayoutPayload(void* ent, const char* layout, bool reset);
void UnicastLayout(void* ent, const char* layout);

bool MgMinigameLayoutRefreshDue() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaLevelFramenum, sizeof(int)))
        return true;
    const int fn = *reinterpret_cast<int*>(reinterpret_cast<char*>(h) + kRvaLevelFramenum);
    return (fn & kLayoutRefreshMask) == 0;
}

void SendMinigameLayout(void* ent, int slot, const char* layout) {
    if (!ent || slot < 1 || !layout || !layout[0])
        return;
    const bool reset = !g_layoutPrimed[slot];
    PushLayoutPayload(ent, layout, reset);
    g_layoutPrimed[slot] = true;
    g_layoutDirty[slot] = false;
}

namespace {

constexpr unsigned kCvarStringOfs = 4;
constexpr int kCsStringPackages = 1463;  // CS_STRING_PACKAGES @ 0x5B7 (retail SoF.exe)
constexpr int kMaxStringPackages = 30;
constexpr const char* kSobuddyBaseName = "sofbuddy";

// SoFree-shaped body: \r\n line endings; no tabs/blank lines. File text uses "%s"
// (SoFree's C source uses "%%s" only because that char[] is a C string literal).
static const char kSobuddySpBody[] =
    "VERSION 1\r\n"
    "ID 7\r\n"
    "REFERENCE SOFBUDDY\r\n"
    "DESCRIPTION \"SoF Buddy\"\r\n"
    "COUNT 2\r\n"
    "INDEX 0\r\n"
    "{\r\n"
    "  REFERENCE LAYOUT_RAW\r\n"
    "  FLAGS SP_FLAG_LAYOUT\r\n"
    "  TEXT_ENGLISH \"%s\"\r\n"
    "}\r\n"
    "INDEX 1\r\n"
    "{\r\n"
    "  REFERENCE CREDIT_RAW\r\n"
    "  FLAGS SP_FLAG_CREDIT\r\n"
    "  TEXT_ENGLISH \"%s\"\r\n"
    "}\r\n";

const char* CvarStr(void* cv) {
    if (!cv)
        return "";
    const char* s = *reinterpret_cast<const char**>(static_cast<char*>(cv) + kCvarStringOfs);
    return (s && s[0]) ? s : "";
}

char* SvConfigstringsEarly() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    constexpr unsigned kRva = 0x3A2374;
    char* base = reinterpret_cast<char*>(h) + kRva;
    if (!Readable(base, 64))
        return nullptr;
    return base;
}

const char* FindStringPackageCs(const char* name) {
    if (!name || !name[0])
        return "";
    char* strings = SvConfigstringsEarly();
    if (!strings)
        return "";
    for (int i = 1; i <= kMaxStringPackages; ++i) {
        char* cs = strings + static_cast<unsigned>(kCsStringPackages + i) * 64;
        if (!Readable(cs, 64) || !cs[0])
            continue;
        if (std::strcmp(cs, name) == 0)
            return cs;
    }
    return "";
}

bool StripVisibleToFs(const char* fs_path) {
    void* buf = nullptr;
    const int len = Buddy_FS_LoadFile(fs_path, &buf, false);
    if (len < 0)
        return false;
    if (buf)
        Buddy_FS_FreeFile(buf);
    return true;
}

uint32_t Crc32(const unsigned char* data, int len) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < len; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b)
            c = (c >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int>(c & 1u))));
    }
    return ~c;
}

void UserDirPath(char* out, std::size_t cap, const char* rel) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    const char* user = Buddy_FS_Userdir();
    if (!user[0])
        user = CvarStr(Buddy_GetEngineCvar("user", "User", 0, nullptr));
    if (!user[0])
        user = "User";
    std::snprintf(out, cap, "%s/%s", user, rel);
}

char g_sobuddyDiskBuf[4096];
int g_sobuddyDiskLen = 0;

bool WriteUserStripFile(const char* disk_path) {
    if (!disk_path || !disk_path[0])
        return false;
    Buddy_FS_CreatePath(disk_path);
    FILE* f = std::fopen(disk_path, "wb");
    if (!f)
        return false;
    const std::size_t n = std::strlen(kSobuddySpBody);
    const bool ok = std::fwrite(kSobuddySpBody, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

void SeedSobuddyDiskTemplate() {
    char path[384];
    UserDirPath(path, sizeof(path), "strip/sofbuddy.sp");
    FILE* f = std::fopen(path, "rb");
    if (f) {
        std::fclose(f);
        return;
    }
    WriteUserStripFile(path);
}

bool SobuddyTemplateBytes(const void** body, int* len) {
    if (!body || !len)
        return false;
    char path[384];
    UserDirPath(path, sizeof(path), "strip/sofbuddy.sp");
    FILE* f = std::fopen(path, "rb");
    if (f) {
        if (std::fseek(f, 0, SEEK_END) == 0) {
            const long n = std::ftell(f);
            if (n > 0 && n < static_cast<long>(sizeof(g_sobuddyDiskBuf)) &&
                std::fseek(f, 0, SEEK_SET) == 0 &&
                std::fread(g_sobuddyDiskBuf, 1, static_cast<std::size_t>(n), f) ==
                    static_cast<std::size_t>(n) &&
                std::strstr(g_sobuddyDiskBuf, "REFERENCE SOFBUDDY")) {
                g_sobuddyDiskLen = static_cast<int>(n);
                *body = g_sobuddyDiskBuf;
                *len = g_sobuddyDiskLen;
                std::fclose(f);
                return true;
            }
        }
        std::fclose(f);
    }
    *body = kSobuddySpBody;
    *len = static_cast<int>(std::strlen(kSobuddySpBody));
    return true;
}

bool BuildSobuddyRegisterName(char* reg, std::size_t cap) {
    if (!reg || cap < 16)
        return false;
    const void* body = nullptr;
    int len = 0;
    if (!SobuddyTemplateBytes(&body, &len) || !body || len <= 0)
        return false;
    const uint32_t crc = Crc32(static_cast<const unsigned char*>(body), len);
    std::snprintf(reg, cap, "%s-%08X", kSobuddyBaseName, crc);
    return true;
}

bool PublishChecksumStrip(const char* reg_name, const void* body, int len) {
    if (!reg_name || !body || len <= 0)
        return false;
    char fs_rel[128];
    std::snprintf(fs_rel, sizeof(fs_rel), "strip/%s.sp", reg_name);
    if (StripVisibleToFs(fs_rel))
        return true;
    char disk[384];
    UserDirPath(disk, sizeof(disk), fs_rel);
    Buddy_FS_CreatePath(disk);
    FILE* f = std::fopen(disk, "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(body, 1, static_cast<std::size_t>(len), f) == static_cast<std::size_t>(len);
    std::fclose(f);
    return ok && StripVisibleToFs(fs_rel);
}

bool SobuddyNeedsRegister() {
    if (g_sobuddySpReady && g_sobuddyRegisterName[0] &&
        FindStringPackageCs(g_sobuddyRegisterName)[0])
        return false;
    return true;
}

constexpr int kCtfSbHintY = 48;  // layout yv -72, just above CTF team header (yv -64)

void EnsureCtfScoreboardHintLayout() {
    if (g_ctfSbHintLayout[0])
        return;
    MgCanvas c;
    MgCanvasClear(c);
    const char* lead = "ACCESS MENU: hold ";
    const char* use = "+use";
    const char* middle = " (open door key) and press ";
    const char* score = "score";
    const int width = static_cast<int>(
        (std::strlen(lead) + std::strlen(use) + std::strlen(middle) +
         std::strlen(score)) * 8);
    int x = 320 - width / 2;
    MgCanvasAltText(c, x, kCtfSbHintY, lead);
    x += static_cast<int>(std::strlen(lead)) * 8;
    MgCanvasPText(c, x, kCtfSbHintY, kMgPYellow, use);
    x += static_cast<int>(std::strlen(use)) * 8;
    MgCanvasAltText(c, x, kCtfSbHintY, middle);
    x += static_cast<int>(std::strlen(middle)) * 8;
    MgCanvasPText(c, x, kCtfSbHintY, kMgPRed, score);
    std::strncpy(g_ctfSbHintLayout, c.text, kMgLayoutCap);
    g_ctfSbHintLayout[kMgLayoutCap - 1] = '\0';
}

void EnsureSobuddyStringPackage() {
    if (!Buddy_GetGameImport())
        return;
    if (!SobuddyNeedsRegister() && g_sobuddySpReady) {
        EnsureCtfScoreboardHintLayout();
        return;
    }
    const unsigned long now = GetTickCount();
    if (g_sobuddyFailMs && (now - g_sobuddyFailMs) < kSobuddyRetryMs)
        return;
    g_sobuddySpReady = false;
    g_sobuddyRegisterName[0] = '\0';
    SeedSobuddyDiskTemplate();
    const void* body = nullptr;
    int blen = 0;
    if (!SobuddyTemplateBytes(&body, &blen) || !body || blen <= 0) {
        PrintOut(PRINT_BAD, "[minigames] sofbuddy template unavailable\n");
        g_sobuddyFailMs = now;
        return;
    }
    char reg[64];
    if (!BuildSobuddyRegisterName(reg, sizeof(reg))) {
        PrintOut(PRINT_BAD, "[minigames] sofbuddy register name build failed\n");
        g_sobuddyFailMs = now;
        return;
    }
    if (!PublishChecksumStrip(reg, body, blen)) {
        PrintOut(PRINT_BAD, "[minigames] strip/%s.sp publish failed\n", reg);
        g_sobuddyFailMs = now;
        return;
    }
    if (!Buddy_SP_Register(reg)) {
        PrintOut(PRINT_BAD, "[minigames] SP_Register(%s) failed\n", reg);
        g_sobuddyFailMs = now;
        return;
    }
    std::strncpy(g_sobuddyRegisterName, reg, sizeof(g_sobuddyRegisterName) - 1);
    g_sobuddyRegisterName[sizeof(g_sobuddyRegisterName) - 1] = '\0';
    if (!FindStringPackageCs(reg)[0]) {
        g_sobuddyRegisterName[0] = '\0';
        PrintOut(PRINT_BAD, "[minigames] SP_Register(%s): no CS_STRING_PACKAGES entry\n", reg);
        g_sobuddyFailMs = now;
        return;
    }
    g_sobuddyFailMs = 0;
    g_sobuddySpReady = true;
    EnsureCtfScoreboardHintLayout();
    PrintOut(PRINT_LOG, "[minigames] registered strip/%s.sp (checksum name)\n", reg);
}

// Register sobuddy.sp off the tick path. GameDllLoaded tries once; client
// commands and layout console cmds retry after backoff (kSobuddyRetryMs).
void MgLazyEnsureSobuddySp() {
    if (!MgPlatformEnabled())
        return;
    if (g_sobuddySpReady && !SobuddyNeedsRegister())
        return;
    EnsureSobuddyStringPackage();
}

}  // namespace

void AppendCtfScoreboardHint(void* ent) {
    if (!ent || !g_sobuddySpReady || !g_ctfSbHintLayout[0])
        return;
    const int slot = MgSlotForEdict(ent);
    if (slot >= 1 && Profiles_SlotHideTips(slot - 1))
        return;
    Buddy_SP_PrintLayout(ent, kSobuddyLayoutRaw, g_ctfSbHintLayout);
}

const char* MinigameLayoutForSlot(int slot) {
    if (slot >= 1 && slot <= kMgMaxSlots && g_layoutCache[slot][0])
        return g_layoutCache[slot];
    return "";
}

void CopyGameId(char* dst, int cap, const char* gameId) {
    if (!dst || cap <= 0)
        return;
    dst[0] = '\0';
    if (!gameId || !gameId[0])
        return;
    std::strncpy(dst, gameId, static_cast<std::size_t>(cap) - 1);
    dst[cap - 1] = '\0';
}

bool DisplayOwnedBy(int slot, const char* gameId) {
    if (slot < 1 || slot > kMgMaxSlots || !gameId || !gameId[0])
        return false;
    return g_displayOwner[slot][0] && SameNoCase(g_displayOwner[slot], gameId);
}

void ClearDisplayOwner(int slot) {
    if (slot < 1 || slot > kMgMaxSlots)
        return;
    g_displayOwner[slot][0] = '\0';
    g_layoutPrimed[slot] = false;
}

int OwnerCountFor(const char* gameId) {
    if (!gameId || !gameId[0])
        return 0;
    int n = 0;
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (DisplayOwnedBy(s, gameId))
            ++n;
    }
    return n;
}

bool RunningSession(const char* gameId) {
    return OwnerCountFor(gameId) > 0;
}

void FireSessionEnd(const char* gameId, int slot1) {
    if (!gameId || !gameId[0])
        return;
    for (const MgGameOps* g : g_games) {
        if (g && g->command && g->onSessionEnd && SameNoCase(g->command, gameId))
            g->onSessionEnd(slot1);
    }
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

void ApplyView(void* ent, int slot, MgView view) {
    void* client = ClientForEnt(ent);
    if (!client || slot < 1 || slot > kMgMaxSlots)
        return;
    if (LevelIntermission() && view != MgView::StockScoreboard)
        view = MgView::StockScoreboard;
    const bool wasMinigame = g_page[slot] == MgView::Minigame;
    if (view != MgView::Minigame)
        g_minigameDeathView[slot] = false;
    if (view != MgView::Minigame)
        g_useNavigation[slot] = false;
    g_pendingInput[slot] = {};
    g_page[slot] = view;
    switch (view) {
    case MgView::Off:
        g_visible[slot] = false;
        g_layoutPrimed[slot] = false;
        if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 0;
        ApplyLayoutClient(ent, false);
        PushLayoutPayload(ent, "", true);
        break;
    case MgView::StockScoreboard:
        g_visible[slot] = false;
        if (Readable(static_cast<char*>(client) + kClientShowinventoryOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowinventoryOfs) = 0;
        if (Readable(static_cast<char*>(client) + kClientShowhelpTimeOfs, sizeof(float)))
            *reinterpret_cast<float*>(static_cast<char*>(client) + kClientShowhelpTimeOfs) = 0.0f;
        if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 1;
        ApplyLayoutClient(ent, true);
        PaintScoreboard(ent);
        break;
    case MgView::Minigame:
        g_visible[slot] = true;
        if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 0;
        ApplyLayoutClient(ent, true);
        if (!wasMinigame)
            menu_OnMinigameTabOpened(slot);
        if (!DisplayOwnedBy(slot, "mg"))
            SendMinigameLayout(ent, slot, MinigameLayoutForSlot(slot));
        break;
    }
}

void __cdecl HkCmd_Score_f(void* ent) {
    if (MgPlatformEnabled() && ent) {
        const int slot = MgSlotForEdict(ent);
        if (slot >= 1) {
            if (KeepMinigameOnDeath(ent, slot))
                return;
            if (ClientWantsStockScoreboard(ent)) {
                ApplyView(ent, slot, MgView::StockScoreboard);
                return;
            }
            const bool minigame = g_page[slot] == MgView::Minigame;
            const bool stock = !minigame && ClientShowscoresOpen(ent);
            const bool swap = Profiles_SlotSwapScoreboard(slot - 1);
            if (ClientUseHeld(ent)) {
                if (!swap) {
                    // +use + score: hidden/stock -> minigame, minigame -> hidden.
                    ApplyView(ent, slot, minigame ? MgView::Off : MgView::Minigame);
                } else {
                    // Swapped: +use + score toggles the vanilla board.
                    // Hidden -> stock, stock -> hidden, minigame -> stock.
                    ApplyView(ent, slot,
                              !minigame && stock ? MgView::Off : MgView::StockScoreboard);
                }
                CancelMenuUseOnScoreToggle(ent, slot);
                return;
            }
            if (!swap) {
                // Score alone opens stock score when hidden and changes minigame
                // view to stock score. An already-open stock score closes normally.
                ApplyView(ent, slot, minigame || !stock ? MgView::StockScoreboard
                                                         : MgView::Off);
                return;
            }
            // Swapped: score alone toggles the new board. Hidden/stock ->
            // minigame, minigame -> hidden.
            ApplyView(ent, slot, minigame ? MgView::Off : MgView::Minigame);
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

void PushLayoutPayload(void* ent, const char* layout, bool reset) {
    if (!ent || !Buddy_GetGameImport())
        return;
    if (layout && layout[0]) {
        UnicastLayout(ent, layout);
        return;
    }
    if (reset)
        Buddy_SP_Print(ent, kDmLayoutReset);
}

bool StockWouldSayAsChat(const char* cmd) {
    return MgClientCmdIsMinigameChatWord(cmd);
}

bool DispatchMinigameClientCmd(void* ent, const char* cmd) {
    for (const MgGameOps* g : g_games) {
        if (!g || !g->command || !g->onClientCmd || !MgClientCmdMatches(cmd, g->command))
            continue;
        if (LevelIntermission())
            return true;
        if (g->menuOnly)
            return true;
        MgLazyEnsureSobuddySp();
        const int slot = MgSlotForEdict(ent);
        if (slot >= 1 && !SameNoCase(g->command, "mg") && !Profiles_SlotActive(slot - 1)) {
            Buddy_ClientPrintf(ent, 2, "A profile is required for .%s\n", g->command);
            return true;
        }
        if (slot >= 1)
            g->onClientCmd(slot);
        else
            Buddy_ClientPrintf(ent, 2, "[minigames] cannot resolve player slot for '%s'\n",
                               g->command);
        return true;
    }
    return false;
}

static bool MgWordIsRegistered(const char* word) {
    for (const MgGameOps* g : g_games) {
        if (g && g->command && g->onClientCmd && MgClientCmdMatches(word, g->command))
            return true;
    }
    return false;
}

// .login <guid> / .logout player self-service (profiles feature). Typed in
// the client console, forwarded as say by sofplus/spsv. Always consumed when
// profiles is on — even usage errors — so the guid never leaks into chat.
static bool DispatchProfileLoginCmd(void* ent, const char* cmd, int splitN,
                                    const char* const* words) {
    if (!Profiles_IsEnabled())
        return false;
    const int slot = MgSlotForEdict(ent);
    if (splitN >= 1 && MgClientCmdMatches(words[0], "login")) {
        const char* guidArg = splitN >= 2 ? words[1] : "";
        Profiles_ClientLogin(ent, slot, guidArg ? guidArg : "");
        return true;
    }
    if (splitN >= 1 && MgClientCmdMatches(words[0], "logout")) {
        Profiles_ClientLogout(ent, slot);
        return true;
    }
    if (MgClientCmdMatches(cmd, "login")) {
        const char* guidArg = Buddy_ClientArgv(1);
        Profiles_ClientLogin(ent, slot, guidArg ? guidArg : "");
        return true;
    }
    if (MgClientCmdMatches(cmd, "logout")) {
        Profiles_ClientLogout(ent, slot);
        return true;
    }
    return false;
}

static bool MgTakeClientCommand(void* ent) {
    if (!MgPlatformEnabled() || !MgEnabled() || !ent)
        return false;
    g_argOverlay = false;
    const char* arg0 = Buddy_ClientArgv(0);
    const char* arg1 = Buddy_ClientArgv(1);
    char splitBuf[256];
    const char* words[8];
    int splitN = 0;
    // Say-box minigame words arrive as one argument. Split them before spsv
    // retokenizes and answers "unknown command".
    if (MgClientCmdIsSay(arg0) && Buddy_ClientArgc() == 2)
        splitN = MgSplitCmdWords(arg1, splitBuf, static_cast<int>(sizeof(splitBuf)), words, 8);
    menu_EnsureRegistered();
    lag_EnsureRegistered();
    ttt_EnsureRegistered();
    cvars_EnsureRegistered();
    cmds_EnsureRegistered();
    settings_EnsureRegistered();
    stats_EnsureRegistered();
    g_clientCmdArgvBase = MgClientCmdArgvBase(arg0, arg1);
    const char* cmd = Buddy_ClientArgv(g_clientCmdArgvBase);
    if (splitN >= 1 && MgWordIsRegistered(words[0])) {
        g_argOverlay = true;
        g_argOverlayArgc = splitN;
        for (int i = 0; i < splitN; ++i)
            g_argOverlayArgv[i] = words[i];
        g_clientCmdArgvBase = 0;
        cmd = words[0];
    }
    bool taken = false;
    // Admin-gated .upload_match / .cancel_upload (stats): runs before sofplus
    // so rejects never reach the scripted handler, while admin requests fall
    // through to it (Stats_Handle returns false) after recording the trust
    // signal. Handles both the say-box ("say .upload_match") and direct
    // console (".upload_match") forms via cmd + words.
    if (cmd || splitN >= 1) {
        const int statsSlot = MgSlotForEdict(ent);
        if (Stats_HandleClientCommand(ent, statsSlot, cmd, splitN, words)) {
            g_argOverlay = false;
            g_clientCmdArgvBase = 0;
            return true;
        }
        // Gamepark transfer observer: records `.rcon map|gamemap` intent for
        // privilege 0/1 slots. Never consumes — rcon auth and the map change
        // stay sofplus-side; the transfer fires only if the map loads.
        Gamepark_HandleClientCommand(ent, statsSlot, cmd, splitN, words);
    }
    if (cmd && cmd[0]) {
        if (DispatchMinigameClientCmd(ent, cmd))
            taken = true;
        else if (DispatchProfileLoginCmd(ent, cmd, splitN, words))
            taken = true;
        else if (StockWouldSayAsChat(cmd)) {
            Buddy_ClientPrintf(
                ent, 2,
                "[minigames] '%s' not registered (minigames off or gamex86.dll too old?)\n",
                cmd);
            taken = true;
        }
    }
    g_argOverlay = false;
    g_clientCmdArgvBase = 0;
    return taken;
}

void __cdecl HkClientCommand(void* ent) {
    if (MgTakeClientCommand(ent))
        return;
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

// spsv ClientCommand_HOOK @ RVA 0x11260 (my_SV_InitGameProgs writes it over ge+0x34
// after GetGameAPI). Dot-commands that are not sofplus functions hit SP_Print
// there and never call through. Detour the entry: mov eax, 0x100C is 5 bytes.
static void __cdecl HkSpsvClientCommand(void* ent) {
    if (MgTakeClientCommand(ent))
        return;
    if (g_spsvCmdTrampoline)
        reinterpret_cast<clientcmd_fn>(g_spsvCmdTrampoline)(ent);
}

static bool SpsvClientCommandPrologue(HMODULE mod) {
    if (!mod)
        return false;
    const auto* p = reinterpret_cast<const unsigned char*>(mod) + 0x11260;
    return p[0] == 0xB8 && p[1] == 0x0C && p[2] == 0x10 && p[3] == 0x00 && p[4] == 0x00;
}

static void MgEnsureClientCommandOuter() {
    if (g_spsvCmdTrampoline)
        return;
    HMODULE spsv = GetModuleHandleA("spsv.dll");
    if (!spsv)
        return;
    if (!SpsvClientCommandPrologue(spsv)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            PrintOut(PRINT_BAD, "[minigames] spsv ClientCommand prologue mismatch\n");
        }
        return;
    }
    void* target = reinterpret_cast<unsigned char*>(spsv) + 0x11260;
    g_spsvCmdTrampoline =
        DetourCreate(target, reinterpret_cast<void*>(&HkSpsvClientCommand), DETOUR_TYPE_JMP, 5);
    if (g_spsvCmdTrampoline)
        PrintOut(PRINT_LOG, "[minigames] ClientCommand ahead of spsv (.commands)\n");
    else
        PrintOut(PRINT_BAD, "[minigames] spsv ClientCommand detour failed\n");
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
        PrintOut(PRINT_LOG, "[minigames] Cmd_Score_f hooked (use+score=minigame toggle)\n");
    else
        PrintOut(PRINT_BAD, "[minigames] Cmd_Score_f hook failed\n");
}

void __thiscall HkDmctfScoreboard(void* thisp, void* ent, void* killer, int log_file) {
    if (auto original = reinterpret_cast<dmctf_scoreboard_fn>(g_dmctfScoreTrampoline))
        original(thisp, ent, killer, log_file);
    if (MgPlatformEnabled() && ent && !log_file)
        AppendCtfScoreboardHint(ent);
}

void InstallDmctfScoreboardHook() {
    if (g_dmctfScoreTrampoline)
        return;
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaDmctfScoreboard, 8))
        return;
    void* target = reinterpret_cast<char*>(h) + kRvaDmctfScoreboard;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(target, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & exec))
        return;
    g_dmctfScoreTrampoline =
        DetourCreate(target, reinterpret_cast<void*>(&HkDmctfScoreboard), DETOUR_TYPE_JMP, 6);
    if (g_dmctfScoreTrampoline)
        PrintOut(PRINT_LOG, "[minigames] dmctf scoreboard hooked (hotkey hint)\n");
    else
        PrintOut(PRINT_BAD, "[minigames] dmctf scoreboard hook failed\n");
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

int MgRegisteredGameCount() {
    int n = 0;
    for (const MgGameOps* g : g_games) {
        if (g && g->command && g->command[0])
            ++n;
    }
    return n;
}

const MgGameOps* RegisteredGame(int index) {
    int n = 0;
    for (const MgGameOps* g : g_games) {
        if (!g || !g->command || !g->command[0])
            continue;
        if (n == index)
            return g;
        ++n;
    }
    return nullptr;
}

const char* MgRegisteredGameCommand(int index) {
    const MgGameOps* g = RegisteredGame(index);
    return g ? g->command : nullptr;
}

const char* MgRegisteredGameDesc(int index) {
    const MgGameOps* g = RegisteredGame(index);
    return g ? g->desc : nullptr;
}

const char* MgRegisteredGameMenuName(int index) {
    const MgGameOps* g = RegisteredGame(index);
    return g ? g->menuName : nullptr;
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
    char* clients = SvClients();
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
    char* clients = SvClients();
    if (!client || !clients)
        return 0;
    const auto address = reinterpret_cast<std::uintptr_t>(client);
    const auto base = reinterpret_cast<std::uintptr_t>(clients);
    if (address < base)
        return 0;
    const auto off = address - base;
    if (off % kClientStride != 0)
        return 0;
    const int slot = static_cast<int>(off / kClientStride) + 1;
    return slot >= 1 && slot <= kMgMaxSlots ? slot : 0;
}

void* MgFastEdictFromClient(void* client) {
    return client ? *reinterpret_cast<void**>(static_cast<char*>(client) + kClientEdictOfs)
                  : nullptr;
}

void MgParseUserCmd(int slot1, void* cmd, MgUserCmdInput* in) {
    if (!in || slot1 < 1 || slot1 > kMgMaxSlots || !cmd)
        return;
    auto* u = static_cast<char*>(cmd);
    const short fwd = *reinterpret_cast<short*>(u + kMgUcmdOffForward);
    const short side = *reinterpret_cast<short*>(u + kMgUcmdOffSide);
    const signed char lean = static_cast<signed char>(u[kMgUcmdOffLean]);
    const unsigned char buttons = static_cast<unsigned char>(u[kMgUcmdOffButtons]);
    PrevUserCmd& prev = g_prevCmd[slot1];

    in->forward = fwd > kMgMoveDeadzone;
    in->back = fwd < -kMgMoveDeadzone;
    in->left = side < -kMgMoveDeadzone;
    in->right = side > kMgMoveDeadzone;
    in->leanLeft = lean > 0;
    in->leanRight = lean < 0;
    in->forwardEdge = in->forward && !(prev.forward > kMgMoveDeadzone);
    in->backEdge = in->back && !(prev.forward < -kMgMoveDeadzone);
    in->leftEdge = in->left && !(prev.side < -kMgMoveDeadzone);
    in->rightEdge = in->right && !(prev.side > kMgMoveDeadzone);
    in->leanLeftEdge = in->leanLeft && prev.lean >= 0;
    in->leanRightEdge = in->leanRight && prev.lean <= 0;
    const bool backDown = in->left || in->leanLeft;
    g_menuBackEdge[slot1] = backDown && !g_menuBackHeld[slot1];
    g_menuBackHeld[slot1] = backDown;
    in->attack = (buttons & kMgBtnAttack) != 0;
    in->attackEdge = in->attack && !(prev.buttons & kMgBtnAttack);
    in->use = (buttons & kMgBtnUse) != 0;
    in->useEdge = in->use && !(prev.buttons & kMgBtnUse);
    in->useRelease = !in->use && (prev.buttons & kMgBtnUse);

    prev.forward = fwd;
    prev.side = side;
    prev.lean = lean;
    prev.buttons = buttons;
}

unsigned char MgMenuInputMask(const MgUserCmdInput& in) {
    return static_cast<unsigned char>(
        (in.forward ? 1 : 0) | (in.back ? 2 : 0) | (in.left ? 4 : 0) |
        (in.right ? 8 : 0) | (in.leanLeft ? 16 : 0) | (in.leanRight ? 32 : 0));
}

void QueueInput(int slot1, const MgUserCmdInput& in) {
    if (slot1 < 1 || slot1 > kMgMaxSlots ||
        !(in.forwardEdge || in.backEdge || in.leftEdge || in.rightEdge ||
          in.leanLeftEdge || in.leanRightEdge || in.attackEdge || in.useEdge ||
          in.useRelease || g_menuBackEdge[slot1]))
        return;
    PendingInput& pending = g_pendingInput[slot1];
    pending.valid = true;
    pending.value.forward = in.forward;
    pending.value.back = in.back;
    pending.value.left = in.left;
    pending.value.right = in.right;
    pending.value.leanLeft = in.leanLeft;
    pending.value.leanRight = in.leanRight;
    pending.value.attack = in.attack;
    pending.value.use = pending.value.use || in.use;
    if (in.forwardEdge && pending.forwardPresses < kMaxPendingNavPresses)
        ++pending.forwardPresses;
    if (in.backEdge && pending.backPresses < kMaxPendingNavPresses)
        ++pending.backPresses;
    if (in.use && g_menuBackEdge[slot1] &&
        pending.menuBackPresses < kMaxPendingNavPresses)
        ++pending.menuBackPresses;
    pending.value.forwardEdge = pending.value.forwardEdge || in.forwardEdge;
    pending.value.backEdge = pending.value.backEdge || in.backEdge;
    pending.value.leftEdge =
        pending.value.leftEdge || (in.use && g_menuBackEdge[slot1] && in.leftEdge);
    pending.value.rightEdge = pending.value.rightEdge || in.rightEdge;
    pending.value.leanLeftEdge =
        pending.value.leanLeftEdge || (in.use && g_menuBackEdge[slot1] && in.leanLeftEdge);
    pending.value.leanRightEdge = pending.value.leanRightEdge || in.leanRightEdge;
    pending.value.attackEdge = pending.value.attackEdge || in.attackEdge;
    pending.value.useEdge = pending.value.useEdge || in.useEdge;
    pending.value.useRelease = pending.value.useRelease || in.useRelease;
}

// SV_ClientThink stays a sanitizer/latch; game callbacks and layout writes run
// once per tick, before CL_SendClientMessages.
void ProcessPendingInputs() {
    if (!MgPlatformEnabled())
        return;
    if (LevelIntermission()) {
        for (int slot = 1; slot <= kMgMaxSlots; ++slot) {
            g_pendingInput[slot] = {};
            g_useNavigation[slot] = false;
            g_menuBackHeld[slot] = false;
            g_menuBackEdge[slot] = false;
        }
        return;
    }
    for (int slot = 1; slot <= kMgMaxSlots; ++slot) {
        PendingInput pending = g_pendingInput[slot];
        g_pendingInput[slot] = {};
        if (!pending.valid)
            continue;
        if (!MgMinigameTabOpen(slot)) {
            if (pending.value.useRelease) {
                g_useNavigation[slot] = false;
            }
            continue;
        }
        MgUserCmdInput in = pending.value;
        auto handleMenuBack = [&]() {
            bool handled = false;
            for (const MgGameOps* g : g_games) {
                if (!g || !g->onBack || !g->command ||
                    !DisplayOwnedBy(slot, g->command) || !RunningSession(g->command))
                    continue;
                if (g->onBack(slot)) {
                    handled = true;
                    break;
                }
            }
            return handled || MgNavigateBack(slot);
        };
        const bool menuBack = in.use && pending.menuBackPresses;
        if (menuBack) {
            for (unsigned char i = 0; i < pending.menuBackPresses; ++i) {
                handleMenuBack();
                g_useNavigation[slot] = true;
            }
        }
        if (in.use && (pending.forwardPresses || pending.backPresses))
            g_useNavigation[slot] = true;
        const bool cancelUse = in.useRelease && g_useNavigation[slot];
        if (in.useRelease) {
            g_useNavigation[slot] = false;
        }
        if (cancelUse)
            in.useRelease = false;
        if (menuBack)
            continue;
        // Deliver this input to whoever owned the display when it was queued.
        // A forward that re-arms mid-dispatch must not re-fire the same tap on
        // the new page (the scoreboard footer tap used to cascade straight
        // through the menu list into its first row, and entry taps spuriously
        // turned ranks/pages on the destination page).
        char dispatchOwner[kMgGameIdLen];
        CopyGameId(dispatchOwner, kMgGameIdLen, g_displayOwner[slot]);
        auto dispatch = [&](MgUserCmdInput& event) {
            for (const MgGameOps* g : g_games) {
                if (!g || !g->onUserCmd || !g->command ||
                    !SameNoCase(g->command, dispatchOwner) ||
                    !DisplayOwnedBy(slot, g->command) || !RunningSession(g->command))
                    continue;
                g->onUserCmd(slot, &event);
            }
        };
        if (pending.forwardPresses || pending.backPresses) {
            MgUserCmdInput step = in;
            step.forwardEdge = false;
            step.backEdge = false;
            step.leftEdge = false;
            step.rightEdge = false;
            step.leanLeftEdge = false;
            step.leanRightEdge = false;
            step.attackEdge = false;
            step.useEdge = false;
            step.useRelease = false;
            for (unsigned char i = 0; i < pending.forwardPresses; ++i) {
                step.forwardEdge = true;
                dispatch(step);
            }
            for (unsigned char i = 0; i < pending.backPresses; ++i) {
                step.forwardEdge = false;
                step.backEdge = true;
                dispatch(step);
            }
            in.forwardEdge = false;
            in.backEdge = false;
        }
        dispatch(in);
    }
}

bool TttInputActive(int slot1) {
    return DisplayOwnedBy(slot1, "mg_ttt") && RunningSession("mg_ttt");
}

void MgStripUserCmd(void* cmd) {
    if (!cmd)
        return;
    auto* u = static_cast<char*>(cmd);
    *reinterpret_cast<short*>(u + kMgUcmdOffForward) = 0;
    *reinterpret_cast<short*>(u + kMgUcmdOffSide) = 0;
    *reinterpret_cast<short*>(u + kMgUcmdOffUp) = 0;
    u[kMgUcmdOffButtons] =
        static_cast<char>(static_cast<unsigned char>(u[kMgUcmdOffButtons]) & ~(kMgBtnAttack | kMgBtnUse));
}

void MgStripMenuUserCmd(void* cmd) {
    if (!cmd)
        return;
    auto* u = static_cast<char*>(cmd);
    *reinterpret_cast<short*>(u + kMgUcmdOffForward) = 0;
    *reinterpret_cast<short*>(u + kMgUcmdOffSide) = 0;
    *reinterpret_cast<short*>(u + kMgUcmdOffUp) = 0;
    u[kMgUcmdOffLean] = 0;
    u[kMgUcmdOffButtons] =
        static_cast<char>(static_cast<unsigned char>(u[kMgUcmdOffButtons]) & ~kMgBtnAttack);
}

int MgArgc() {
    if (g_argOverlay)
        return g_argOverlayArgc;
    const int c = Buddy_ClientArgc();
    return c > g_clientCmdArgvBase ? c - g_clientCmdArgvBase : 0;
}

const char* MgArgv(int n) {
    if (n < 0)
        return "";
    if (g_argOverlay)
        return n < g_argOverlayArgc ? g_argOverlayArgv[n] : "";
    return Buddy_ClientArgv(n + g_clientCmdArgvBase);
}

bool MgEnabled() {
    return MgPlatformEnabled();
}

static bool MgGameLevel0Only(const char* gameId) {
    return gameId && (SameNoCase(gameId, "mg_cmds") || SameNoCase(gameId, "mg_cvars") ||
                      SameNoCase(gameId, "mg_lag"));
}

// Server admins plus map admins (privilege 0 or 1): the upload-match
// action row. The stats detail page itself is open to every tier.
static bool MgGameLevel01Only(const char* gameId) {
    return gameId && SameNoCase(gameId, "mg_upload");
}

bool MgGameNeedsProfile(const char* gameId) {
    return gameId && gameId[0] && !SameNoCase(gameId, "mg") &&
           !SameNoCase(gameId, "mg_menu") && !SameNoCase(gameId, kMgScriptGameId);
}

const MgGameOps* FindGame(const char* gameId) {
    if (!gameId || !gameId[0])
        return nullptr;
    for (const MgGameOps* g : g_games) {
        if (g && g->command && SameNoCase(g->command, gameId))
            return g;
    }
    return nullptr;
}

bool InvokeGameNoArgs(int slot1, const char* gameId) {
    if (LevelIntermission())
        return false;
    const MgGameOps* game = FindGame(gameId);
    if (!game || !game->onClientCmd || slot1 < 1 || slot1 > kMgMaxSlots)
        return false;
    if (MgGameLevel0Only(game->command) && !Profiles_IsLevel0Admin(slot1 - 1))
        return false;
    if (MgGameLevel01Only(game->command) && !Profiles_IsLevel01Admin(slot1 - 1))
        return false;
    if (MgGameNeedsProfile(game->command) && !Profiles_SlotActive(slot1 - 1))
        return false;
    MgLazyEnsureSobuddySp();
    g_argOverlay = true;
    g_argOverlayArgc = 1;
    g_argOverlayArgv[0] = game->command;
    g_clientCmdArgvBase = 0;
    game->onClientCmd(slot1);
    g_argOverlay = false;
    g_clientCmdArgvBase = 0;
    return true;
}

void MgNavigationReset(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_navDepth[slot1] = 0;
    g_pendingInput[slot1] = {};
}

bool MgNavigateForward(int slot1, const char* gameId) {
    if (LevelIntermission() || !MgMinigameTabOpen(slot1) || !gameId || !gameId[0] ||
        !g_displayOwner[slot1][0] || SameNoCase(g_displayOwner[slot1], gameId) ||
        !FindGame(gameId) || g_navDepth[slot1] >= kMgNavDepth)
        return false;
    if (MgGameLevel0Only(gameId) && !Profiles_IsLevel0Admin(slot1 - 1))
        return false;
    if (MgGameLevel01Only(gameId) && !Profiles_IsLevel01Admin(slot1 - 1))
        return false;
    if (MgGameNeedsProfile(gameId) && !Profiles_SlotActive(slot1 - 1))
        return false;
    // Remember where the current page's cursor was, so popping back restores
    // the menu position instead of resetting to the top.
    int selected = -1;
    if (const MgGameOps* cur = FindGame(g_displayOwner[slot1]))
        if (cur->saveSelected)
            selected = cur->saveSelected(slot1);
    MgNavEntry& entry = g_navStack[slot1][g_navDepth[slot1]++];
    CopyGameId(entry.game, kMgGameIdLen, g_displayOwner[slot1]);
    entry.selected = selected;
    if (InvokeGameNoArgs(slot1, gameId) && DisplayOwnedBy(slot1, gameId))
        return true;
    --g_navDepth[slot1];
    return false;
}

bool MgNavigateBack(int slot1) {
    if (LevelIntermission() || !MgMinigameTabOpen(slot1))
        return false;
    // Pop toward the mg scoreboard root. The root is unpoppable (back there
    // is a no-op), and every exit lands on a shown page: a refused page
    // falls home instead of stranding a blank, closed view.
    for (int guard = 0; guard <= kMgNavDepth; ++guard) {
        if (!g_displayOwner[slot1][0] || !MgMinigameTabOpen(slot1)) {
            menu_OpenFromNavigation(slot1);
            return true;
        }
        char current[kMgGameIdLen];
        CopyGameId(current, kMgGameIdLen, g_displayOwner[slot1]);
        if (g_navDepth[slot1] <= 0) {
            if (SameNoCase(current, "mg"))
                return true;  // already home; mg is never popped off
            FireSessionEnd(current, slot1);
            MgClearLayout(slot1, current);
            menu_OpenFromNavigation(slot1);
            return true;
        }
        char previous[kMgGameIdLen] = {};
        int selected = -1;
        {
            MgNavEntry& entry = g_navStack[slot1][--g_navDepth[slot1]];
            CopyGameId(previous, kMgGameIdLen, entry.game);
            selected = entry.selected;
            entry.selected = -1;
        }
        if (!SameNoCase(current, "mg")) {
            FireSessionEnd(current, slot1);
            MgClearLayout(slot1, current);
        }
        if (!previous[0] || SameNoCase(previous, "mg")) {
            menu_OpenFromNavigation(slot1);
            return true;
        }
        if (SameNoCase(previous, "mg_menu")) {
            menu_OpenMenuFromNavigation(slot1, selected);
            return true;
        }
        if (InvokeGameNoArgs(slot1, previous) && DisplayOwnedBy(slot1, previous)) {
            MgReshowCurrentLayout(slot1);
            return true;
        }
        // Refused (lost privilege/profile): owner is empty now; loop falls
        // home to the scoreboard instead of leaving a blank view.
    }
    menu_OpenFromNavigation(slot1);
    return true;
}

bool MgTakeDisplay(int slot1, const char* gameId) {
    if (LevelIntermission() || slot1 < 1 || slot1 > kMgMaxSlots || !gameId || !gameId[0])
        return false;
    if (MgGameLevel0Only(gameId) && !Profiles_IsLevel0Admin(slot1 - 1))
        return false;
    if (MgGameLevel01Only(gameId) && !Profiles_IsLevel01Admin(slot1 - 1))
        return false;
    if (MgGameNeedsProfile(gameId) && !Profiles_SlotActive(slot1 - 1))
        return false;
    char prev[kMgGameIdLen];
    CopyGameId(prev, kMgGameIdLen, g_displayOwner[slot1]);
    if (prev[0] && !SameNoCase(prev, gameId)) {
        FireSessionEnd(prev, slot1);
        g_layoutCache[slot1][0] = '\0';
        g_layoutDirty[slot1] = false;
    }
    CopyGameId(g_displayOwner[slot1], kMgGameIdLen, gameId);
    return true;
}

void MgReleaseDisplay(int slot1, const char* gameId) {
    if (!DisplayOwnedBy(slot1, gameId))
        return;
    ClearDisplayOwner(slot1);
}

bool MgDisplayOwnedBy(int slot1, const char* gameId) {
    return DisplayOwnedBy(slot1, gameId);
}

bool MgDisplayTakenByOther(int slot1, const char* gameId) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !gameId || !gameId[0])
        return false;
    return g_displayOwner[slot1][0] && !SameNoCase(g_displayOwner[slot1], gameId);
}

bool MgRunningSession(const char* gameId) {
    return RunningSession(gameId);
}

bool MgMinigameTabOpen(int slot1) {
    return slot1 >= 1 && slot1 <= kMgMaxSlots && g_page[slot1] == MgView::Minigame;
}

bool LayoutCacheStore(int slot1, const char* text) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !text)
        return false;
    if (std::strncmp(g_layoutCache[slot1], text, kMgLayoutCap) == 0)
        return false;
    std::strncpy(g_layoutCache[slot1], text, kMgLayoutCap);
    g_layoutCache[slot1][kMgLayoutCap - 1] = '\0';
    g_layoutDirty[slot1] = true;
    return true;
}

void MgPutLayoutCache(int slot1, const char* gameId, const MgCanvas& canvas) {
    if (!DisplayOwnedBy(slot1, gameId) || !RunningSession(gameId))
        return;
    LayoutCacheStore(slot1, canvas.text);
}

void MgShowLayout(int slot1, const char* gameId, bool on) {
    if (on && LevelIntermission())
        return;
    if (on) {
        if (!MgTakeDisplay(slot1, gameId))
            return;
    } else if (DisplayOwnedBy(slot1, gameId))
        MgReleaseDisplay(slot1, gameId);
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        g_visible[slot1] = on;
    void* ent = MgEdictForSlot(slot1);
    if (!ent)
        return;
    if (on)
        ApplyView(ent, slot1, MgView::Minigame);
    else
        ApplyView(ent, slot1, MgView::Off);
}

void MgReshowCurrentLayout(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_displayOwner[slot1][0] ||
        LevelIntermission())
        return;
    void* ent = MgEdictForSlot(slot1);
    if (!ent)
        return;
    // Same visible state ApplyView(Minigame) establishes, minus the
    // menu_OnMinigameTabOpened callback (stack returns must not reset the
    // nav stack or re-arm the mg root).
    g_visible[slot1] = true;
    ApplyLayoutClient(ent, true);
    g_page[slot1] = MgView::Minigame;
    const char* layout = MinigameLayoutForSlot(slot1);
    if (layout && layout[0])
        SendMinigameLayout(ent, slot1, layout);
}

void MgPushLayout(int slot1, const char* gameId, const MgCanvas& canvas) {
    if (LevelIntermission() || !DisplayOwnedBy(slot1, gameId) || !RunningSession(gameId))
        return;
    void* ent = MgEdictForSlot(slot1);
    if (!ent) {
        Buddy_DebugPrintf("[minigames] push layout: slot %d has no edict\n", slot1);
        return;
    }
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        LayoutCacheStore(slot1, canvas.text);
    const char* layout = MinigameLayoutForSlot(slot1);
    if (!layout[0])
        return;
    if (g_visible[slot1] && g_page[slot1] == MgView::Minigame)
        SendMinigameLayout(ent, slot1, layout);
    else if (g_visible[slot1] && g_page[slot1] == MgView::Off)
        ApplyView(ent, slot1, MgView::Minigame);
    else if (!g_visible[slot1])
        PushLayoutPayload(ent, layout, !g_layoutPrimed[slot1]);
}

void MgClearLayout(int slot1, const char* gameId) {
    if (!DisplayOwnedBy(slot1, gameId))
        return;
    MgReleaseDisplay(slot1, gameId);
    g_layoutCache[slot1][0] = '\0';
    g_layoutDirty[slot1] = false;
    g_visible[slot1] = false;
    g_page[slot1] = MgView::Off;
    if (void* ent = MgEdictForSlot(slot1))
        ApplyView(ent, slot1, MgView::Off);
}

void MgShowIdleLayout(int slot1, const char* gameId) {
    if (LevelIntermission() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    if (!DisplayOwnedBy(slot1, gameId))
        return;
    MgReleaseDisplay(slot1, gameId);
    g_layoutCache[slot1][0] = '\0';
    g_visible[slot1] = true;
    g_page[slot1] = MgView::Minigame;
    if (void* ent = MgEdictForSlot(slot1))
        ApplyView(ent, slot1, MgView::Minigame);
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
    MgLazyEnsureSobuddySp();
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
    MgTakeDisplay(internal, kMgScriptGameId);
    MgPushLayout(internal, kMgScriptGameId, c);
    MgShowLayout(internal, kMgScriptGameId, true);
}

extern "C" void __cdecl mg_Show_f() {
    if (!MgPlatformEnabled())
        return;
    MgLazyEnsureSobuddySp();
    const int slot = std::atoi(Buddy_ClientArgv(1));
    const int on = std::atoi(Buddy_ClientArgv(2));
    const int internal = MgUserToInternal(slot);
    if (!internal || Buddy_ClientArgc() < 3) {
        Buddy_DebugPrintf("usage: mg_show <slot 0-based> <0|1>\n");
        return;
    }
    if (on)
        MgTakeDisplay(internal, kMgScriptGameId);
    MgShowLayout(internal, kMgScriptGameId, on != 0);
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
    MgClearLayout(internal, kMgScriptGameId);
}

extern "C" void __cdecl mg_Idle_f() {
    if (!MgPlatformEnabled())
        return;
    MgLazyEnsureSobuddySp();
    const int internal = MgUserToInternal(std::atoi(Buddy_ClientArgv(1)));
    if (!internal) {
        Buddy_DebugPrintf("usage: mg_idle <slot 0-based>\n");
        return;
    }
    MgShowIdleLayout(internal, kMgScriptGameId);
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
    MgLazyEnsureSobuddySp();
    const int internal = MgUserToInternal(std::atoi(Buddy_ClientArgv(1)));
    if (!internal || !MgSlotSpawned(internal)) {
        Buddy_DebugPrintf("usage: mg_test <slot 0-based>\n");
        return;
    }
    MgTakeDisplay(internal, kMgScriptGameId);
    MgShowLayout(internal, kMgScriptGameId, true);
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
    menu_EnsureRegistered();
    lag_EnsureRegistered();
    ttt_EnsureRegistered();
    cvars_EnsureRegistered();
    cmds_EnsureRegistered();
    settings_EnsureRegistered();
    stats_EnsureRegistered();
    InstallCmdScoreHook();
    InstallDmctfScoreboardHook();
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
    if (KeepMinigameOnDeath(ent, slot))
        return;
    if (LevelIntermission() || ClientWantsStockScoreboard(ent)) {
        EnsureScoreboardPage(ent, slot);
        return;
    }
    if (g_page[slot] == MgView::StockScoreboard) {
        void* client = ClientForEnt(ent);
        const bool scores =
            client && Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)) &&
            *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) != 0;
        if (!scores) {
            ApplyView(ent, slot, MgView::Off);
            return;
        }
        ApplyLayoutClient(ent, true);
        if (MgMinigameLayoutRefreshDue())
            PaintScoreboard(ent);
        return;
    }
    if (g_page[slot] == MgView::Minigame) {
        if (void* client = ClientForEnt(ent))
            SuppressStockLayoutRefresh(client);
        ApplyLayoutClient(ent, true);
        if (!Profiles_SlotActive(slot - 1)) {
            lag_MaintainForSlot(slot);
            cvars_MaintainForSlot(slot);
            cmds_MaintainForSlot(slot);
            stats_MaintainForSlot(slot);
            if (MgGameNeedsProfile(g_displayOwner[slot])) {
                char id[kMgGameIdLen];
                CopyGameId(id, kMgGameIdLen, g_displayOwner[slot]);
                MgReleaseDisplay(slot, id);
            }
            if (!DisplayOwnedBy(slot, "mg") && !DisplayOwnedBy(slot, "mg_menu"))
                menu_OnMinigameTabOpened(slot);
        }
        if (MgMinigameLayoutRefreshDue()) {
            lag_MaintainForSlot(slot);
            cvars_MaintainForSlot(slot);
            cmds_MaintainForSlot(slot);
            menu_MaintainForSlot(slot);
            settings_MaintainForSlot(slot);
            stats_MaintainForSlot(slot);
            if (g_layoutDirty[slot]) {
                const char* layout = MinigameLayoutForSlot(slot);
                if (layout[0])
                    SendMinigameLayout(ent, slot, layout);
            }
        }
    }
}

// Map change clears the client scoreboard but not these per-slot view flags.
void ForgetScoreView(int slot) {
    if (slot < 1 || slot > kMgMaxSlots)
        return;
    g_pendingInput[slot] = {};
    g_useNavigation[slot] = false;
    g_menuBackHeld[slot] = false;
    g_menuBackEdge[slot] = false;
    g_page[slot] = MgView::Off;
    g_minigameDeathView[slot] = false;
    g_visible[slot] = false;
    g_layoutPrimed[slot] = false;
    g_layoutDirty[slot] = false;
    g_layoutCache[slot][0] = '\0';
    void* ent = MgEdictForSlot(slot);
    void* client = ent ? ClientForEnt(ent) : nullptr;
    if (!client)
        return;
    if (Readable(static_cast<char*>(client) + kClientShowscoresOfs, sizeof(int)))
        *reinterpret_cast<int*>(static_cast<char*>(client) + kClientShowscoresOfs) = 0;
    PokeLayoutBit(client, false);
}

char g_scoreMapCs[64] = {};
bool g_scoreMapFresh = false;

void SyncScoreMap() {
    const char* cs = MgMapChecksum();
    if (!cs || !cs[0] || std::strcmp(g_scoreMapCs, cs) == 0)
        return;
    std::strncpy(g_scoreMapCs, cs, sizeof(g_scoreMapCs) - 1);
    g_scoreMapCs[sizeof(g_scoreMapCs) - 1] = '\0';
    g_scoreMapFresh = true;
    const int n = MgMaxClients();
    for (int slot = 1; slot <= n && slot <= kMgMaxSlots; ++slot)
        ForgetScoreView(slot);
}

void mg_ClientEndServerFramePre(void*& ent) {
    SyncScoreMap();
    if (!MgPlatformEnabled() || !ent)
        return;
    const int slot = MgSlotForEdict(ent);
    if (slot < 1)
        return;
    if (KeepMinigameOnDeath(ent, slot))
        return;
    if (!g_scoreMapFresh && (LevelIntermission() || ClientWantsStockScoreboard(ent))) {
        EnsureScoreboardPage(ent, slot);
        return;
    }
    if (g_page[slot] != MgView::Minigame)
        return;
    if (void* client = ClientForEnt(ent))
        SuppressStockLayoutRefresh(client);
}

void mg_ClientEndServerFramePost(void* ent) {
    if (!MgPlatformEnabled() || !ent)
        return;
    const int slot = MgSlotForEdict(ent);
    if (slot < 1 || g_page[slot] == MgView::Off)
        return;
    if (!MgSlotSpawned(slot)) {
        if (g_page[slot] == MgView::Minigame) {
            KeepMinigameOnDeath(ent, slot);
            return;
        }
        ForgetScoreView(slot);
        return;
    }
    MaintainLayoutClient(ent, slot);
}

void mg_CL_SendClientMessagesPre() {
    if (!MgPlatformEnabled() || g_scoreMapFresh)
        return;
    ProcessPendingInputs();
    const int n = MgMaxClients();
    for (int slot = 1; slot <= n; ++slot) {
        if (g_page[slot] == MgView::Off)
            continue;
        if (void* ent = MgEdictForSlot(slot)) {
            if (KeepMinigameOnDeath(ent, slot))
                continue;
            if (LevelIntermission() || ClientWantsStockScoreboard(ent)) {
                EnsureScoreboardPage(ent, slot);
            } else {
                ApplyLayoutClient(ent, true);
            }
        }
    }
}

void mg_SvFramePost(int msec) {
    (void)msec;
    SyncScoreMap();
    g_scoreMapFresh = false;
    MgEnsureClientCommandOuter();
    if (!MgPlatformEnabled())
        return;
    // After CL_SendClientMessages (SV_Frame Post). Never SP_Register from
    // scoreboard hooks — that runs inside multicast buildup and can SZ_GetSpace.
    if (!g_sobuddySpReady || SobuddyNeedsRegister())
        MgLazyEnsureSobuddySp();
}

void mg_SvClientThinkPre(void*& client, void*& cmd) {
    // Engine-owned pointers are valid here. Keep this hook limited to edge
    // capture and command filtering; mutations belong in ProcessPendingInputs.
    g_think = {client, cmd, 0, false};
    if (!MgPlatformEnabled() || !client || !cmd)
        return;
    const int slot = MgSlotFromClient(client);
    if (slot < 1)
        return;
    g_think.slot = slot;
    g_think.active = true;
    MgUserCmdInput in = {};
    MgParseUserCmd(slot, cmd, &in);
    if (!MgMinigameTabOpen(slot)) {
        const unsigned char held = MgMenuInputMask(in);
        if (g_menuInputLatch[slot] & held) {
            MgStripMenuUserCmd(cmd);
            return;
        }
        if (!held)
            g_menuInputLatch[slot] = 0;
        else
            g_menuInputLatch[slot] &= held;
        g_prevCmd[slot] = {};
        return;
    }
    g_menuInputLatch[slot] = 0;
    QueueInput(slot, in);
    if (in.use)
        MgStripMenuUserCmd(cmd);
    else if (TttInputActive(slot))
        MgStripUserCmd(cmd);
}

void mg_SvClientThinkPost(void* client, void* cmd) {
    if (!g_think.active || g_think.client != client || g_think.cmd != cmd)
        return;
    const int slot = g_think.slot;
    g_think.active = false;
    if (slot < 1 || (!MgMinigameTabOpen(slot) && !g_menuInputLatch[slot]))
        return;
    auto* u = static_cast<char*>(cmd);
    if (MgMinigameTabOpen(slot) &&
        !(static_cast<unsigned char>(u[kMgUcmdOffButtons]) & kMgBtnUse))
        return;
    if (void* ent = MgFastEdictFromClient(client)) {
        auto* viewside = reinterpret_cast<int*>(static_cast<char*>(ent) + kEdictViewsideOfs);
        *viewside = 0;
    }
}

extern "C" void Minigames_Shutdown() {
    if (g_originalClientCommand && g_geClientCommandSlot &&
        Readable(g_geClientCommandSlot, sizeof(void*))) {
        *reinterpret_cast<void**>(g_geClientCommandSlot) =
            reinterpret_cast<void*>(g_originalClientCommand);
    }
    g_originalClientCommand = nullptr;
    g_geClientCommandSlot = nullptr;
    if (g_spsvCmdTrampoline) {
        DetourRemove(&g_spsvCmdTrampoline);
        g_spsvCmdTrampoline = nullptr;
    }
    if (g_scoreTrampoline) {
        DetourRemove(&g_scoreTrampoline);
        g_scoreTrampoline = nullptr;
    }
    if (g_dmctfScoreTrampoline) {
        DetourRemove(&g_dmctfScoreTrampoline);
        g_dmctfScoreTrampoline = nullptr;
    }
    g_edicts = nullptr;
    g_edictSize = 0;
    g_svClients = nullptr;
    g_think = {};
    for (bool& v : g_visible)
        v = false;
    for (char* row : g_layoutCache)
        row[0] = '\0';
    for (bool& d : g_layoutDirty)
        d = false;
    for (bool& p : g_layoutPrimed)
        p = false;
    for (MgView& p : g_page)
        p = MgView::Off;
    for (bool& d : g_minigameDeathView)
        d = false;
    for (PendingInput& p : g_pendingInput)
        p = {};
    for (bool& n : g_useNavigation)
        n = false;
    for (bool& h : g_menuBackHeld)
        h = false;
    for (bool& e : g_menuBackEdge)
        e = false;
    g_scoreMapCs[0] = '\0';
    g_scoreMapFresh = false;
    for (char* row : g_displayOwner)
        row[0] = '\0';
    g_ctfSbHintLayout[0] = '\0';
    g_sobuddySpReady = false;
    g_sobuddyRegisterName[0] = '\0';
}
