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

void ClampMonitor_LogSessionSummary() {}

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

namespace qpcstub {
std::int64_t lastSeen = -1;
int repeats = 0;
void Reset() { lastSeen = -1; repeats = 0; }
}  // namespace qpcstub

BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    // A real QPC read costs tens of nanoseconds and perturbs nothing, so this
    // must not charge for one: the paced runs read the clock more often than
    // the stock ones, and charging per read shows up as virtual-time drift in
    // exactly the comparisons that assert the two are identical.
    //
    // A busy-wait does burn time, though, and the harness would hang without
    // it. So the clock moves only once a caller has read the same value many
    // times over - which is what a spin looks like and nothing else does.
    //
    // State is resettable (see Reset) because exact-ms assertions elsewhere
    // must not depend on how many reads earlier tests happened to perform.
    if (fake::qpc == qpcstub::lastSeen) {
        if (++qpcstub::repeats >= 16) {
            ++fake::qpc;
            qpcstub::repeats = 0;
        }
    } else {
        qpcstub::lastSeen = fake::qpc;
        qpcstub::repeats = 0;
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
    if (name && std::strcmp(name, "kernel32") == 0) {
        static int kernelToken = 0;
        return &kernelToken;
    }
    return nullptr;
}

void FakeKernelSleep(DWORD) {}

FARPROC GetProcAddress(HMODULE, const char* name) {
    if (name && std::strcmp(name, "Sleep") == 0)
        return reinterpret_cast<FARPROC>(&FakeKernelSleep);
    return nullptr;
}

BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) {
    if (old)
        *old = PAGE_READWRITE;
    return 1;
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

// ---- cmdpark backlog stubs -------------------------------------------------
// Production definitions live in src/features/cpu_optimizations/cmdtext_parking/
// cmdpark.cpp (same DLL, direct call). The harness links only the tick_pacing
// TUs, so stub them here — controllable, so the gate test can drive them.
namespace cmdpark {
int g_stubParkBytes = 0;
bool g_stubHoldArmed = false;
int ParkBytes() { return g_stubParkBytes; }
bool HoldArmed() { return g_stubHoldArmed; }
}  // namespace cmdpark

#define TICKPACE_HARNESS 1
#include "../../../src/features/cpu_optimizations/tick_pacing/engine.cpp"
#include "../../../src/features/cpu_optimizations/tick_pacing/cvar.cpp"
#include "../../../src/features/cpu_optimizations/tick_pacing/tick_pacing.cpp"
#include "../../../src/features/cpu_optimizations/tick_pacing/sleep_gate.cpp"

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
    double coarseSleepMs = 0.0;   // >0: Sleep(1) really costs this (coarse host
                                  // timer) unless the Sleep-skip gate fires
    double frameMs = 0.0;         // what the game frame costs
    double cmdCostMs = 0.0;       // what one console command costs
    double cmdCostJitterMs = 0.0; // + uniform [0, jitter) per command (live
                                  // sofplus drains vary 1-8ms; default 0 keeps
                                  // every older test bit-identical)

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
    double asyncOffsetMs = 0.0;   // first async command at wallOrigin+offset
    double iterCmdMs = 0.0;       // extra command cost queued every loop iter

    bool   paced = true;
    bool   svsInitialized = true;
    bool   parkStrict = false;    // cmdpark strict: hold pre-SV drain, drip after tick
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

    // Q2/SoF CL_AddEntities showclamp: cl.time vs cl.frame.servertime.
    // Instant 0-ping snapshots at tick wall time; cl.time tracks wall between
    // snapshots and snaps back on highclamp (cl_ents.c / SoF.exe 0x20004460).
    int    clientHighclamps = 0;
    int    clientHighclampMax = 0;
    double lastSnapWall = -1.0;
    double clTime = 0.0;
    bool   clInit = false;
    int    lastSnapServertime = 0;
    double clientPollAt = 0.0;
    int    showclampPrints = 0;     // CL_AddEntities prints at 7ms client frames
    int    showclampPrintMax = 0;
    int    snapGaps = 0;
    int    snapGapGe5 = 0;          // wall gap between snapshots >= 105ms
    double snapGapSum = 0.0;
    double snapGapMax = 0.0;

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
std::vector<double> g_park;
// Deterministic per-command cost jitter (own LCG; reset every run so all
// arms being compared see the identical sequence — a fair A/B).
std::uint32_t g_jitterState = 0;

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
        double cost = fake::cmdQueue.front();
        fake::cmdQueue.erase(fake::cmdQueue.begin());
        fake::CmdCursize() = static_cast<std::int32_t>(fake::cmdQueue.size());

        if (g_sim.cmdCostJitterMs > 0.0) {
            g_jitterState = g_jitterState * 1103515245u + 12345u;
            cost += (static_cast<double>((g_jitterState >> 7) & 0x7fffffff) /
                     2147483648.0) * g_sim.cmdCostJitterMs;
        }
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

void ClientPoll();

/** SV_Frame, transcribed. The `if (!svs.initialized) return;` comes *before*
 *  the accumulate (0x2005F5D2 vs 0x2005F5D8). */
void EngineSvFrame(int msec) {
    if (!fake::SvsInit())
        return;

    fake::Realtime() += static_cast<std::uint32_t>(msec);

    if (g_sim.paced)
        tickpace_ReadPacketsPre();

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

    // Snapshot is sent at the end of SV_Frame (CL_SendClientMessages).
    ClientPoll();
    const double wall = fake::NowMs();
    const int servertime = static_cast<int>(fake::SvTime());
    if (!g_run.clInit) {
        g_run.clTime = 0.0;
        g_run.clInit = true;
        g_run.clientPollAt = wall;
    } else {
        const double gap = wall - g_run.lastSnapWall;
        ++g_run.snapGaps;
        g_run.snapGapSum += gap;
        if (gap > g_run.snapGapMax)
            g_run.snapGapMax = gap;
        if (gap >= 105.0)
            ++g_run.snapGapGe5;
        const int hi = static_cast<int>(gap - 100.0);
        if (hi > 0) {
            ++g_run.clientHighclamps;
            if (hi > g_run.clientHighclampMax)
                g_run.clientHighclampMax = hi;
        }
    }
    // CL_ParseFrame @0x2000306c: snap cl.time into [servertime-100, servertime].
    if (g_run.clTime > static_cast<double>(servertime))
        g_run.clTime = static_cast<double>(servertime);
    else if (g_run.clTime < static_cast<double>(servertime) - 100.0)
        g_run.clTime = static_cast<double>(servertime) - 100.0;
    g_run.lastSnapServertime = servertime;
    g_run.lastSnapWall = wall;
}

/** Dedicated server has no CL_Frame; this is the connecting client's
 *  CL_UpdateSimulationTimeInfo + CL_AddEntities at ~7ms/frame (the live
 *  "high clamp 7" print is one client frame of overshoot, not drain remainder). */
void ClientPoll() {
    if (!g_run.clInit)
        return;
    constexpr double kClFrameMs = 7.0;
    const double now = fake::NowMs();
    while (g_run.clientPollAt + kClFrameMs <= now) {
        g_run.clientPollAt += kClFrameMs;
        g_run.clTime += kClFrameMs;
        const int hi = static_cast<int>(g_run.clTime -
                                        static_cast<double>(g_run.lastSnapServertime));
        if (g_run.clTime > static_cast<double>(g_run.lastSnapServertime)) {
            ++g_run.showclampPrints;
            if (hi > g_run.showclampPrintMax)
                g_run.showclampPrintMax = hi;
            g_run.clTime = static_cast<double>(g_run.lastSnapServertime);
        }
    }
}

void EngineQcommonFrame(int msec) {
    if (g_sim.paced)
        tickpace_QcommonFrame(msec);
    if (g_sim.parkStrict && !fake::cmdQueue.empty()) {
        g_park.insert(g_park.end(), fake::cmdQueue.begin(), fake::cmdQueue.end());
        fake::cmdQueue.clear();
        fake::CmdCursize() = 0;
    }
    cmdpark::g_stubHoldArmed = g_sim.parkStrict;
    cmdpark::g_stubParkBytes = static_cast<int>(g_park.size()) * 16;
    FakeCbufOriginal();
    if (g_sim.paced)
        tickpace_SvFramePre(msec);
    const int svBefore = static_cast<int>(fake::SvTime());
    EngineSvFrame(msec);
    if (g_sim.paced)
        tickpace_SvFramePost(msec);
    const bool ticked = static_cast<int>(fake::SvTime()) != svBefore;
    if (g_sim.parkStrict && ticked && !g_park.empty()) {
        fake::cmdQueue.swap(g_park);
        fake::CmdCursize() = static_cast<int>(fake::cmdQueue.size());
        FakeCbufOriginal();
        g_park.swap(fake::cmdQueue);
        fake::CmdCursize() = static_cast<int>(fake::cmdQueue.size());
    }
    cmdpark::g_stubParkBytes = static_cast<int>(g_park.size()) * 16;
}

/** One WinMain iteration: Sleep(1), message pump, spin until at least one
 *  whole millisecond has passed, then Qcommon_frame(msec). With a coarse host
 *  timer the Sleep itself overshoots; the Sleep-skip gate (production: an IAT
 *  patch on WinMain's Sleep call) lets near-boundary iterations skip it. */
void EngineLoopIteration(std::uint32_t& oldtime) {
    const bool skipSleep = g_sim.paced &&
        tickpace::SleepGate_ShouldSkip(1, tickpace::SleepGate_WinMainSleepRet());
    if (skipSleep)
        fake::AdvanceMs(0.05);
    else if (g_sim.coarseSleepMs > 0.0)
        fake::AdvanceMs(g_sim.coarseSleepMs);
    else
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
    if (g_sim.iterCmdMs > 0.0)
        fake::QueueCommand(g_sim.iterCmdMs);

    g_run.realtimeShadow += static_cast<std::uint32_t>(msec);
    EngineQcommonFrame(msec);
    ClientPoll();
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
    qpcstub::Reset();
    g_sim = sim;
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();
    g_park.clear();
    cmdpark::g_stubParkBytes = 0;
    cmdpark::g_stubHoldArmed = false;

    fake::SvState() = 2;               // ss_game
    fake::SvsInit() = sim.svsInitialized ? 1 : 0;
    fake::SvTime() = 0;
    fake::Realtime() = 0;
    g_run.wallOrigin = fake::NowMs();
    g_nextAsyncMs = fake::NowMs() + sim.asyncOffsetMs;
    g_jitterState = 0x12345678u;

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
    qpcstub::Reset();
    g_sim = sim;
    g_run = Run();
    tickpace::g = tickpace::State();
    fake::ClearCommands();
    g_park.clear();
    cmdpark::g_stubParkBytes = 0;
    cmdpark::g_stubHoldArmed = false;

    fake::SvState() = 2;
    fake::SvsInit() = sim.svsInitialized ? 1 : 0;
    fake::SvTime() = 0;
    fake::Realtime() = 0;
    g_run.wallOrigin = fake::NowMs();
    g_nextAsyncMs = fake::NowMs() + sim.asyncOffsetMs;
    g_jitterState = 0x12345678u;

    const double until = fake::NowMs() + wallMs;
    std::uint32_t oldtime = fake::EngineMs();
    while (fake::NowMs() < until)
        EngineLoopIteration(oldtime);
    g_run.wallMs = fake::NowMs() - g_run.wallOrigin;
    return g_run;
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
    SetCvar("_sofbuddy_tickpace_settle", 1);
    SetCvar("_sofbuddy_cmdpark_strict", 0);
    SetCvar("_sofbuddy_tickpace_reserve_ms", 0);
    SetCvar("_sofbuddy_tickpace_defer_max_ms", 200);
    cmdpark::g_stubHoldArmed = false;
}

// ---------------------------------------------------------------------------
void Test_RealtimeIsNeverInflated() {
    ResetCvars();

    // Console work arriving at an arbitrary phase, 30ms of it every 40ms: the
    // case where the correction is large and applied on nearly every drain.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 30.0; sim.asyncPeriodMs = 40.0;
    Run paced = RunLoop(sim, 400);

    CHECK(paced.highclamps == 0, "unexpected highclamps: %d", paced.highclamps);
    CHECK(RateError(paced) <= 100.0,
          "%d ticks over %.0fms of wall clock is not 10Hz",
          paced.framenum, paced.wallMs);

    // And with the reserve on, so the limiter is in play too.
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);
    Run reserved = RunLoop(sim, 400);
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

void Test_NoLowclampAfterTickConsumesSettle() {
    ResetCvars();
    // Fast 1-2ms loops with light drains: the old direct-write design unwound a
    // tick-consumed correction after `+= msec`, briefly stepping the clock
    // backwards and widening deficit past 100 in the field. Folding the delta
    // into `msec` keeps the add atomic, so this stays at zero lowclamps.
    Sim sim;
    sim.sleepMs = 0.05;
    sim.cmdCostMs = 8.0;
    sim.cmdsPerTick = 1;
    sim.frameMs = 2.0;
    sim.asyncPeriodMs = 0;
    Run r = RunLoopFor(sim, 12000.0);
    CHECK(r.lowclamps == 0, "post-tick settle unwind caused %d lowclamps", r.lowclamps);
}

void Test_NoSpuriousLowclamps() {
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
    CHECK(paced.lowclamps == 0, "the correction caused %d lowclamps", paced.lowclamps);

    // And with the reserve on, so the drain gate is in play as well.
    SetCvar("_sofbuddy_tickpace_reserve_ms", 3);
    Run reserved = RunLoopFor(sim, 8000.0);
    CHECK(reserved.lowclamps == 0, "the reserve caused %d lowclamps", reserved.lowclamps);

}

void Test_TicksFireCloserToTheirBoundary() {
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

    // Wall lateness can match stock (we no longer pull the tick into the
    // drain). Client interpolation must not get worse: that is the stutter.
    CHECK(paced.clientHighclampMax <= stock.clientHighclampMax + 1,
          "paced client highclamp max %d vs stock %d",
          paced.clientHighclampMax, stock.clientHighclampMax);
    CHECK(CvarValue("_sofbuddy_tickpace_saved") > 0.0f,
          "saved counter never moved: %.0f", CvarValue("_sofbuddy_tickpace_saved"));
}

void Test_MapLoadIntervalIsNotCredited() {
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

void Test_SleepSkipFiresTicksOnTime() {
    ResetCvars();

    // A host whose Sleep(1) really costs ~8ms: the boundary can only be
    // noticed on an 8ms grid, so a tick is on average 4ms late. Skipping the
    // oversleeping Sleep (spin window) notices it sooner.
    Sim sim; sim.coarseSleepMs = 8.0; sim.frameMs = 2.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 12000.0);
    sim.paced = true;
    SetCvar("_sofbuddy_tickpace_spin_ms", 0);
    Run skipOnly = RunLoopFor(sim, 12000.0);
    SetCvar("_sofbuddy_tickpace_spin_ms", 10);
    Run gated = RunLoopFor(sim, 12000.0);
    SetCvar("_sofbuddy_tickpace_spin_ms", 0);

    CHECK(gated.AvgLate() < skipOnly.AvgLate() * 0.5,
          "spin did not halve average lateness vs skip-only (%.2f vs %.2f)",
          gated.AvgLate(), skipOnly.AvgLate());
    CHECK(RateError(gated) <= 100.0,
          "gate broke the 10Hz rate: %d ticks over %.0fms",
          gated.framenum, gated.wallMs);
}

void Test_DisabledIsByteIdentical() {
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

void Test_HighclampAnchorIsLeftAlone() {
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

// When SV_RunGameFrame highclamps, drop only the settle debt that was still
// over sv.time — the engine already removed it from svs.realtime.
void Test_HighclampForgivesSettleOverDebt() {
    ResetCvars();
    g_run = Run();
    g_run.framenum = 1;
    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::SvTime() = 100;
    fake::Realtime() = 0;
    int msec = 250;
    int boot = 0;
    tickpace_QcommonFrame(boot);
    tickpace::HarnessSetSettleMs(120);
    tickpace::g.projectedRealtime = 250;
    tickpace::g.haveProjected = true;
    tickpace::g.svTimeAtPre = 100;
    tickpace::g.measuring = true;
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(fake::Realtime() == fake::SvTime(),
          "highclamp anchor expected (rt=%u sv=%u)",
          fake::Realtime(), fake::SvTime());
    CHECK(tickpace::HarnessSettleMs() == 70,
          "settle debt over sv.time not forgiven (got %d)",
          tickpace::HarnessSettleMs());
}

void Test_NoWritesOutsideSsGame() {
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
    ResetCvars();

    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 30.0; sim.asyncPeriodMs = 40.0;
    sim.svsInitialized = false;
    RunLoop(sim, 50);

    CHECK(fake::Realtime() == 0,
          "svs.realtime=%u was written while the engine never accumulates it",
          fake::Realtime());
}

void Test_SleepSkipGate() {
    ResetCvars();
    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;

    void* site = tickpace::SleepGate_WinMainSleepRet();
    CHECK(site != nullptr, "WinMain Sleep return address did not resolve");
    void* elsewhere = static_cast<char*>(site) + 1;

    // Tick due in 10ms, window 10: skip WinMain's Sleep(1), nothing else.
    fake::SvTime() = 1000;
    fake::Realtime() = 990;
    SetCvar("_sofbuddy_tickpace_spin_ms", 10);
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == true, "due-in-window Sleep(1) not skipped");
    CHECK(tickpace::SleepGate_ShouldSkip(1, elsewhere) == false, "foreign caller skipped");
    CHECK(tickpace::SleepGate_ShouldSkip(0, site) == false, "Sleep(0) skipped");
    CHECK(tickpace::SleepGate_ShouldSkip(50, site) == false, "Sleep(50) skipped");

    // Tick 30ms out with a 10ms window: sleep normally.
    fake::Realtime() = 970;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false, "out-of-window Sleep(1) skipped");

    // Overdue tick: never sleep, catch up instead.
    fake::SvTime() = 1000;
    fake::Realtime() = 1005;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == true, "overdue tick still sleeps");

    // Window off: stock behavior, always sleep (unless another reason fires).
    SetCvar("_sofbuddy_tickpace_spin_ms", 0);
    fake::Realtime() = 995;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false, "window-0 Sleep(1) skipped");

    // Spin needs tickpace master as well as spin_ms > 0.
    SetCvar("_sofbuddy_tickpace", 0);
    SetCvar("_sofbuddy_tickpace_spin_ms", 10);
    fake::Realtime() = 990;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false,
          "spin skipped with tickpace off");
    SetCvar("_sofbuddy_tickpace", 1);
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == true,
          "spin skipped with tickpace on");
    SetCvar("_sofbuddy_tickpace_spin_ms", 0);

    // Drain-boundary one-shot requires tickpace master.
    tickpace::g.skipNextSleep = true;
    SetCvar("_sofbuddy_tickpace", 0);
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false,
          "boundary skip ignored when tickpace off");
    SetCvar("_sofbuddy_tickpace", 1);
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == true,
          "boundary skip needs tickpace on");
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false,
          "boundary skip flag consumed");

    // No map running: always sleep.
    SetCvar("_sofbuddy_tickpace_spin_ms", 10);
    fake::SvState() = 1;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false, "Sleep(1) skipped while loading");
    fake::SvState() = 2;

    // Install writes the gate into the IAT slot, remove restores Sleep.
    tickpace::SleepGate_Install();
    void** slot = reinterpret_cast<void**>(fake::image + 0x11114C);
    CHECK(*slot != reinterpret_cast<void*>(&FakeKernelSleep), "install left stock Sleep in place");
    tickpace::SleepGate_Remove();
    CHECK(*slot == reinterpret_cast<void*>(&FakeKernelSleep), "remove did not restore Sleep");

    SetCvar("_sofbuddy_tickpace_spin_ms", 0);
}

