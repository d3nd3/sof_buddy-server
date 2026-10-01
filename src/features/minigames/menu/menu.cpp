// .mg opens a centered layout listing every minigame, including itself.
// .mg list prints that same list.

#include "menu.h"

#include "buddy_import.h"
#include "log.h"
#include "../../profiles/cvar.h"

#include <cstdio>
#include "../minigames_api.h"
#include "../minigames_clientcmd_logic.h"

namespace {

constexpr char kGameId[] = "mg";
bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};

struct MenuRow {
    char name[24];
    const char* desc;
};

int ColumnTop(int lines) {
    constexpr int kLine = 16;
    return (480 - ((lines - 1) * kLine + 8)) / 2;
}

void Render(MgCanvas& c) {
    MgCanvasClear(c);
    constexpr int kLine = 16;
    MenuRow rows[8];
    int n = 0;
    int leftLines = 1;
    const int count = MgRegisteredGameCount();
    for (int i = 0; i < count && n < 8; ++i) {
        const char* cmd = MgRegisteredGameCommand(i);
        if (!cmd || !cmd[0])
            continue;
        std::snprintf(rows[n].name, sizeof(rows[n].name), ".%s", cmd[0] == '.' ? cmd + 1 : cmd);
        rows[n].desc = MgRegisteredGameDesc(i);
        leftLines += (rows[n].desc && rows[n].desc[0]) ? 2 : 1;
        ++n;
    }
    if (n == 0)
        leftLines = 2;
    int y = ColumnTop(leftLines);
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 160, y, "minigames");
    y += kLine;
    if (n == 0) {
        MgCanvasTc(c, kMgColWhite);
        MgCanvasCenter(c, 160, y, "(none)");
    } else {
        for (int i = 0; i < n; ++i) {
            MgCanvasTc(c, kMgColWhite);
            if (!MgCanvasCenter(c, 160, y, rows[i].name))
                break;
            y += kLine;
            if (rows[i].desc && rows[i].desc[0]) {
                MgCanvasTc(c, kMgColGreen);
                if (!MgCanvasCenter(c, 160, y, rows[i].desc))
                    break;
                y += kLine;
            }
        }
    }

    char who[16][24];
    int wn = 0;
    for (int i = 0; wn < 15; ++i) {
        int slot0 = 0;
        char name[18];
        if (!Profiles_SignedInAt(i, &slot0, name, static_cast<int>(sizeof(name))))
            break;
        std::snprintf(who[wn], sizeof(who[0]), "%d  %.16s", slot0, name);
        ++wn;
    }
    if (wn == 15) {
        int slot0 = 0;
        char name[4];
        if (Profiles_SignedInAt(15, &slot0, name, static_cast<int>(sizeof(name))))
            std::snprintf(who[wn++], sizeof(who[0]), "...");
    }
    const int rightLines = 1 + (wn > 0 ? wn : 1);
    y = ColumnTop(rightLines);
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 480, y, "signed in");
    y += kLine;
    MgCanvasTc(c, kMgColWhite);
    if (wn == 0) {
        MgCanvasCenter(c, 480, y, "(none)");
        return;
    }
    for (int i = 0; i < wn; ++i) {
        if (!MgCanvasCenter(c, 480, y, who[i]))
            break;
        y += kLine;
    }
}

void Push(int slot1, bool send) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    Render(c);
    MgPutLayoutCache(slot1, kGameId, c);
    if (send && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        MgTakeDisplay(slot1, kGameId);
        Push(slot1, true);
        return;
    }
    if (!MgDisplayOwnedBy(slot1, kGameId))
        return;
    MgCanvas c;
    MgCanvasClear(c);
    MgPutLayoutCache(slot1, kGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kGameId, c);
    MgReleaseDisplay(slot1, kGameId);
}

void OnSessionEnd() {
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (g_armed[s])
            SetArmed(s, false);
    }
}

void PrintList(void* ent) {
    if (!ent)
        return;
    Buddy_ClientPrintf(ent, 2, "minigames:\n");
    int shown = 0;
    const int n = MgRegisteredGameCount();
    for (int i = 0; i < n; ++i) {
        const char* cmd = MgRegisteredGameCommand(i);
        if (!cmd || !cmd[0])
            continue;
        const char* desc = MgRegisteredGameDesc(i);
        const char* name = cmd[0] == '.' ? cmd + 1 : cmd;
        if (desc && desc[0])
            Buddy_ClientPrintf(ent, 2, ".%s  %s\n", name, desc);
        else
            Buddy_ClientPrintf(ent, 2, ".%s\n", name);
        ++shown;
    }
    if (shown == 0)
        Buddy_ClientPrintf(ent, 2, "(none)\n");
}

void OnClientCmd(int slot1) {
    if (!MgEnabled())
        return;
    if (MgArgc() >= 2) {
        if (!MgClientCmdMatches(MgArgv(1), "list")) {
            if (void* ent = MgEdictForSlot(slot1))
                Buddy_ClientPrintf(ent, 2, "mg [list]\n");
            return;
        }
        PrintList(MgEdictForSlot(slot1));
        return;
    }
    const bool on = !g_armed[slot1];
    SetArmed(slot1, on);
    if (void* ent = MgEdictForSlot(slot1))
        Buddy_ClientPrintf(ent, 2, "Minigames %s (+use+score to view)\n", on ? "menu" : "off");
}

void TryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {kGameId, OnClientCmd, nullptr, OnSessionEnd, "list of minigames"};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[minigames] menu MgRegisterGame failed\n");
        return;
    }
    g_registered = true;
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
    SetArmed(slot1, true);
}

void menu_MaintainForSlot(int slot1) {
    if (!MgEnabled() || slot1 < 1 || !g_armed[slot1])
        return;
    if (!MgMinigameTabOpen(slot1) || MgDisplayTakenByOther(slot1, kGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kGameId))
        MgTakeDisplay(slot1, kGameId);
    if (!MgRunningSession(kGameId))
        return;
    Push(slot1, false);
}
