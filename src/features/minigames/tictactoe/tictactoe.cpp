// tictactoe: noughts-and-crosses on the minigames platform. Rules +
// drawing here; routing, layout channel and visibility in ../minigames.

#include "cvar.h"
#include "tictactoe_logic.h"

#include "buddy_import.h"
#include "log.h"
#include "../minigames_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kEndDisplayMs = 5000;
constexpr char kTttGameId[] = "ttt";

TttGame g_game;
bool g_active = false;
bool g_dirty = false;
bool g_registered = false;
bool g_resultPending = false;
int g_endMsLeft = 0;
int g_resultSlotX = 0;
int g_resultSlotO = 0;
char g_resultLine[96] = {};

void TttTryRegister();

void PushAll() {
    MgCanvas c;
    const int cur = (g_active && !g_resultPending && !g_game.over) ? g_game.cursor : -1;
    TttRenderBoard(g_game, c, g_resultLine[0] ? g_resultLine : nullptr, cur);
    MgPushLayout(g_game.slotX, kTttGameId, c);
    MgPushLayout(g_game.slotO, kTttGameId, c);
}

void ResetState() {
    g_active = false;
    g_dirty = false;
    g_resultPending = false;
    g_endMsLeft = 0;
    g_resultSlotX = 0;
    g_resultSlotO = 0;
    g_resultLine[0] = '\0';
    g_game = {};
}

void ShowIdleForPlayers() {
    if (g_resultSlotX >= 1)
        MgShowIdleLayout(g_resultSlotX, kTttGameId);
    if (g_resultSlotO >= 1 && g_resultSlotO != g_resultSlotX)
        MgShowIdleLayout(g_resultSlotO, kTttGameId);
}

void CompleteResult() {
    if (!g_resultPending)
        return;
    ShowIdleForPlayers();
    ResetState();
}

void ScheduleResult(const char* statusOverride) {
    if (g_resultPending)
        return;
    g_active = false;
    g_resultPending = true;
    g_endMsLeft = kEndDisplayMs;
    g_resultSlotX = g_game.slotX;
    g_resultSlotO = g_game.slotO;
    g_resultLine[0] = '\0';
    if (statusOverride && statusOverride[0])
        std::strncpy(g_resultLine, statusOverride, sizeof(g_resultLine) - 1);
    g_resultLine[sizeof(g_resultLine) - 1] = '\0';
    g_dirty = true;
    PushAll();
}

void EndGameImmediate() {
    if (g_resultSlotX >= 1 || g_game.slotX >= 1) {
        const int sx = g_resultSlotX ? g_resultSlotX : g_game.slotX;
        const int so = g_resultSlotO ? g_resultSlotO : g_game.slotO;
        if (sx >= 1)
            MgClearLayout(sx, kTttGameId);
        if (so >= 1 && so != sx)
            MgClearLayout(so, kTttGameId);
    }
    ResetState();
}

bool StartGame(int slotX, int slotO) {
    if (slotX < 1 || slotO < 1 || slotX == slotO)
        return false;
    if (!MgSlotSpawned(slotX) || !MgSlotSpawned(slotO))
        return false;
    if (!MgEdictForSlot(slotX) || !MgEdictForSlot(slotO))
        return false;
    TttTryRegister();
    EndGameImmediate();
    TttReset(g_game, slotX, slotO);
    g_active = true;
    MgShowLayout(slotX, kTttGameId, true);
    MgShowLayout(slotO, kTttGameId, true);
    PushAll();
    g_dirty = false;
    Buddy_BroadcastPrintf(2,
                          "TicTacToe: slot %d (X) vs slot %d (O) - WASD move, fire to place, or ttt <1-9>\n",
                          MgInternalToUser(slotX), MgInternalToUser(slotO));
    return true;
}

bool PlayCell(int slot1, int cell1) {
    if (!g_active || g_game.over || g_resultPending)
        return false;
    const int mark = (slot1 == g_game.slotX) ? kTttX
                     : (slot1 == g_game.slotO) ? kTttO
                                               : 0;
    if (mark == 0)
        return false;
    if ((g_game.turn == kTttX && mark != kTttX) ||
        (g_game.turn == kTttO && mark != kTttO))
        return false;
    if (!TttPlay(g_game, cell1 - 1))
        return false;
    g_dirty = true;
    if (g_game.over) {
        if (g_game.winner == kTttDraw)
            Buddy_BroadcastPrintf(2, "TicTacToe: draw!\n");
        else
            Buddy_BroadcastPrintf(2, "TicTacToe: %c (slot %d) wins!\n",
                                  g_game.winner == kTttX ? 'X' : 'O',
                                  MgInternalToUser(g_game.winner == kTttX ? g_game.slotX
                                                                          : g_game.slotO));
        ScheduleResult(nullptr);
    }
    return true;
}

int ParseCell(const char* s) {
    if (!s || !s[0] || s[1] != '\0')
        return 0;
    if (s[0] < '1' || s[0] > '9')
        return 0;
    return s[0] - '0';
}

void OnClientCmd(int slot1) {
    if (!Ttt_Enabled())
        return;
    const int cell = ParseCell(MgArgv(1));
    void* ent = MgEdictForSlot(slot1);
    if (g_resultPending) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "TicTacToe: game over\n");
        return;
    }
    if (!g_active) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "TicTacToe: no game running (admin: ttt_start <slotX> <slotO>)\n");
    } else if (g_game.over) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "TicTacToe: game over\n");
    } else if (cell < 1) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "TicTacToe: use ttt <1-9> - empty cells show their number\n");
    } else if (!PlayCell(slot1, cell)) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "TicTacToe: illegal move (not your turn or cell taken)\n");
    }
}

