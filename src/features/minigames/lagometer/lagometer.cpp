// lagometer: tick budget breakdown HUD on the minigames layout tab.

#include "cvar.h"
#include "lagometer.h"
#include "lagometer_logic.h"
#include "../../profiles/cvar.h"

#include "buddy_clock.h"
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
constexpr unsigned kGeClientPreConnect = 0x20;
constexpr unsigned kGeClientConnect = 0x24;
constexpr unsigned kGeClientBegin = 0x28;
constexpr unsigned kGeClientUserinfo = 0x2C;
constexpr unsigned kGeClientDisconnect = 0x30;
constexpr int kLagEventFramenumSlop = 4;

struct LagTickSample {
    float sim_ms = 0.0f;
    float engine_ms = 0.0f;
    float send_ms = 0.0f;
    float clcmove_ms = 0.0f;
    float buffer_ms = 0.0f;
    float spare_ms = 100.0f;
    float raw_total_ms = 0.0f;
    float wall_total_ms = 0.0f;
    float gap_ms = 0.0f;
    float wall_rp_ms = 0.0f;
    float wall_move_ms = 0.0f;
    float wall_run_ms = 0.0f;
    float cpu_rp_ms = 0.0f;
    float rp_sched_ms = 0.0f;
    float shell_wall_ms = 0.0f;
    float shell_sched_ms = 0.0f;
    int rp_calls = 0;
    int move_calls = 0;
    int pkt_in = 0;
    float rp_visit_max_wall_ms = 0.0f;
    float rp_visit_max_sched_ms = 0.0f;
    int msec_sum = 0;
    int highclamp_lost_ms = 0;
    char event_tag[kLagEventTagCap] = {};
    int sv_framenum = -1;
    bool valid = false;
};

// sv.framenum values to drop (_init / map script ticks). Read the HUD, then add here.
constexpr int kLagSkipSvFramenum[] = {};
constexpr int kLagSettleTicks = 3;
constexpr int kLagClcMoveStack = 16;
constexpr int kLagTopN = 10;

struct LagometerTrack {
    char map_id[64] = {};
    LagTickSample top[kLagTopN] = {};
    int top_n = 0;
    LagTickSample live = {};
    bool tick_armed = false;
    bool tick_body = false;
    float pending_sim_ms = 0.0f;
    float pending_sim_wall_ms = 0.0f;
    float pending_clcmove_ms = 0.0f;
    float pending_clcmove_wall_ms = 0.0f;
    float pending_readpackets_wall_ms = 0.0f;
    float pending_readpackets_qpc_ms = 0.0f;
    float pending_send_ms = 0.0f;
    float pending_send_wall_ms = 0.0f;
    float pending_buffer_ms = 0.0f;
    int pending_highclamp_lost_ms = 0;
    char pending_event_tag[kLagEventTagCap] = {};
    char sticky_event_tag[kLagEventTagCap] = {};
    int sticky_event_framenum = -1;
    int settle = kLagSettleTicks;
    int map_warmup_frames = 0;
    int map_warmup_target = 0;
    int last_sv_framenum = -1;
    int pending_sv_framenum = -1;
    std::uint32_t sv_time_at_pre = 0;
    double tick_frame_start_ms = 0.0;
    double tick_frame_start_wall_ms = 0.0;
    double runframe_end_ms = 0.0;
    double runframe_end_wall_ms = 0.0;
    bool in_readpackets = false;
    double readpackets_start_ms = 0.0;
    double readpackets_start_wall_ms = 0.0;
    int pending_clcmove_calls = 0;
    int pending_rp_calls = 0;
    int pending_pkt_in = 0;
    float pending_rp_visit_max_wall_ms = 0.0f;
    int pending_msec_sum = 0;
    int clcmove_sp = 0;
    int send_depth = 0;
    double send_t0 = 0.0;
    double send_t0_wall = 0.0;
    double clcmove_t0[kLagClcMoveStack] = {};
    double clcmove_t0_wall[kLagClcMoveStack] = {};
    bool clock_ok = false;
};

static LagometerTrack g_track;
static int g_viewRank[kMgMaxSlots + 1] = {};
static bool g_viewLive[kMgMaxSlots + 1] = {};
static bool g_viewDebug[kMgMaxSlots + 1] = {};
static bool g_live_updated = false;
static LagSnapshot g_lastLiveView;
static int g_liveHoldMove = 0;
static int g_liveHoldEng = 0;

