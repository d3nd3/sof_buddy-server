// commands: sof_buddy server-command catalog on the minigames layout tab.
//
// The menu-only command page owns the +use+score view; server console arms
// it via cmds_show and dumps it via cmds_list. Static paged catalog lives in
// commands_logic.h. No _sofbuddy_ internal cvars here by policy: per-slot
// page/armed state is in-memory. If a cvar were ever needed for plumbing it
// would use the _sb_internal_ prefix, never _sofbuddy_.

#include "cvar.h"
#include "commands.h"
#include "commands_logic.h"

#include "buddy_import.h"
#include "log.h"
#include "../../profiles/cvar.h"
#include "../../cpu_optimizations/cmd_cost/cmd_cost.h"
#include "../minigames_api.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr char kCmdsGameId[] = "mg_cmds";
constexpr char kPlayerCmdsGameId[] = "mg_playercmds";
constexpr int kPlayerCmdsPerPage = 8;
constexpr int kPlayerCmdsMax = 256;

bool g_registered = false;
bool g_playerRegistered = false;
bool g_armed[kMgMaxSlots + 1] = {};
int g_category[kMgMaxSlots + 1] = {};
int g_page[kMgMaxSlots + 1] = {};
int g_pageSelection[kMgMaxSlots + 1] = {};
bool g_playerArmed[kMgMaxSlots + 1] = {};
int g_playerPage[kMgMaxSlots + 1] = {};
int g_playerPageSelection[kMgMaxSlots + 1] = {};

void Paint(int slot1, bool push) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    const bool showTips = !Profiles_SlotHideTips(slot1 - 1);
    if (g_category[slot1] < 0)
        CmdsRenderCategories(c, g_pageSelection[slot1], showTips);
    else
        CmdsRender(g_category[slot1], g_page[slot1], c, g_pageSelection[slot1], showTips);
    MgPutLayoutCache(slot1, kCmdsGameId, c);
    if (push && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kCmdsGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        g_category[slot1] = -1;
        g_pageSelection[slot1] = 0;
        g_page[slot1] = CmdsClampPage(g_page[slot1]);
        MgTakeDisplay(slot1, kCmdsGameId);
        Paint(slot1, true);
        return;
    }
    if (!MgDisplayOwnedBy(slot1, kCmdsGameId))
        return;
    MgClearLayout(slot1, kCmdsGameId);
}

void TurnPage(int slot1, int delta) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1] || g_category[slot1] < 0)
        return;
    g_page[slot1] = CmdsClampCategoryPage(
        g_category[slot1], g_page[slot1] + delta);
    Paint(slot1, true);
}

void MovePageSelection(int slot1, int delta) {
    int& selected = g_pageSelection[slot1];
    if (selected < 0)
        selected = delta < 0 ? kCmdsActionCount - 1 : 0;
    else
        selected = (selected + delta + kCmdsActionCount) % kCmdsActionCount;
    Paint(slot1, true);
}

void MoveCategorySelection(int slot1, int delta) {
    int& selected = g_pageSelection[slot1];
    if (selected < 0)
        selected = delta < 0 ? kCmdsCategoryCount - 1 : 0;
    else
        selected = (selected + delta + kCmdsCategoryCount) % kCmdsCategoryCount;
    Paint(slot1, true);
}

void SelectCategory(int slot1) {
    const int selected = g_pageSelection[slot1];
    if (selected < 0 || selected >= kCmdsCategoryCount)
        return;
    g_category[slot1] = selected;
    g_page[slot1] = 0;
    g_pageSelection[slot1] = 0;
    Paint(slot1, true);
}

void OnUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || slot1 < 1 || slot1 > kMgMaxSlots ||
        !g_armed[slot1] || !MgMinigameTabOpen(slot1) ||
        !MgDisplayOwnedBy(slot1, kCmdsGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    if (in->use && in->forwardEdge) {
        if (g_category[slot1] < 0)
            MoveCategorySelection(slot1, -1);
        else
            MovePageSelection(slot1, -1);
    } else if (in->use && in->backEdge) {
        if (g_category[slot1] < 0)
            MoveCategorySelection(slot1, 1);
        else
            MovePageSelection(slot1, 1);
    } else if (in->useRelease) {
        if (g_category[slot1] < 0) {
            if (g_pageSelection[slot1] < 0)
                MoveCategorySelection(slot1, 1);
            else
                SelectCategory(slot1);
        } else if (g_pageSelection[slot1] < 0) {
            MovePageSelection(slot1, 1);
        } else {
            TurnPage(slot1, g_pageSelection[slot1] == 0 ? 1 : -1);
        }
    }
}

bool OnBack(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1] ||
        g_category[slot1] < 0 || !MgDisplayOwnedBy(slot1, kCmdsGameId))
        return false;
    // Back out of the category page: the cursor returns to the category just
    // left instead of resetting to the top.
    const int backTo = g_category[slot1];
    g_category[slot1] = -1;
    g_pageSelection[slot1] =
        (backTo >= 0 && backTo < kCmdsCategoryCount) ? backTo : 0;
    Paint(slot1, true);
    return true;
}

#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
using PlayerCmdRow = cmdcost::UserCmdInfo;
#else
struct PlayerCmdRow {
    char name[64];
    char params[96];
};
#endif

void CleanField(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0)
        return;
    int i = 0;
    for (; src && src[i] && i < cap - 1; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[i] = c < 32 || c == '"' || c == '\\' ? ' ' : static_cast<char>(c);
    }
    dst[i] = '\0';
}

int CollectPlayerCmds(PlayerCmdRow* out, int cap) {
#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
    if (!out || cap <= 0)
        return 0;
    const int want = cap < kPlayerCmdsMax ? cap : kPlayerCmdsMax;
    const int n = cmdcost::CopyUserCommands(out, want);
    for (int i = 0; i < n; ++i) {
        CleanField(out[i].name, static_cast<int>(sizeof(out[i].name)), out[i].name);
        CleanField(out[i].params, static_cast<int>(sizeof(out[i].params)), out[i].params);
    }
    return n;
#else
    (void)out;
    (void)cap;
    return 0;
#endif
}

int PlayerCmdsPageCount(int n) {
    return n > 0 ? (n + kPlayerCmdsPerPage - 1) / kPlayerCmdsPerPage : 1;
}

void PaintPlayerCmds(int slot1, bool push) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_playerArmed[slot1])
        return;
    PlayerCmdRow rows[kPlayerCmdsMax];
    const int n = CollectPlayerCmds(rows, kPlayerCmdsMax);
    const int pages = PlayerCmdsPageCount(n);
    if (g_playerPage[slot1] >= pages)
        g_playerPage[slot1] = pages - 1;
    if (g_playerPage[slot1] < 0)
        g_playerPage[slot1] = 0;
    MgCanvas c;
    MgCanvasClear(c);
    MgCanvasPCenter(c, 320, 36, kMgPGreen, "PLAYER COMMANDS");
    char line[96];
    std::snprintf(line, sizeof(line), "page %d/%d  (%d commands)", g_playerPage[slot1] + 1,
                  pages, n);
    MgCanvasPCenter(c, 320, 54, kMgPWhite, line);
    if (!n) {
        MgCanvasPCenter(c, 320, 96, kMgPWhite, "no .COMMANDS found");
    } else {
        const int begin = g_playerPage[slot1] * kPlayerCmdsPerPage;
        int y = 78;
        for (int i = begin; i < n && i < begin + kPlayerCmdsPerPage; ++i) {
            if (rows[i].params[0])
                std::snprintf(line, sizeof(line), "%.52s %.20s", rows[i].name, rows[i].params);
            else
                std::snprintf(line, sizeof(line), "%.72s", rows[i].name);
            if (!MgCanvasPText(c, 24, y, kMgPWhite, line))
                break;
            y += 20;
        }
    }
    const char* actions[] = {"next page", "previous page"};
    for (int i = 0; i < 2; ++i) {
        std::snprintf(line, sizeof(line), "%s%s", g_playerPageSelection[slot1] == i ? "> " : "",
                      actions[i]);
        MgCanvasPText(c, 24, 242 + i * 18,
                      g_playerPageSelection[slot1] == i ? kMgMenuSelectedColor
                                                        : kMgMenuNormalColor,
                      line);
    }
    if (!Profiles_SlotHideTips(slot1 - 1))
        MgCanvasPCenter(c, 320, 284, kMgPGreen,
                        "tap +use select | hold +use forward/back | hold +use +moveleft back");
    MgPutLayoutCache(slot1, kPlayerCmdsGameId, c);
    if (push && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kPlayerCmdsGameId, c);
}

void SetPlayerCmdsArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_playerArmed[slot1] = on;
    if (on) {
        g_playerPageSelection[slot1] = 0;
        MgTakeDisplay(slot1, kPlayerCmdsGameId);
        PaintPlayerCmds(slot1, true);
    } else if (MgDisplayOwnedBy(slot1, kPlayerCmdsGameId)) {
        MgClearLayout(slot1, kPlayerCmdsGameId);
    }
}

void TurnPlayerCmdsPage(int slot1, int delta) {
    PlayerCmdRow rows[kPlayerCmdsMax];
    const int n = CollectPlayerCmds(rows, kPlayerCmdsMax);
    const int pages = PlayerCmdsPageCount(n);
    g_playerPage[slot1] = (g_playerPage[slot1] + delta + pages) % pages;
    PaintPlayerCmds(slot1, true);
}

void MovePlayerPageSelection(int slot1, int delta) {
    int& selected = g_playerPageSelection[slot1];
    if (selected < 0)
        selected = delta < 0 ? 1 : 0;
    else
        selected = (selected + delta + 2) % 2;
    PaintPlayerCmds(slot1, true);
}

void OnPlayerCmdsUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || slot1 < 1 || slot1 > kMgMaxSlots ||
        !g_playerArmed[slot1] || !MgMinigameTabOpen(slot1) ||
        !MgDisplayOwnedBy(slot1, kPlayerCmdsGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    if (in->use && in->forwardEdge)
        MovePlayerPageSelection(slot1, -1);
    else if (in->use && in->backEdge)
        MovePlayerPageSelection(slot1, 1);
    else if (in->useRelease) {
        if (g_playerPageSelection[slot1] < 0)
            MovePlayerPageSelection(slot1, 1);
        else
            TurnPlayerCmdsPage(slot1,
                               g_playerPageSelection[slot1] == 0 ? 1 : -1);
    }
}

void OnPlayerCmdsClientCmd(int slot1) {
    if (!Cmds_Enabled())
        return;
    if (!g_playerArmed[slot1] || !MgDisplayOwnedBy(slot1, kPlayerCmdsGameId)) {
        g_playerPage[slot1] = 0;
        SetPlayerCmdsArmed(slot1, true);
    }
}

void OnPlayerCmdsSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_playerArmed[slot1])
        g_playerArmed[slot1] = false;
}

extern "C" void __cdecl cmds_Show_f();
extern "C" void __cdecl cmds_List_f();

void OnSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_armed[slot1])
        g_armed[slot1] = false;
}

void OnCmdsClientCmd(int slot1) {
    if (!Cmds_Enabled())
        return;
    const bool on = !g_armed[slot1] || !MgDisplayOwnedBy(slot1, kCmdsGameId);
    if (on)
        g_page[slot1] = 0;
    SetArmed(slot1, on);
}

