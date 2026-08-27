// Host-side harness for src/features/tick_pacing/*.cpp.
//
// The feature's three translation units are #included below so the tests drive
// the real code (including its file-static state) against:
//   - a synthetic engine image: a heap buffer with valid DOS/NT headers and
//     sv.state / sv.time / svs.realtime at their real RVAs;
//   - a virtual clock that only moves when the harness (or a busy-wait in the
//     code under test) moves it;
//   - a transcription of the engine's own WinMain loop, Qcommon_frame and
//     SV_Frame, from IDA - see src/features/tick_pacing/engine.h for the
//     addresses and the shape of each.
//
// Every test runs the same loop twice, once with the hooks wired in and once
// without, so "what the stock engine would have done" is measured rather than
// assumed.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "windows.h"
#include "buddy_import.h"
#include "log.h"

// ---- virtual clock ---------------------------------------------------------
namespace fake {

// 1 count = 1 microsecond.
constexpr std::int64_t kQpcHz = 1000000;
std::int64_t qpc = 0;

void AdvanceMs(double ms) {
    if (ms > 0.0)
        qpc += static_cast<std::int64_t>(ms * 1000.0 + 0.5);
}
double NowMs() { return static_cast<double>(qpc) / 1000.0; }

/** The engine's Sys_Milliseconds(): timeGetTime(), whole milliseconds. */
std::uint32_t EngineMs() { return static_cast<std::uint32_t>(qpc / 1000); }

}  // namespace fake

BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    // A real QPC read costs tens of nanoseconds and perturbs nothing, so this
    // must not charge for one: the paced runs read the clock more often than
    // the stock ones, and charging per read shows up as virtual-time drift in
    // exactly the comparisons that assert the two are identical.
    //
    // A busy-wait does burn time, though, and the harness would hang without
    // it. So the clock moves only once a caller has read the same value many
    // times over - which is what a spin looks like and nothing else does.
    static std::int64_t lastSeen = -1;
    static int repeats = 0;
    if (fake::qpc == lastSeen) {
        if (++repeats >= 16) {
            ++fake::qpc;
            repeats = 0;
        }
    } else {
        lastSeen = fake::qpc;
        repeats = 0;
    }
    out->QuadPart = fake::qpc;
    return 1;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = fake::kQpcHz;
    return 1;
}

// ---- synthetic engine image ------------------------------------------------
namespace fake {

constexpr std::uint32_t kImageSize = 0x400000;
constexpr unsigned kRvaSvState = 0x3A1F20;
constexpr unsigned kRvaSvTime = 0x3A1F28;
constexpr unsigned kRvaSvsInitialized = 0x396DE0;
constexpr unsigned kRvaSvsRealtime = 0x396DE4;
constexpr unsigned kRvaDedicated = 0x249618;
constexpr unsigned kRvaCmdTextCursize = 0x23F830;
constexpr unsigned kRvaCmdWait = 0x23F838;

char* image = nullptr;

void InitImage() {
    image = static_cast<char*>(std::calloc(1, kImageSize));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.SizeOfImage = kImageSize;
}

std::int32_t&  SvState()  { return *reinterpret_cast<std::int32_t*>(image + kRvaSvState); }
std::uint32_t& SvTime()   { return *reinterpret_cast<std::uint32_t*>(image + kRvaSvTime); }
std::uint32_t& Realtime() { return *reinterpret_cast<std::uint32_t*>(image + kRvaSvsRealtime); }
std::int32_t&  SvsInit()  { return *reinterpret_cast<std::int32_t*>(image + kRvaSvsInitialized); }
void*&         DedicatedSlot() { return *reinterpret_cast<void**>(image + kRvaDedicated); }
std::int32_t&  CmdCursize() { return *reinterpret_cast<std::int32_t*>(image + kRvaCmdTextCursize); }
std::int32_t&  CmdWait()    { return *reinterpret_cast<std::int32_t*>(image + kRvaCmdWait); }

/** The command buffer, as a queue of per-command costs in ms. cmd_text.cursize
 *  is kept in step with it because Cbuf_Execute reads that global directly and
 *  so does the feature. */
std::vector<double> cmdQueue;
int cmdsQueuedTotal = 0;

void QueueCommand(double costMs) {
    cmdQueue.push_back(costMs);
    CmdCursize() = static_cast<std::int32_t>(cmdQueue.size());
    ++cmdsQueuedTotal;
}

void ClearCommands() {
    cmdQueue.clear();
    CmdCursize() = 0;
    CmdWait() = 0;
    cmdsQueuedTotal = 0;
}

}  // namespace fake

HMODULE GetModuleHandleA(const char* name) {
    if (name && std::strcmp(name, "SoF.exe") == 0)
        return fake::image;
    return nullptr;
}

SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len) {
    (void)addr;
    if (!mbi || len < sizeof(*mbi))
        return 0;
    mbi->BaseAddress = fake::image;
    mbi->AllocationBase = fake::image;
    mbi->AllocationProtect = PAGE_READWRITE;
    mbi->RegionSize = fake::kImageSize;
    mbi->State = MEM_COMMIT;
    mbi->Protect = PAGE_READWRITE;
    mbi->Type = 0;
    return sizeof(*mbi);
}

