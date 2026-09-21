// qpc_timer: feed the engine QPC milliseconds instead of timeGetTime.
//
// On some hosts timeGetTime steps ~15.6 ms, so WinMain's
// `while (newtime - oldtime < 1)` busy-waits a whole quantum and `msec`
// jumps. This override keeps the engine's millisecond clock smooth, which
// everything downstream (tick timing, ping, netchan, downloads) shares.
//
// Moved out of tick_pacing untouched in behavior; only the cvar moved with
// it (`_sofbuddy_tickpace_qpc` is now `_sofbuddy_qpc`). Deliberately outside
// `_sofbuddy_cpuopt` / `_sofbuddy_tickpace`, so flipping those never jumps
// the shared clock.

#include "cvar.h"
#include "engine.h"

#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <windows.h>

namespace qpctimer {
namespace {

struct State {
    // Stock value at lock + QPC elapsed, uint32 wrap.
    bool         engineMsLocked = false;
    std::int64_t engineMsQpc = 0;
    std::uint32_t engineMsOrigin = 0;
    std::uint32_t engineMsLast = 0;
    std::int64_t qpcFreq = 0;
};

State g;

bool FreqReady() {
    if (g.qpcFreq <= 0) {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0)
            g.qpcFreq = f.QuadPart;
    }
    return g.qpcFreq > 0;
}

std::uint64_t QpcMsBetween(std::int64_t startQpc, std::int64_t nowQpc) {
    if (nowQpc < startQpc || g.qpcFreq <= 0)
        return 0;
    const std::uint64_t d =
        static_cast<std::uint64_t>(nowQpc - startQpc);
    return d * 1000ull / static_cast<std::uint64_t>(g.qpcFreq);
}

}  // namespace

}  // namespace qpctimer

int qpc_SysMilliseconds(detour_Sys_Milliseconds::tSys_Milliseconds original) {
    using namespace qpctimer;
    if (!original) return 0;
    if (!UseQpcClock()) {
        g.engineMsLocked = false;
        return original();
    }
    if (!FreqReady() || !EngineReady() || !Engine().curtime)
        return original();

    LARGE_INTEGER c;
    if (!QueryPerformanceCounter(&c))
        return static_cast<int>(g.engineMsLast);

    if (!g.engineMsLocked) {
        const auto stock = static_cast<std::uint32_t>(original());
        g.engineMsQpc = c.QuadPart;
        g.engineMsOrigin = g.engineMsLast = stock;
        g.engineMsLocked = true;
        *Engine().curtime = stock;
        return static_cast<int>(stock);
    }

    // Compute elapsed milliseconds directly from fixed QPC origin
    auto ms = g.engineMsOrigin +
              static_cast<std::uint32_t>(QpcMsBetween(g.engineMsQpc, c.QuadPart));

    // Enforce monotonicity (clamp forward-only, never ratchet the origin)
    if (static_cast<std::int32_t>(ms - g.engineMsLast) < 0)
        ms = g.engineMsLast;

    if (ms != g.engineMsLast) {
        g.engineMsLast = ms;
        *Engine().curtime = ms;
    }
    return static_cast<int>(ms);
}

void qpc_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    qpctimer::InitCvars();
    qpctimer::EngineReady();
}