void CmdsTryRegister() {
    if (!MgEnabled())
        return;
    static const MgGameOps kOps = {"mg_cmds", OnCmdsClientCmd, OnUserCmd, OnSessionEnd,
                                   "browse server-side commands", "server-side commands", true, OnBack};
    if (!g_registered) {
        if (!MgRegisterGame(&kOps)) {
            PrintOut(PRINT_BAD, "[cmds] MgRegisterGame failed (table full?)\n");
            return;
        }
        MgRegisterConsoleCommand("cmds_show", reinterpret_cast<void*>(&cmds_Show_f));
        MgRegisterConsoleCommand("cmds_list", reinterpret_cast<void*>(&cmds_List_f));
        g_registered = true;
    }
    if (!g_playerRegistered) {
        static const MgGameOps kPlayerOps = {
            kPlayerCmdsGameId, OnPlayerCmdsClientCmd, OnPlayerCmdsUserCmd,
            OnPlayerCmdsSessionEnd, ".COMMANDS available to players", "player commands", true};
        if (!MgRegisterGame(&kPlayerOps)) {
            PrintOut(PRINT_BAD, "[cmds] player command page registration failed\n");
            return;
        }
        g_playerRegistered = true;
    }
}

}  // namespace

void cmds_EnsureRegistered() {
    CmdsTryRegister();
}

extern "C" void __cdecl cmds_Show_f() {
    if (!Cmds_Enabled() || !MgEnabled())
        return;
    CmdsTryRegister();
    if (MgArgc() < 2) {
        Buddy_DebugPrintf("usage: cmds_show <slot 0-based> [page 1-based|next|prev]\n");
        return;
    }
    const int slot = MgUserToInternal(std::atoi(MgArgv(1)));
    if (!slot || !MgSlotSpawned(slot)) {
        Buddy_DebugPrintf("[cmds] slot not spawned\n");
        return;
    }
    if (MgArgc() >= 3) {
        const int page = CmdsParsePageArg(MgArgv(2), g_page[slot]);
        if (page < 0) {
            Buddy_DebugPrintf("usage: cmds_show <slot 0-based> [page 1-based|next|prev]\n");
            return;
        }
        g_page[slot] = page;
    }
    SetArmed(slot, true);
    Buddy_DebugPrintf("[cmds] armed slot %d page %d/%d - player opens with +use+score\n",
                      MgInternalToUser(slot), g_page[slot] + 1, CmdsPageCount());
}

extern "C" void __cdecl cmds_List_f() {
    if (!Cmds_Enabled())
        return;
    Buddy_DebugPrintf("[cmds] %d server-side commands:\n", kCmdsEntryCount);
    for (int i = 0; i < kCmdsEntryCount; ++i) {
        const CmdsEntry& e = kCmdsEntries[i];
        if (e.syntax && e.syntax[0])
            Buddy_DebugPrintf("  %s %s -- %s [%s]\n", e.name, e.syntax, e.blurb, e.side);
        else
            Buddy_DebugPrintf("  %s -- %s [%s]\n", e.name, e.blurb, e.side);
    }
}

void cmds_OnGameDllLoaded(void* gameExport) {
    (void)gameExport;
    CmdsTryRegister();
}

void cmds_MaintainForSlot(int slot1) {
    if (slot1 >= 1 && !Profiles_SlotActive(slot1 - 1)) {
        if (g_armed[slot1])
            SetArmed(slot1, false);
        if (g_playerArmed[slot1])
            SetPlayerCmdsArmed(slot1, false);
        return;
    }
    if (slot1 >= 1 && !Profiles_IsLevel0Admin(slot1 - 1)) {
        if (g_armed[slot1]) {
            if (!MgDisplayOwnedBy(slot1, kCmdsGameId) || !MgMinigameTabOpen(slot1) ||
                !MgNavigateBack(slot1))
                SetArmed(slot1, false);
        }
        if (!g_playerArmed[slot1])
            return;
    }
    if (!Cmds_Enabled() || !MgEnabled() || slot1 < 1 ||
        (!g_armed[slot1] && !g_playerArmed[slot1]))
        return;
    if (!MgMinigameTabOpen(slot1))
        return;
    if (MgDisplayOwnedBy(slot1, kPlayerCmdsGameId))
        return;
    if (MgDisplayTakenByOther(slot1, kCmdsGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kCmdsGameId))
        MgTakeDisplay(slot1, kCmdsGameId);
    if (!MgRunningSession(kCmdsGameId))
        return;
    Paint(slot1, false);
}
