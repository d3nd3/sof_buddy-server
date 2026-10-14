// tick_pacing: notice the 100ms tick boundary without inflating svs.realtime.
//
// WinMain samples msec *before* Cbuf_Execute, so a boundary that passes during
// a drain is invisible until the next loop. Skipping the next Sleep(1) lets the
// following sample's msec contain the drain.
//
// Settle: add untilAfterMsec so svs.realtime + msec == sv.time this frame.
// That tick is on time. oldtime is a WinMain local we cannot advance, so the
// next msec still contains this drain. Unwind that credit from the next sample
// or it is counted twice and the clock runs fast.
//
// Credit only when Sys_Milliseconds has also advanced by untilAfterMsec
// during the drain. The private QPC Clock is float elapsed for skip-sleep;
// msec is integer Sys_Milliseconds.

#include "cvar.h"
#include "engine.h"
#include "sleep_gate.h"
#include "../cpuopt.h"

#include "generated_detours.h"
#include "log.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace tickpace {
namespace {

constexpr int kLateSamples = 20;

struct Clock {
    LARGE_INTEGER freq = {};
    LARGE_INTEGER qpcStart = {};
    double        scale = 0.0;
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
    Config cfg;
    double frameEntryMs = 0.0;
    int    frameEntryEngineMs = 0;
    int    qcfMsec = 0;
    bool   inFrame = false;
    bool   measuring = false;
    std::uint32_t svTimeAtQcf = 0;
    std::int32_t  svStateAtQcf = 0;
    bool          haveQcfAnchor = false;
    int settleMs = 0;
    bool          skipNextSleep = false;
    bool          pendingSaved = false;
    bool          straddleThisFrame = false;
    bool          skipRescueActive = false;
    std::uint32_t leftAt = 0;
    bool          haveLeftAt = false;
    std::uint32_t svTimeAtPre = 0;
    std::int64_t  projectedRealtime = 0;
    bool          haveProjected = false;
    double        pendingLateMs = 0.0;
    double        rescueLateMs = 0.0;
    double lateRing[kLateSamples] = {};
    int    lateCount = 0;
    int    lateHead = 0;
    long long saved = 0;
};

State g;

#ifdef TICKPACE_HARNESS
bool g_simulateElapsedCredit = false;
int  g_engineNowOverride = -1;
#endif

int EngineNowMs() {
#ifdef TICKPACE_HARNESS
    if (g_engineNowOverride >= 0)
        return g_engineNowOverride;
    return static_cast<int>(g.clock.NowMs());
#else
    return detour_Sys_Milliseconds::hkSys_Milliseconds();
#endif
}

void EnsureReady() {
    if (g.ready)
        return;
    InitCvars();
    g.clock.Init();
    g.ready = g.clock.Ready() && EngineReady();
    if (!g.ready)
        PrintOut(PRINT_BAD, "[tick_pacing] disabled: no usable clock or engine globals\n");
}

double ElapsedMs() { return g.clock.NowMs() - g.frameEntryMs; }

// Raw svs.realtime + msec only — do not subtract settleMs here; that phantom-
// straddles when the sample already reached sv.time but settleMs > 0.
std::int32_t MsUntilTickAfterMsec(int msec) {
    const std::int64_t until =
        static_cast<std::int64_t>(*Engine().svTime) -
        static_cast<std::int64_t>(*Engine().svsRealtime) - msec;
    if (until > static_cast<std::int64_t>(INT32_MAX))
        return INT32_MAX;
    if (until < static_cast<std::int64_t>(INT32_MIN))
        return INT32_MIN;
    return static_cast<std::int32_t>(until);
}

void ForgetSettleIfEngineRewroteRealtime() {
    if (!g.haveLeftAt || *Engine().svsRealtime != g.leftAt)
        g.settleMs = 0;
}

void RecordProjectedRealtime(int msec) {
    g.projectedRealtime =
        static_cast<std::int64_t>(*Engine().svsRealtime) + msec;
    g.haveProjected = true;
}

// SV_RunGameFrame highclamp sets svs.realtime = sv.time, dropping anything
// that was still over the new sv.time. Do not unwind that portion from msec
// next frame — the engine already removed it from the clock.
void ForgiveSettleDebtOverSvTime() {
    if (g.settleMs <= 0 || !g.haveProjected)
        return;
    if (*Engine().svsRealtime != *Engine().svTime)
        return;
    const std::int64_t over =
        g.projectedRealtime - static_cast<std::int64_t>(*Engine().svTime);
    if (over <= 0)
        return;
    const int drop = over > static_cast<std::int64_t>(INT32_MAX)
                         ? INT32_MAX
                         : static_cast<int>(over);
    g.settleMs = (g.settleMs > drop) ? g.settleMs - drop : 0;
}

void ApplyCorrectionViaMsec(int& msec, int targetSettle) {
    const int delta = targetSettle - g.settleMs;
    if (delta < 0) {
        const int unwind = (-delta < msec) ? -delta : msec;
        msec -= unwind;
        g.settleMs -= unwind;
    } else {
        msec += delta;
        g.settleMs = targetSettle;
    }
}

void PushLate(double ms) {
    g.lateRing[g.lateHead] = ms;
    g.lateHead = (g.lateHead + 1) % kLateSamples;
    if (g.lateCount < kLateSamples)
        ++g.lateCount;
}

float LateAverage() {
    if (g.lateCount == 0)
        return 0.0f;
    double sum = 0.0;
    for (int i = 0; i < g.lateCount; ++i)
        sum += g.lateRing[i];
    return static_cast<float>(sum / g.lateCount);
}

}  // namespace