using tClientConnect = int(__cdecl*)(void* ent, char* userinfo);
using tClientBegin = void(__cdecl*)(void* ent);
using tClientUserinfo = void(__cdecl*)(void* ent, char* userinfo, int not_first);
using tClientDisconnect = void(__cdecl*)(void* ent);
// ge+0x20 clientPreConnect(edict*) — one arg on retail gamex86 (see GE table / retail disassembly @ 0x500F4D70).
using tClientPreConnect = int(__cdecl*)(void* ent);

static tClientPreConnect g_orig_preconnect = nullptr;
static tClientConnect g_orig_connect = nullptr;
static tClientBegin g_orig_begin = nullptr;
static tClientUserinfo g_orig_userinfo = nullptr;
static tClientDisconnect g_orig_disconnect = nullptr;

static volatile std::int32_t* SvFramenum();

static int LagCurrentFramenum() {
    volatile std::int32_t* fn = SvFramenum();
    return fn ? *fn : -1;
}

static int __cdecl LagHkPreConnect(void* ent);
static int __cdecl LagHkConnect(void* ent, char* userinfo);
static void __cdecl LagHkBegin(void* ent);
static void __cdecl LagHkUserinfo(void* ent, char* userinfo, int not_first);
static void __cdecl LagHkDisconnect(void* ent);

static void LagNoteEvent(const char* tag) {
    if (!Lag_Enabled() || !tag || !tag[0])
        return;
    const int fn = LagCurrentFramenum();
    if (g_track.tick_armed) {
        LagAppendEventTag(g_track.pending_event_tag, sizeof(g_track.pending_event_tag), tag);
        return;
    }
    if (fn < 0)
        return;
    LagAppendEventTag(g_track.sticky_event_tag, sizeof(g_track.sticky_event_tag), tag);
    g_track.sticky_event_framenum = fn;
}

static void LagApplyStickyEvent(int commit_framenum) {
    if (commit_framenum < 0 || g_track.sticky_event_framenum < 0 ||
        !g_track.sticky_event_tag[0])
        return;
    const int d = commit_framenum - g_track.sticky_event_framenum;
    if (d < -kLagEventFramenumSlop || d > kLagEventFramenumSlop)
        return;
    LagAppendEventTag(g_track.pending_event_tag, sizeof(g_track.pending_event_tag),
                      g_track.sticky_event_tag);
}

static void LagClearStickyEvent() {
    g_track.sticky_event_tag[0] = '\0';
    g_track.sticky_event_framenum = -1;
}

static bool LagHookGe(void* game_export, unsigned ofs, void* hook, void** orig) {
    if (!game_export || !hook || !orig)
        return false;
    void** slot = reinterpret_cast<void**>(static_cast<char*>(game_export) + ofs);
    void* target = *slot;
    if (!target)
        return false;
    *orig = target;
    *slot = hook;
    return true;
}

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
    BuddyClockSetMode(BuddyClockMode::ThreadCpu);
    LARGE_INTEGER freq = {};
    if (BuddyClockInitQpc(freq)) {
        g_track.clock_ok = true;
        if (Lag_Enabled()) {
            PrintOut(PRINT_LOG, "[lagometer] clock %s%s\n", BuddyClockThreadCpuBackendLabel(),
                      Buddy_IsWine() ? " (wine)" : "");
        }
    }
}

void lag_PrimeClock() {
    InitClock();
}

static double NowCpuMs() {
    if (!g_track.clock_ok)
        return 0.0;
    return BuddyClockCpuNowMs();
}