// ---- fake engine cvars -----------------------------------------------------
namespace fake {

// Matches the verified SoF cvar_t layout: name +0x00, string +0x04, value +0x18.
struct Cvar {
    char* name;
    char* string;
    char* latched;
    int flags;
    int unknown;
    int modified;
    float value;
    void* next;
};
static_assert(sizeof(Cvar) == 0x20, "cvar_t stub layout");

std::vector<Cvar*> cvars;

Cvar* Find(const char* name) {
    for (Cvar* c : cvars)
        if (std::strcmp(c->name, name) == 0)
            return c;
    return nullptr;
}

std::vector<std::string> logs;

}  // namespace fake

extern "C" void* Buddy_GetEngineCvar(const char* name, const char* value, int flags, void*) {
    if (fake::Cvar* existing = fake::Find(name))
        return existing;
    auto* c = static_cast<fake::Cvar*>(std::calloc(1, sizeof(fake::Cvar)));
    c->name = strdup(name);
    c->string = strdup(value);  // engine-owned allocation
    c->flags = flags;
    c->value = static_cast<float>(std::atof(value));
    fake::cvars.push_back(c);
    return c;
}

extern "C" float Buddy_ReadCvarValue(void* cv, float def) {
    if (!cv)
        return def;
    return *reinterpret_cast<float*>(static_cast<char*>(cv) + 0x18);
}

extern "C" void PrintOutImpl(int, const char* msg, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    fake::logs.push_back(buf);
}

#include "../../../src/features/tick_pacing/engine.cpp"
#include "../../../src/features/tick_pacing/cvar.cpp"
#include "../../../src/features/tick_pacing/tick_pacing.cpp"

// ---- test driver -----------------------------------------------------------
int g_failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } while (0)

// ---------------------------------------------------------------------------
// A transcription of the engine's loop: WinMain @0x20066300, Qcommon_frame
// @0x2001F720, SV_Frame @0x2005F5B0, SV_RunGameFrame @0x2005F3F0, and
// Cbuf_Execute @0x20018530 (which is stock Quake 2, cmd_wait and all).
// ---------------------------------------------------------------------------
struct Sim {
    double sleepMs = 1.0;         // what the loop's Sleep(1) actually costs
    double frameMs = 0.0;         // what the game frame costs
    double cmdCostMs = 0.0;       // what one console command costs

    // Two producers, because which one dominates decides how much the settle
    // correction is worth on a given server:
    //   cmdsPerTick   commands queued by the game frame itself - sofplus
    //                 reacting to game events. These are drained on the very
    //                 next iteration, at maximum headroom, so they rarely
    //                 straddle a boundary.
    //   asyncPeriodMs commands arriving at an arbitrary phase - Sys_ConsoleInput
    //                 (console, rcon) and anything on its own timer. These are
    //                 what actually land mid-tick.
    int    cmdsPerTick = 0;
    double asyncPeriodMs = 0.0;

    bool   paced = true;
    bool   svsInitialized = true;
};

struct Run {
    int framenum = 0;
    int cbufRuns = 0;             // Cbuf_Execute calls that actually drained
    int cmdsRun = 0;              // commands executed
    int highclamps = 0;
    int lowclamps = 0;
    double wallOrigin = 0.0;
    double wallMs = 0.0;
    std::vector<double> lateMs;   // per tick: fired-at minus ideal boundary
    std::vector<int> overshootMs; // per tick: svs.realtime - the deadline it
                                  // crossed. The engine's clamp deletes
                                  // whatever exceeds 100.
    std::uint32_t realtimeShadow = 0;

    double MaxLate() const {
        double m = 0.0;
        for (double v : lateMs) if (v > m) m = v;
        return m;
    }
    double AvgLate() const {
        if (lateMs.empty()) return 0.0;
        double s = 0.0;
        for (double v : lateMs) s += v;
        return s / static_cast<double>(lateMs.size());
    }
    int MaxOvershoot() const {
        int m = 0;
        for (int v : overshootMs) if (v > m) m = v;
        return m;
    }
};

Sim g_sim;
Run g_run;
double g_nextAsyncMs = 0.0;

/** How far the server's own clock has drifted from wall clock, in ms. The
 *  engine's contract is sv.time == 100 * framenum == elapsed wall time, so
 *  anything the feature does must keep this near zero. */
double RateError(const Run& r) {
    const double drift = r.wallMs - 100.0 * static_cast<double>(r.framenum);
    return drift < 0.0 ? -drift : drift;
}

/** Cbuf_Execute, transcribed instruction-for-instruction from 0x20018530:
 *  drain one line at a time, removing it from the buffer *before* running it,
 *  and break out on cmd_wait, clearing the flag on the way. */
void FakeCbufOriginal() {
    ++g_run.cbufRuns;
    while (fake::CmdCursize() != 0) {
        const double cost = fake::cmdQueue.front();
        fake::cmdQueue.erase(fake::cmdQueue.begin());
        fake::CmdCursize() = static_cast<std::int32_t>(fake::cmdQueue.size());

        fake::AdvanceMs(cost);
        ++g_run.cmdsRun;

        // The engine's own cmd_wait break (0x200185C1 / 0x200185E6). The
        // feature must never set this - see Test_DrainsAreNeverSplit.
        if (fake::CmdWait()) {
            fake::CmdWait() = 0;
            break;
        }
    }
}

/** SV_Frame, transcribed. The `if (!svs.initialized) return;` comes *before*
 *  the accumulate (0x2005F5D2 vs 0x2005F5D8). */
