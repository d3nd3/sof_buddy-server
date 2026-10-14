// .mg opens the scoreboard view (mg) by default (SIGNED IN + footer entry).
// Tap +use on the footer pushes the centered minigame list (mg_menu) so
// hold +use + moveleft pops back here via the nav stack.
// .mg list prints that same list.

#include "menu.h"

#include "buddy_import.h"
#include "log.h"
#include "../../profiles/cvar.h"
#include "../../profiles/stats/stats.h"
#include "../../profiles/stats/stats_logic.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>
#include "../minigames_api.h"
#include "../minigames_clientcmd_logic.h"

namespace {

constexpr char kGameId[] = "mg";
constexpr char kMenuGameId[] = "mg_menu";
constexpr int kMenuLine = 18;
constexpr int kCenterX = 320;
constexpr int kPanelLift = 32;
constexpr char kMenuRule[] = "----------------";
constexpr char kEnterMenuLabel[] = "minigames";
bool g_registered = false;
bool g_menuRegistered = false;
bool g_armed[kMgMaxSlots + 1] = {};
bool g_menuArmed[kMgMaxSlots + 1] = {};
int g_selected[kMgMaxSlots + 1] = {};

bool IsMenuId(const char* command) {
    return MgClientCmdMatches(command, kGameId) ||
           MgClientCmdMatches(command, kMenuGameId);
}

struct MenuRow {
    char name[24];
    const char* command;
    const char* desc;
};

bool Level0Only(const char* command) {
    return command && (!std::strcmp(command, "mg_cmds") ||
                       !std::strcmp(command, "mg_cvars") ||
                       !std::strcmp(command, "mg_lag"));
}

// Server admins plus map admins (privilege 0 or 1): the upload-match
// action row. The stats detail page itself is open to every tier.
bool Level01Only(const char* command) {
    return command && !std::strcmp(command, "mg_upload");
}

bool VisibleToSlot(int slot1, const char* command) {
    if (Level0Only(command))
        return Profiles_IsLevel0Admin(slot1 - 1);
    if (Level01Only(command))
        return Profiles_IsLevel01Admin(slot1 - 1);
    return true;
}

int MenuRequiredPrivilege(const char* command) {
    if (Level0Only(command))
        return 0;
    if (Level01Only(command))
        return 1;
    return 2;
}

const char* MenuMarkerColor(const char* command) {
    switch (MenuRequiredPrivilege(command)) {
    case 0:
        return kMgPPurple;
    case 1:
        return kMgPCyan;
    default:
        return kMgPGreen;
    }
}

int ColumnTop(int lines) {
    return (480 - ((lines - 1) * kMenuLine + 8)) / 2;
}

int BuildRows(int slot1, MenuRow* rows, int cap) {
    if (!rows || cap <= 0)
        return 0;
    int n = 0;
    const int count = MgRegisteredGameCount();
    for (int i = 0; i < count && n < cap; ++i) {
        const char* cmd = MgRegisteredGameCommand(i);
        if (!cmd || !cmd[0])
            continue;
        if (IsMenuId(cmd))
            continue;
        if (!VisibleToSlot(slot1, cmd))
            continue;
        const char* label = MgRegisteredGameMenuName(i);
        if (label && label[0])
            std::snprintf(rows[n].name, sizeof(rows[n].name), "%s", label);
        else
            std::snprintf(rows[n].name, sizeof(rows[n].name), ".%s",
                          cmd[0] == '.' ? cmd + 1 : cmd);
        rows[n].command = cmd;
        rows[n].desc = MgRegisteredGameDesc(i);
        ++n;
    }
    for (int i = 1; i < n; ++i) {
        MenuRow row = rows[i];
        const int privilege = MenuRequiredPrivilege(row.command);
        int j = i;
        while (j > 0 && MenuRequiredPrivilege(rows[j - 1].command) > privilege) {
            rows[j] = rows[j - 1];
            --j;
        }
        rows[j] = row;
    }
    return n;
}

struct ControlPiece {
    const char* text;
    const char* color;
    bool alt;
};

void RenderControlLine(MgCanvas& c, int y, const ControlPiece* pieces, int count) {
    int width = 0;
    for (int i = 0; i < count; ++i)
        width += static_cast<int>(std::strlen(pieces[i].text)) * 8;
    int x = 320 - width / 2;
    for (int i = 0; i < count; ++i) {
        const ControlPiece& piece = pieces[i];
        if (piece.alt)
            MgCanvasPAltText(c, x, y, piece.color, piece.text);
        else
            MgCanvasPText(c, x, y, piece.color, piece.text);
        x += static_cast<int>(std::strlen(piece.text)) * 8;
    }
}

void RenderControls(MgCanvas& c) {
    MgCanvasPCenter(c, 320, 34, kMgPGreen, "CONTROL MENU");
    const ControlPiece line1[] = {
        {"tap ", kMgPWhite, true}, {"+use", kMgPYellow, false},
        {"  SELECT", kMgPWhite, true}};
    const ControlPiece line2[] = {
        {"hold ", kMgPWhite, true}, {"+use", kMgPYellow, false}, {" + ", kMgPWhite, true},
        {"forward", kMgPRed, false}, {" / ", kMgPWhite, true}, {"back", kMgPRed, false},
        {"  MOVE", kMgPWhite, true}};
    const ControlPiece line3[] = {
        {"hold ", kMgPWhite, true}, {"+use", kMgPYellow, false}, {" + ", kMgPWhite, true},
        {"moveleft", kMgPRed, false}, {"  BACK", kMgPWhite, true}};
    RenderControlLine(c, 54, line1, static_cast<int>(sizeof(line1) / sizeof(line1[0])));
    RenderControlLine(c, 72, line2, static_cast<int>(sizeof(line2) / sizeof(line2[0])));
    RenderControlLine(c, 90, line3, static_cast<int>(sizeof(line3) / sizeof(line3[0])));
}

void RenderPanelHead(MgCanvas& c, int x, int& y, const char* title) {
    MgCanvasPCenter(c, x, y, kMgPGreen, title);
    y += kMenuLine;
    MgCanvasPCenter(c, x, y, kMgPHWhite, kMenuRule);
    y += kMenuLine;
}

bool RenderMenuRow(MgCanvas& c, int x, int y, const char* name, bool selected,
                   const char* markerColor) {
    if (!selected)
        return MgCanvasPCenter(c, x, y, kMgMenuNormalColor, name);
    const int markerWidth = 2 * 8;
    const int nameWidth = static_cast<int>(std::strlen(name ? name : "")) * 8;
    const int left = x - (markerWidth + nameWidth) / 2;
    return MgCanvasPText(c, left, y, markerColor, "> ") &&
           MgCanvasPText(c, left + markerWidth, y, kMgMenuSelectedColor, name);
}

// Engine offsets for the live score read (same as the stats snapshot:
// gclient ps.stats[STAT_FRAGS] via edict->client).
constexpr unsigned kMenuEdictClientOfs = 0x74;
constexpr unsigned kMenuClientStatsOfs = 164;
constexpr int kMenuStatFrags = 10;

// This map's score for one 1-based slot. Zero when the edict/client is
// unreadable — the row still renders, just without a live number that tick.
int LandingLiveFrags(int slot1) {
    void* ent = MgEdictForSlot(slot1);
    if (!ent || IsBadReadPtr(static_cast<char*>(ent) + kMenuEdictClientOfs, sizeof(void*)))
        return 0;
    void* client =
        *reinterpret_cast<void**>(static_cast<char*>(ent) + kMenuEdictClientOfs);
    if (!client || IsBadReadPtr(static_cast<char*>(client) + kMenuClientStatsOfs + 32, 32))
        return 0;
    const short* stats =
        reinterpret_cast<const short*>(static_cast<char*>(client) + kMenuClientStatsOfs);
    if (IsBadReadPtr(const_cast<short*>(stats + kMenuStatFrags), sizeof(short)))
        return 0;
    return static_cast<int>(stats[kMenuStatFrags]);
}

struct SignedInRow {
    char nickname[16];
    char slotName[16];
    const char* nicknameColor;
    int liveFrags = 0;          // this map's score
    long long matches = 0;      // lifetime recorded matches
    long long totalFrags = 0;   // lifetime recorded frags
    bool hasLifetime = false;   // lifetime numbers present (stats on + recorded)
};

const char* SignedInNicknameColor(int slot0) {
    const int privilege = Profiles_SlotPrivilege(slot0);
    return privilege == 0 ? kMgPPurple : privilege == 1 ? kMgPCyan : kMgPGreen;
}

bool RenderSignedInRow(MgCanvas& c, int x, int y, const SignedInRow& row) {
    if (!row.slotName[0])
        return MgCanvasPCenter(c, x, y, row.nicknameColor, row.nickname);
    // Identity plus main stats, same vocabulary as the mg_stats detail page:
    // "nick [as live] - 12 frags | 3 matches | 145 total". The " as live"
    // part is dropped when the live name equals the nickname (common case).
    // Width math counts visible chars only (color codes are zero-width).
    char statsText[64];
    if (row.hasLifetime) {
        std::string s = stats::FormatLandingStats(row.liveFrags, row.totalFrags, row.matches);
        std::snprintf(statsText, sizeof(statsText), "%s", s.c_str());
    } else {
        std::snprintf(statsText, sizeof(statsText), "%d frags", row.liveFrags);
    }
    const bool showLive = row.slotName[0] &&
                          std::strcmp(row.nickname, row.slotName) != 0;
    char text[192];
    int visible = 0;
    if (showLive) {
        std::snprintf(text, sizeof(text), "%s%s%s as %s - %s", row.nicknameColor,
                      row.nickname, kMgPWhite, row.slotName, statsText);
        visible = static_cast<int>(std::strlen(row.nickname)) + 4 +
                  static_cast<int>(std::strlen(row.slotName)) + 3 +
                  static_cast<int>(std::strlen(statsText));
    } else {
        std::snprintf(text, sizeof(text), "%s%s%s - %s", row.nicknameColor, row.nickname,
                      kMgPWhite, statsText);
        visible = static_cast<int>(std::strlen(row.nickname)) + 3 +
                  static_cast<int>(std::strlen(statsText));
    }
    const int left = x - (visible * 8) / 2;
    return MgCanvasPText(c, left, y, nullptr, text);
}

constexpr int kMaxProfilesShown = 8;

// Fills who[0..cap) and returns total signed-in profiles; shown count via outShown.
int CollectSignedIn(SignedInRow* who, int cap, int* outShown) {
    int wn = 0;
    int totalProfiles = 0;
    for (int i = 0; i < kMgMaxSlots; ++i) {
        int slot0 = 0;
        char nick[16];
        char slotName[16];
        if (!Profiles_SignedInAt(i, &slot0, nick, static_cast<int>(sizeof(nick)), slotName,
                                 static_cast<int>(sizeof(slotName))))
            break;
        ++totalProfiles;
        if (who && wn < cap && wn < kMaxProfilesShown) {
            std::snprintf(who[wn].nickname, sizeof(who[wn].nickname), "%.14s", nick);
            std::snprintf(who[wn].slotName, sizeof(who[wn].slotName), "%.14s", slotName);
            who[wn].nicknameColor = SignedInNicknameColor(slot0);
            who[wn].liveFrags = LandingLiveFrags(slot0 + 1);
            who[wn].hasLifetime =
                Stats_LifetimeForSlot(slot0, &who[wn].matches, &who[wn].totalFrags);
            ++wn;
        }
    }
    if (totalProfiles > wn && who && wn < cap) {
        std::snprintf(who[wn].nickname, sizeof(who[wn].nickname), "...");
        who[wn].slotName[0] = '\0';
        who[wn].nicknameColor = kMgPWhite;
        ++wn;
    }
    if (outShown)
        *outShown = wn;
    return totalProfiles;
}

void RenderScoreboard(int slot1, MgCanvas& c) {
    MgCanvasClear(c);
    SignedInRow who[kMaxProfilesShown + 1] = {};
    int wn = 0;
    const int totalProfiles = CollectSignedIn(
        who, static_cast<int>(sizeof(who) / sizeof(who[0])), &wn);

    long long sumMatches = 0, sumPlayers = 0;
    const bool showSummary = Stats_IsEnabled();
    if (showSummary)
        Stats_Summary(&sumMatches, &sumPlayers);
    const int lines = 2 + 1 + (showSummary ? 1 : 0) + (wn > 0 ? wn : 1) + 1 + 1;
    const int panelTop = ColumnTop(lines) - kPanelLift;

    int y = panelTop;
    RenderPanelHead(c, kCenterX, y, "SIGNED IN");
    char count[32];
    if (totalProfiles == 1)
        std::snprintf(count, sizeof(count), "1 profile active");
    else
        std::snprintf(count, sizeof(count), "%d profiles active", totalProfiles);
    MgCanvasPCenter(c, kCenterX, y, kMgPYellow, count);
    y += kMenuLine;
    if (showSummary) {
        std::string summary = stats::FormatTotalsSummary(sumMatches, sumPlayers);
        char summaryLine[96];
        std::snprintf(summaryLine, sizeof(summaryLine), "%s", summary.c_str());
        MgCanvasPCenter(c, kCenterX, y, kMgPWhite, summaryLine);
        y += kMenuLine;
    }
    if (wn == 0) {
        MgCanvasPCenter(c, kCenterX, y, kMgPWhite, "(none)");
        y += kMenuLine;
    } else {
        for (int i = 0; i < wn; ++i) {
            if (!RenderSignedInRow(c, kCenterX, y, who[i]))
                break;
            y += kMenuLine;
        }
    }
    y += kMenuLine;
    RenderMenuRow(c, kCenterX, y, kEnterMenuLabel, true, kMgPGreen);

    if (!Profiles_SlotHideTips(slot1 - 1))
        RenderControls(c);
}

void RenderMenuList(int slot1, MgCanvas& c) {
    MgCanvasClear(c);
    MenuRow rows[8];
    const int n = BuildRows(slot1, rows, static_cast<int>(sizeof(rows) / sizeof(rows[0])));
    if (n > 0 && (g_selected[slot1] < 0 || g_selected[slot1] >= n))
        g_selected[slot1] = 0;
    const int selected = g_selected[slot1];
    const char* selectedDesc =
        selected >= 0 && selected < n ? rows[selected].desc : nullptr;

    const int lines = 2 + (selectedDesc && selectedDesc[0] ? 1 : 0) + (n > 0 ? n : 1);
    const int panelTop = ColumnTop(lines) - kPanelLift;

    if (!Profiles_SlotHideTips(slot1 - 1))
        RenderControls(c);

    int y = panelTop;
    RenderPanelHead(c, kCenterX, y, "MINIGAMES");
    if (selectedDesc && selectedDesc[0]) {
        char descLine[128];
        std::snprintf(descLine, sizeof(descLine), "%s", selectedDesc);
        MgCanvasPCenter(c, kCenterX, y, kMgPYellow, descLine);
        y += kMenuLine;
    }
    if (n == 0) {
        MgCanvasPCenter(c, kCenterX, y, kMgPWhite, "(none)");
    } else {
        for (int i = 0; i < n; ++i) {
            const bool isSelected = selected == i;
            if (!RenderMenuRow(c, kCenterX, y, rows[i].name, isSelected,
                               MenuMarkerColor(rows[i].command)))
                break;
            y += kMenuLine;
        }
    }
}

void MoveSelection(int slot1, int delta) {
    MenuRow rows[8];
    const int n = BuildRows(slot1, rows, static_cast<int>(sizeof(rows) / sizeof(rows[0])));
    if (n <= 0)
        return;
    int& selected = g_selected[slot1];
    if (selected < 0 || selected >= n)
        selected = delta < 0 ? n - 1 : 0;
    else
        selected = (selected + delta + n) % n;
}

int SaveSelected(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return -1;
    return g_selected[slot1];
}

void PushScoreboard(int slot1, bool send) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    RenderScoreboard(slot1, c);
    MgPutLayoutCache(slot1, kGameId, c);
    if (send && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kGameId, c);
}