static double NowWallMs() {
    if (!g_track.clock_ok)
        return 0.0;
    return BuddyClockWallNowMs();
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
    g_track.top_n = 0;
    g_track.live = {};
    g_lastLiveView = LagSnapshot{};
    g_liveHoldMove = 0;
    g_liveHoldEng = 0;
    g_track.tick_armed = false;
    g_track.tick_body = false;
    g_track.in_readpackets = false;
    g_track.clcmove_sp = 0;
    g_track.pending_clcmove_ms = 0.0f;
    g_track.pending_clcmove_wall_ms = 0.0f;
    g_track.pending_readpackets_wall_ms = 0.0f;
    g_track.pending_readpackets_qpc_ms = 0.0f;
    g_track.pending_clcmove_calls = 0;
    g_track.pending_rp_calls = 0;
    g_track.pending_pkt_in = 0;
    g_track.settle = kLagSettleTicks;
    g_track.map_warmup_target = Lag_MapWarmupFrames();
    g_track.map_warmup_frames = g_track.map_warmup_target;
    g_track.last_sv_framenum = -1;
    LagNoteEvent("mapchg");
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

static void LagMaybeLogSched(const LagTickSample& s) {
    if (s.gap_ms < 20.0f)
        return;
    static double last_log_ms = 0.0;
    const double now = BuddyClockWallNowMs();
    if (now > 0.0 && now - last_log_ms < 1000.0)
        return;
    last_log_ms = now;
    PrintOut(PRINT_LOG,
              "[lagometer] sched fn=%d gap=%.1f msec=%d rpN=%d pkt=%d rpMax=%.1f rpG=%.1f "
              "shellG=%.1f cpu=%.1f wall=%.1f\n",
              s.sv_framenum, s.gap_ms, s.msec_sum, s.rp_calls, s.pkt_in, s.rp_visit_max_wall_ms,
              s.rp_sched_ms, s.shell_sched_ms, s.raw_total_ms, s.wall_total_ms);
}

static void CommitTickSample(float sim_ms, float engine_ms, float send_ms, float clcmove_ms,
                             float buffer_ms, float wall_total_ms, float wall_rp_ms,
                             float wall_move_ms, float wall_run_ms, float cpu_rp_ms,
                             float rp_sched_ms, float shell_wall_ms, float shell_sched_ms,
                             int rp_calls, int move_calls, int pkt_in, float rp_visit_max_wall_ms,
                             int msec_sum, int highclamp_lost_ms, const char* event_tag,
                             int sv_framenum, bool record_busiest) {
    if (!LagFramenumContinuous(sv_framenum)) {
        g_track.settle = kLagSettleTicks;
        LagAppendEventTag(g_track.pending_event_tag, sizeof(g_track.pending_event_tag), "framenum_gap");
    }
    if (g_track.settle > 0) {
        LagAppendEventTag(g_track.pending_event_tag, sizeof(g_track.pending_event_tag), "settle");
        --g_track.settle;
    }
    g_track.last_sv_framenum = sv_framenum;

    const float sim_raw = std::max(0.0f, sim_ms);
    const float eng_raw = std::max(0.0f, engine_ms);
    const float send_raw = std::max(0.0f, send_ms);
    const float move_raw = std::max(0.0f, clcmove_ms);
    const float buf_raw = std::max(0.0f, buffer_ms);
    const float raw_total = sim_raw + eng_raw + send_raw + move_raw + buf_raw;
    // Zero-total is not a real tick measurement — keep the previous live sample.
    if (raw_total <= 0.0f)
        return;

    LagTickSample sample;
    sample.sim_ms = sim_raw;
    sample.engine_ms = eng_raw;
    sample.send_ms = send_raw;
    sample.buffer_ms = buf_raw;
    sample.clcmove_ms = move_raw;
    sample.raw_total_ms = raw_total;
    sample.wall_total_ms = std::max(0.0f, wall_total_ms);
    sample.gap_ms = sample.wall_total_ms - raw_total;
    if (sample.gap_ms < 0.0f)
        sample.gap_ms = 0.0f;
    sample.wall_rp_ms = std::max(0.0f, wall_rp_ms);
    sample.wall_move_ms = std::max(0.0f, wall_move_ms);
    sample.wall_run_ms = std::max(0.0f, wall_run_ms);
    sample.cpu_rp_ms = std::max(0.0f, cpu_rp_ms);
    sample.rp_sched_ms = std::max(0.0f, rp_sched_ms);
    sample.shell_wall_ms = std::max(0.0f, shell_wall_ms);
    sample.shell_sched_ms = std::max(0.0f, shell_sched_ms);
    sample.rp_calls = rp_calls;
    sample.move_calls = move_calls;
    sample.pkt_in = pkt_in;
    sample.rp_visit_max_wall_ms = std::max(0.0f, rp_visit_max_wall_ms);
    sample.msec_sum = msec_sum;
    sample.highclamp_lost_ms = std::max(0, highclamp_lost_ms);
    if (event_tag && event_tag[0]) {
        std::strncpy(sample.event_tag, event_tag, sizeof(sample.event_tag) - 1);
        sample.event_tag[sizeof(sample.event_tag) - 1] = '\0';
    }
    sample.sv_framenum = sv_framenum;
    sample.valid = true;
    float bar_sim = sim_raw;
    float bar_eng = eng_raw;
    float bar_buf = buf_raw;
    float bar_move = move_raw;
    LagNormalizeBreakdown(bar_sim, bar_eng, bar_buf, bar_move, sample.spare_ms);
    g_track.live = sample;
    g_live_updated = true;
    LagMaybeLogSched(sample);
    if (!record_busiest)
        return;
    int n = g_track.top_n;
    if (n == kLagTopN && raw_total <= g_track.top[n - 1].raw_total_ms)
        return;
    int at = 0;
    const int have = n < kLagTopN ? n : kLagTopN;
    while (at < have && g_track.top[at].raw_total_ms >= raw_total)
        ++at;
    if (at >= kLagTopN)
        return;
    const int last = n < kLagTopN ? n : kLagTopN - 1;
    for (int j = last; j > at; --j)
        g_track.top[j] = g_track.top[j - 1];
    g_track.top[at] = sample;
    if (n < kLagTopN)
        g_track.top_n = n + 1;
}

static int LagClampRank(int rank) {
    if (g_track.top_n <= 0)
        return 0;
    if (rank < 0)
        return 0;
    if (rank >= g_track.top_n)
        return g_track.top_n - 1;
    return rank;
}

static void FillSnapshot(const LagTickSample& s, int rank, LagSnapshot& snapshot) {
    snapshot.game_ms = s.sim_ms;
    snapshot.engine_ms = s.engine_ms;
    snapshot.send_ms = s.send_ms;
    snapshot.cmd_ms = s.buffer_ms;
    snapshot.clcmove_ms = s.clcmove_ms;
    snapshot.spare_ms = s.spare_ms;
    snapshot.raw_total_ms = s.raw_total_ms;
    snapshot.wall_total_ms = s.wall_total_ms;
    snapshot.gap_ms = s.gap_ms;
    snapshot.wall_rp_ms = s.wall_rp_ms;
    snapshot.wall_move_ms = s.wall_move_ms;
    snapshot.wall_run_ms = s.wall_run_ms;
    snapshot.cpu_rp_ms = s.cpu_rp_ms;
    snapshot.rp_sched_ms = s.rp_sched_ms;
    snapshot.shell_wall_ms = s.shell_wall_ms;
    snapshot.shell_sched_ms = s.shell_sched_ms;
    snapshot.rp_calls = s.rp_calls;
    snapshot.move_calls = s.move_calls;
    snapshot.pkt_in = s.pkt_in;
    snapshot.highclamp_lost_ms = s.highclamp_lost_ms;
    std::strncpy(snapshot.event_tag, s.event_tag, sizeof(snapshot.event_tag));
    snapshot.server_frame = s.sv_framenum;
    snapshot.rank = rank + 1;
    snapshot.rank_count = g_track.top_n;
    snapshot.live = false;
}

LagSnapshot ReadSnapshot(int rank) {
    SyncMap();
    LagSnapshot snapshot;
    if (g_track.top_n <= 0)
        return snapshot;
    rank = LagClampRank(rank);
    FillSnapshot(g_track.top[rank], rank, snapshot);
    return snapshot;
}

static bool ReadView(int slot1, LagSnapshot& out) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_viewLive[slot1]) {
        SyncMap();
        LagSnapshot sample;
        if (g_track.live.valid)
            FillSnapshot(g_track.live, -1, sample);
        if (!LagFillLiveView(g_track.live.valid, g_track.live.raw_total_ms, sample, out))
            return false;
        LagStabilizeLiveKeys(g_lastLiveView, out, g_liveHoldMove, g_liveHoldEng);
        out.lag_debug = g_viewDebug[slot1];
        g_lastLiveView = out;
        return true;
    }
    const int rank = (slot1 >= 1 && slot1 <= kMgMaxSlots) ? g_viewRank[slot1] : 0;
    out = ReadSnapshot(rank);
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        out.lag_debug = g_viewDebug[slot1];
    return g_track.top_n > 0;
}

