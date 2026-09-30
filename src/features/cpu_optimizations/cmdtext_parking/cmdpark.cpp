// Park every cmd_text insert that arrives within reserve_ms of the next tick
// (console, .COMMAND, clc_stringcmd, timers, Cbuf_ExecuteText insert/append).
// Drip one fitting chunk per later sub-tick. Hold Cbuf_Execute while the tick
// is close. tick_pacing keeps boundary skip + spin only.

#include "cvar.h"
#include "cmdpark.h"
#include "engine.h"
#include "../cpuopt.h"

#ifdef SOF_FEATURE_PRINT_GUARD
#include "../../print_guard/cbuf_guard.h"
#endif

#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace cmdpark {
namespace {

constexpr int kParkMax = 0x20000;

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
    int    qcfMsec = 0;
    bool   inFrame = false;
    bool   inCbuf = false;
    int    nestedCbuf = 0;
    bool   inReadPackets = false;
    bool   passLevelChange = false;  // rest of this ReadPackets stays in cmd_text
    bool   inSvFrame = false;
    bool   dripping = false;
    int    fillMax = 0;
    double cbufMaxMs = 0.0;
    long long defers = 0;
    char* park = nullptr;
    int   parkLen = 0;
    int   parkCap = 0;
    std::uint32_t svTimeAtPre = 0;
    bool          measuring = false;
};

State g;

void EnsureReady() {
    if (g.ready)
        return;
    InitCvars();
    g.clock.Init();
    g.ready = g.clock.Ready() && EngineReady();
    if (!g.ready)
        PrintOut(PRINT_BAD, "[cmdpark] disabled: no usable clock or engine globals\n");
}

double ElapsedMs() { return g.clock.NowMs() - g.frameEntryMs; }

double MsUntilTickDue(int msec) {
    const std::uint32_t due = *Engine().svTime;
    const auto owed =
        static_cast<std::int32_t>(due - *Engine().svsRealtime - static_cast<std::uint32_t>(msec));
    return static_cast<double>(owed) - (g.inFrame ? ElapsedMs() : 0.0);
}

// True once the tick is within reserve_ms: the same strict-style protection
// (park arrivals, hold the queued drain), time-gated instead of unconditional.
// Pure X — no worst-drain adaptive term, so the cvar means what it says.
bool InWindow() {
    if (!(g.cfg.reserveMs > 0.0f))
        return false;
    const int msec = g.inFrame ? g.qcfMsec : 0;
    return MsUntilTickDue(msec) <= static_cast<double>(g.cfg.reserveMs);
}

// `map` / `gamemap` at a command boundary. SoFPlus queues
// `;map @real@ #_sp_sv_info_map_next;` from inside SV_ReadPackets; parking
// that appends it behind the drip backlog (one newline chunk per tick).
bool IsMapToken(const char* s, int n) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
        ++i;
    auto word = [&](const char* w, int wn) {
        if (i + wn > n || std::memcmp(s + i, w, static_cast<std::size_t>(wn)) != 0)
            return false;
        const int e = i + wn;
        return e == n || s[e] == ' ' || s[e] == '\t' || s[e] == ';' ||
               s[e] == '\n' || s[e] == '\r' || s[e] == '\0';
    };
    return word("gamemap", 7) || word("map", 3);
}

bool TextHasLevelChange(const char* s, int n) {
    if (!s || n <= 0)
        return false;
    for (int i = 0; i < n;) {
        if (IsMapToken(s + i, n - i))
            return true;
        while (i < n && s[i] != ';' && s[i] != '\n')
            ++i;
        if (i < n)
            ++i;
    }
    return false;
}

bool QueueHasLevelChange() {
    const int n = CmdTextBytes();
    unsigned char* data = CmdTextData();
    return n > 0 && data && TextHasLevelChange(reinterpret_cast<const char*>(data), n);
}

bool ShouldPark() {
    if (g.dripping || g.inCbuf)
        return false;
    if (!g.ready || !g.cfg.enabled || !g.cfg.dedicated || !ServerRunning())
        return false;
    if (g.inReadPackets)
        return true;
    if (InWindow())
        return true;
    if (g.cfg.strict && !g.inSvFrame)
        return true;
    return false;
}

bool GrowPark(int need) {
    if (g.parkLen + need < g.parkCap)
        return true;
    int cap = g.parkCap ? g.parkCap : 0x2000;
    while (cap <= g.parkLen + need && cap < kParkMax)
        cap *= 2;
    if (g.parkLen + need >= cap)
        return false;
    char* p = static_cast<char*>(std::realloc(g.park, static_cast<std::size_t>(cap)));
    if (!p)
        return false;
    g.park = p;
    g.parkCap = cap;
    return true;
}