void PushMenu(int slot1, bool send) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_menuArmed[slot1])
        return;
    MgCanvas c;
    RenderMenuList(slot1, c);
    MgPutLayoutCache(slot1, kMenuGameId, c);
    if (send && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kMenuGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        MgTakeDisplay(slot1, kGameId);
        PushScoreboard(slot1, true);
        return;
    }
    if (MgDisplayOwnedBy(slot1, kGameId))
        MgClearLayout(slot1, kGameId);
}

void SetMenuArmed(int slot1, bool on, bool resetSelection = false) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_menuArmed[slot1] = on;
    if (on) {
        if (resetSelection)
            g_selected[slot1] = 0;
        MgTakeDisplay(slot1, kMenuGameId);
        PushMenu(slot1, true);
        return;
    }
    if (MgDisplayOwnedBy(slot1, kMenuGameId))
        MgClearLayout(slot1, kMenuGameId);
}

void OnScoreboardUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || !MgMinigameTabOpen(slot1) || slot1 < 1 ||
        slot1 > kMgMaxSlots || !g_armed[slot1] ||
        !MgDisplayOwnedBy(slot1, kGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    // Scoreboard is the nav root: back does nothing, forward/back has no
    // other item to move to. Tap +use on the footer pushes the menu list
    // so hold +use + moveleft can pop back here via the stack.
    if (in->use && (in->forwardEdge || in->backEdge)) {
        PushScoreboard(slot1, true);
        return;
    }
    if (in->useRelease) {
        if (!MgNavigateForward(slot1, kMenuGameId)) {
            g_selected[slot1] = 0;
            PushScoreboard(slot1, true);
        }
    }
}

void OnMenuUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || !MgMinigameTabOpen(slot1) || slot1 < 1 ||
        slot1 > kMgMaxSlots || !g_menuArmed[slot1] ||
        !MgDisplayOwnedBy(slot1, kMenuGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    // Back is stack-driven (ProcessPendingInputs -> MgNavigateBack pops to
    // the scoreboard), so only movement + select live here.
    if (in->use && in->forwardEdge) {
        MoveSelection(slot1, -1);
        PushMenu(slot1, true);
        return;
    }
    if (in->use && in->backEdge) {
        MoveSelection(slot1, 1);
        PushMenu(slot1, true);
        return;
    }
    if (in->useRelease && g_selected[slot1] < 0) {
        MoveSelection(slot1, 1);
        PushMenu(slot1, true);
        return;
    }
    if (in->useRelease) {
        MenuRow rows[8];
        const int n = BuildRows(slot1, rows, static_cast<int>(sizeof(rows) / sizeof(rows[0])));
        if (g_selected[slot1] < n && !IsMenuId(rows[g_selected[slot1]].command))
            MgNavigateForward(slot1, rows[g_selected[slot1]].command);
    }
}

void OnScoreboardSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_armed[slot1])
        g_armed[slot1] = false;
}

void OnMenuSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_menuArmed[slot1])
        g_menuArmed[slot1] = false;
}

