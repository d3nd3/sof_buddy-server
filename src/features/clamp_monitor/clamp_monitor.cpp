// Clamp monitor: measures server-tick starvation exactly, by hooking
// G_RunFrame (see detours.yaml) and reading sv.time/svs.realtime the moment
// the real game frame returns - i.e. strictly before SV_RunGameFrame's own
// "if (sv.time < svs.realtime) svs.realtime = sv.time;" runs on those same
// globals. That is an exact, per-event replica of the engine's own decision:
// no polling gaps, no inference (see README.md for the IDA trail).
//
// Everything lives on the engine's own thread: the hook samples, folds the
// sample into the rolling window, and publishes the output cvars in the same
// call. There is no background thread - an earlier design polled
// svs.realtime from one, which cannot work (the clamp is instantaneous and
// resets svs.realtime *upwards*, so a poll only ever sees it grow) and which
// also raced this file's own state.

#include "cvar.h"
#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdint>
#include <windows.h>

namespace {

// ---------------------------------------------------------------------------
// Engine globals in SoF.exe / SoF-spsv.exe (shared .text, base 0x20000000).
// Canonical tick state used by SV_Frame/SV_RunGameFrame; all three verified in
// IDA by cross-reference (SpawnServer writes all of them; SV_RunGameFrame
// reads sv_time/svs_realtime for the clamp). The similar-looking pair near
// 0x20004460 belongs to an unrelated dedicated-mode limiter and stays zero.
// ---------------------------------------------------------------------------
constexpr unsigned kRvaSvState     = 0x3A1F20;  // sv.state    (server_state_t)
constexpr unsigned kRvaSvTime      = 0x3A1F28;  // sv.time     (unsigned ms)
constexpr unsigned kRvaSvsRealtime = 0x396DE4;  // svs.realtime(int ms)

// server_state_t: ss_dead=0, ss_loading=1, ss_game=2 (confirmed in IDA:
// SpawnServer branches on `serverstate == 2` to load the real .bsp).
constexpr std::int32_t kSvStateGame = 2;

// sv.time advances in 100ms steps (sv.time = sv.framenum * 100), so one tick
// is 100ms and the sample ring is indexed in ticks.
constexpr int kTicksPerSecond = 10;
constexpr int kMaxSamples = 16 * kTicksPerSecond;  // 16 s of history

// Ticks ignored after a discontinuity (map load, cinematic loop, first tick
// ever). SpawnServer resets sv.time to 1000 and svs.realtime to 0, then the
// engine folds the entire map-load duration into svs.realtime on the next
// SV_Frame - so the first real tick of a new map reports a multi-second
// "clamp" that is map loading, not server starvation. One tick is enough for
// the engine to delete it; two is margin.
constexpr int kSettleTicks = 2;

// Engine bprintf print level (q_shared.h) - NOT this shim's PRINT_* from
// log.h, which is for PrintOut (our own logger) only.
//
// SV_BroadcastPrintf (IDA @ 0x200618d0 in SoF.exe) filters per client with
// `if (printlevel < cl->messagelevel) skip;` before writing svc_print.
// PRINT_CHAT (3) is the highest level the engine defines, so it is the only
// one no client-side message filter can drop.
constexpr int kEnginePrintChat = 3;

constexpr DWORD kNotifyMinIntervalMs = 1000;   // at most one log line per second
constexpr float kDefaultNotifyMs = 5.0f;
constexpr float kDefaultWindowSec = 2.0f;
constexpr float kDefaultBroadcastIntervalSec = 30.0f;

// ---------------------------------------------------------------------------
// Engine global resolution - done once, not per tick.
// ---------------------------------------------------------------------------
struct EngineGlobals {
    volatile const std::uint32_t* svTime = nullptr;
    volatile const std::uint32_t* svsRealtime = nullptr;
    volatile const std::int32_t*  svState = nullptr;
};

EngineGlobals g_engine;
bool g_engineResolved = false;

HMODULE ExeMod() {
    // The RVAs above were reverse-engineered from the retail engine image.
    // Resolve it by name rather than via GetModuleHandleA(nullptr), which
    // returns whatever executable happens to host the process. Both shipped
    // engine binaries carry the same .text, so either name is correct.
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

bool IsSafeMemoryBlock(const void* ptr, std::size_t size) {
    auto addr = reinterpret_cast<std::uintptr_t>(ptr);
    if (!ptr || size == 0 || addr + size < addr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    return addr + size <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (!IsSafeMemoryBlock(dos, sizeof(IMAGE_DOS_HEADER)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!IsSafeMemoryBlock(nt, sizeof(IMAGE_NT_HEADERS)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    return rva <= nt->OptionalHeader.SizeOfImage && rva + size <= nt->OptionalHeader.SizeOfImage;
}

/** Resolves the three engine globals once. Returns false (quietly, after one
 *  log line) forever after if the engine image can't be validated. */
bool EngineGlobalsReady() {
    if (g_engineResolved)
        return g_engine.svTime != nullptr;
    g_engineResolved = true;

    HMODULE h = ExeMod();
    const bool ok = IsValidModuleRva(h, kRvaSvTime, sizeof(std::uint32_t)) &&
                    IsValidModuleRva(h, kRvaSvsRealtime, sizeof(std::uint32_t)) &&
                    IsValidModuleRva(h, kRvaSvState, sizeof(std::int32_t));
    if (!ok) {
        PrintOut(PRINT_BAD,
                 "[clamp_monitor] engine globals not resolvable (module %p) - monitor disabled\n",
                 static_cast<void*>(h));
        return false;
    }
    auto* base = reinterpret_cast<char*>(h);
    g_engine.svTime      = reinterpret_cast<volatile const std::uint32_t*>(base + kRvaSvTime);
    g_engine.svsRealtime = reinterpret_cast<volatile const std::uint32_t*>(base + kRvaSvsRealtime);
    g_engine.svState     = reinterpret_cast<volatile const std::int32_t*>(base + kRvaSvState);
    return true;
}

// ---------------------------------------------------------------------------
// Monitor state - touched only from the G_RunFrame hook, i.e. only ever from
// the engine's own thread. No locking and no atomics needed by construction.
// ---------------------------------------------------------------------------
struct MonitorState {
    std::int32_t samples[kMaxSamples] = {};  // exact lost ms per real tick
    int head = 0;
    int count = 0;

    std::int32_t lastFrame = -1;   // sv.framenum of the previous call we saw
    int settle = kSettleTicks;     // real ticks still to skip after a resync

    long long highClamps = 0;      // clamp events since boot
    long long lostMs = 0;          // total ms deleted since boot
    std::int32_t lastLost = 0;     // lost ms on the most recent real tick
    float avgMs = 0.0f;            // rolling average over the window cvar

    long long lowClamps = 0;       // lowclamp events since boot
    long long lowChecks = 0;       // SV_Frames on which the test was actually run
    long long gainedMs = 0;        // total ms the engine invented, since boot
    std::int32_t worstLow = 0;     // biggest single forward jump
    DWORD lastLowNotifyTick = 0;
    bool lowNotified = false;

    DWORD lastNotifyTick = 0;
    DWORD lastBroadcastTick = 0;
    bool notified = false;
    bool broadcast = false;
};

MonitorState g_state;

void ResetWindow(MonitorState& s) {
    s.head = 0;
    s.count = 0;
    s.lastLost = 0;
    s.avgMs = 0.0f;
}

void PushSample(MonitorState& s, std::int32_t lost) {
    s.samples[s.head] = lost;
    s.head = (s.head + 1) % kMaxSamples;
    if (s.count < kMaxSamples)
        ++s.count;
}

/** Mean of the last `window_ticks` samples (fewer if history is shorter). */
float RollingAverage(const MonitorState& s, int window_ticks) {
    const int n = window_ticks < s.count ? window_ticks : s.count;
    if (n <= 0)
        return 0.0f;
    double sum = 0.0;
    for (int i = 1; i <= n; ++i) {
        int idx = s.head - i;
        if (idx < 0)
            idx += kMaxSamples;
        sum += s.samples[idx];
    }
    return static_cast<float>(sum / n);
}

/** _sofbuddy_clamp_window in seconds -> sample count, clamped to the ring. */
int WindowTicks() {
    float sec = Buddy_ReadCvarValue(ClampMonitor_WindowCvar(), kDefaultWindowSec);
    if (!(sec > 0.0f))  // also catches NaN
        sec = kDefaultWindowSec;
    // Rounded, not truncated: 2.1 * 10 is 20.999998f in binary float, and
    // truncation would silently give a 2.0s window for a 2.1s setting.
    int ticks = static_cast<int>(sec * kTicksPerSecond + 0.5f);
    if (ticks < 1)
        ticks = 1;
    if (ticks > kMaxSamples)
        ticks = kMaxSamples;
    return ticks;
}

/** True when this G_RunFrame call is a real, continuous server tick.
 *
 *  G_RunFrame is *not* only called from SV_RunGameFrame: SpawnServer calls it
 *  twice at ss_loading ("run two frames to allow everything to settle", with
 *  sv.framenum still 0/1), and SV_RunGameFrame's cinematic loop can call it
 *  repeatedly for one sv.framenum. Neither advances sv.time, so treating them
 *  as ticks corrupts the window - and the tick immediately after a map load
 *  carries the whole load duration in svs.realtime, which the engine deletes
 *  in a single multi-second clamp that has nothing to do with starvation.
 *
 *  So a sample is only taken while a map is actually running and sv.framenum
 *  advanced by exactly one since the last call, and only after kSettleTicks
 *  such ticks have passed since the last discontinuity. */
bool IsRealTick(MonitorState& s, std::int32_t serverframe, std::int32_t svstate) {
    const bool continuous = (svstate == kSvStateGame) && (serverframe == s.lastFrame + 1);
    s.lastFrame = serverframe;

    if (!continuous) {
        // New measurement epoch: the old window describes a different map (or
        // no map at all) and must not be averaged into the new one.
        s.settle = kSettleTicks;
        ResetWindow(s);
        return false;
    }
    if (s.settle > 0) {
        --s.settle;
        return false;
    }
    return true;
}

void MaybeLogClamp(MonitorState& s, std::int32_t lost, DWORD now) {
    const float notify = Buddy_ReadCvarValue(ClampMonitor_NotifyCvar(), kDefaultNotifyMs);
    if (!(notify > 0.0f) || static_cast<float>(lost) < notify)
        return;
    if (s.notified && now - s.lastNotifyTick < kNotifyMinIntervalMs)
        return;
    s.notified = true;
    s.lastNotifyTick = now;
    PrintOut(PRINT_LOG, "[clamp_monitor] highclamp: %i ms deleted\n", lost);
}

void MaybeBroadcastLag(MonitorState& s, DWORD now) {
    // Off by default: this is player-visible, so it is opt-in per server.
    const float threshold = Buddy_ReadCvarValue(ClampMonitor_BroadcastThresholdCvar(), 0.0f);
    if (!(threshold > 0.0f) || s.avgMs < threshold)
        return;

    float intervalSec = Buddy_ReadCvarValue(ClampMonitor_BroadcastIntervalCvar(),
                                            kDefaultBroadcastIntervalSec);
    if (!(intervalSec > 0.0f))
        intervalSec = kDefaultBroadcastIntervalSec;
    const DWORD intervalMs = static_cast<DWORD>(intervalSec * 1000.0f);
    if (s.broadcast && now - s.lastBroadcastTick < intervalMs)
        return;

    s.broadcast = true;
    s.lastBroadcastTick = now;

    // Server-side confirmation that the threshold fired at all, independent of
    // whether any client displays it: separates "never reached" from "not
    // delivered". PrintOut never calls back into the engine (see log.cpp).
    PrintOut(PRINT_LOG, "[clamp_monitor] broadcasting lag notice: %.1fms avg\n",
             static_cast<double>(s.avgMs));

    // gi.bprintf is safe here - the hook runs synchronously on the engine's
    // own thread, the same context stock game code calls it from constantly
    // (e.g. p_client.cpp's disconnect message).
    Buddy_BroadcastPrintf(kEnginePrintChat, "[sofbuddy] server lag: %.1fms avg clamp delay\n",
                          static_cast<double>(s.avgMs));
}

}  // namespace

float clampmon_RunFrame(int serverframe, detour_G_RunFrame::tG_RunFrame original) {
    const float result = original ? original(serverframe) : 0.0f;

    if (!EngineGlobalsReady())
        return result;

    MonitorState& s = g_state;

    // Read the same three globals the engine is about to act on. We run before
    // SV_RunGameFrame regains control, so nothing has touched them since the
    // game frame ended - these are exactly the pre-clamp values.
    const std::uint32_t svtime = *g_engine.svTime;
    const std::uint32_t realtime = *g_engine.svsRealtime;
    const std::int32_t svstate = *g_engine.svState;

    if (!IsRealTick(s, serverframe, svstate))
        return result;

    // The engine's own test, byte for byte: `cmp sv_time, svs_realtime / jnb`
    // - an unsigned compare (IDA @ 0x2005f488). The difference is what
    // `svs_realtime = sv_time` is about to delete.
    const std::int32_t lost =
        (svtime < realtime) ? static_cast<std::int32_t>(realtime - svtime) : 0;

    // One exact sample per real tick, 0 included: pushing non-clamp ticks too
    // is what lets the average decay back down once clamping stops.
    PushSample(s, lost);
    s.lastLost = lost;
    s.avgMs = RollingAverage(s, WindowTicks());

    if (lost > 0) {
        ++s.highClamps;
        s.lostMs += lost;
        const DWORD now = GetTickCount();
        MaybeLogClamp(s, lost, now);
        MaybeBroadcastLag(s, now);
    }

    // Published every tick, from the engine thread, so console reads and
    // sofplus scripts always see the current window (the old once-a-second
    // flush existed only to serve a background thread that no longer exists).
    ClampMonitor_SetOutputs(s.highClamps, s.avgMs, s.lastLost, s.lostMs);

    // Republished here, not just on a lowclamp: the event path can go forever
    // without firing (that is the healthy case), and a counter that is only
    // written when something goes wrong cannot be distinguished from a counter
    // that is never written.
    ClampMonitor_SetLowOutputs(s.lowClamps, s.lowChecks, s.gainedMs, s.worstLow);
    return result;
}

/** SV_ReadPackets Post - the lowclamp side of the same measurement.
 *
 *  Lowclamp frames never reach G_RunFrame: SV_Frame takes the
 *  `if (svs.realtime < sv.time)` branch at 0x2005F617 and returns without ever
 *  calling SV_RunGameFrame. So the highclamp hook cannot see them, and this
 *  needs its own anchor.
 *
 *  SV_ReadPackets is that anchor. It is called from SV_Frame @0x2005F5EF and
 *  from nowhere else, after `svs.realtime += msec` (0x2005F5E2) and before the
 *  lowclamp test (0x2005F60A). Returning from it puts us at exactly the
 *  instant the engine is about to compute `sv.time - svs.realtime > 100`, on
 *  the same two globals, with nothing in between - the same standard of
 *  exactness the G_RunFrame hook meets for highclamp.
 *
 *  What a lowclamp means, and why it is worth counting separately: highclamp
 *  deletes time the server owed and could not deliver (it fell behind).
 *  Lowclamp is the opposite - the engine finds the server clock more than a
 *  whole tick *behind* game time and jumps it forward to close the gap,
 *  inventing the difference. The stock engine cannot reach that from the tick
 *  path, because a tick only fires once svs.realtime has caught sv.time and so
 *  leaves a deficit of at most exactly 100. Reaching it means something moved
 *  sv.time forward, or svs.realtime backward, outside that path - a map change
 *  (SpawnServer sets sv.time to 1000 and svs.realtime to 0, which is the one
 *  benign case, and is why this skips the settle window), or a shim. */
void clampmon_ReadPacketsPost() {
    if (!EngineGlobalsReady())
        return;

    MonitorState& s = g_state;

    const std::uint32_t svtime = *g_engine.svTime;
    const std::uint32_t realtime = *g_engine.svsRealtime;

    // Only while a map is really running, and only once the highclamp side has
    // finished settling after the last discontinuity - the first SV_Frame of a
    // new map is legitimately ~1000ms behind and lowclamps exactly once.
    if (*g_engine.svState != kSvStateGame || s.settle > 0)
        return;

    // The denominator. Without it `_sofbuddy_lowclamps 0` is unfalsifiable: it
    // reads the same whether the test ran a million times and never fired, or
    // this hook never fired at all. `_sofbuddy_lowclamp_checks` climbing at
    // roughly the loop rate (hundreds per second, not ten) is the proof the
    // sample point is live - it counts SV_Frames, not ticks.
    ++s.lowChecks;

    // The engine's own test, byte for byte: `cmp svs_realtime, sv_time / jnb`
    // then `sub edx, eax / cmp edx, 64h / jbe` - both unsigned (0x2005F615,
    // 0x2005F61D). Note the boundary is strict: a deficit of exactly 100 is
    // the normal post-tick state and is *not* a clamp.
    if (realtime >= svtime)
        return;
    const std::uint32_t deficit = svtime - realtime;
    if (deficit <= 100)
        return;

    // `svs.realtime = sv.time - 100` is about to run, so this much time is
    // conjured out of nothing.
    const auto gained = static_cast<std::int32_t>(deficit - 100u);
    ++s.lowClamps;
    s.gainedMs += gained;
    if (gained > s.worstLow)
        s.worstLow = gained;

    const DWORD now = GetTickCount();
    if (!s.lowNotified || now - s.lastLowNotifyTick >= kNotifyMinIntervalMs) {
        s.lowNotified = true;
        s.lastLowNotifyTick = now;
        PrintOut(PRINT_LOG,
                 "[clamp_monitor] lowclamp: server clock was %i ms behind sv.time; "
                 "%i ms invented\n",
                 static_cast<int>(deficit), gained);
    }

    ClampMonitor_SetLowOutputs(s.lowClamps, s.lowChecks, s.gainedMs, s.worstLow);
}

void clampmon_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    ClampMonitor_InitCvars();
}