// SoF.exe CL_AddEntities @0x20004460 / Q2 cl_ents.c: high clamp N means the
// client's cl.time ran N ms past cl.frame.servertime (lerpfrac stuck at 1).
// That is the micro-stutter: interpolation has nothing newer to blend toward.
//
// Settle must credit untilAfterMsec (shortage to sv.time), not post-drain
// elapsed. Crediting elapsed overshoots by the drain remainder (~5-7ms) and
// reproduces client showclamp 5-7.
void Test_SettleHitsBoundaryWithoutOvershoot() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace_settle", 1);

    Sim sim;
    sim.sleepMs = 1.0;
    sim.frameMs = 2.0;
    sim.cmdCostMs = 7.0;
    sim.asyncPeriodMs = 100.0;
    sim.asyncOffsetMs = 95.0;

    Run r = RunLoopFor(sim, 20000.0);
    CHECK(r.MaxOvershoot() <= 2,
          "settle overshot the boundary by %d ms (want <=2; old bug was ~5-7)",
          r.MaxOvershoot());
}

// A 7ms sofplus timer that straddles the 100ms boundary must not phase-lock
// snapshot spacing (client high clamp N = wall gap between snapshots - 100).
void Test_ClientShowclampMatchesStock() {
    ResetCvars();

    Sim sim;
    sim.sleepMs = 1.0;
    sim.frameMs = 2.0;
    sim.cmdCostMs = 7.0;
    sim.asyncPeriodMs = 100.0;
    sim.asyncOffsetMs = 95.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 20000.0);
    sim.paced = true;
    SetCvar("_sofbuddy_tickpace_settle", 0);
    Run skipOnly = RunLoopFor(sim, 20000.0);
    SetCvar("_sofbuddy_tickpace_settle", 1);
    Run settleOn = RunLoopFor(sim, 20000.0);

    std::printf("  showclamp: stock max=%d n=%d prints=%d gapge5=%d | "
                "skip max=%d n=%d prints=%d | settle max=%d n=%d prints=%d "
                "gapge5=%d gapmax=%.1f\n",
                stock.clientHighclampMax, stock.clientHighclamps,
                stock.showclampPrints, stock.snapGapGe5,
                skipOnly.clientHighclampMax, skipOnly.clientHighclamps,
                skipOnly.showclampPrints,
                settleOn.clientHighclampMax, settleOn.clientHighclamps,
                settleOn.showclampPrints, settleOn.snapGapGe5,
                settleOn.snapGapMax);
    CHECK(settleOn.clientHighclampMax <= skipOnly.clientHighclampMax + 1,
          "settle client highclamp max %d vs skip-only %d",
          settleOn.clientHighclampMax, skipOnly.clientHighclampMax);
    CHECK(settleOn.clientHighclampMax <= stock.clientHighclampMax + 1,
          "settle client highclamp max %d vs stock %d",
          settleOn.clientHighclampMax, stock.clientHighclampMax);
    CHECK(settleOn.lowclamps == stock.lowclamps,
          "settle caused %d lowclamps (stock %d)",
          settleOn.lowclamps, stock.lowclamps);
}