void EngineSvFrame(int msec) {
    if (!fake::SvsInit())
        return;

    fake::Realtime() += static_cast<std::uint32_t>(msec);

    if (fake::Realtime() < fake::SvTime()) {
        if (fake::SvTime() - fake::Realtime() > 100) {
            fake::Realtime() = fake::SvTime() - 100;   // "sv lowclamp"
            ++g_run.lowclamps;
        }
        return;
    }

    // SV_RunGameFrame
    g_run.overshootMs.push_back(static_cast<int>(fake::Realtime() - fake::SvTime()));
    ++g_run.framenum;
    fake::SvTime() = static_cast<std::uint32_t>(g_run.framenum) * 100u;

    const double ideal = g_run.wallOrigin + 100.0 * static_cast<double>(g_run.framenum - 1);
    g_run.lateMs.push_back(fake::NowMs() - ideal);

    fake::AdvanceMs(g_sim.frameMs);                    // the game frame itself
    for (int i = 0; i < g_sim.cmdsPerTick; ++i)        // sofplus, from the frame
        fake::QueueCommand(g_sim.cmdCostMs);

    if (fake::SvTime() < fake::Realtime()) {
        fake::Realtime() = fake::SvTime();             // "sv highclamp"
        ++g_run.highclamps;
    }
}

void EngineQcommonFrame(int msec) {
    if (g_sim.paced)
        tickpace_QcommonFrame(msec);

    if (g_sim.paced)
        tickpace_CbufExecute(&FakeCbufOriginal);
    else
        FakeCbufOriginal();

    if (g_sim.paced)
        tickpace_SvFramePre(msec);
    EngineSvFrame(msec);
    if (g_sim.paced)
        tickpace_SvFramePost(msec);
}

/** One WinMain iteration: Sleep(1), message pump, spin until at least one
 *  whole millisecond has passed, then Qcommon_frame(msec). */
void EngineLoopIteration(std::uint32_t& oldtime) {
    fake::AdvanceMs(g_sim.sleepMs);

    std::uint32_t newtime = fake::EngineMs();
    while (newtime - oldtime < 1) {          // the engine's own `while (time < 1)`
        fake::AdvanceMs(0.05);
        newtime = fake::EngineMs();
    }
    const int msec = static_cast<int>(newtime - oldtime);
    oldtime = newtime;

    // Sys_ConsoleInput / anything on its own timer, arriving mid-tick.
    if (g_sim.asyncPeriodMs > 0.0) {
        while (fake::NowMs() >= g_nextAsyncMs) {
            fake::QueueCommand(g_sim.cmdCostMs);
            g_nextAsyncMs += g_sim.asyncPeriodMs;
        }
    }

    g_run.realtimeShadow += static_cast<std::uint32_t>(msec);
    EngineQcommonFrame(msec);
}

void SetCvar(const char* name, float v) {
    fake::Cvar* c = fake::Find(name);
    if (c)
        c->value = v;
}
float CvarValue(const char* name) {
    fake::Cvar* c = fake::Find(name);
    return c ? c->value : -1.0f;
}

/** Boots a running map and runs `iterations` engine loop iterations. */
Run RunLoop(const Sim& sim, int iterations) {
    g_sim = sim;
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();

    fake::SvState() = 2;               // ss_game
    fake::SvsInit() = sim.svsInitialized ? 1 : 0;
    fake::SvTime() = 0;
    fake::Realtime() = 0;
    g_run.wallOrigin = fake::NowMs();
    g_nextAsyncMs = fake::NowMs();

    std::uint32_t oldtime = fake::EngineMs();
    for (int i = 0; i < iterations; ++i)
        EngineLoopIteration(oldtime);
    g_run.wallMs = fake::NowMs() - g_run.wallOrigin;
    return g_run;
}

/** Same, but bounded by wall clock instead of iteration count, so two runs
 *  being compared cover the same span and the same number of tick deadlines.
 *  Comparing at a fixed iteration count does not: firing ticks earlier changes
 *  how much wall time an iteration count buys. */
Run RunLoopFor(const Sim& sim, double wallMs) {
    g_sim = sim;
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();

    fake::SvState() = 2;
    fake::SvsInit() = sim.svsInitialized ? 1 : 0;
    fake::SvTime() = 0;
    fake::Realtime() = 0;
    g_run.wallOrigin = fake::NowMs();
    g_nextAsyncMs = fake::NowMs();

    const double until = fake::NowMs() + wallMs;
    std::uint32_t oldtime = fake::EngineMs();
    while (fake::NowMs() < until)
        EngineLoopIteration(oldtime);
    g_run.wallMs = fake::NowMs() - g_run.wallOrigin;
    return g_run;
}

/** The carry invariant. svs.realtime is deliberately no longer identical to
 *  the engine's own accumulation - the settle correction is carried forward in
 *  it rather than added and taken back off (which is what caused lowclamp
 *  spam). What must still hold is that the correction only ever pushes it
 *  *forward*, and never by more than one frame's worth: svs.realtime is still
 *  `newtime - starttime` plus a bounded, non-negative amount of "time that has
 *  passed since the engine sampled newtime". */