#ifdef TICKPACE_HARNESS
void SetSimulateElapsedCredit(bool on) { g_simulateElapsedCredit = on; }
void SetEngineNowOverride(int ms) { g_engineNowOverride = ms; }
int HarnessMsUntilTickAfterMsec(int msec) { return MsUntilTickAfterMsec(msec); }
int HarnessSettleMs() { return g.settleMs; }
void HarnessSetSettleMs(int ms) { g.settleMs = ms; }
#endif

std::uint32_t CurrentSettleMs() {
    return g.settleMs > 0 ? static_cast<std::uint32_t>(g.settleMs) : 0u;
}

bool ConsumeSkipNextSleep() {
    const bool v = g.skipNextSleep;
    g.skipNextSleep = false;
    if (v)
        g.skipRescueActive = true;
    return v;
}

DebugSnap GetDebugSnap() {
    DebugSnap d;
    d.settleMs = CurrentSettleMs();
    d.enabled  = ReadConfig().enabled;
    return d;
}

}  // namespace tickpace

void tickpace_QcommonFrame(int& msec) {
    using namespace tickpace;
    EnsureReady();
    if (!g.ready)
        return;
    g.cfg = ReadConfig();
    g.frameEntryMs = g.clock.NowMs();
    g.frameEntryEngineMs = EngineNowMs();
    g.qcfMsec = msec;
    g.inFrame = true;
    g.haveQcfAnchor = true;
    g.svTimeAtQcf = *Engine().svTime;
    g.svStateAtQcf = *Engine().svState;
}