// Parked backlog skips Sleep(1) only while cmdtext_parking is armed.
void Test_BacklogSkipsSleep() {
    ResetCvars();
    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    void* site = tickpace::SleepGate_WinMainSleepRet();
    SetCvar("_sofbuddy_tickpace_spin_ms", 0);

    cmdpark::g_stubParkBytes = 300;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false,
          "backlog skipped while parking disarmed");

    cmdpark::g_stubHoldArmed = true;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == true,
          "armed backlog did not skip");

    cmdpark::g_stubParkBytes = 0;
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == false,
          "empty park skipped Sleep(1)");

    cmdpark::g_stubParkBytes = 300;
    SetCvar("_sofbuddy_tickpace", 0);
    CHECK(tickpace::SleepGate_ShouldSkip(1, site) == true,
          "armed backlog skip is independent of tickpace");
    SetCvar("_sofbuddy_tickpace", 1);

    cmdpark::g_stubParkBytes = 0;
    cmdpark::g_stubHoldArmed = false;
}

// Variable drains: 2-8ms timers every ~9ms while the server holds 10Hz.
void Test_SettleDebtUnderContinuousDrains() {
    ResetCvars();

    Sim sim;
    sim.sleepMs = 1.0;
    sim.frameMs = 2.0;
    sim.cmdCostMs = 2.0;
    sim.cmdCostJitterMs = 6.0;
    sim.asyncPeriodMs = 9.0;
    sim.asyncOffsetMs = 0.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 20000.0);
    sim.paced = true;
    Run paced = RunLoopFor(sim, 20000.0);

    CHECK(paced.clientHighclampMax <= stock.clientHighclampMax + 1,
          "paced client highclamp max %d vs stock %d",
          paced.clientHighclampMax, stock.clientHighclampMax);
    CHECK(paced.MaxLate() <= stock.MaxLate() + 1.0,
          "paced worst lateness %.1f vs stock %.1f",
          paced.MaxLate(), stock.MaxLate());
    CHECK(paced.lowclamps == stock.lowclamps,
          "paced caused %d lowclamps (stock %d)",
          paced.lowclamps, stock.lowclamps);
}

