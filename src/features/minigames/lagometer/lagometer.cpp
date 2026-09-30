// lagometer: live server tick / drain diagnostic on the minigames layout tab.

#include "cvar.h"
#include "lagometer.h"
#include "lagometer_logic.h"

#include "buddy_import.h"
#include "log.h"
#include "../minigames_api.h"

#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
#include "../../cpu_optimizations/cmdtext_parking/cmdpark.h"
#endif

#include <cstdlib>
#include <cstring>

namespace {

constexpr unsigned kCvarValueOfs = 0x18;

bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};
constexpr char kLagGameId[] = "lag";

void* g_cvDedicated = nullptr;

float CvarF(const char* name, float dflt) {
    void* cv = Buddy_GetEngineCvar(name, nullptr, 0, nullptr);
    if (!cv)
        return dflt;
    return *reinterpret_cast<volatile float*>(static_cast<char*>(cv) + kCvarValueOfs);
}

int CvarI(const char* name, int dflt) {
    return static_cast<int>(CvarF(name, static_cast<float>(dflt)));
}

LagSnapshot ReadSnapshot() {
    LagSnapshot s;
    s.drainLastMs = CvarF("_sofbuddy_cmdpark_cbuf_last", 0.0f);
    s.drainMaxMs = CvarF("_sofbuddy_cmdpark_cbuf_max", 0.0f);
    s.lateAvgMs = CvarF("_sofbuddy_tickpace_late_avg", 0.0f);
    s.highclamps = static_cast<long long>(CvarF("_sofbuddy_highclamps", 0.0f));
    s.lowclamps = static_cast<long long>(CvarF("_sofbuddy_lowclamps", 0.0f));
    s.clampLastMs = CvarI("_sofbuddy_clamp_last", 0);
    s.clampLostMs = static_cast<long long>(CvarF("_sofbuddy_clamp_lost_ms", 0.0f));
    s.clampAvgMs = CvarF("_sofbuddy_clamp_avg", 0.0f);
    s.lowclampWorstMs = CvarI("_sofbuddy_lowclamp_worst", 0);
    s.tickpaceSaved = static_cast<long long>(CvarF("_sofbuddy_tickpace_saved", 0.0f));
    s.parkDefers = static_cast<long long>(CvarF("_sofbuddy_cmdpark_defers", 0.0f));
    s.cbufBytes = CvarI("_sofbuddy_cmdpark_cbuf_cursize", 0);
    s.cbufFillMax = CvarI("_sofbuddy_cmdpark_cbuf_fill_max", 0);
    s.cmdcostMaxMs = CvarF("_sofbuddy_cmdcost_max", 0.0f);
#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
    s.parkBytes = cmdpark::ParkBytes();
#endif
    if (!g_cvDedicated)
        g_cvDedicated = Buddy_GetEngineCvar("dedicated", "0", 0, nullptr);
    s.dedicated = static_cast<int>(Buddy_ReadCvarValue(g_cvDedicated, 0.0f));
    s.cpuopt = CvarI("_sofbuddy_cpuopt", 1);
    s.tickpace = CvarI("_sofbuddy_tickpace", 1);
    s.tickpaceSettle = CvarI("_sofbuddy_tickpace_settle", 1);
    s.spinMs = CvarI("_sofbuddy_tickpace_spin_ms", 0);
    s.cmdpark = CvarI("_sofbuddy_cmdpark", 1);
    s.cmdparkStrict = CvarI("_sofbuddy_cmdpark_strict", 1);
    s.reserveMs = CvarI("_sofbuddy_cmdpark_reserve_ms", 0);
    s.qpc = CvarI("_sofbuddy_qpc", 1);
    return s;
}

void RefreshLagCanvas(int slot1, MgCanvas& c) {
    LagRender(ReadSnapshot(), c);
    MgPutLayoutCache(slot1, kLagGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kLagGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        MgTakeDisplay(slot1, kLagGameId);
        return;
    }
    if (!MgDisplayOwnedBy(slot1, kLagGameId))
        return;
    MgCanvas c;
    MgCanvasClear(c);
    MgPutLayoutCache(slot1, kLagGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kLagGameId, c);
    MgReleaseDisplay(slot1, kLagGameId);
}

extern "C" void __cdecl lag_Show_f();

void OnSessionEnd() {
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (!g_armed[s])
            continue;
        SetArmed(s, false);
    }
}

void OnLagClientCmd(int slot1) {
    if (!Lag_Enabled()) {
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, "Lagometer disabled on this server\n");
        return;
    }
    const bool on = !g_armed[slot1];
    SetArmed(slot1, on);
    if (void* ent = MgEdictForSlot(slot1))
        Buddy_ClientPrintf(ent, 2, "Lagometer %s (+use+score to view)\n", on ? "armed" : "off");
}

void LagTryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOpsPrimary = {"sofbuddy_lag", OnLagClientCmd, nullptr, OnSessionEnd};
    static const MgGameOps kOpsLag = {"lag", OnLagClientCmd, nullptr, nullptr};
    static const MgGameOps kOpsDot = {".lag", OnLagClientCmd, nullptr, nullptr};
    const bool a = MgRegisterGame(&kOpsPrimary);
    const bool b = MgRegisterGame(&kOpsLag);
    const bool c = MgRegisterGame(&kOpsDot);
    if (!a && !b && !c) {
        PrintOut(PRINT_BAD, "[lagometer] MgRegisterGame failed (table full?)\n");
        return;
    }
    MgRegisterConsoleCommand("lag_show", reinterpret_cast<void*>(&lag_Show_f));
    g_registered = true;
    PrintOut(PRINT_LOG, "[lagometer] registered (lag=%d sofbuddy_lag=%d .lag=%d)\n", b ? 1 : 0,
             a ? 1 : 0, c ? 1 : 0);
}

}  // namespace

void lag_EnsureRegistered() {
    LagTryRegister();
}

extern "C" void __cdecl lag_Show_f() {
    if (!Lag_Enabled() || !MgEnabled())
        return;
    LagTryRegister();
    if (MgArgc() < 2) {
        Buddy_DebugPrintf("usage: lag_show <slot 0-based>\n");
        return;
    }
    const int slot = MgUserToInternal(std::atoi(MgArgv(1)));
    if (!slot || !MgSlotSpawned(slot)) {
        Buddy_DebugPrintf("[lagometer] slot not spawned\n");
        return;
    }
    SetArmed(slot, true);
    Buddy_DebugPrintf("[lagometer] armed slot %d — player opens with +use+score\n",
                      MgInternalToUser(slot));
}

void lag_OnGameDllLoaded(void* gameExport) {
    (void)gameExport;
    LagTryRegister();
}

void lag_SvFramePost(int msec) {
    (void)msec;
}

void lag_ClientEndServerFramePost(void* ent) {
    if (!Lag_Enabled() || !MgEnabled() || !ent)
        return;
    const int slot = MgSlotForEdict(ent);
    if (slot < 1 || !g_armed[slot] || !MgRunningSession(kLagGameId))
        return;
    if (!MgDisplayOwnedBy(slot, kLagGameId))
        MgTakeDisplay(slot, kLagGameId);
    MgCanvas c;
    RefreshLagCanvas(slot, c);
}

void lag_OnMinigameTabOpened(int slot1) {
    if (!Lag_Enabled() || !MgEnabled() || slot1 < 1)
        return;
    lag_EnsureRegistered();
    if (MgDisplayTakenByOther(slot1, kLagGameId))
        return;
    SetArmed(slot1, true);
    MgCanvas c;
    RefreshLagCanvas(slot1, c);
}