void PrintList(int slot1, void* ent) {
    if (!ent)
        return;
    Buddy_ClientPrintf(ent, 2, "minigames:\n");
    int shown = 0;
    const int n = MgRegisteredGameCount();
    for (int i = 0; i < n; ++i) {
        const char* cmd = MgRegisteredGameCommand(i);
        if (!cmd || !cmd[0])
            continue;
        if (IsMenuId(cmd))
            continue;
        if (!VisibleToSlot(slot1, cmd))
            continue;
        const char* desc = MgRegisteredGameDesc(i);
        const char* name = MgRegisteredGameMenuName(i);
        if (!name || !name[0])
            name = cmd[0] == '.' ? cmd + 1 : cmd;
        if (desc && desc[0])
            Buddy_ClientPrintf(ent, 2, "%s  %s\n", name, desc);
        else
            Buddy_ClientPrintf(ent, 2, "%s\n", name);
        ++shown;
    }
    if (shown == 0)
        Buddy_ClientPrintf(ent, 2, "(none)\n");
}

void OnScoreboardClientCmd(int slot1) {
    if (!MgEnabled())
        return;
    if (MgArgc() >= 2) {
        if (!MgClientCmdMatches(MgArgv(1), "list")) {
            if (void* ent = MgEdictForSlot(slot1))
                Buddy_ClientPrintf(ent, 2, "mg [list]\n");
            return;
        }
        PrintList(slot1, MgEdictForSlot(slot1));
        return;
    }
    const bool on = !g_armed[slot1] || MgDisplayTakenByOther(slot1, kGameId);
    if (on)
        MgNavigationReset(slot1);
    SetArmed(slot1, on);
}

