#include "settings.h"

#include "../../profiles/cvar.h"
#include "../minigames_api.h"

#include "log.h"

#include <cstdio>

namespace {

constexpr char kGameId[] = "mg_settings";
constexpr char kRule[] = "----------------";
constexpr int kRowCount = 2;
bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};
int g_selected[kMgMaxSlots + 1] = {};

void Render(int slot1, MgCanvas& c) {
    MgCanvasClear(c);
    const bool hide = Profiles_SlotHideTips(slot1 - 1);
    const bool swap = Profiles_SlotSwapScoreboard(slot1 - 1);
    if (g_selected[slot1] < 0 || g_selected[slot1] >= kRowCount)
        g_selected[slot1] = 0;
    MgCanvasPCenter(c, 320, 112, kMgPGreen, "SETTINGS");
    MgCanvasPCenter(c, 320, 130, kMgPHWhite, kRule);

    char line[48];
    std::snprintf(line, sizeof(line), "%sHide tips/tutorial: %s",
                  g_selected[slot1] == 0 ? "> " : "", hide ? "yes" : "no");
    MgCanvasPCenter(c, 320, 148, g_selected[slot1] == 0 ? kMgPWhite : kMgPHWhite,
                    line);
    std::snprintf(line, sizeof(line), "%sSwitch default scoreboard: %s",
                  g_selected[slot1] == 1 ? "> " : "", swap ? "yes" : "no");
    MgCanvasPCenter(c, 320, 166, g_selected[slot1] == 1 ? kMgPWhite : kMgPHWhite,
                    line);
    if (!hide)
        MgCanvasPCenter(c, 320, 214, kMgPYellow,
                        "tap +use toggle | hold +use +moveleft back");
}

void Push(int slot1, bool send) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    Render(slot1, c);
    MgPutLayoutCache(slot1, kGameId, c);
    if (send && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kGameId, c);
}

void OnClientCmd(int slot1) {
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = true;
    g_selected[slot1] = 0;
    if (!MgTakeDisplay(slot1, kGameId))
        return;
    Push(slot1, true);
}

void ToggleSelected(int slot1) {
    if (g_selected[slot1] == 1)
        Profiles_SetSlotSwapScoreboard(slot1 - 1,
                                       !Profiles_SlotSwapScoreboard(slot1 - 1));
    else
        Profiles_SetSlotHideTips(slot1 - 1, !Profiles_SlotHideTips(slot1 - 1));
    Push(slot1, true);
}

void OnUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || slot1 < 1 || slot1 > kMgMaxSlots ||
        !g_armed[slot1] || !MgMinigameTabOpen(slot1) ||
        !MgDisplayOwnedBy(slot1, kGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    if (in->use && in->forwardEdge) {
        g_selected[slot1] = (g_selected[slot1] + kRowCount - 1) % kRowCount;
        Push(slot1, true);
        return;
    }
    if (in->use && in->backEdge) {
        g_selected[slot1] = (g_selected[slot1] + 1) % kRowCount;
        Push(slot1, true);
        return;
    }
    if (in->useRelease) {
        // No entry-tap guard needed: input dispatch snapshots the display
        // owner, so the tap that opened this page never reaches us.
        ToggleSelected(slot1);
    }
}

void OnSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        g_armed[slot1] = false;
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        g_selected[slot1] = 0;
}

void TryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {
        kGameId, OnClientCmd, OnUserCmd, OnSessionEnd,
        "personal menu settings", "settings", true};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[settings] MgRegisterGame failed\n");
        return;
    }
    g_registered = true;
}

}  // namespace

void settings_EnsureRegistered() {
    TryRegister();
}

void settings_MaintainForSlot(int slot1) {
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1] ||
        !MgMinigameTabOpen(slot1) || MgDisplayTakenByOther(slot1, kGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kGameId))
        MgTakeDisplay(slot1, kGameId);
    if (MgRunningSession(kGameId))
        Push(slot1, false);
}
