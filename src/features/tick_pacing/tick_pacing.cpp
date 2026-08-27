// tick_pacing: make server ticks fire on their own 100ms boundary instead of
// on whatever loop iteration happens to notice the boundary has passed.
//
// The engine's main loop (WinMain @ 0x20066300) is:
//
//     Sleep(1); PeekMessage-pump;
//     do { newtime = Sys_Milliseconds(); msec = newtime - oldtime; }
//         while (msec < 1);
//     Qcommon_frame(msec);            //  Sys_ConsoleInput -> Cbuf_Execute
//                                     //  -> SV_Frame(msec) -> CL_Frame(msec)
//     oldtime = newtime;
//
// and SV_Frame (@0x2005F5B0) does `svs.realtime += msec` and only then decides
// `if (svs.realtime < sv.time) return;  else SV_RunGameFrame();`.
//
// The clock is sampled *before* Cbuf_Execute runs, so however long console
// commands take (sofplus scripting lives here), that time is not in
// svs.realtime when the tick decision is made - it lands in the *next*
// iteration's msec instead. A tick whose boundary passes during Cbuf_Execute
// is therefore not noticed until the loop has gone all the way round again:
// another Sleep(1), another pump, another Cbuf_Execute. The tick doesn't just
// slip by the length of the commands, it slips by a whole extra iteration.
//
// Nothing is lost permanently by that on its own - svs.realtime is exactly
// `newtime - starttime`, so the *average* rate stays 10Hz - but each tick
// fires late by up to one loop period, and once a loop period reaches 100ms
// the engine skips a whole tick and deletes the overshoot ("sv highclamp",
// which is what clamp_monitor counts).
//
// Corrections and optimizations:
//
//   settle (always, when the feature is on)
//       Carry svs.realtime forward to "now" just before SV_Frame's comparison,
//       so the tick decision is made against the real current time instead of
//       against a sample taken before Cbuf_Execute ran.
//
//       The correction is *carried*, not applied-and-undone. An earlier version
//       added the elapsed time in SV_Frame Pre and subtracted it again in Post,
//       which spams "sv lowclamp": SV_Frame @0x2005F620 lowclamps whenever
//       `sv.time - svs.realtime > 100`, and the stock engine can never hit that
//       from the tick path because a tick only fires once svs.realtime has
//       reached sv.time, leaving a deficit of at most 100. Firing the tick on
//       the corrected clock and then putting svs.realtime *back* breaks exactly
//       that invariant: sv.time went up by 100 while svs.realtime went down by
//       the correction, so the next frame sees a deficit of 100 + correction
//       and lowclamps - once per tick the correction saved, which is the whole
//       point of the feature. Carrying it forward instead keeps the engine's
//       invariant intact, because svs.realtime only ever moves forward.
//
//       `carried` is how much of svs.realtime is ours rather than the engine's.
//       Each frame applies the single delta `want - carried`, so a frame where
//       the feature is off or the server is stopped unwinds the correction
//       instead of stranding it, and the next msec (which spans the same
//       elapsed time) is never double-counted.
//
//   spin (_sofbuddy_tickpace_spin_ms)
//       When the tick is due within spin_ms, busy-wait to the boundary.
//
//   reserve (_sofbuddy_tickpace_reserve_ms)
//       Don't *start* a drain that will not fit before the next boundary.
//
// See README.md for details.

#include "cvar.h"
#include "engine.h"

#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <windows.h>

namespace tickpace {
namespace {

// sv.time advances in 100ms steps, so the rolling window is counted in ticks.
constexpr int kLateSamples = 20;  // 2 seconds of tick history
constexpr double kTickMs = 100.0;

constexpr double kMaxCorrectionMs = kTickMs;

constexpr int kDrainSamples = 32;
constexpr double kMaxDrainSampleMs = kTickMs;
constexpr double kMaxRoomNeededMs = 10.0;

/** QueryPerformanceCounter high-resolution engine clock. */
struct Clock {
    LARGE_INTEGER freq = {};
    LARGE_INTEGER qpcStart = {};
    double        scale = 0.0;  // ms per count
    bool          ready = false;

    void Init() {
        if (ready)
            return;
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0 &&
            QueryPerformanceCounter(&qpcStart)) {
            scale = 1000.0 / static_cast<double>(freq.QuadPart);
            ready = true;
        }
    }

    bool Ready() const { return ready; }

    double NowMs() const {
        LARGE_INTEGER c;
        if (!QueryPerformanceCounter(&c))
            return 0.0;
        return static_cast<double>(c.QuadPart) * scale;
    }

};

struct State {
    Clock clock;
    bool  ready = false;

    // --- per engine-loop iteration ---
    Config cfg;                     // snapshot taken at Qcommon_frame entry
    double frameEntryMs = 0.0;      // == the engine's own `newtime` sample point
    int    qcfMsec = 0;             // msec the loop measured for this iteration
    bool   inFrame = false;         // between Qcommon_frame entry and SV_Frame entry
    bool   inCbuf = false;          // the in-frame drain is running
    int    nestedCbuf = 0;          // Cbuf_Execute re-entered from inside it

