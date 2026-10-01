// lagometer: tick budget breakdown HUD on the minigames layout tab.

#include "cvar.h"
#include "lagometer.h"
#include "lagometer_logic.h"
#include "../../profiles/cvar.h"

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
    float pending_clcmove_ms = 0.0f;
    float pending_readpackets_wall_ms = 0.0f;
    float pending_send_ms = 0.0f;
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
    double runframe_end_ms = 0.0;
    bool in_readpackets = false;
    double readpackets_start_ms = 0.0;
    int pending_clcmove_calls = 0;
    int clcmove_sp = 0;
    int send_depth = 0;
    double send_t0 = 0.0;
    double clcmove_t0[kLagClcMoveStack] = {};
    LARGE_INTEGER qpc_freq = {};
    bool clock_ok = false;
};

static LagometerTrack g_track;
static int g_viewRank[kMgMaxSlots + 1] = {};
static bool g_viewLive[kMgMaxSlots + 1] = {};
static bool g_live_updated = false;

using tClientConnect = int(__cdecl*)(void* ent, char* userinfo);
using tClientBegin = void(__cdecl*)(void* ent);
using tClientUserinfo = void(__cdecl*)(void* ent, char* userinfo, int not_first);
using tClientDisconnect = void(__cdecl*)(void* ent);
// ge+0x20 clientPreConnect(edict*) — one arg on retail gamex86 (see GE table / IDA @ 0x500F4D70).
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
    g_track.top_n = 0;
    g_track.live = {};
    g_track.tick_armed = false;
    g_track.tick_body = false;
    g_track.in_readpackets = false;
    g_track.clcmove_sp = 0;
    g_track.pending_clcmove_ms = 0.0f;
    g_track.pending_readpackets_wall_ms = 0.0f;
    g_track.pending_clcmove_calls = 0;
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

static void CommitTickSample(float sim_ms, float engine_ms, float send_ms, float clcmove_ms,
                             float buffer_ms, int highclamp_lost_ms, const char* event_tag,
                             int sv_framenum) {
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

    LagTickSample sample;
    sample.sim_ms = sim_raw;
    sample.engine_ms = eng_raw;
    sample.send_ms = send_raw;
    sample.buffer_ms = buf_raw;
    sample.clcmove_ms = move_raw;
    sample.raw_total_ms = raw_total;
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

static LagSnapshot ReadView(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_viewLive[slot1]) {
        SyncMap();
        LagSnapshot snapshot;
        if (!g_track.live.valid)
            return snapshot;
        FillSnapshot(g_track.live, -1, snapshot);
        snapshot.live = true;
        snapshot.rank = 0;
        return snapshot;
    }
    const int rank = (slot1 >= 1 && slot1 <= kMgMaxSlots) ? g_viewRank[slot1] : 0;
    return ReadSnapshot(rank);
}

namespace {

bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};
constexpr char kLagGameId[] = "mg_lag";

void UpdateLagCache(int slot1) {
    MgCanvas c;
    LagRender(ReadView(slot1), c);
    MgPutLayoutCache(slot1, kLagGameId, c);
}

void PushLag(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    LagRender(ReadView(slot1), c);
    MgPutLayoutCache(slot1, kLagGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kLagGameId, c);
}

static bool LagWord(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb + 32);
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
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
    if (MgArgc() >= 2) {
        const char* sub = MgArgv(1);
        if (!g_armed[slot1])
            SetArmed(slot1, true);
        if (LagWord(sub, "live")) {
            g_viewLive[slot1] = true;
            PushLag(slot1);
            if (void* ent = MgEdictForSlot(slot1))
                Buddy_ClientPrintf(ent, 2, "Lagometer live\n");
            return;
        }
        g_viewLive[slot1] = false;
        int& r = g_viewRank[slot1];
        const int n = g_track.top_n;
        if (n <= 0) {
            if (void* ent = MgEdictForSlot(slot1))
                Buddy_ClientPrintf(ent, 2, "Lagometer has no ticks yet\n");
            return;
        }
        if (LagWord(sub, "max"))
            r = 0;
        else if (LagWord(sub, "min"))
            r = n > 0 ? n - 1 : 0;
        else if (LagWord(sub, "next")) {
            if (n > 0 && r < n - 1)
                ++r;
        } else if (LagWord(sub, "prev")) {
            if (r > 0)
                --r;
        } else {
            if (void* ent = MgEdictForSlot(slot1))
                Buddy_ClientPrintf(ent, 2, "mg_lag [live|next|prev|max|min]\n");
            return;
        }
        r = LagClampRank(r);
        PushLag(slot1);
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, "Lagometer #%d / %d\n", r + 1, n);
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
    static const MgGameOps kOps = {"mg_lag", OnLagClientCmd, nullptr, OnSessionEnd,
                                   "server tick times"};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[lagometer] MgRegisterGame failed (table full?)\n");
        return;
    }
    MgRegisterConsoleCommand("lag_show", reinterpret_cast<void*>(&lag_Show_f));
    g_registered = true;
    PrintOut(PRINT_LOG, "[lagometer] registered (.mg_lag)\n");
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
    g_track.pending_readpackets_wall_ms = 0.0f;
    g_track.pending_clcmove_calls = 0;
}