void CheckCarry(const Run& r, const char* what) {
    const std::int32_t carry =
        static_cast<std::int32_t>(fake::Realtime() - r.realtimeShadow);
    CHECK(carry >= 0, "%s pushed svs.realtime %d ms *behind* the engine's own clock",
          what, -carry);
    CHECK(carry <= 100, "%s left a %d ms correction in svs.realtime (max 100)",
          what, carry);
    CHECK(static_cast<std::uint32_t>(carry) == tickpace::g.carried,
          "%s: svs.realtime is %d ms ahead but the feature only accounts for %u",
          what, carry, tickpace::g.carried);
}

double AvgOvershoot(const Run& r) {
    if (r.overshootMs.empty()) return 0.0;
    double s = 0.0;
    for (int v : r.overshootMs) s += v;
    return s / static_cast<double>(r.overshootMs.size());
}

/** The default tunables for a test that is not exercising a specific knob. */
void ResetCvars() {
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_spin_ms", 0);
    SetCvar("_sofbuddy_tickpace_reserve_ms", 0);
    SetCvar("_sofbuddy_tickpace_defer_max_ms", 200);
}

// ---------------------------------------------------------------------------
void Test_RealtimeIsNeverInflated() {
    std::printf("svs.realtime keeps its `newtime - starttime` meaning\n");
    ResetCvars();

    // Console work arriving at an arbitrary phase, 30ms of it every 40ms: the
    // case where the correction is large and applied on nearly every drain.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 30.0; sim.asyncPeriodMs = 40.0;
    Run paced = RunLoop(sim, 400);

    CHECK(paced.highclamps == 0, "unexpected highclamps: %d", paced.highclamps);
    CheckCarry(paced, "settle");
    CHECK(RateError(paced) <= 100.0,
          "%d ticks over %.0fms of wall clock is not 10Hz",
          paced.framenum, paced.wallMs);

    // And with the reserve on, so the limiter is in play too.
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);
    Run reserved = RunLoop(sim, 400);
    CheckCarry(reserved, "reserve");
    CHECK(RateError(reserved) <= 100.0,
          "reserve broke the 10Hz rate: %d ticks over %.0fms",
          reserved.framenum, reserved.wallMs);
}

void Sweep_Lowclamps() {
    std::printf("SWEEP: lowclamps across the parameter space\n");
    const double costs[]  = {0.0, 2.0, 8.0, 30.0, 70.0, 140.0, 400.0};
    const double periods[]= {0.0, 7.0, 40.0, 95.0, 100.0, 220.0};
    const double sleeps[] = {1.0, 4.0, 17.0};
    const double frames[] = {0.0, 20.0, 60.0, 110.0};
    const int    perTick[]= {0, 1, 6};
    const float  reserves[]={0.0f, 3.0f};
    const float  spins[]  = {0.0f, 5.0f};
    int worst = 0; char worstDesc[256] = "none";
    int configs = 0;
    for (double c : costs) for (double pd : periods) for (double sl : sleeps)
    for (double fr : frames) for (int pt : perTick) for (float rs : reserves) for (float sp : spins) {
        ResetCvars();
        SetCvar("_sofbuddy_tickpace_reserve_ms", rs);
        SetCvar("_sofbuddy_tickpace_spin_ms", sp);
        Sim sim; sim.sleepMs = sl; sim.cmdCostMs = c; sim.asyncPeriodMs = pd;
        sim.frameMs = fr; sim.cmdsPerTick = pt;
        Sim st = sim; st.paced = false;
        Run base = RunLoopFor(st, 4000.0);
        Run pac  = RunLoopFor(sim, 4000.0);
        ++configs;
        const int extra = pac.lowclamps - base.lowclamps;
        if (extra > worst) {
            worst = extra;
            std::snprintf(worstDesc, sizeof(worstDesc),
                "cost=%.0f period=%.0f sleep=%.0f frame=%.0f perTick=%d reserve=%.0f spin=%.0f "
                "-> stock %d, paced %d (ticks %d)",
                c, pd, sl, fr, pt, (double)rs, (double)sp, base.lowclamps, pac.lowclamps, pac.framenum);
        }
    }
    std::printf("    %d configs; worst extra lowclamps = %d\n    %s\n", configs, worst, worstDesc);
}

void Test_NoSpuriousLowclamps() {
    std::printf("the correction never leaves svs.realtime behind enough to lowclamp\n");
    ResetCvars();

    // SV_Frame @0x2005F61D lowclamps when `sv.time - svs.realtime > 100`. The
    // stock engine cannot reach that from the tick path: a tick only fires
    // once svs.realtime has caught sv.time, so the deficit straight after one
    // is at most 100. Bursty console work is what breaks a correction that is
    // applied and then taken back off - a big drain fires the tick, sv.time
    // goes up 100, svs.realtime goes back down by the correction, and the very
    // next frame is 100 + correction behind.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 30.0; sim.asyncPeriodMs = 40.0;

    Sim stock = sim; stock.paced = false;
    Run base = RunLoopFor(stock, 8000.0);
    CHECK(base.lowclamps == 0, "the stock engine lowclamped %d times - bad baseline",
          base.lowclamps);

    Run paced = RunLoopFor(sim, 8000.0);
    std::printf("    lowclamps - stock: %d | paced: %d   (ticks %d, saved %s)\n",
                base.lowclamps, paced.lowclamps, paced.framenum,
                fake::Find("_sofbuddy_tickpace_saved")->string);
    CHECK(paced.lowclamps == 0, "the correction caused %d lowclamps", paced.lowclamps);

    // And with the reserve on, so the drain gate is in play as well.
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);
    Run reserved = RunLoopFor(sim, 8000.0);
    CHECK(reserved.lowclamps == 0, "the reserve caused %d lowclamps", reserved.lowclamps);

    // Turning the feature off mid-run must unwind the correction, not strand
    // it in svs.realtime.
    SetCvar("_sofbuddy_tickpace_reserve_ms", 0);
    Run half = RunLoop(sim, 200);
    SetCvar("_sofbuddy_tickpace", 0);
    for (int i = 0; i < 50; ++i) {
        std::uint32_t oldtime = fake::EngineMs();
        EngineLoopIteration(oldtime);
    }
    CHECK(tickpace::g.carried == 0, "correction left stranded after disabling: %u",
          tickpace::g.carried);
    (void)half;
}