namespace {

bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};
int g_viewSelection[kMgMaxSlots + 1] = {};
constexpr char kLagGameId[] = "mg_lag";

// True while any slot actually views the lagometer (owns it with the tab
// open). Tick instrumentation runs only then: opening the view resumes
// collection, leaving it pauses collection — nothing samples in background.
bool LagAnyViewer() {
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (MgDisplayOwnedBy(s, kLagGameId) && MgMinigameTabOpen(s))
            return true;
    }
    return false;
}

void UpdateLagCache(int slot1) {
    LagSnapshot view;
    if (!ReadView(slot1, view))
        return;  // keep prior canvas — never paint Sim 0.0
    MgCanvas c;
    LagRender(view, c, g_viewSelection[slot1], !Profiles_SlotHideTips(slot1 - 1));
    MgPutLayoutCache(slot1, kLagGameId, c);
}

void PushLag(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    LagSnapshot view;
    if (!ReadView(slot1, view))
        return;
    MgCanvas c;
    LagRender(view, c, g_viewSelection[slot1], !Profiles_SlotHideTips(slot1 - 1));
    MgPutLayoutCache(slot1, kLagGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kLagGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        if (g_viewSelection[slot1] < 0 || g_viewSelection[slot1] >= 2)
            g_viewSelection[slot1] = 0;
        MgTakeDisplay(slot1, kLagGameId);
        return;
    }
    if (!MgDisplayOwnedBy(slot1, kLagGameId))
        return;
    MgClearLayout(slot1, kLagGameId);
}