    // --- settle bookkeeping, SV_Frame Pre -> Post ---
    bool          measuring = false;       // Pre ran on a live server; Post may publish
    std::uint32_t carried = 0;             // how much of svs.realtime is our correction
    std::uint32_t leftAt = 0;              // svs.realtime as SV_Frame Post last left it
    bool          haveLeftAt = false;      // leftAt holds a value we can compare against
    std::uint32_t svTimeAtPre = 0;         // sv.time before the engine may advance it
    double        pendingLateMs = 0.0;     // how late this tick's decision was

    // --- command scheduling ---
    double deferSinceMs = 0.0;
    bool   deferring = false;
    double drainRing[kDrainSamples] = {};
    int    drainCount = 0;
    int    drainHead = 0;

    // --- measurement ---
    double lateRing[kLateSamples] = {};
    int    lateCount = 0;
    int    lateHead = 0;
    double lateMaxMs = 0.0;
    double cbufMaxMs = 0.0;
    long long saved = 0;
    long long defers = 0;
};

State g;

void EnsureReady() {
    if (g.ready)
        return;
    g.clock.Init();
    g.ready = g.clock.Ready() && EngineReady();
    if (!g.ready)
        PrintOut(PRINT_BAD, "[tick_pacing] disabled: no usable clock or engine globals\n");
}

double ElapsedMs() { return g.clock.NowMs() - g.frameEntryMs; }

/** svs.realtime with our carried correction taken back out - i.e. the value
 *  the engine itself accumulated, `newtime - starttime`. Every "how far to the
 *  boundary" question is asked against this, never against the raw global,
 *  which still holds last frame's correction. */
std::uint32_t RawRealtime() { return *Engine().svsRealtime - g.carried; }

/** Milliseconds from *now* until sv.time comes due, given that the engine is
 *  about to add `msec` to svs.realtime. Negative means the boundary has already
 *  passed and the tick is overdue. */
double MsUntilTickDue(int msec) {
    const std::uint32_t due = *Engine().svTime;
    const auto owed =
        static_cast<std::int32_t>(due - RawRealtime() - static_cast<std::uint32_t>(msec));
    return static_cast<double>(owed) - ElapsedMs();
}

void PushDrainCost(double ms) {
    if (ms < 0.0)
        ms = 0.0;
    if (ms > kMaxDrainSampleMs)
        ms = kMaxDrainSampleMs;
    g.drainRing[g.drainHead] = ms;
    g.drainHead = (g.drainHead + 1) % kDrainSamples;
    if (g.drainCount < kDrainSamples)
        ++g.drainCount;
}

double RecentWorstDrainMs() {
    double worst = 0.0;
    for (int i = 0; i < g.drainCount; ++i)
        if (g.drainRing[i] > worst)
            worst = g.drainRing[i];
    return worst;
}

double RoomNeededMs(float reserveMs) {
    double gate = RecentWorstDrainMs() + static_cast<double>(reserveMs);
    if (gate > kMaxRoomNeededMs)
        gate = kMaxRoomNeededMs;
    return gate;
}

void PushLate(double ms) {
    g.lateRing[g.lateHead] = ms;
    g.lateHead = (g.lateHead + 1) % kLateSamples;
    if (g.lateCount < kLateSamples)
        ++g.lateCount;
    if (ms > g.lateMaxMs)
        g.lateMaxMs = ms;
}

float LateAverage() {
    if (g.lateCount == 0)
        return 0.0f;
    double sum = 0.0;
    for (int i = 0; i < g.lateCount; ++i)
        sum += g.lateRing[i];
    return static_cast<float>(sum / g.lateCount);
}

void SpinToBoundary(double owedMs) {
    const double target = g.clock.NowMs() + owedMs;
    const double giveUp = target + 1.0;
    while (true) {
        const double now = g.clock.NowMs();
        if (now >= target || now >= giveUp)
            return;
        YieldProcessor();
    }
}

}  // namespace
}  // namespace tickpace

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

/** Qcommon_frame Pre - the top of one engine loop iteration. */
void tickpace_QcommonFrame(int& msec) {
    using namespace tickpace;
    EnsureReady();
    if (!g.ready)
        return;

    g.cfg = ReadConfig();
    g.frameEntryMs = g.clock.NowMs();
    g.qcfMsec = msec;
    g.inFrame = true;
}

