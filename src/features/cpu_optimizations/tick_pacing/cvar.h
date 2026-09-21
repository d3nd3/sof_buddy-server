#pragma once

#include <cstdint>

namespace tickpace {

struct Config {
    bool  enabled   = false;
    bool  strict    = false;  // _sofbuddy_cmdpark_strict: retained for compat;
                               // tick_pacing no longer reports violations
    float spinMs    = 0.0f;
    bool  dedicated = false;
    bool  settle    = false;  // straddle-only msec credit (needs enabled)
};

void InitCvars();
Config ReadConfig();

/** Live `_sofbuddy_tickpace_spin_ms`, clamped to 0..20. Now the Sleep-skip
 *  window (see sleep_gate.h), not a busy-wait. 0 = stock Sleep behavior. */
float SpinWindowMs();

/** ms credited into msec beyond the engine sample (0 when idle). */
std::uint32_t CurrentSettleMs();

/** True if SvFramePre saw the tick boundary pass during this drain. Consumed
 *  by the Sleep-skip gate so the next WinMain Sleep(1) is skipped. */
bool ConsumeSkipNextSleep();

struct DebugSnap {
    unsigned settleMs = 0;
    bool     enabled  = false;
};
DebugSnap GetDebugSnap();
void SetOutputs(float lateAvgMs, long long saved);

/** Shim-log tickpace gauges (strict-mode session summary). */
void TickPace_LogSessionSummary();

}  // namespace tickpace

extern "C" {
void TickPacing_Shutdown();
}