void TurnRank(int slot1, int delta) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1] || g_track.top_n <= 0)
        return;
    g_viewLive[slot1] = false;
    g_viewRank[slot1] = LagClampRank(g_viewRank[slot1] + delta);
    PushLag(slot1);
}

void MoveSelection(int slot1, int delta) {
    int& selected = g_viewSelection[slot1];
    if (selected < 0)
        selected = delta < 0 ? 1 : 0;
    else
        selected = (selected + delta + 2) % 2;
    PushLag(slot1);
}

void OnUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || slot1 < 1 || slot1 > kMgMaxSlots ||
        !g_armed[slot1] || !MgMinigameTabOpen(slot1) ||
        !MgDisplayOwnedBy(slot1, kLagGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    if (in->use && in->forwardEdge)
        MoveSelection(slot1, -1);
    else if (in->use && in->backEdge)
        MoveSelection(slot1, 1);
    else if (in->useRelease) {
        if (g_viewSelection[slot1] < 0)
            MoveSelection(slot1, 1);
        else
            TurnRank(slot1, g_viewSelection[slot1] == 0 ? -1 : 1);
    }
}

extern "C" void __cdecl lag_Show_f();

void OnSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_armed[slot1])
        g_armed[slot1] = false;
}

void OnLagClientCmd(int slot1) {
    if (!Lag_Enabled())
        return;
    // Highest admin level only (privilege 0). Navigation already gates via
    // MgGameLevel0Only, but enforce here too so no caller can arm it behind
    // the platform's back.
    if (slot1 < 1 || slot1 > kMgMaxSlots || !Profiles_IsLevel0Admin(slot1 - 1)) {
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, "Server admins only.\n");
        return;
    }
    const bool on = !g_armed[slot1];
    if (on) {
        g_viewRank[slot1] = 0;
        g_viewLive[slot1] = false;
        g_viewDebug[slot1] = false;
        g_viewSelection[slot1] = 0;
    }
    SetArmed(slot1, on);
    if (on)
        PushLag(slot1);
}

void LagTryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {"mg_lag", OnLagClientCmd, OnUserCmd, OnSessionEnd,
                                   "server tick times", "lagometer", true};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[lagometer] MgRegisterGame failed (table full?)\n");
        return;
    }
    MgRegisterConsoleCommand("lag_show", reinterpret_cast<void*>(&lag_Show_f));
    g_registered = true;
    PrintOut(PRINT_LOG, "[lagometer] registered (menu page)\n");
}

// Only live bypasses the minigame layout cadence (every 32 frames).
void RefreshLiveViews() {
    if (!g_live_updated)
        return;
    g_live_updated = false;
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (!g_armed[s] || !g_viewLive[s])
            continue;
        PushLag(s);
    }
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

static void LagInstallGeHooks(void* game_export) {
    if (!game_export || !Lag_Enabled())
        return;
    g_orig_preconnect = nullptr;
    g_orig_connect = nullptr;
    g_orig_begin = nullptr;
    g_orig_userinfo = nullptr;
    g_orig_disconnect = nullptr;
    LagHookGe(game_export, kGeClientPreConnect, reinterpret_cast<void*>(&LagHkPreConnect),
              reinterpret_cast<void**>(&g_orig_preconnect));
    LagHookGe(game_export, kGeClientConnect, reinterpret_cast<void*>(&LagHkConnect),
              reinterpret_cast<void**>(&g_orig_connect));
    LagHookGe(game_export, kGeClientBegin, reinterpret_cast<void*>(&LagHkBegin),
              reinterpret_cast<void**>(&g_orig_begin));
    LagHookGe(game_export, kGeClientUserinfo, reinterpret_cast<void*>(&LagHkUserinfo),
              reinterpret_cast<void**>(&g_orig_userinfo));
    LagHookGe(game_export, kGeClientDisconnect, reinterpret_cast<void*>(&LagHkDisconnect),
              reinterpret_cast<void**>(&g_orig_disconnect));
}

static int __cdecl LagHkPreConnect(void* ent) {
    const int rc = g_orig_preconnect ? g_orig_preconnect(ent) : 1;
    LagNoteEvent("preconnect");
    return rc;
}

static int __cdecl LagHkConnect(void* ent, char* userinfo) {
    const int rc = g_orig_connect ? g_orig_connect(ent, userinfo) : 1;
    LagNoteEvent("connect");
    return rc;
}