void Park(const char* text) {
    if (!text || !text[0])
        return;
    const int n = static_cast<int>(std::strlen(text));
    if (n <= 0 || !GrowPark(n + 1))
        return;
    std::memcpy(g.park + g.parkLen, text, static_cast<std::size_t>(n));
    g.parkLen += n;
    g.park[g.parkLen] = '\0';
}

void NoteFill() {
    const int n = CmdTextBytes();
    if (n > g.fillMax)
        g.fillMax = n;
}

#ifdef SOF_FEATURE_PRINT_GUARD
void DrainCbuf(detour_Cbuf_Execute::tCbuf_Execute original) {
    Pg_SafeCbufExecute(original);
}
#else
void DrainCbuf(detour_Cbuf_Execute::tCbuf_Execute original) {
    if (original)
        original();
}
#endif

void RunDrain(detour_Cbuf_Execute::tCbuf_Execute original) {
    if (!original)
        return;
    NoteFill();
    const double startMs = g.clock.NowMs();
    g.inCbuf = true;
    g.nestedCbuf = 0;
    DrainCbuf(original);
    g.inCbuf = false;
    const double tookMs = g.clock.NowMs() - startMs;
    if (tookMs > g.cbufMaxMs)
        g.cbufMaxMs = tookMs;
    NoteFill();
}

// Move the whole queued buffer into the side store (appended after older
// parked bytes) instead of running it here. Appending keeps each store's
// internal order; the bytes rejoin through DripOne after the tick. Returns
// false — leaving the queue to drain as-is — when there is nothing queued,
// the buffer is unresolvable, or the park cannot grow: a full buffer must
// drain, tick or not.
bool MoveQueueToPark() {
    const int n = CmdTextBytes();
    if (n <= 0)
        return false;
    unsigned char* data = CmdTextData();
    if (!data)
        return false;
    if (!GrowPark(n + 1))
        return false;
    std::memcpy(g.park + g.parkLen, data, static_cast<std::size_t>(n));
    g.parkLen += n;
    g.park[g.parkLen] = '\0';
    ClearCmdText();
    ++g.defers;
    return true;
}

void DripOne() {    auto add = detour_Cbuf_AddText::oCbuf_AddText;
    if (!add || g.parkLen <= 0)
        return;
    int room = CmdTextMax() - CmdTextBytes() - 1;
    if (room > kSofplusTextMax)
        room = kSofplusTextMax;
    if (room <= 0)
        return;
    int n = room < g.parkLen ? room : g.parkLen;
    if (n < g.parkLen) {
        int cut = n;
        while (cut > 0 && g.park[cut - 1] != '\n')
            --cut;
        if (cut == 0)
            return;
        n = cut;
    }
    const char saved = g.park[n];
    g.park[n] = '\0';
    g.dripping = true;
    add(g.park);
    g.dripping = false;
    g.park[n] = saved;
    std::memmove(g.park, g.park + n, static_cast<std::size_t>(g.parkLen - n));
    g.parkLen -= n;
    g.park[g.parkLen] = '\0';
}

}  // namespace

bool Take(char* text) {
    EnsureReady();
    if (!g.inFrame)
        g.cfg = ReadConfig();
    if (!ShouldPark())
        return false;
    // Level change stays in cmd_text so the next drain starts the map. Later
    // inserts in this ReadPackets stay too: SoFPlus InsertText's map_on_rotate
    // in front of `;map @real@ ...`, and parking only the script would reorder it.
    if (g.passLevelChange ||
        (text && TextHasLevelChange(text, static_cast<int>(std::strlen(text))))) {
        g.passLevelChange = true;
        return false;
    }
    Park(text);
    return true;
}

int ParkBytes() { return g.parkLen; }
bool HoldArmed() {
    return g.cfg.enabled && (g.cfg.reserveMs > 0.0f || g.cfg.strict);
}
int ParkLines() {
    int n = 0;
    for (int i = 0; i < g.parkLen; ++i)
        if (g.park[i] == '\n')
            ++n;
    return n;
}
void ParkTimerText(const char* text) { Park(text); }
void DripTimers() { DripOne(); }
void FreePark() {
    std::free(g.park);
    g.park = nullptr;
    g.parkLen = 0;
    g.parkCap = 0;
}

}  // namespace cmdpark

void cmdpark_CbufAddText(char* text, detour_Cbuf_AddText::tCbuf_AddText original) {
    if (cmdpark::Take(text))
        return;
    if (original)
        original(text);
}