// Live server regime: 7ms sofplus drains every ~9ms, late_avg swung 5-7 with
// tickpace on. Must not spam client high clamp 5-7 (settle crediting elapsed).
void Test_LiveLikeNoShowclampSpam() {
    ResetCvars();

    Sim sim;
    sim.sleepMs = 1.0;
    sim.frameMs = 2.0;
    sim.cmdCostMs = 7.0;
    sim.cmdCostJitterMs = 1.0;
    sim.asyncPeriodMs = 9.0;
    sim.asyncOffsetMs = 3.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 60000.0);
    sim.paced = true;
    SetCvar("_sofbuddy_tickpace_settle", 0);
    Run skipOnly = RunLoopFor(sim, 60000.0);
    SetCvar("_sofbuddy_tickpace_settle", 1);
    Run settleOn = RunLoopFor(sim, 60000.0);

    std::printf("  live-like: stock max=%d n=%d prints=%d gapge5=%d late=%.1f ticks=%d wall=%.0f\n"
                "             skip  max=%d n=%d prints=%d gapge5=%d late=%.1f ticks=%d\n"
                "             settle max=%d n=%d prints=%d gapge5=%d late=%.1f ticks=%d"
                " over=%d saved=%.0f gapmax=%.1f\n",
                stock.clientHighclampMax, stock.clientHighclamps,
                stock.showclampPrints, stock.snapGapGe5, stock.AvgLate(),
                stock.framenum, stock.wallMs,
                skipOnly.clientHighclampMax, skipOnly.clientHighclamps,
                skipOnly.showclampPrints, skipOnly.snapGapGe5, skipOnly.AvgLate(),
                skipOnly.framenum,
                settleOn.clientHighclampMax, settleOn.clientHighclamps,
                settleOn.showclampPrints, settleOn.snapGapGe5, settleOn.AvgLate(),
                settleOn.framenum,
                settleOn.MaxOvershoot(), CvarValue("_sofbuddy_tickpace_saved"),
                settleOn.snapGapMax);
    // The elapsed-credit bug shows up as server overshoot 5-7 at tick fire.
    CHECK(settleOn.MaxOvershoot() <= 2,
          "settle overshot %d ms (want <=2; elapsed-credit bug was 5-7)",
          settleOn.MaxOvershoot());
    CHECK(settleOn.clientHighclampMax <= stock.clientHighclampMax + 1,
          "settle client max %d vs stock %d",
          settleOn.clientHighclampMax, stock.clientHighclampMax);
    CHECK(settleOn.AvgLate() <= skipOnly.AvgLate() + 0.5,
          "settle late %.1f vs skip-only %.1f",
          settleOn.AvgLate(), skipOnly.AvgLate());
}