static void __cdecl LagHkBegin(void* ent) {
    if (g_orig_begin)
        g_orig_begin(ent);
    LagNoteEvent("spawn");
}

static void __cdecl LagHkUserinfo(void* ent, char* userinfo, int not_first) {
    if (g_orig_userinfo)
        g_orig_userinfo(ent, userinfo, not_first);
    LagNoteEvent("userinfo");
}

static void __cdecl LagHkDisconnect(void* ent) {
    if (g_orig_disconnect)
        g_orig_disconnect(ent);
    LagNoteEvent("disconnect");
}

void lag_OnGameDllLoaded(void* gameExport) {
    LagTryRegister();
    g_track = LagometerTrack{};
    LagInstallGeHooks(gameExport);
}

static void ClearPacketAccum() {
    g_track.pending_clcmove_ms = 0.0f;
    g_track.pending_clcmove_wall_ms = 0.0f;
    g_track.pending_readpackets_wall_ms = 0.0f;
    g_track.pending_readpackets_qpc_ms = 0.0f;
    g_track.pending_clcmove_calls = 0;
    g_track.pending_rp_calls = 0;
    g_track.pending_pkt_in = 0;
    g_track.pending_rp_visit_max_wall_ms = 0.0f;
    g_track.pending_msec_sum = 0;
}

int lag_NetGetPacketPost(int result, int sock, void* net_from, void* net_message) {
    (void)sock;
    (void)net_from;
    (void)net_message;
    if (Lag_Enabled() && g_track.in_readpackets && result)
        ++g_track.pending_pkt_in;
    return result;
}

void lag_ReadPacketsPre() {
    if (!Lag_Enabled() || !LagAnyViewer())
        return;
    InitClock();
    SyncMap();
    // SV_Frame calls ReadPackets even when the game tick will not run. clc_move
    // on those calls still belongs to the next committed tick.
    g_track.in_readpackets = true;
    g_track.readpackets_start_ms = NowCpuMs();
    g_track.readpackets_start_wall_ms = NowWallMs();
    g_track.clcmove_sp = 0;
    if (!EngineTickWillRun())
        return;
    g_track.tick_armed = true;
    g_track.tick_body = false;
    g_track.pending_sim_ms = 0.0f;
    g_track.pending_sim_wall_ms = 0.0f;
    g_track.pending_send_ms = 0.0f;
    g_track.pending_send_wall_ms = 0.0f;
    g_track.pending_buffer_ms = 0.0f;
    g_track.pending_highclamp_lost_ms = 0;
    g_track.pending_event_tag[0] = '\0';
    g_track.pending_sv_framenum = -1;
    g_track.runframe_end_ms = 0.0;
    g_track.runframe_end_wall_ms = 0.0;
    g_track.send_depth = 0;
}

static void LagFlushClcMoveStack() {
    while (g_track.clcmove_sp > 0) {
        const int i = --g_track.clcmove_sp;
        g_track.pending_clcmove_ms +=
            static_cast<float>(NowCpuMs() - g_track.clcmove_t0[i]);
        g_track.pending_clcmove_wall_ms +=
            static_cast<float>(NowWallMs() - g_track.clcmove_t0_wall[i]);
    }
}

void lag_ReadPacketsPost() {
    if (!Lag_Enabled() || !g_track.in_readpackets)
        return;
    LagFlushClcMoveStack();
    const float rp_cpu =
        static_cast<float>(NowCpuMs() - g_track.readpackets_start_ms);
    const float rp_wall =
        static_cast<float>(NowWallMs() - g_track.readpackets_start_wall_ms);
    if (rp_cpu > 0.0f)
        g_track.pending_readpackets_wall_ms += rp_cpu;
    if (rp_wall > 0.0f)
        g_track.pending_readpackets_qpc_ms += rp_wall;
    if (rp_wall >= g_track.pending_rp_visit_max_wall_ms)
        g_track.pending_rp_visit_max_wall_ms = rp_wall;
    ++g_track.pending_rp_calls;
    g_track.in_readpackets = false;
    if (!g_track.tick_armed)
        return;
    // Only start the body clock once per armed tick. Re-entering ReadPackets
    // while still armed must not wipe runframe_end / restart the window.
    if (!g_track.tick_body) {
        g_track.tick_body = true;
        g_track.tick_frame_start_ms = NowCpuMs();
        g_track.tick_frame_start_wall_ms = NowWallMs();
        g_track.runframe_end_ms = 0.0;
        g_track.runframe_end_wall_ms = 0.0;
        volatile std::int32_t* fn = SvFramenum();
        if (fn)
            g_track.pending_sv_framenum = *fn;
    }
}