void OnMenuClientCmd(int slot1) {
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    if (!g_menuArmed[slot1] || !MgDisplayOwnedBy(slot1, kMenuGameId)) {
        g_selected[slot1] = 0;
        SetMenuArmed(slot1, true, true);
    }
}

void TryRegister() {
    if (!MgEnabled() || (g_registered && g_menuRegistered))
        return;
    if (!g_registered) {
        static const MgGameOps kOps = {kGameId, OnScoreboardClientCmd,
                                       OnScoreboardUserCmd, OnScoreboardSessionEnd,
                                       "list of minigames", "minigames"};
        if (!MgRegisterGame(&kOps)) {
            PrintOut(PRINT_BAD, "[minigames] menu MgRegisterGame failed\n");
            return;
        }
        g_registered = true;
    }
    if (!g_menuRegistered) {
        static const MgGameOps kMenuOps = {
            kMenuGameId, OnMenuClientCmd, OnMenuUserCmd, OnMenuSessionEnd,
            "minigame list", "minigames", true, nullptr, SaveSelected};
        if (!MgRegisterGame(&kMenuOps)) {
            PrintOut(PRINT_BAD, "[minigames] menu list MgRegisterGame failed\n");
            return;
        }
        g_menuRegistered = true;
    }
    PrintOut(PRINT_LOG, "[minigames] menu registered (.mg / .mg list)\n");
}

}  // namespace