// Unsigned sv.time - svs - msec wraps; signed path must stay <= 0 (no phantom
// straddle) when svs.realtime + msec_sampled already reached sv.time.
void Test_SignedNoPhantomStraddle() {
    ResetCvars();
    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;

    fake::SvTime() = 100;
    fake::Realtime() = 102;
    int msec = 5;
    tickpace_QcommonFrame(msec);
    const int until = tickpace::HarnessMsUntilTickAfterMsec(msec);
    CHECK(until <= 0, "already-past boundary: untilAfter=%d (want <=0)", until);

    // Old unsigned path: static_cast<int32_t>(100u - 102u - 5u) == -7 too,
    // but settleMs>svs used to wrap RealtimeWithoutSettle to UINT32_MAX.
    fake::SvTime() = 100;
    fake::Realtime() = 10;
    tickpace::HarnessSetSettleMs(15);
    msec = 5;
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(2.0);
    tickpace_SvFramePre(msec);
    CHECK(tickpace::HarnessSettleMs() == 0,
          "settleMs>svs must be cleared (got %d)", tickpace::HarnessSettleMs());

    fake::SvTime() = 100;
    fake::Realtime() = 105;
    tickpace::HarnessSetSettleMs(0);
    msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(2.0);
    tickpace_SvFramePre(msec);
    CHECK(tickpace::HarnessSettleMs() == 0,
          "already-past must not settle (got %d)", tickpace::HarnessSettleMs());

    // settleMs>0 must not phantom-straddle when sample already reached sv.time.
    fake::SvTime() = 100;
    fake::Realtime() = 105;
    tickpace::HarnessSetSettleMs(15);
    msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(2.0);
    tickpace_SvFramePre(msec);
    CHECK(tickpace::g.pendingSaved == false && tickpace::g.saved == 0,
          "settleMs must not phantom-straddle (pending=%d saved=%lld)",
          tickpace::g.pendingSaved ? 1 : 0,
          static_cast<long long>(tickpace::g.saved));
}

// gap_trace scenario driven through the real SvFramePre hook.
void Test_DirectStraddleCredit() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);

    auto oneStraddle = [](bool elapsedCredit) {
        tickpace::g = tickpace::State();
        fake::SvState() = 2;
        fake::SvsInit() = 1;
        fake::SvTime() = 100;
        fake::Realtime() = 93;
        int msec = 5;
        tickpace_QcommonFrame(msec);
        fake::AdvanceMs(7.0);
        tickpace::SetSimulateElapsedCredit(elapsedCredit);
        tickpace_SvFramePre(msec);
        return 93 + msec - 100;
    };

    const int fixedOver = oneStraddle(false);
    const int buggyOver = oneStraddle(true);
    std::printf("  direct straddle: buggy over=%d fixed over=%d (want 5 and 0)\n",
                buggyOver, fixedOver);
    CHECK(buggyOver >= 5 && buggyOver <= 7,
          "buggy credit overshoot %d", buggyOver);
    CHECK(fixedOver == 0, "fixed credit overshoot %d", fixedOver);
}