void lag_ReadPacketsPre() {
    if (!Lag_Enabled())
        return;
    InitClock();
    SyncMap();
    // SV_Frame calls ReadPackets even when the game tick will not run. clc_move
    // on those calls still belongs to the next committed tick.
    g_track.in_readpackets = true;
    g_track.readpackets_start_ms = NowMs();
    g_track.clcmove_sp = 0;
    if (!EngineTickWillRun())
        return;
    g_track.tick_armed = true;
    g_track.tick_body = false;
    g_track.pending_sim_ms = 0.0f;
    g_track.pending_send_ms = 0.0f;
    g_track.pending_buffer_ms = 0.0f;
    g_track.pending_highclamp_lost_ms = 0;
    g_track.pending_event_tag[0] = '\0';
    g_track.pending_sv_framenum = -1;
    g_track.runframe_end_ms = 0.0;
    g_track.send_depth = 0;
}

static void LagFlushClcMoveStack() {
    while (g_track.clcmove_sp > 0) {
        g_track.pending_clcmove_ms +=
            static_cast<float>(NowMs() - g_track.clcmove_t0[--g_track.clcmove_sp]);
    }
}

void lag_ReadPacketsPost() {
    if (!Lag_Enabled() || !g_track.in_readpackets)
        return;
    LagFlushClcMoveStack();
    const float wall = static_cast<float>(NowMs() - g_track.readpackets_start_ms);
    if (wall > 0.0f)
        g_track.pending_readpackets_wall_ms += wall;
    g_track.in_readpackets = false;
    if (!g_track.tick_armed)
        return;
    g_track.tick_body = true;
    g_track.tick_frame_start_ms = NowMs();
    g_track.runframe_end_ms = 0.0;
    volatile std::int32_t* fn = SvFramenum();
    if (fn)
        g_track.pending_sv_framenum = *fn;
}

void lag_NoteRunGameFrameBodyEnd() {
    if (!lag_TickBodyActive())
        return;
    g_track.runframe_end_ms = NowMs();
}

void lag_RunGameFramePost() {
    if (!lag_TickBodyActive())
        return;
    g_track.runframe_end_ms = NowMs();
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

void lag_NoteSimFrameWallMs(float wall_ms) {
    if (!lag_TickBodyActive() || wall_ms < 0.0f)
        return;
    g_track.pending_sim_ms += wall_ms;
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

// Full clc_move cost: SV_ExecuteClientMessage per client packet. IDA SoF.exe
// @0x200636C0 shows why this is bigger than one SV_ClientThink: the clc_move
// branch does checksum + lastServerFrame + InPacket decrypt +
// 3x MSG_ReadDeltaUserCmd + COM_BlockSequenceCRCByte + every SV_ClientThink
// for that packet's backlog. Timing the outer dispatch captures all of it.
void lag_ClcMovePre(void*& client) {
    (void)client;
    if (!g_track.in_readpackets || g_track.clcmove_sp >= kLagClcMoveStack)
        return;
    g_track.clcmove_t0[g_track.clcmove_sp++] = NowMs();
    ++g_track.pending_clcmove_calls;
}

void lag_ClcMovePost(void* client) {
    (void)client;
    if (!g_track.in_readpackets || g_track.clcmove_sp <= 0)
        return;
    g_track.pending_clcmove_ms +=
        static_cast<float>(NowMs() - g_track.clcmove_t0[--g_track.clcmove_sp]);
}

void lag_SendClientMessagesPre() {
    if (!lag_TickBodyActive())
        return;
    if (g_track.send_depth++ == 0)
        g_track.send_t0 = NowMs();
}

void lag_SendClientMessagesPost() {
    if (!lag_TickBodyActive() || g_track.send_depth <= 0)
        return;
    if (--g_track.send_depth == 0)
        g_track.pending_send_ms += static_cast<float>(NowMs() - g_track.send_t0);
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
    if (framenum >= 0 && LagSkipFramenum(framenum)) {
        ClearPacketAccum();
        return;
    }
    const int warmup_want = Lag_MapWarmupFrames();
    if (warmup_want > g_track.map_warmup_target) {
        g_track.map_warmup_target = warmup_want;
        g_track.map_warmup_frames = warmup_want;
        g_track.top_n = 0;
        g_track.live = {};
        ClearPacketAccum();
    }
    if (g_track.map_warmup_frames > 0) {
        --g_track.map_warmup_frames;
        ClearPacketAccum();
        return;
    }

    LagApplyStickyEvent(framenum);

    float runframe_ms = 0.0f;
    if (g_track.runframe_end_ms > g_track.tick_frame_start_ms)
        runframe_ms =
            static_cast<float>(g_track.runframe_end_ms - g_track.tick_frame_start_ms);
    const float sim = g_track.pending_sim_ms;
    const float rp = g_track.pending_readpackets_wall_ms;
    float move = g_track.pending_clcmove_ms;
    if (rp > 0.0f && move > rp)
        move = rp;
    const float buf = g_track.pending_buffer_ms;
    const float send_ms = std::max(0.0f, g_track.pending_send_ms);
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
    CommitTickSample(sim, engine, send_ms, move, buf, g_track.pending_highclamp_lost_ms,
                     g_track.pending_event_tag, framenum);
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

