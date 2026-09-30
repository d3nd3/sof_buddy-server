// lagometer: tick budget breakdown HUD on the minigames layout tab.

#include "cvar.h"
#include "lagometer.h"
#include "lagometer_logic.h"

#include "buddy_import.h"
#include "log.h"
#include "../minigames_api.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

constexpr unsigned kRvaSvTime = 0x3A1F28;
constexpr unsigned kRvaSvsRealtime = 0x396DE4;
constexpr unsigned kRvaSvFramenum = 0x3A1F30;

struct LagTickSample {
    float sim_ms = 0.0f;
    float engine_ms = 0.0f;
    float clientthink_ms = 0.0f;
    float buffer_ms = 0.0f;
    float spare_ms = 100.0f;
    int sv_framenum = -1;
    bool valid = false;
};

// sv.framenum values to drop (_init / map script ticks). Read the HUD, then add here.
constexpr int kLagSkipSvFramenum[] = {};
constexpr int kLagSettleTicks = 3;

struct LagometerTrack {
    char map_id[64] = {};
    LagTickSample worst = {};
    float worst_used_ms = 0.0f;
    bool tick_armed = false;
    bool tick_body = false;
    float pending_sim_ms = 0.0f;
    float pending_clientthink_ms = 0.0f;
    float pending_buffer_ms = 0.0f;
    int settle = kLagSettleTicks;
    int last_sv_framenum = -1;
    int pending_sv_framenum = -1;
    std::uint32_t sv_time_at_pre = 0;
    double tick_frame_start_ms = 0.0;
    int clientthink_depth = 0;
    double clientthink_start_ms = 0.0;
    LARGE_INTEGER qpc_freq = {};
    bool clock_ok = false;
};

static LagometerTrack g_track;

static bool LagSkipFramenum(int framenum) {
    for (int skip : kLagSkipSvFramenum) {
        if (skip == framenum)
            return true;
    }
    return false;
}

static HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

static volatile std::uint32_t* SvTime() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    return reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<char*>(h) + kRvaSvTime);
}

static volatile std::uint32_t* SvsRealtime() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    return reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<char*>(h) + kRvaSvsRealtime);
}

static volatile std::int32_t* SvFramenum() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    return reinterpret_cast<volatile std::int32_t*>(reinterpret_cast<char*>(h) + kRvaSvFramenum);
}

static void InitClock() {
    if (g_track.clock_ok)
        return;
    if (QueryPerformanceFrequency(&g_track.qpc_freq) && g_track.qpc_freq.QuadPart > 0)
        g_track.clock_ok = true;
}

static double NowMs() {
    LARGE_INTEGER now = {};
    if (!g_track.clock_ok || !QueryPerformanceCounter(&now))
        return 0.0;
    return static_cast<double>(now.QuadPart) * 1000.0 /
           static_cast<double>(g_track.qpc_freq.QuadPart);
}

// Same gate as SV_Frame before SV_RunGameFrame: svs.realtime >= sv.time (unsigned).
static bool EngineTickWillRun() {
    volatile std::uint32_t* sv_time = SvTime();
    volatile std::uint32_t* realtime = SvsRealtime();
    if (!sv_time || !realtime)
        return false;
    return *realtime >= *sv_time;
}

static void ResetMapPeaks() {
    g_track.worst = LagTickSample{};
    g_track.worst_used_ms = 0.0f;
    g_track.tick_armed = false;
    g_track.tick_body = false;
    g_track.clientthink_depth = 0;
    g_track.settle = kLagSettleTicks;
    g_track.last_sv_framenum = -1;
}

static void SyncMap() {
    const char* id = MgMapChecksum();
    if (!id || !id[0])
        return;
    if (std::strncmp(g_track.map_id, id, sizeof(g_track.map_id)) == 0)
        return;
    std::strncpy(g_track.map_id, id, sizeof(g_track.map_id) - 1);
    g_track.map_id[sizeof(g_track.map_id) - 1] = '\0';
    ResetMapPeaks();
}

static bool LagFramenumContinuous(int framenum) {
    if (framenum < 0)
        return false;
    if (g_track.last_sv_framenum < 0)
        return true;
    return framenum == g_track.last_sv_framenum + 1;
}

static void CommitTickSample(float sim_ms, float engine_ms, float clientthink_ms, float buffer_ms,
                             int sv_framenum) {
    if (g_track.settle > 0) {
        --g_track.settle;
        return;
    }
    if (!LagFramenumContinuous(sv_framenum)) {
        g_track.settle = kLagSettleTicks;
        g_track.last_sv_framenum = sv_framenum;
        return;
    }
    g_track.last_sv_framenum = sv_framenum;

    float sim = std::max(0.0f, sim_ms);
    float eng = std::max(0.0f, engine_ms);
    float think = std::max(0.0f, clientthink_ms);
    float buf = std::max(0.0f, buffer_ms);
    float used = sim + eng + think + buf;
    if (used > kLagTickBudgetMs && used > 0.0f) {
        const float scale = kLagTickBudgetMs / used;
        sim *= scale;
        eng *= scale;
        think *= scale;
        buf *= scale;
    }

    LagTickSample sample;
    sample.sim_ms = sim;
    sample.engine_ms = eng;
    sample.buffer_ms = buf;
    sample.clientthink_ms = think;
    sample.sv_framenum = sv_framenum;
    sample.valid = true;
    LagNormalizeBreakdown(sample.sim_ms, sample.engine_ms, sample.buffer_ms, sample.clientthink_ms,
                          sample.spare_ms);
    const float used_ms = kLagTickBudgetMs - sample.spare_ms;
    if (!g_track.worst.valid || used_ms > g_track.worst_used_ms) {
        g_track.worst = sample;
        g_track.worst_used_ms = used_ms;
    }
}