// QPC already crossed the boundary but Sys_Milliseconds has not (Wine
// timeGetTime bucket / spsv samples then runs scripts). Crediting U here
// sends now and unwinds from a 1ms sample — client high clamp ~U.
void Test_SettleNeedsEngineElapsed() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);

    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::SvTime() = 100;
    fake::Realtime() = 93;
    int msec = 5;
    tickpace_QcommonFrame(msec);
    const int frozen = static_cast<int>(fake::NowMs());
    fake::AdvanceMs(7.0);
    tickpace::SetEngineNowOverride(frozen);
    tickpace_SvFramePre(msec);
    tickpace::SetEngineNowOverride(-1);
    CHECK(msec == 5, "must not credit while engine ms frozen (msec=%d)", msec);
    CHECK(tickpace::HarnessSettleMs() == 0, "settleMs=%d", tickpace::HarnessSettleMs());
    CHECK(tickpace::g.skipNextSleep, "QPC straddle still skips Sleep");
}

// Full-loop A/B: elapsed credit must produce worse overshoot than untilAfterMsec.
// _saved must advance only when a straddle actually produces a tick, not on
// phantom sub-tick straddle detection.
void Test_SavedOnlyOnTick() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);

    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::SvTime() = 100;
    fake::Realtime() = 93;
    int msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(7.0);
    tickpace_SvFramePre(msec);
    CHECK(tickpace::g.saved == 0 && tickpace::g.pendingSaved,
          "pending before tick (saved=%lld pending=%d)",
          static_cast<long long>(tickpace::g.saved),
          tickpace::g.pendingSaved ? 1 : 0);

    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 1,
          "settle same-frame saved=%lld",
          static_cast<long long>(tickpace::g.saved));

    SetCvar("_sofbuddy_tickpace_settle", 0);
    tickpace::g = tickpace::State();
    fake::SvTime() = 100;
    fake::Realtime() = 93;
    msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(7.0);
    tickpace_SvFramePre(msec);
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 0, "skip w/o rescue skip: saved=%lld",
          static_cast<long long>(tickpace::g.saved));
    (void)tickpace::ConsumeSkipNextSleep();
    tickpace_QcommonFrame(msec);
    tickpace_SvFramePre(msec);
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 1, "skip rescue saved=%lld",
          static_cast<long long>(tickpace::g.saved));

    tickpace::g = tickpace::State();
    fake::SvTime() = 100;
    fake::Realtime() = 105;
    msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(2.0);
    tickpace_SvFramePre(msec);
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 0 && !tickpace::g.pendingSaved,
          "already-past must not save (saved=%lld)",
          static_cast<long long>(tickpace::g.saved));
}

void Test_SettleArmsOnlyWhenReachable() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);

    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::SvTime() = 100;
    fake::Realtime() = 93;
    int msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(7.0);
    tickpace_SvFramePre(msec);
    CHECK(tickpace::g.pendingSaved, "legit straddle must arm pending");
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 1, "legit saved=%lld",
          static_cast<long long>(tickpace::g.saved));

    tickpace::g = tickpace::State();
    fake::SvTime() = 100;
    fake::Realtime() = 105;
    msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(2.0);
    tickpace_SvFramePre(msec);
    CHECK(!tickpace::g.pendingSaved, "already-past must not arm pending");
}

void Test_37msSavedNotEveryTick() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);

    Sim sim;
    sim.sleepMs = 1.0;
    sim.frameMs = 5.0;
    sim.cmdCostMs = 25.0;
    sim.asyncPeriodMs = 37.0;
    sim.paced = true;
    Run r = RunLoopFor(sim, 60000.0);
    const float saved = CvarValue("_sofbuddy_tickpace_saved");
    const double ratio = r.framenum > 0 ? saved / r.framenum : 0;
    std::printf("  37ms regime: ticks=%d saved=%.0f ratio=%.3f\n",
                r.framenum, saved, ratio);
    CHECK(ratio < 0.85, "saved/ticks=%.2f (session bug was ~1.0)", ratio);
}

void Test_SettleStalePendingCleared() {
    ResetCvars();
    SetCvar("_sofbuddy_tickpace", 1);
    SetCvar("_sofbuddy_tickpace_settle", 1);

    tickpace::g = tickpace::State();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::SvTime() = 100;
    fake::Realtime() = 93;
    int msec = 5;
    tickpace_QcommonFrame(msec);
    fake::AdvanceMs(7.0);
    tickpace_SvFramePre(msec);
    msec = 6;  // botched: need 7 to reach sv.time
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 0 && !tickpace::g.pendingSaved,
          "under-credit stale pending (saved=%lld pending=%d)",
          static_cast<long long>(tickpace::g.saved),
          tickpace::g.pendingSaved ? 1 : 0);

    msec = 10;
    EngineSvFrame(msec);
    tickpace_SvFramePost(msec);
    CHECK(tickpace::g.saved == 0,
          "later tick must not count stale pending: %lld",
          static_cast<long long>(tickpace::g.saved));
}