extern "C" void __cdecl ttt_Start_f();
extern "C" void __cdecl ttt_End_f();
extern "C" void __cdecl ttt_Move_f();

extern "C" void __cdecl ttt_Start_f() {
    if (!Ttt_Enabled())
        return;
    TttTryRegister();
    if (MgArgc() < 3) {
        Buddy_DebugPrintf("usage: ttt_start <slotX 0-based> <slotO 0-based>\n");
        return;
    }
    const int ux = std::atoi(MgArgv(1));
    const int uo = std::atoi(MgArgv(2));
    const int maxc = MgMaxClients();
    if (ux < 0 || uo < 0 || ux >= maxc || uo >= maxc) {
        Buddy_DebugPrintf("[tictactoe] ttt_start: slot out of range (0-%d)\n", maxc - 1);
        return;
    }
    if (ux == uo) {
        Buddy_DebugPrintf("[tictactoe] ttt_start: slots must differ\n");
        return;
    }
    const int slotX = MgUserToInternal(ux);
    const int slotO = MgUserToInternal(uo);
    if (!MgSlotSpawned(slotX) || !MgSlotSpawned(slotO)) {
        Buddy_DebugPrintf("[tictactoe] ttt_start: both players must be spawned\n");
        return;
    }
    if (!StartGame(slotX, slotO))
        Buddy_DebugPrintf("[tictactoe] ttt_start failed\n");
}

extern "C" void __cdecl ttt_End_f() {
    if (!Ttt_Enabled() || (!g_active && !g_resultPending))
        return;
    ScheduleResult("GAME OVER");
    Buddy_BroadcastPrintf(2, "TicTacToe: game over\n");
}

extern "C" void __cdecl ttt_Move_f() {
    if (!Ttt_Enabled() || !g_active || g_resultPending)
        return;
    if (MgArgc() < 3) {
        Buddy_DebugPrintf("usage: ttt_move <slot 0-based> <cell 1-9>\n");
        return;
    }
    const int cell = std::atoi(MgArgv(2));
    const int slot = MgUserToInternal(std::atoi(MgArgv(1)));
    if (cell < 1 || cell > 9 || !slot || !PlayCell(slot, cell))
        Buddy_DebugPrintf("[tictactoe] illegal move\n");
}

void OnUserCmd(int slot1, MgUserCmdInput* in) {
    if (!Ttt_Enabled() || !in || !g_active || g_resultPending || g_game.over)
        return;
    if (slot1 != g_game.slotX && slot1 != g_game.slotO)
        return;
    in->consumed = true;
    const int turnSlot = (g_game.turn == kTttX) ? g_game.slotX : g_game.slotO;
    if (slot1 != turnSlot)
        return;
    bool moved = false;
    if (in->forwardEdge && g_game.cursor >= 3) {
        g_game.cursor -= 3;
        moved = true;
    }
    if (in->backEdge && g_game.cursor < 6) {
        g_game.cursor += 3;
        moved = true;
    }
    if (in->leftEdge && (g_game.cursor % 3) > 0) {
        g_game.cursor -= 1;
        moved = true;
    }
    if (in->rightEdge && (g_game.cursor % 3) < 2) {
        g_game.cursor += 1;
        moved = true;
    }
    if (moved)
        g_dirty = true;
    if (in->attackEdge)
        PlayCell(slot1, g_game.cursor + 1);
}

void OnSessionEnd() {
    if (!g_active && !g_resultPending)
        return;
    EndGameImmediate();
}

const MgGameOps kTttOps = {"ttt", OnClientCmd, OnUserCmd, OnSessionEnd};

void TttTryRegister() {
    if (!Ttt_Enabled() || g_registered)
        return;
    MgRegisterGame(&kTttOps);
    MgRegisterConsoleCommand("ttt_start", reinterpret_cast<void*>(&ttt_Start_f));
    MgRegisterConsoleCommand("ttt_end", reinterpret_cast<void*>(&ttt_End_f));
    MgRegisterConsoleCommand("ttt_move", reinterpret_cast<void*>(&ttt_Move_f));
    g_registered = true;
    PrintOut(PRINT_LOG, "[tictactoe] registered (_sofbuddy_ttt_enable 1)\n");
}

}  // namespace

void TttOnEnabled() {
    TttTryRegister();
}

void ttt_EnsureRegistered() {
    TttOnEnabled();
}

void ttt_OnGameDllLoaded(void* gameExport) {
    (void)gameExport;
    TttOnEnabled();
}

void ttt_SvFramePost(int msec) {
    if (!Ttt_Enabled() || !MgRunningSession(kTttGameId) || !g_resultPending)
        return;
    g_endMsLeft -= msec;
    if (g_endMsLeft <= 0)
        CompleteResult();
}

void ttt_ClientEndServerFramePost(void* ent) {
    if (!Ttt_Enabled() || !MgRunningSession(kTttGameId) || !ent)
        return;
    if (g_resultPending) {
        if (!MgSlotSpawned(g_resultSlotX) && !MgSlotSpawned(g_resultSlotO))
            CompleteResult();
        return;
    }
    if (!g_active)
        return;
    if (!MgSlotSpawned(g_game.slotX) || !MgSlotSpawned(g_game.slotO)) {
        ScheduleResult("Game aborted");
        Buddy_BroadcastPrintf(2, "TicTacToe: game aborted (player left)\n");
    }
}