void Test_TicksFireCloserToTheirBoundary() {
    std::printf("a tick that comes due during a drain no longer waits a whole iteration\n");
    ResetCvars();

    // 25ms of console work arriving every 37ms. 37 and 100 are deliberately
    // not commensurate: an arrival period that divides the tick phase-locks
    // the drain to a fixed offset and the boundary is never straddled, which
    // is exactly the case this correction does not address.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 25.0; sim.asyncPeriodMs = 37.0;
    sim.frameMs = 5.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 20000.0);
    sim.paced = true;
    Run paced = RunLoopFor(sim, 20000.0);

    std::printf("    stock:  %d ticks, wall late avg %.1fms max %.1fms | overshoot avg %.1fms max %dms\n",
                stock.framenum, stock.AvgLate(), stock.MaxLate(),
                AvgOvershoot(stock), stock.MaxOvershoot());
    std::printf("    settle: %d ticks, wall late avg %.1fms max %.1fms | overshoot avg %.1fms max %dms\n",
                paced.framenum, paced.AvgLate(), paced.MaxLate(),
                AvgOvershoot(paced), paced.MaxOvershoot());
    std::printf("    ticks rescued from the following iteration: %.0f\n",
                CvarValue("_sofbuddy_tickpace_saved"));

    // Wall lateness is what a client feels: the decision is made at the same
    // instant either way, settle just lets it come out the right way round, so
    // a tick can only ever fire at the same wall time or earlier.
    CHECK(paced.AvgLate() < stock.AvgLate(),
          "paced avg wall lateness %.2f not better than stock %.2f",
          paced.AvgLate(), stock.AvgLate());
    CHECK(paced.MaxLate() <= stock.MaxLate() + 0.01,
          "paced max wall lateness %.2f worse than stock %.2f",
          paced.MaxLate(), stock.MaxLate());
    CHECK(CvarValue("_sofbuddy_tickpace_saved") > 0.0f,
          "saved counter never moved: %.0f", CvarValue("_sofbuddy_tickpace_saved"));
}

void Test_ReserveKeepsSmallDrainsOffTheBoundary() {
    std::printf("the reserve keeps ordinary console work off the tick boundary\n");
    ResetCvars();

    // Many small commands - the shape a sofplus server actually produces
    // between the occasional heavy one. 3ms each, one every 7ms: 43% duty,
    // and 7 against 100 walks the arrival across every phase.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 3.0; sim.asyncPeriodMs = 7.0;
    sim.frameMs = 10.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 30000.0);
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);
    sim.paced = true;
    Run reserved = RunLoopFor(sim, 30000.0);

    std::printf("    max overshoot - stock: %dms | +reserve: %dms   (drains held: %lld)\n",
                stock.MaxOvershoot(), reserved.MaxOvershoot(), tickpace::g.defers);

    CHECK(reserved.MaxOvershoot() <= stock.MaxOvershoot(),
          "reserve made the clamp margin worse (%d vs %d)",
          reserved.MaxOvershoot(), stock.MaxOvershoot());
    CHECK(RateError(reserved) <= 100.0,
          "reserve broke the 10Hz rate: %d ticks over %.0fms",
          reserved.framenum, reserved.wallMs);
    CHECK(reserved.cmdsRun + static_cast<int>(fake::cmdQueue.size()) == fake::cmdsQueuedTotal,
          "%d run + %zu queued != %d queued",
          reserved.cmdsRun, fake::cmdQueue.size(), fake::cmdsQueuedTotal);
}

void Test_ReserveDoesNotHarmASaturatedServer() {
    std::printf("a drain bigger than the gate is left alone, not held into a spiral\n");
    ResetCvars();

    // One 70ms command every 90ms - 78% duty plus a 10ms frame. No phase of a
    // tick has room for a 70ms drain, so holding cannot help and, if the gate
    // were allowed to grow to fit, would defer arriving work and compound.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 70.0; sim.asyncPeriodMs = 90.0;
    sim.frameMs = 10.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 30000.0);
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);
    sim.paced = true;
    Run reserved = RunLoopFor(sim, 30000.0);

    std::printf("    max overshoot - stock: %dms | +reserve: %dms | max late %.0fms vs %.0fms\n",
                stock.MaxOvershoot(), reserved.MaxOvershoot(),
                reserved.MaxLate(), stock.MaxLate());

    CHECK(reserved.MaxOvershoot() <= stock.MaxOvershoot() + 5,
          "reserve made overshoot worse on a saturated server (%d vs %d)",
          reserved.MaxOvershoot(), stock.MaxOvershoot());
    CHECK(reserved.MaxLate() <= stock.MaxLate() + 20.0,
          "reserve made lateness worse (%.0fms vs %.0fms)",
          reserved.MaxLate(), stock.MaxLate());
    CHECK(RateError(reserved) <= 150.0,
          "the server fell behind: %d ticks over %.0fms",
          reserved.framenum, reserved.wallMs);
}