void DumpArm(const char* name, const Run& r) {
    std::printf("    %-7s ticks=%d prints=%d gapge5=%d gapmax=%.1f late=%.1f "
                "over=%d rate_err=%.1f\n",
                name, r.framenum, r.showclampPrints, r.snapGapGe5, r.snapGapMax,
                r.AvgLate(), r.MaxOvershoot(), RateError(r));
}

// Live defaults: cmdpark strict (pre-SV drain held, drip after tick) plus
// Wine Sleep(1) often costing ~15.6ms. The no-park 1ms-sleep harness cannot
// tell tickpace from stock; this can.
void Test_LiveMissingFactors() {
    std::printf("live missing factors (park strict + coarse Sleep)\n");
    ResetCvars();

    auto run3 = [](Sim sim) {
        sim.paced = false;
        Run stock = RunLoopFor(sim, 20000.0);
        sim.paced = true;
        SetCvar("_sofbuddy_tickpace_settle", 0);
        Run skip = RunLoopFor(sim, 20000.0);
        SetCvar("_sofbuddy_tickpace_settle", 1);
        Run settle = RunLoopFor(sim, 20000.0);
        DumpArm("stock", stock);
        DumpArm("skip", skip);
        DumpArm("settle", settle);
        return settle.showclampPrints > stock.showclampPrints + 20;
    };

    Sim base;
    base.frameMs = 2.0;
    base.cmdCostMs = 7.0;
    base.cmdCostJitterMs = 1.0;
    base.asyncPeriodMs = 9.0;
    base.asyncOffsetMs = 3.0;

    std::printf("  park-strict, Sleep(1)=1ms\n");
    Sim a = base;
    a.parkStrict = true;
    a.sleepMs = 1.0;
    const bool parkBad = run3(a);

    std::printf("  no park, Sleep(1)=15.6ms\n");
    Sim b = base;
    b.coarseSleepMs = 15.6;
    const bool coarseBad = run3(b);

    std::printf("  park-strict + Sleep(1)=15.6ms\n");
    Sim c = base;
    c.parkStrict = true;
    c.coarseSleepMs = 15.6;
    const bool bothBad = run3(c);

    if (!parkBad && !coarseBad && !bothBad)
        std::printf("  (none of these arms made settle spam vs stock)\n");
}

void Test_BuggyElapsedCreditOvershoots() {
    ResetCvars();
    Sim sim;
    sim.sleepMs = 1.0;
    sim.frameMs = 2.0;
    sim.cmdCostMs = 7.0;
    sim.cmdCostJitterMs = 1.0;
    sim.asyncPeriodMs = 9.0;
    sim.asyncOffsetMs = 3.0;
    sim.paced = true;
    SetCvar("_sofbuddy_tickpace_settle", 1);

    tickpace::SetSimulateElapsedCredit(true);
    Run buggy = RunLoopFor(sim, 60000.0);
    tickpace::SetSimulateElapsedCredit(false);
    Run fixed = RunLoopFor(sim, 60000.0);

    std::printf("  live-like A/B: buggy over_max=%d | fixed over_max=%d\n",
                buggy.MaxOvershoot(), fixed.MaxOvershoot());
    CHECK(buggy.MaxOvershoot() > fixed.MaxOvershoot(),
          "buggy over_max %d should exceed fixed %d",
          buggy.MaxOvershoot(), fixed.MaxOvershoot());
    CHECK(fixed.MaxOvershoot() <= 2,
          "fixed over_max %d (want <=2)", fixed.MaxOvershoot());
}

void Test_ShutdownRestoresCvarStrings() {
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
    Test_NoLowclampAfterTickConsumesSettle();
    Test_NoSpuriousLowclamps();
    Test_TicksFireCloserToTheirBoundary();
    Test_MapLoadIntervalIsNotCredited();
    Test_SleepSkipFiresTicksOnTime();
    Test_DisabledIsByteIdentical();
    Test_HighclampAnchorIsLeftAlone();
    Test_HighclampForgivesSettleOverDebt();
    Test_NoWritesOutsideSsGame();
    Test_SvsUninitializedIsSafe();
    Test_SleepSkipGate();
    Test_BacklogSkipsSleep();
    Test_SettleHitsBoundaryWithoutOvershoot();
    Test_ClientShowclampMatchesStock();
    Test_SettleDebtUnderContinuousDrains();
    Test_LiveLikeNoShowclampSpam();
    Test_SignedNoPhantomStraddle();
    Test_DirectStraddleCredit();
    Test_SettleNeedsEngineElapsed();
    Test_SavedOnlyOnTick();
    Test_SettleArmsOnlyWhenReachable();
    Test_37msSavedNotEveryTick();
    Test_SettleStalePendingCleared();
    Test_BuggyElapsedCreditOvershoots();
    Test_LiveMissingFactors();
    Test_ShutdownRestoresCvarStrings();

    if (g_failures == 0)
        std::printf("\nAll tick_pacing tests passed.\n");
    else
        std::printf("\n%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