/** Cbuf_Execute override - runs the console command buffer, or holds it. */
void tickpace_CbufExecute(detour_Cbuf_Execute::tCbuf_Execute original) {
    using namespace tickpace;

    if (!original)
        return;
    if (!g.ready || !g.cfg.enabled) {
        original();
        return;
    }

    if (g.inCbuf) {                       // nested EXEC_NOW - always runs
        ++g.nestedCbuf;
        original();
        --g.nestedCbuf;
        return;
    }
    if (!g.inFrame) {
        original();
        return;
    }

    const double startMs = g.clock.NowMs();
    bool forceDrain = false;

    if (!ServerRunning()) {
        g.deferring = false;
        g.deferSinceMs = 0.0;
        original();
        return;
    }

    if (g.cfg.reserveMs > 0.0f && g.cfg.dedicated && CommandsQueued()) {
        if (MsUntilTickDue(g.qcfMsec) < RoomNeededMs(g.cfg.reserveMs)) {
            if (!g.deferring) {
                g.deferring = true;
                g.deferSinceMs = startMs;
            }
            if (startMs - g.deferSinceMs < static_cast<double>(g.cfg.deferMaxMs)) {
                ++g.defers;
                return;
            }
            forceDrain = true;
        }
    }

    (void)forceDrain;
    g.deferring = false;
    g.inCbuf = true;
    g.nestedCbuf = 0;
    original();
    g.inCbuf = false;

    const double tookMs = g.clock.NowMs() - startMs;
    if (tookMs > g.cbufMaxMs)
        g.cbufMaxMs = tookMs;
    PushDrainCost(tookMs);
}

/** SV_Frame Pre - carries svs.realtime up to "now", and optionally spins.
 *
 *  Runs before the engine's own `svs.realtime += msec` (0x2005F5E2), so the
 *  delta applied here and that msec compose into "svs.realtime as of now". */
void tickpace_SvFramePre(int& msec) {
    using namespace tickpace;

    const bool wasInFrame = g.inFrame;
    g.inFrame = false;
    g.measuring = false;

    if (!g.ready)
        return;

    // Reconcile the carry with whatever happened since Post last looked. If
    // svs.realtime is not where we left it, something outside SV_Frame moved
    // it - SpawnServer resets it to 0 on a map change - and our correction
    // went wherever the old value went. Forgetting it is the only safe move:
    // subtracting it from a value we did not write would drag the engine's own
    // clock backwards.
    if (!g.haveLeftAt || *Engine().svsRealtime != g.leftAt)
        g.carried = 0;

    // How much correction this frame wants. Zero whenever the feature is off
    // or no map is running, which makes the delta below unwind the carry
    // rather than strand it.
    std::uint32_t want = 0;

    if (wasInFrame && g.cfg.enabled && ServerRunning()) {
        const auto owedBefore = static_cast<std::int32_t>(
            *Engine().svTime - RawRealtime() - static_cast<std::uint32_t>(msec));
        double owed = static_cast<double>(owedBefore) - ElapsedMs();

        if (g.cfg.spinMs > 0.0f && g.cfg.dedicated &&
            owed > 0.0 && owed <= static_cast<double>(g.cfg.spinMs)) {
            SpinToBoundary(owed);
            owed = static_cast<double>(owedBefore) - ElapsedMs();
        }

        g.svTimeAtPre = *Engine().svTime;
        g.pendingLateMs = owed <= 0.0 ? -owed : 0.0;
        g.measuring = true;

        double elapsed = ElapsedMs();
        if (elapsed > kMaxCorrectionMs)
            elapsed = kMaxCorrectionMs;
        if (elapsed > 0.0)
            want = static_cast<std::uint32_t>(elapsed);

        if (owedBefore > 0 && owed <= 0.0)
            ++g.saved;
    }

    // One delta, either sign. svs.realtime is unsigned modular arithmetic, so
    // a negative delta is a plain wrapping subtract.
    if (want != g.carried) {
        *Engine().svsRealtime += want - g.carried;
        g.carried = want;
    }
}

/** SV_Frame Post - records where we left svs.realtime, and publishes telemetry.
 *
 *  The correction is deliberately *not* removed here; see the settle note at
 *  the top of this file. */
void tickpace_SvFramePost(int msec) {
    using namespace tickpace;
    (void)msec;

    if (!g.ready)
        return;

    g.leftAt = *Engine().svsRealtime;
    g.haveLeftAt = true;

    if (!g.measuring)
        return;
    g.measuring = false;

    if (*Engine().svTime != g.svTimeAtPre) {
        PushLate(g.pendingLateMs);
        SetOutputs(LateAverage(), static_cast<float>(g.lateMaxMs), g.saved,
                   static_cast<float>(g.cbufMaxMs), g.defers);
    }
}

void tickpace_OnGameDllLoaded(void* game_export) {
    using namespace tickpace;
    (void)game_export;

    InitCvars();
    EnsureReady();
    if (!g.ready)
        return;

    const Config c = ReadConfig();
    PrintOut(PRINT_LOG,
             "[tick_pacing] %s (dedicated=%d): spin_ms=%.1f reserve_ms=%.1f defer_max_ms=%.0f\n",
             c.enabled ? "on" : "off", IsDedicated() ? 1 : 0,
             static_cast<double>(c.spinMs), static_cast<double>(c.reserveMs),
             static_cast<double>(c.deferMaxMs));
}