void Test_DrainsAreNeverSplit() {
    std::printf("a drain that starts runs to completion, and cmd_wait is never touched\n");
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);

    g_sim = Sim();
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();
    fake::SvState() = 2;

    // 20 commands of 5ms each - 100ms of work - queued with only 50ms of
    // headroom, i.e. a drain that provably will not fit.
    //
    // The old drain limiter stopped part-way here, which is what corrupted
    // sofplus: sp_sc_func_exec_ binds a function's arguments into the global
    // `~1`/`~2` cvars and then inserts the body, and a body left queued across
    // a tick has those cvars rebound by whatever sofplus's frame hook inserts
    // ahead of it. Overrunning the boundary is the lesser evil.
    fake::Realtime() = 100000;
    fake::SvTime() = 100050;
    for (int i = 0; i < 20; ++i)
        fake::QueueCommand(5.0);

    int msec = 1;
    tickpace_QcommonFrame(msec);
    tickpace_CbufExecute(&FakeCbufOriginal);

    CHECK(g_run.cmdsRun == 20 || g_run.cmdsRun == 0,
          "drain was split: %d of 20 commands ran", g_run.cmdsRun);
    if (g_run.cmdsRun == 20)
        CHECK(fake::cmdQueue.empty(), "%zu commands left after a drain",
              fake::cmdQueue.size());
    CHECK(fake::CmdWait() == 0, "the feature set cmd_wait");
}

void Test_StartGateIsBounded() {
    std::printf("the start gate never waits for more than its ceiling\n");
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);

    g_sim = Sim();
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();
    fake::SvState() = 2;
    fake::Realtime() = 200000;

    // Teach the window that drains here cost ~40ms.
    for (int round = 0; round < 4; ++round) {
        fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 100;
        fake::QueueCommand(40.0);
        int msec = 1;
        tickpace_QcommonFrame(msec);
        tickpace_CbufExecute(&FakeCbufOriginal);
    }
    CHECK(tickpace::RecentWorstDrainMs() > 35.0,
          "drain window learned %.1fms, expected ~40",
          tickpace::RecentWorstDrainMs());

    // ...but the gate is capped, so a 40ms drain is NOT held off for 43ms of
    // headroom. Holding that long would defer 40ms of arriving work and make
    // the next drain longer still.
    CHECK(tickpace::RoomNeededMs(3.0f) <= 10.0,
          "gate grew to %.1fms despite the ceiling", tickpace::RoomNeededMs(3.0f));

    // 20ms of headroom is over the gate, so the drain runs rather than being
    // held for a window it will never get.
    const long long before = tickpace::g.defers;
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 20;
    fake::QueueCommand(40.0);
    int msec = 1;
    tickpace_QcommonFrame(msec);
    tickpace_CbufExecute(&FakeCbufOriginal);
    CHECK(tickpace::g.defers == before, "drain was held with 20ms of room");
    CHECK(fake::cmdQueue.empty(), "drain did not run");
}

void Test_NestedDrainRunsWhole() {
    std::printf("a nested Cbuf_ExecuteText(EXEC_NOW) runs whole\n");
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);

    g_sim = Sim();
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();
    fake::SvState() = 2;
    fake::Realtime() = 100000;
    fake::SvTime() = 100200;      // 200ms away: the outer drain starts

    static int nested = 0;
    static int outer = 0;
    nested = 0;
    outer = 0;
    struct Cmd {
        static void Run() {
            ++outer;
            // This command carries us past the tick boundary and then issues
            // Cbuf_ExecuteText(EXEC_NOW, ...), which re-enters Cbuf_Execute.
            // That drain must complete: its caller expects the text to have
            // executed by the time the call returns.
            fake::AdvanceMs(250.0);
            fake::QueueCommand(1.0);
            fake::QueueCommand(1.0);
            const int before = g_run.cmdsRun;
            tickpace_CbufExecute(&FakeCbufOriginal);
            nested = g_run.cmdsRun - before;
        }
    };

    int msec = 1;
    tickpace_QcommonFrame(msec);
    tickpace::g.inCbuf = true;
    tickpace::g.nestedCbuf = 0;
    Cmd::Run();
    tickpace::g.inCbuf = false;

    CHECK(outer == 1, "outer command did not run");
    CHECK(nested == 2, "nested drain was cut short: %d of 2 commands", nested);
    CHECK(fake::cmdQueue.empty(), "%zu commands left after the nested drain",
          fake::cmdQueue.size());
    CHECK(fake::CmdWait() == 0, "the feature set cmd_wait");
}