void lag_NoteRunGameFrameBodyEnd() {
    if (!lag_TickBodyActive())
        return;
    g_track.runframe_end_ms = NowCpuMs();
    g_track.runframe_end_wall_ms = NowWallMs();
}

void lag_RunGameFramePost() {
    if (!lag_TickBodyActive())
        return;
    g_track.runframe_end_ms = NowCpuMs();
    g_track.runframe_end_wall_ms = NowWallMs();
    volatile std::int32_t* fn = SvFramenum();
    if (fn)
        g_track.pending_sv_framenum = *fn;
}

bool lag_TickArmed() {
    return Lag_Enabled() && g_track.tick_armed;
}

bool lag_TickBodyActive() {
    return Lag_Enabled() && g_track.tick_armed && g_track.tick_body;
}

void lag_NoteSimFrameMs(float cpu_ms, float wall_ms) {
    if (!lag_TickBodyActive() || cpu_ms < 0.0f)
        return;
    g_track.pending_sim_ms += cpu_ms;
    if (wall_ms >= 0.0f)
        g_track.pending_sim_wall_ms += wall_ms;
}

void lag_NoteHighclampLostMs(int lost_ms) {
    if (!lag_TickBodyActive() || lost_ms <= 0)
        return;
    if (lost_ms > g_track.pending_highclamp_lost_ms)
        g_track.pending_highclamp_lost_ms = lost_ms;
}

void lag_NoteGameEvent(const char* tag) {
    LagNoteEvent(tag);
}

void lag_PutClientInServerPost(void* ent) {
    (void)ent;
    LagNoteEvent("putclient");
}

short lag_RespawnPost(short result, void* ent) {
    (void)ent;
    LagNoteEvent("respawn");
    return result;
}

void lag_PlayerDiePost(void* self, void* inflictor, void* attacker, int damage, float* point) {
    (void)self;
    (void)inflictor;
    (void)attacker;
    (void)damage;
    (void)point;
    LagNoteEvent("death");
}

// Full clc_move cost: SV_ExecuteClientMessage per client packet. retail disassembly SoF.exe
// @0x200636C0 shows why this is bigger than one SV_ClientThink: the clc_move
// branch does checksum + lastServerFrame + InPacket decrypt +
// 3x MSG_ReadDeltaUserCmd + COM_BlockSequenceCRCByte + every SV_ClientThink
// for that packet's backlog. Timing the outer dispatch captures all of it.
void lag_ClcMovePre(void*& client) {
    (void)client;
    if (!g_track.in_readpackets || g_track.clcmove_sp >= kLagClcMoveStack)
        return;
    g_track.clcmove_t0[g_track.clcmove_sp] = NowCpuMs();
    g_track.clcmove_t0_wall[g_track.clcmove_sp++] = NowWallMs();
    ++g_track.pending_clcmove_calls;
}

void lag_ClcMovePost(void* client) {
    (void)client;
    if (!g_track.in_readpackets || g_track.clcmove_sp <= 0)
        return;
    const int i = --g_track.clcmove_sp;
    g_track.pending_clcmove_ms += static_cast<float>(NowCpuMs() - g_track.clcmove_t0[i]);
    g_track.pending_clcmove_wall_ms +=
        static_cast<float>(NowWallMs() - g_track.clcmove_t0_wall[i]);
}

void lag_SendClientMessagesPre() {
    if (!lag_TickBodyActive())
        return;
    if (g_track.send_depth++ == 0) {
        g_track.send_t0 = NowCpuMs();
        g_track.send_t0_wall = NowWallMs();
    }
}

void lag_SendClientMessagesPost() {
    if (!lag_TickBodyActive() || g_track.send_depth <= 0)
        return;
    if (--g_track.send_depth == 0) {
        g_track.pending_send_ms += static_cast<float>(NowCpuMs() - g_track.send_t0);
        g_track.pending_send_wall_ms +=
            static_cast<float>(NowWallMs() - g_track.send_t0_wall);
    }
}

void lag_NoteTickCmdDrain(float cmd_ms) {
    if (!lag_TickBodyActive() || cmd_ms < 0.0f)
        return;
    g_track.pending_buffer_ms = cmd_ms;
}

void lag_SvFramePre(int& msec) {
    if (!Lag_Enabled() || !LagAnyViewer())
        return;
    if (msec > 0 && g_track.pending_rp_calls > 0)
        g_track.pending_msec_sum += msec;
    InitClock();
    SyncMap();
    volatile std::uint32_t* sv_time = SvTime();
    if (sv_time)
        g_track.sv_time_at_pre = *sv_time;
}

