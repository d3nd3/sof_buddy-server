// commands: sof_buddy server-command catalog on the minigames layout tab.
//
// Client word mg_cmds arms per-slot, +use+score views; server console arms
// via cmds_show and dumps via cmds_list. Static paged catalog lives in
// commands_logic.h. No _sofbuddy_ internal cvars here by policy: per-slot
// page/armed state is in-memory. If a cvar were ever needed for plumbing it
// would use the _sb_internal_ prefix, never _sofbuddy_.

#include "cvar.h"
#include "commands.h"
#include "commands_logic.h"

#include "buddy_import.h"
#include "log.h"
#include "../../profiles/cvar.h"
#include "../minigames_api.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr char kCmdsGameId[] = "mg_cmds";

bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};
int g_page[kMgMaxSlots + 1] = {};

void Paint(int slot1, bool push) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    CmdsRender(g_page[slot1], c);
    MgPutLayoutCache(slot1, kCmdsGameId, c);
    if (push && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kCmdsGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        g_page[slot1] = CmdsClampPage(g_page[slot1]);
        MgTakeDisplay(slot1, kCmdsGameId);
        Paint(slot1, true);
        return;
    }
    if (!MgDisplayOwnedBy(slot1, kCmdsGameId))
        return;
    MgCanvas c;
    MgCanvasClear(c);
    MgPutLayoutCache(slot1, kCmdsGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kCmdsGameId, c);
    MgReleaseDisplay(slot1, kCmdsGameId);
}

extern "C" void __cdecl cmds_Show_f();
extern "C" void __cdecl cmds_List_f();

void OnSessionEnd() {
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (!g_armed[s])
            continue;
        SetArmed(s, false);
    }
}

void OnCmdsClientCmd(int slot1) {
    if (!Cmds_Enabled()) {
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, "Commands viewer disabled on this server\n");
        return;
    }
    const int argc = MgArgc();
    if (argc < 2) {
        const bool on = !g_armed[slot1];
        g_page[slot1] = 0;
        SetArmed(slot1, on);
        if (void* ent = MgEdictForSlot(slot1)) {
            if (on)
                Buddy_ClientPrintf(ent, 2, "Commands armed\n.mg_cmds <page>\n");
            else
                Buddy_ClientPrintf(ent, 2, "Commands off\n");
        }
        return;
    }
    const char* tok = MgArgv(1);
    const int page = CmdsParsePageArg(tok, g_armed[slot1] ? g_page[slot1] : 0);
    if (page < 0) {
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, ".mg_cmds [page|next|prev] (1-%d)\n", CmdsPageCount());
        return;
    }
    g_page[slot1] = page;
    if (!g_armed[slot1])
        SetArmed(slot1, true);
    else
        Paint(slot1, true);
    if (void* ent = MgEdictForSlot(slot1))
        Buddy_ClientPrintf(ent, 2, "Commands page %d/%d (+use+score to view)\n", page + 1,
                           CmdsPageCount());
}

void CmdsTryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {"mg_cmds", OnCmdsClientCmd, nullptr, OnSessionEnd,
                                   "server command list"};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[cmds] MgRegisterGame failed (table full?)\n");
        return;
    }
    MgRegisterConsoleCommand("cmds_show", reinterpret_cast<void*>(&cmds_Show_f));
    MgRegisterConsoleCommand("cmds_list", reinterpret_cast<void*>(&cmds_List_f));
    g_registered = true;
    PrintOut(PRINT_LOG, "[cmds] registered (.mg_cmds [page])\n");
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
    Buddy_DebugPrintf("[cmds] %d commands (S=server console, C=client word):\n",
                      kCmdsEntryCount);
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
        return;
    }
    if (!Cmds_Enabled() || !MgEnabled() || slot1 < 1 || !g_armed[slot1])
        return;
    if (!MgMinigameTabOpen(slot1))
        return;
    if (MgDisplayTakenByOther(slot1, kCmdsGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kCmdsGameId))
        MgTakeDisplay(slot1, kCmdsGameId);
    if (!MgRunningSession(kCmdsGameId))
        return;
    Paint(slot1, false);
}