void Test_BehindServerStillDrains() {
    std::printf("a permanently-behind server still runs its command buffer\n");
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_reserve_ms", 50);
    SetCvar("_sofbuddy_tickpace_defer_max_ms", 200);

    // Every game frame overruns a whole tick, so the boundary is always "due".
    // Without the valve - and without the valve disarming the limiter - the
    // console and every sofplus timer would go dead.
    Sim sim; sim.sleepMs = 1.0; sim.frameMs = 130.0;
    sim.cmdCostMs = 2.0; sim.cmdsPerTick = 4;
    Run r = RunLoop(sim, 80);

    std::printf("    %d commands run of %d queued over %.0fms\n",
                r.cmdsRun, fake::cmdsQueuedTotal, r.wallMs);
    CHECK(r.cmdsRun > 0, "command buffer starved completely");
    CHECK(r.cmdsRun >= fake::cmdsQueuedTotal / 2,
          "only %d of %d commands ran - the valve is not draining properly",
          r.cmdsRun, fake::cmdsQueuedTotal);
}

void Test_MapLoadIntervalIsNotCredited() {
    std::printf("a multi-second drain (a `map` command) is not credited as tick time\n");
    ResetCvars();

    g_sim = Sim();
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();
    fake::SvState() = 2;

    // SpawnServer has just run inside Cbuf_Execute: sv.time is 1000, realtime
    // is 0, and four seconds of wall clock went by loading the map.
    int msec = 1;
    tickpace_QcommonFrame(msec);
    fake::SvTime() = 1000;
    fake::Realtime() = 0;
    fake::AdvanceMs(4000.0);

    tickpace_SvFramePre(msec);
    const std::uint32_t afterPre = fake::Realtime();
    CHECK(afterPre <= 100, "credited %u ms of map load to svs.realtime", afterPre);

    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    // The stock engine lowclamps here (sv.time is 1000, realtime is ~101).
    // Without the cap, svs.realtime would have taken the whole 4000ms, run a
    // tick immediately and taken a ~3900ms highclamp - the bogus map-change
    // spike clamp_monitor documents.
    CHECK(g_run.framenum == 0, "map-load interval fired a bogus tick");
    CHECK(g_run.lowclamps == 1, "expected the engine's own lowclamp, got %d",
          g_run.lowclamps);
}

void Test_SpinPullsTicksOntoTheBoundary() {
    std::printf("the pre-tick spin lands ticks on the boundary when Sleep is coarse\n");
    ResetCvars();

    // A host whose Sleep(1) really costs ~8ms: the boundary can only be
    // noticed on an 8ms grid, so a tick is on average 4ms late.
    Sim sim; sim.sleepMs = 8.0; sim.frameMs = 2.0;

    Run without = RunLoop(sim, 300);
    SetCvar("_sofbuddy_tickpace_spin_ms", 10);
    Run with = RunLoop(sim, 300);

    std::printf("    spin off: avg late %.2fms | spin on: avg %.2fms\n",
                without.AvgLate(), with.AvgLate());

    CHECK(with.AvgLate() < without.AvgLate() * 0.5,
          "spin did not halve average lateness (%.2f vs %.2f)",
          with.AvgLate(), without.AvgLate());
    CHECK(RateError(with) <= 100.0,
          "spin broke the 10Hz rate: %d ticks over %.0fms",
          with.framenum, with.wallMs);
}

void Test_DisabledIsByteIdentical() {
    std::printf("_sofbuddy_tickpace 0 leaves the engine exactly as it found it\n");
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);

    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 20.0; sim.asyncPeriodMs = 45.0;
    sim.frameMs = 5.0;

    sim.paced = false;
    Run stock = RunLoop(sim, 400);
    const std::uint32_t stockRealtime = fake::Realtime();
    const std::uint32_t stockSvTime = fake::SvTime();
    const int stockCmds = stock.cmdsRun;

    SetCvar("_sofbuddy_tickpace", 0);
    sim.paced = true;
    Run off = RunLoop(sim, 400);

    CHECK(off.framenum == stock.framenum, "tick count differs: %d vs %d",
          off.framenum, stock.framenum);
    CHECK(fake::Realtime() == stockRealtime, "svs.realtime differs: %u vs %u",
          fake::Realtime(), stockRealtime);
    CHECK(fake::SvTime() == stockSvTime, "sv.time differs: %u vs %u",
          fake::SvTime(), stockSvTime);
    CHECK(off.cmdsRun == stockCmds, "%d commands ran vs %d",
          off.cmdsRun, stockCmds);
    SetCvar("_sofbuddy_tickpace", 1);
}

void Test_ReserveZeroIsStockScheduling() {
    std::printf("_sofbuddy_tickpace_reserve_ms 0 restores stock command scheduling\n");
    ResetCvars();

    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 20.0; sim.asyncPeriodMs = 45.0;
    sim.frameMs = 5.0;

    sim.paced = false;
    Run stock = RunLoop(sim, 400);
    const int stockCmds = stock.cmdsRun;
    const int stockDrains = stock.cbufRuns;

    sim.paced = true;                      // settle on, reserve 0
    Run paced = RunLoop(sim, 400);

    CHECK(paced.cbufRuns == stockDrains,
          "Cbuf_Execute ran %d times vs %d - scheduling changed with reserve 0",
          paced.cbufRuns, stockDrains);
    CHECK(paced.cmdsRun == stockCmds, "%d commands ran vs %d",
          paced.cmdsRun, stockCmds);
    CHECK(tickpace::g.defers == 0, "held %lld drains with reserve 0",
          tickpace::g.defers);
}