void tickpace_SvFramePre(int& msec) {
    using namespace tickpace;
    const bool fromQcommon = g.inFrame;
    g.inFrame = false;
    g.measuring = false;
    if (!g.ready)
        return;

    ForgetSettleIfEngineRewroteRealtime();
    if (g.settleMs > static_cast<int>(*Engine().svsRealtime))
        g.settleMs = 0;

    const bool mapLoad = g.haveQcfAnchor &&
        (*Engine().svTime != g.svTimeAtQcf || *Engine().svState != g.svStateAtQcf);
    if (mapLoad) {
        g.settleMs = 0;
        g.skipNextSleep = false;
        g.pendingSaved = false;
        return;
    }

    if (!Engine().svsInitialized || *Engine().svsInitialized == 0)
        return;

    if (fromQcommon && g.cfg.enabled && ServerRunning()) {
        const int original = msec;
        const double elapsedRaw = ElapsedMs();
        const std::int32_t untilAfterMsec = MsUntilTickAfterMsec(original);
        const double untilNow = static_cast<double>(untilAfterMsec) - elapsedRaw;
        int elapsedEngine = EngineNowMs() - g.frameEntryEngineMs;
        if (elapsedEngine < 0)
            elapsedEngine = 0;

        g.straddleThisFrame = false;
        g.svTimeAtPre = *Engine().svTime;
        g.pendingLateMs = untilNow <= 0.0 ? -untilNow : 0.0;
        g.measuring = true;
        const bool drainStraddled = untilAfterMsec > 0 && untilNow <= 0.0;
        if (drainStraddled) {
            g.straddleThisFrame = true;
            g.rescueLateMs = g.pendingLateMs;
            g.skipNextSleep = true;
            if (g.cfg.settle && elapsedEngine >= untilAfterMsec) {
                int target = untilAfterMsec;
#ifdef TICKPACE_HARNESS
                if (g_simulateElapsedCredit) {
                    double e = elapsedRaw;
                    if (e > 100.0) e = 100.0;
                    if (e > 0.0)
                        target = static_cast<int>(e);
                }
#endif
                ApplyCorrectionViaMsec(msec, target);
                const std::int64_t projected =
                    static_cast<std::int64_t>(*Engine().svsRealtime) + msec;
                if (projected >= static_cast<std::int64_t>(*Engine().svTime))
                    g.pendingSaved = true;
                RecordProjectedRealtime(msec);
                return;
            }
            g.pendingSaved = true;
        }
    } else {
        g.skipNextSleep = false;
    }

    if (g.settleMs != 0)
        ApplyCorrectionViaMsec(msec, 0);

    if (Engine().svsInitialized && *Engine().svsInitialized != 0)
        RecordProjectedRealtime(msec);
}

void tickpace_ReadPacketsPre() {
}

void tickpace_SvFramePost(int msec) {
    using namespace tickpace;
    (void)msec;
    if (!g.ready)
        return;
    g.leftAt = *Engine().svsRealtime;
    g.haveLeftAt = true;
    const bool ticked = *Engine().svTime != g.svTimeAtPre;
    if (ticked)
        ForgiveSettleDebtOverSvTime();
    if (g.measuring) {
        g.measuring = false;
        if (ticked) {
            if (g.pendingSaved) {
                const bool count = g.cfg.settle ? g.straddleThisFrame
                                                : g.skipRescueActive;
                if (count && g.rescueLateMs > 0.0)
                    ++g.saved;
                g.pendingSaved = false;
                g.rescueLateMs = 0.0;
            }
            PushLate(g.pendingLateMs);
            SetOutputs(LateAverage(), g.saved);
        } else if (g.pendingSaved && g.cfg.settle && g.straddleThisFrame) {
            g.pendingSaved = false;
        }
    } else if (g.pendingSaved) {
        if (ticked || g.cfg.settle)
            g.pendingSaved = false;
    }
    g.skipRescueActive = false;
}

void tickpace_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    tickpace::InitCvars();
    tickpace::EnsureReady();
    tickpace::SleepGate_Install();
}

void TickPace_LogSessionSummary() {
    using namespace tickpace;
    static bool logged = false;
    if (logged)
        return;
    logged = true;
    if (!g.ready)
        return;
    char saved[24];
    std::snprintf(saved, sizeof(saved), "%lld", static_cast<long long>(g.saved));
    PrintOut(PRINT_LOG,
             "[cpuopt] tickpace gauges:"
             " _sofbuddy_tickpace_saved=%s"
             " _sofbuddy_tickpace_late_avg=%.2f\n",
             saved, static_cast<double>(LateAverage()));
}