void cmdpark_CbufExecuteText(int exec_when, char* text,
                             detour_Cbuf_ExecuteText::tCbuf_ExecuteText original) {
#ifdef SOF_FEATURE_PRINT_GUARD
    if (exec_when == 0) {
        Pg_SafeCbufExecuteText(text);
        return;
    }
#endif
    if (exec_when != 0 && cmdpark::Take(text))
        return;
    if (original)
        original(exec_when, text);
}

void cmdpark_QcommonFrame(int& msec) {
    using namespace cmdpark;
    EnsureReady();
    if (!g.ready)
        return;
    g.cfg = ReadConfig();
    g.frameEntryMs = g.clock.NowMs();
    g.qcfMsec = msec;
    g.inFrame = true;
}

void cmdpark_CbufExecute(detour_Cbuf_Execute::tCbuf_Execute original) {
    using namespace cmdpark;
    if (!original)
        return;
    if (!g.ready || !g.cfg.enabled) {
        DrainCbuf(original);
        return;
    }
    if (g.inCbuf) {
        ++g.nestedCbuf;
        DrainCbuf(original);
        --g.nestedCbuf;
        return;
    }
    if (!g.inFrame) {
        DrainCbuf(original);
        return;
    }
    NoteFill();
    if (!ServerRunning()) {
        RunDrain(original);
        return;
    }
    // Single wall sample: MsUntilTickDue() reads ElapsedMs() internally, so
    // calling it and then InWindow() (which calls it again) can straddle an
    // integer boundary and disagree within one frame (park but drip, or move
    // but run). Derive both decisions from the same instant.
    const double until = MsUntilTickDue(g.qcfMsec);
    const bool tickNow = until <= 0.0;
    const bool inWindow = (g.cfg.reserveMs > 0.0f) &&
                          (until <= static_cast<double>(g.cfg.reserveMs));
    // Strict moves every pre-frame queue aside (no between-tick execution);
    // otherwise move only inside the window. Either way the bytes rejoin the
    // one park and come back through DripOne after the tick.
    // A queued map/gamemap runs on this drain. Moving it aside would put the
    // level change behind the one-chunk-per-tick drip.
    if (g.cfg.dedicated && (g.cfg.strict || inWindow) && !QueueHasLevelChange())
        MoveQueueToPark();
    // Safe pre-frame drips are the non-strict path: strict runs its backlog
    // post-tick only (see SvFramePost), so dripping here would reintroduce
    // exactly the between-tick execution strict exists to stop.
    if (!g.cfg.strict && !tickNow && !inWindow)
        DripOne();
    RunDrain(original);
}

void cmdpark_ReadPacketsPre() {
    cmdpark::g.inReadPackets = true;
}
void cmdpark_ReadPacketsPost() {
    cmdpark::g.inReadPackets = false;
    cmdpark::g.passLevelChange = false;
}
void cmdpark_SvFramePre(int& msec) {
    using namespace cmdpark;
    (void)msec;
    g.inFrame = false;
    g.inSvFrame = true;
    g.measuring = g.ready && g.cfg.enabled && ServerRunning();
    if (g.measuring)
        g.svTimeAtPre = *Engine().svTime;
}
void cmdpark_SvFramePost(int msec) {
    using namespace cmdpark;
    (void)msec;
    g.inSvFrame = false;
    if (!g.ready)
        return;
    NoteFill();
    const bool ticked = g.measuring && *Engine().svTime != g.svTimeAtPre;
    if (ticked && HoldArmed()) {
    // Post-tick processing slot: the boundary just passed, so this is the
    // max-headroom moment. Drip one parked chunk and run whatever is
    // staged. This is what keeps a permanently-behind server draining —
    // pre-frame work near the boundary is always moved aside, post-tick
    // always makes progress. Under strict it is the ONLY execution (no
    // between-tick runs at all). Gated on HoldArmed so everything-off stays
    // bit-identical stock.
        DripOne();
        if (CommandsQueued()) {
            auto exec = detour_Cbuf_Execute::oCbuf_Execute;
            if (exec)
                RunDrain(exec);
        }
    }
    if (ticked)
        SetOutputs(static_cast<float>(g.cbufMaxMs), g.defers, CmdTextBytes(), g.fillMax);
    g.measuring = false;
}

void cmdpark_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    cmdpark::InitCvars();
    cmdpark::EnsureReady();
}

extern "C" void CmdPark_Shutdown() {
    cmdpark::FreePark();
    cmdpark::RestoreOutputs();
}