void Test_HighclampAnchorIsLeftAlone() {
    std::printf("a clamp re-anchors svs.realtime and the correction is not taken back off\n");
    ResetCvars();

    // Frames that overrun a whole tick: every tick highclamps.
    Sim sim; sim.sleepMs = 1.0; sim.frameMs = 140.0;
    sim.cmdCostMs = 10.0; sim.cmdsPerTick = 1;
    Run r = RunLoop(sim, 40);

    CHECK(r.highclamps > 0, "test did not produce a clamp");
    // After a highclamp the engine's own invariant is svs.realtime == sv.time.
    // If the correction had been subtracted anyway, realtime would sit below
    // sv.time and the very next tick would fire early, forever.
    CHECK(fake::Realtime() <= fake::SvTime(),
          "svs.realtime=%u is ahead of sv.time=%u after clamping",
          fake::Realtime(), fake::SvTime());
    CHECK(fake::SvTime() - fake::Realtime() <= 100,
          "svs.realtime=%u fell %u ms behind sv.time=%u - correction was double-removed",
          fake::Realtime(), fake::SvTime() - fake::Realtime(), fake::SvTime());
}

void Test_NoWritesOutsideSsGame() {
    std::printf("nothing is touched while sv.state is not ss_game\n");
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);

    // SV_Frame itself does not look at sv.state (it gates on svs.initialized),
    // so the engine keeps accumulating and can even run a tick here. What must
    // hold is that the paced run is indistinguishable from the stock one:
    // ss_loading is where map-load wall time lands, and correcting into it is
    // what gave clamp_monitor its false multi-second spike.
    auto loading = [](bool paced) {
        Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 30.0; sim.asyncPeriodMs = 40.0;
        sim.paced = paced;
        g_sim = sim;
        g_run = Run();
        tickpace::g = tickpace::State();
        fake::ClearCommands();

        fake::SvState() = 1;             // ss_loading
        fake::SvTime() = 1000;
        fake::Realtime() = 0;
        g_run.wallOrigin = fake::NowMs();
        g_nextAsyncMs = fake::NowMs();

        std::uint32_t oldtime = fake::EngineMs();
        for (int i = 0; i < 50; ++i)
            EngineLoopIteration(oldtime);
        return fake::Realtime();
    };

    const std::uint32_t stock = loading(false);
    const int stockTicks = g_run.framenum;
    const int stockCmds = g_run.cmdsRun;
    const std::uint32_t paced = loading(true);

    CHECK(paced == stock, "svs.realtime=%u during ss_loading, stock engine had %u",
          paced, stock);
    CHECK(g_run.framenum == stockTicks, "tick count during ss_loading changed: %d vs %d",
          g_run.framenum, stockTicks);
    CHECK(g_run.cmdsRun == stockCmds, "%d commands ran during ss_loading vs %d",
          g_run.cmdsRun, stockCmds);
    fake::SvState() = 2;
}

void Test_SvsUninitializedIsSafe() {
    // Not reachable on a real engine (svs.initialized is set for as long as
    // sv.state is ss_game, which Pre gates on), but if it ever were, a
    // correction added and never accumulated would be stranded and would
    // inflate svs.realtime once per iteration until the server ran its clock
    // away. The Post hook recognises the shape and undoes it.
    std::printf("SV_Frame's early return (svs.initialized == 0) leaves no correction behind\n");
    ResetCvars();

    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 30.0; sim.asyncPeriodMs = 40.0;
    sim.svsInitialized = false;
    RunLoop(sim, 50);

    CHECK(fake::Realtime() == 0,
          "svs.realtime=%u was written while the engine never accumulates it",
          fake::Realtime());
}

void Test_ShutdownRestoresCvarStrings() {
    std::printf("detach hands cvar_t.string back to the engine\n");
    fake::Cvar* late = fake::Find("_sofbuddy_tickpace_late_avg");
    CHECK(late != nullptr, "output cvar missing");
    if (!late)
        return;
    char* engineOwned = tickpace::g_outLateAvg.original;
    CHECK(engineOwned != nullptr, "bind did not capture the engine string");
    CHECK(late->string != engineOwned, "publish did not repoint cvar_t.string");
    TickPacing_Shutdown();
    CHECK(late->string == engineOwned,
          "detach left cvar_t.string pointing into this image");
}

int main() {
    fake::InitImage();
    tickpace::InitCvars();

    // The engine's `dedicated` cvar, as WinMain reads it.
    fake::DedicatedSlot() = Buddy_GetEngineCvar("dedicated", "1", 0, nullptr);

    Test_RealtimeIsNeverInflated();
    if (std::getenv("SWEEP")) { Sweep_Lowclamps(); return 0; }
    Test_NoSpuriousLowclamps();
    Test_TicksFireCloserToTheirBoundary();
    Test_ReserveKeepsSmallDrainsOffTheBoundary();
    Test_ReserveDoesNotHarmASaturatedServer();
    Test_DrainsAreNeverSplit();
    Test_StartGateIsBounded();
    Test_NestedDrainRunsWhole();
    Test_BehindServerStillDrains();
    Test_MapLoadIntervalIsNotCredited();
    Test_SpinPullsTicksOntoTheBoundary();
    Test_DisabledIsByteIdentical();
    Test_ReserveZeroIsStockScheduling();
    Test_HighclampAnchorIsLeftAlone();
    Test_NoWritesOutsideSsGame();
    Test_SvsUninitializedIsSafe();
    Test_ShutdownRestoresCvarStrings();

    if (g_failures == 0)
        std::printf("\nAll tick_pacing tests passed.\n");
    else
        std::printf("\n%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