LagSnapshot ReadSnapshot() {
    SyncMap();
    LagSnapshot snapshot;
    if (!g_track.worst.valid)
        return snapshot;
    snapshot.game_ms = g_track.worst.sim_ms;
    snapshot.engine_ms = g_track.worst.engine_ms;
    snapshot.cmd_ms = g_track.worst.buffer_ms;
    snapshot.shell_ms = g_track.worst.clientthink_ms;
    snapshot.spare_ms = g_track.worst.spare_ms;
    snapshot.server_frame = g_track.worst.sv_framenum;
    return snapshot;
}

namespace {

bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};
constexpr char kLagGameId[] = "lag";

void UpdateLagCache(int slot1) {
    MgCanvas c;
    LagRender(ReadSnapshot(), c);
    MgPutLayoutCache(slot1, kLagGameId, c);
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
    g_track = LagometerTrack{};
}

void lag_ReadPacketsPre() {
    if (!Lag_Enabled())
        return;
    InitClock();
    SyncMap();
    if (!EngineTickWillRun())
        return;
    g_track.tick_armed = true;
    g_track.tick_body = false;
    g_track.pending_sim_ms = 0.0f;
    g_track.pending_clientthink_ms = 0.0f;
    g_track.pending_buffer_ms = 0.0f;
    g_track.pending_sv_framenum = -1;
    g_track.clientthink_depth = 0;
}

void lag_ReadPacketsPost() {
    if (!Lag_Enabled() || !g_track.tick_armed)
        return;
    g_track.tick_body = true;
    g_track.tick_frame_start_ms = NowMs();
}

bool lag_TickBodyActive() {
    return Lag_Enabled() && g_track.tick_armed && g_track.tick_body;
}

void lag_NoteSimFrameWallMs(float wall_ms) {
    if (!lag_TickBodyActive() || wall_ms < 0.0f)
        return;
    g_track.pending_sim_ms += wall_ms;
}

void lag_SvClientThinkPre(void*& client, void*& cmd) {
    (void)client;
    (void)cmd;
    if (!lag_TickBodyActive())
        return;
    if (g_track.clientthink_depth++ == 0)
        g_track.clientthink_start_ms = NowMs();
}

void lag_SvClientThinkPost(void* client, void* cmd) {
    (void)client;
    (void)cmd;
    if (!lag_TickBodyActive() || g_track.clientthink_depth <= 0)
        return;
    if (--g_track.clientthink_depth == 0) {
        g_track.pending_clientthink_ms +=
            static_cast<float>(NowMs() - g_track.clientthink_start_ms);
    }
}

void lag_NoteTickCmdDrain(float cmd_ms) {
    if (!lag_TickBodyActive() || cmd_ms < 0.0f)
        return;
    g_track.pending_buffer_ms = cmd_ms;
}

void lag_SvFramePre(int& msec) {
    (void)msec;
    if (!Lag_Enabled())
        return;
    InitClock();
    SyncMap();
    volatile std::uint32_t* sv_time = SvTime();
    if (sv_time)
        g_track.sv_time_at_pre = *sv_time;
}

void lag_SvFramePost(int msec) {
    (void)msec;
    if (!Lag_Enabled() || !g_track.clock_ok)
        return;
    volatile std::uint32_t* sv_time = SvTime();
    if (!sv_time || *sv_time == g_track.sv_time_at_pre)
        return;
    if (!g_track.tick_body)
        return;

    int framenum = g_track.pending_sv_framenum;
    if (framenum < 0) {
        volatile std::int32_t* fn = SvFramenum();
        if (fn)
            framenum = *fn;
    }
    g_track.tick_armed = false;
    g_track.tick_body = false;
    if (framenum >= 0 && LagSkipFramenum(framenum))
        return;

    const float frame_ms = static_cast<float>(NowMs() - g_track.tick_frame_start_ms);
    const float sim = g_track.pending_sim_ms;
    const float think = g_track.pending_clientthink_ms;
    const float buf = g_track.pending_buffer_ms;
    float engine = frame_ms - sim - think - buf;
    if (engine < 0.0f)
        engine = 0.0f;
    if (sim > frame_ms)
        engine = 0.0f;
    CommitTickSample(sim, engine, think, buf, framenum);
}

void lag_MaintainForSlot(int slot1) {
    if (!Lag_Enabled() || !MgEnabled() || slot1 < 1 || !g_armed[slot1])
        return;
    if (!MgMinigameTabOpen(slot1))
        return;
    if (MgDisplayTakenByOther(slot1, kLagGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kLagGameId))
        MgTakeDisplay(slot1, kLagGameId);
    if (!MgRunningSession(kLagGameId))
        return;
    UpdateLagCache(slot1);
}

void lag_OnMinigameTabOpened(int slot1) {
    if (!Lag_Enabled() || !MgEnabled() || slot1 < 1)
        return;
    lag_EnsureRegistered();
    if (MgDisplayTakenByOther(slot1, kLagGameId))
        return;
    SetArmed(slot1, true);
    UpdateLagCache(slot1);
}