void lag_SvFramePost(int msec) {
    (void)msec;
    if (!Lag_Enabled() || !g_track.clock_ok || !LagAnyViewer())
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
    if (framenum >= 0 && LagSkipFramenum(framenum)) {
        ClearPacketAccum();
        return;
    }
    const int warmup_want = Lag_MapWarmupFrames();
    if (warmup_want > g_track.map_warmup_target) {
        g_track.map_warmup_target = warmup_want;
        g_track.map_warmup_frames = warmup_want;
        g_track.top_n = 0;  // busiest only — do not clear live
    }
    const bool record_busiest = !LagWarmupBlocksBusiest(g_track.map_warmup_frames);
    if (!record_busiest)
        --g_track.map_warmup_frames;

    LagApplyStickyEvent(framenum);

    float runframe_ms = 0.0f;
    float runframe_wall_ms = 0.0f;
    if (g_track.runframe_end_ms > g_track.tick_frame_start_ms)
        runframe_ms =
            static_cast<float>(g_track.runframe_end_ms - g_track.tick_frame_start_ms);
    if (g_track.runframe_end_wall_ms > g_track.tick_frame_start_wall_ms)
        runframe_wall_ms = static_cast<float>(g_track.runframe_end_wall_ms -
                                              g_track.tick_frame_start_wall_ms);
    const float sim = g_track.pending_sim_ms;
    const float sim_wall = g_track.pending_sim_wall_ms;
    const float rp = g_track.pending_readpackets_wall_ms;
    const float rp_wall = g_track.pending_readpackets_qpc_ms;
    float move = g_track.pending_clcmove_ms;
    float move_wall = g_track.pending_clcmove_wall_ms;
    if (rp > 0.0f && move > rp)
        move = rp;
    if (rp_wall > 0.0f && move_wall > rp_wall)
        move_wall = rp_wall;
    const float buf = g_track.pending_buffer_ms;
    const float send_ms = std::max(0.0f, g_track.pending_send_ms);
    const float send_wall = std::max(0.0f, g_track.pending_send_wall_ms);
    if (move >= 40.0f && g_track.pending_clcmove_calls > 1) {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "move%d", g_track.pending_clcmove_calls);
        LagAppendEventTag(g_track.pending_event_tag, sizeof(g_track.pending_event_tag), tag);
    }
    // Move is SV_ExecuteClientMessage since the previous commit, including
    // ReadPackets on SV_Frames that did not run a game tick. The rest of that
    // ReadPackets wall joins the post-ReadPackets engine sliver. Buffer stays
    // the cmdpark drain and is not part of engine.
    float engine = runframe_ms - sim;
    if (engine < 0.0f)
        engine = 0.0f;
    const float rp_other = rp - move;
    if (rp_other > 0.0f)
        engine += rp_other;
    float engine_wall = runframe_wall_ms - sim_wall;
    if (engine_wall < 0.0f)
        engine_wall = 0.0f;
    const float rp_other_wall = rp_wall - move_wall;
    if (rp_other_wall > 0.0f)
        engine_wall += rp_other_wall;
    const float wall_total =
        sim_wall + engine_wall + send_wall + move_wall + buf;
    const float cpu_rp = rp;
    const float rp_sched = std::max(0.0f, rp_wall - cpu_rp);
    const float shell_wall = std::max(0.0f, rp_wall - move_wall);
    const float shell_cpu = std::max(0.0f, cpu_rp - move);
    const float shell_sched = std::max(0.0f, shell_wall - shell_cpu);
    CommitTickSample(sim, engine, send_ms, move, buf, wall_total, rp_wall, move_wall,
                     runframe_wall_ms, cpu_rp, rp_sched, shell_wall, shell_sched,
                     g_track.pending_rp_calls, g_track.pending_clcmove_calls,
                     g_track.pending_pkt_in, g_track.pending_rp_visit_max_wall_ms,
                     g_track.pending_msec_sum, g_track.pending_highclamp_lost_ms,
                     g_track.pending_event_tag, framenum, record_busiest);
    ClearPacketAccum();
    LagClearStickyEvent();
    RefreshLiveViews();
}

void lag_MaintainForSlot(int slot1) {
    if (slot1 >= 1 && !Profiles_SlotActive(slot1 - 1)) {
        if (g_armed[slot1])
            SetArmed(slot1, false);
        return;
    }
    // Highest admin level only: close the view when the slot stops being a
    // server admin (mirrors the cmds/cvars pages).
    if (slot1 >= 1 && !Profiles_IsLevel0Admin(slot1 - 1)) {
        if (g_armed[slot1])
            SetArmed(slot1, false);
        return;
    }
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