void menu_EnsureRegistered() {
    TryRegister();
}

void menu_OnMinigameTabOpened(int slot1) {
    if (!MgEnabled() || slot1 < 1)
        return;
    menu_EnsureRegistered();
    if (MgDisplayTakenByOther(slot1, kGameId))
        return;
    MgNavigationReset(slot1);
    g_selected[slot1] = 0;
    SetArmed(slot1, true);
}

void menu_MaintainForSlot(int slot1) {
    if (!MgEnabled() || slot1 < 1)
        return;
    if (!MgMinigameTabOpen(slot1))
        return;
    if (MgDisplayOwnedBy(slot1, kGameId)) {
        if (!g_armed[slot1])
            return;
        if (MgDisplayTakenByOther(slot1, kGameId))
            return;
        if (!MgRunningSession(kGameId))
            return;
        PushScoreboard(slot1, false);
        return;
    }
    if (MgDisplayOwnedBy(slot1, kMenuGameId)) {
        if (!g_menuArmed[slot1])
            return;
        if (MgDisplayTakenByOther(slot1, kMenuGameId))
            return;
        if (!MgRunningSession(kMenuGameId))
            return;
        PushMenu(slot1, false);
    }
}

void menu_OpenFromNavigation(int slot1) {
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    menu_EnsureRegistered();
    // Landing on the root clears any stale stack (e.g. after a refused page
    // falls home): mg is the bottom, there is nothing above it to keep.
    MgNavigationReset(slot1);
    g_armed[slot1] = true;
    g_selected[slot1] = 0;
    MgShowLayout(slot1, kGameId, true);
    PushScoreboard(slot1, true);
}

void menu_OpenMenuFromNavigation(int slot1, int selected) {
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    menu_EnsureRegistered();
    // Stack return to the minigame list: re-take the display, re-show the
    // view bits MgClearLayout dropped, then push. Deliberately not
    // MgShowLayout: ApplyView would fire menu_OnMinigameTabOpened, resetting
    // the nav stack and arming the scoreboard instead. The popped stack
    // entry's cursor is authoritative: the view resets to the top, then the
    // saved position is restored, so Back returns to where the player was.
    g_menuArmed[slot1] = true;
    g_selected[slot1] = 0;
    MgTakeDisplay(slot1, kMenuGameId);
    MgReshowCurrentLayout(slot1);
    if (selected >= 0)
        g_selected[slot1] = selected;
    PushMenu(slot1, true);
}

void menu_CloseFromNavigation(int slot1) {
    SetArmed(slot1, false);
    SetMenuArmed(slot1, false);
}
