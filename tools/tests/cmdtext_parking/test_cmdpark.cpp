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
constexpr unsigned kRvaCmdTextMaxsize = 0x23F82C;
constexpr unsigned kRvaCmdTextData = 0x23F828;
constexpr unsigned kRvaCmdWait = 0x23F838;
constexpr int kCmdTextBufSize = 0x2000;

char* image = nullptr;
char* cmdBuf = nullptr;

void InitImage() {
    image = static_cast<char*>(std::calloc(1, kImageSize));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.SizeOfImage = kImageSize;
    // A real 8KB cmd_text heap, as SZ_Init(&cmd_text, &cmd_text_buf, 0x2000)
    // leaves it: the data pointer, maxsize, and cursize globals all live.
    cmdBuf = static_cast<char*>(std::calloc(1, kCmdTextBufSize));
    *reinterpret_cast<char**>(image + kRvaCmdTextData) = cmdBuf;
    *reinterpret_cast<std::int32_t*>(image + kRvaCmdTextMaxsize) = kCmdTextBufSize;
}

std::int32_t&  SvState()  { return *reinterpret_cast<std::int32_t*>(image + kRvaSvState); }
std::uint32_t& SvTime()   { return *reinterpret_cast<std::uint32_t*>(image + kRvaSvTime); }
std::uint32_t& Realtime() { return *reinterpret_cast<std::uint32_t*>(image + kRvaSvsRealtime); }
std::int32_t&  SvsInit()  { return *reinterpret_cast<std::int32_t*>(image + kRvaSvsInitialized); }
void*&         DedicatedSlot() { return *reinterpret_cast<void**>(image + kRvaDedicated); }
std::int32_t&  CmdCursize() { return *reinterpret_cast<std::int32_t*>(image + kRvaCmdTextCursize); }
std::int32_t&  CmdWait()    { return *reinterpret_cast<std::int32_t*>(image + kRvaCmdWait); }

/** The command buffer, as live bytes in the fake cmd_text heap: one line per
 *  command, `Q<cost>\n` (control lines arriving through Take carry no marker
 *  and run free). Costs ride in the bytes themselves, so park/evacuate moves —
 *  which only see bytes — stay exact with no parallel queue to drift. */
int cmdsQueuedTotal = 0;

void QueueCommand(double costMs) {
    char line[32];
    std::snprintf(line, sizeof(line), "Q%.2f\n", costMs);
    const int n = static_cast<int>(std::strlen(line));
    // The engine's own overflow compare: cursize + len >= maxsize drops.
    if (CmdCursize() + n >= kCmdTextBufSize)
        return;
    std::memcpy(cmdBuf + CmdCursize(), line, static_cast<std::size_t>(n));
    CmdCursize() += n;
    ++cmdsQueuedTotal;
}

/** Commands still staged (whole-lines invariant: every arrival is \n-ended). */
int QueuedLines() {
    int n = 0;
    for (int i = 0; i < CmdCursize(); ++i)
        if (cmdBuf[i] == '\n')
            ++n;
    return n;
}

void ClearCommands() {
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

void* CpuOpt_MasterCvar() {
    return Buddy_GetEngineCvar("_sofbuddy_cpuopt", "1", 1, nullptr);
}
void* CpuOpt_StrictCvar() {
    return Buddy_GetEngineCvar("_sofbuddy_cmdpark_strict", "1", 1, nullptr);
}
bool CpuOpt_Enabled() {
    return Buddy_ReadCvarValue(CpuOpt_MasterCvar(), 1.0f) != 0.0f;
}
bool CpuOpt_Strict() {
    return CpuOpt_Enabled() && Buddy_ReadCvarValue(CpuOpt_StrictCvar(), 1.0f) != 0.0f;
}

#include "../../../src/features/cpu_optimizations/cmdtext_parking/engine.cpp"
#include "../../../src/features/cpu_optimizations/cmdtext_parking/cvar.cpp"
#include "../../../src/features/cpu_optimizations/cmdtext_parking/cmdpark.cpp"

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
 *  and break out on cmd_wait, clearing the flag on the way. Cost rides in
 *  `Q<cost>` lines; anything else runs free. */
void FakeCbufOriginal() {
    ++g_run.cbufRuns;
    while (fake::CmdCursize() != 0) {
        // Front line extent (whole-lines invariant: always \n-ended).
        int len = 0;
        while (len < fake::CmdCursize() && fake::cmdBuf[len] != '\n')
            ++len;
        if (len < fake::CmdCursize())
            ++len;  // include the newline
        char line[64];
        const int copy = len < static_cast<int>(sizeof(line)) - 1 ? len : static_cast<int>(sizeof(line)) - 1;
        std::memcpy(line, fake::cmdBuf, static_cast<std::size_t>(copy));
        line[copy] = '\0';
        const double cost = line[0] == 'Q' ? std::atof(line + 1) : 0.0;
        std::memmove(fake::cmdBuf, fake::cmdBuf + len,
                     static_cast<std::size_t>(fake::CmdCursize() - len));
        fake::CmdCursize() -= len;

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

/** The engine's Cbuf_AddText append path, for DripOne to call: append when it
 *  fits, drop on overflow, exactly like 0x2001819C / 0x2001822A. */
void FakeCbufAddText(char* text) {
    if (!text)
        return;
    const int n = static_cast<int>(std::strlen(text));
    if (n <= 0 || fake::CmdCursize() + n >= fake::kCmdTextBufSize)
        return;
    std::memcpy(fake::cmdBuf + fake::CmdCursize(), text, static_cast<std::size_t>(n));
    fake::CmdCursize() += n;
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
        cmdpark_QcommonFrame(msec);

    if (g_sim.paced)
        cmdpark_CbufExecute(&FakeCbufOriginal);
    else
        FakeCbufOriginal();

    if (g_sim.paced)
        cmdpark_SvFramePre(msec);
    EngineSvFrame(msec);
    if (g_sim.paced)
        cmdpark_SvFramePost(msec);
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
    cmdpark::g = cmdpark::State();
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
    cmdpark::g = cmdpark::State();
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
double AvgOvershoot(const Run& r) {
    if (r.overshootMs.empty()) return 0.0;
    double s = 0.0;
    for (int v : r.overshootMs) s += v;
    return s / static_cast<double>(r.overshootMs.size());
}

/** The default tunables for a test that is not exercising a specific knob. */
void ResetCvars() {
    SetCvar("_sofbuddy_cmdpark", 1);
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 0);
    SetCvar("_sofbuddy_cmdpark_strict", 0);
    SetCvar("_sofbuddy_cpuopt", 1);
}

// ---------------------------------------------------------------------------
void Test_ReserveKeepsSmallDrainsOffTheBoundary() {
    ResetCvars();

    // Many small commands - the shape a sofplus server actually produces
    // between the occasional heavy one. 3ms each, one every 7ms: 43% duty,
    // and 7 against 100 walks the arrival across every phase.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 3.0; sim.asyncPeriodMs = 7.0;
    sim.frameMs = 10.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 30000.0);
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 3);
    sim.paced = true;
    Run reserved = RunLoopFor(sim, 30000.0);

    CHECK(reserved.MaxOvershoot() <= stock.MaxOvershoot(),
          "reserve made the clamp margin worse (%d vs %d)",
          reserved.MaxOvershoot(), stock.MaxOvershoot());
    CHECK(RateError(reserved) <= 100.0,
          "reserve broke the 10Hz rate: %d ticks over %.0fms",
          reserved.framenum, reserved.wallMs);
    CHECK(reserved.cmdsRun + fake::QueuedLines() + cmdpark::ParkLines() == fake::cmdsQueuedTotal,
          "%d run + %d staged + %d parked != %d queued",
          reserved.cmdsRun, fake::QueuedLines(), cmdpark::ParkLines(), fake::cmdsQueuedTotal);
}

void Test_ReserveDoesNotHarmASaturatedServer() {
    ResetCvars();

    // One 70ms command every 90ms - 78% duty plus a 10ms frame. No phase of a
    // tick has room for a 70ms drain, so holding cannot help and, if the gate
    // were allowed to grow to fit, would defer arriving work and compound.
    Sim sim; sim.sleepMs = 1.0; sim.cmdCostMs = 70.0; sim.asyncPeriodMs = 90.0;
    sim.frameMs = 10.0;

    sim.paced = false;
    Run stock = RunLoopFor(sim, 30000.0);
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 3);
    sim.paced = true;
    Run reserved = RunLoopFor(sim, 30000.0);

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
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 3);

    g_sim = Sim();
    g_run = Run();
    cmdpark::g = cmdpark::State();
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
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);

    CHECK(g_run.cmdsRun == 20 || g_run.cmdsRun == 0,
          "drain was split: %d of 20 commands ran", g_run.cmdsRun);
    if (g_run.cmdsRun == 20)
        CHECK(fake::CmdCursize() == 0, "%d bytes left after a drain",
              fake::CmdCursize());
    CHECK(fake::CmdWait() == 0, "the feature set cmd_wait");
}

void Test_StartGateIsBounded() {
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 3);

    g_sim = Sim();
    g_run = Run();
    cmdpark::g = cmdpark::State();
    fake::ClearCommands();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::Realtime() = 200000;

    // 20ms of headroom is outside a 3ms window: the drain runs.
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 20;
    fake::QueueCommand(40.0);
    int msec = 1;
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);
    CHECK(cmdpark::g.defers == 0, "drain was held with 20ms of room");
    CHECK(fake::CmdCursize() == 0, "drain did not run");

    // 2ms of headroom is inside it: the queue moves aside instead of running
    // on the boundary.
    const long long before = cmdpark::g.defers;
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 2;
    fake::QueueCommand(40.0);
    msec = 1;
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);
    CHECK(cmdpark::g.defers == before + 1, "queue was not moved aside 2ms before the tick");
    CHECK(fake::CmdCursize() == 0, "boundary-near queue ran anyway");
    CHECK(cmdpark::ParkBytes() > 0, "moved queue is not in the park");

    // ...and it comes back on safe ground: far headroom drips and runs it.
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 100;
    msec = 1;
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);
    CHECK(cmdpark::ParkBytes() == 0, "parked queue did not drip back");
    CHECK(g_run.cmdsRun == 2, "moved command never ran (%d)", g_run.cmdsRun);
}

static int g_addCalls;
void CountingAdd(char* text) {
    (void)text;
    ++g_addCalls;
}
void CountingExecText(int, char*) { ++g_addCalls; }

void Test_ReserveParksEveryInsert() {
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 3);
    g_sim = Sim();
    g_run = Run();
    cmdpark::FreePark();
    cmdpark::g = cmdpark::State();
    fake::ClearCommands();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::Realtime() = 200000;
    for (int round = 0; round < 4; ++round) {
        fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 100;
        fake::QueueCommand(40.0);
        int msec = 1;
        cmdpark_QcommonFrame(msec);
        cmdpark_CbufExecute(&FakeCbufOriginal);
    }
    fake::ClearCommands();
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 2;
    int msec = 1;
    cmdpark_QcommonFrame(msec);
    g_addCalls = 0;
    char a[] = "echo add\n";
    char i[] = "echo ins\n";
    char p[] = "echo app\n";
    cmdpark_CbufAddText(a, &CountingAdd);
    CHECK(cmdpark::Take(i), "InsertText path was not parked");
    cmdpark_CbufExecuteText(1, i, &CountingExecText);
    cmdpark_CbufExecuteText(2, p, &CountingExecText);
    CHECK(g_addCalls == 0, "insert leaked into cmd_text (%d calls)", g_addCalls);
    CHECK(cmdpark::ParkBytes() > 0, "park empty after reserved inserts");
    cmdpark_CbufExecuteText(0, a, &CountingExecText);
    CHECK(g_addCalls == 1, "EXEC_NOW was parked");
}

void Test_LevelChangeDoesNotWaitOnPark() {
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_strict", 1);
    g_sim = Sim();
    g_run = Run();
    cmdpark::FreePark();
    cmdpark::g = cmdpark::State();
    fake::ClearCommands();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::Realtime() = 200000;
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 100;

    // Rcon is inside SV_ReadPackets. Strict would park every insert there and
    // drip one newline chunk per tick — the map line waits behind the backlog.
    cmdpark_ReadPacketsPre();
    char backlog[] = "echo parked\n";
    CHECK(cmdpark::Take(backlog), "backlog insert was not parked");
    const int parked = cmdpark::ParkBytes();
    char mapcmd[] = ";map @real@ #_sp_sv_info_map_next;";
    CHECK(!cmdpark::Take(mapcmd), "map insert was parked");
    CHECK(cmdpark::ParkBytes() == parked, "map bytes entered the park");
    char rot[] = "echo rotate\n";
    CHECK(!cmdpark::Take(rot), "map_on_rotate insert was parked");
    cmdpark_ReadPacketsPost();

    cmdpark_ReadPacketsPre();
    char later[] = "echo later\n";
    CHECK(cmdpark::Take(later), "insert after the burst was not parked");
    cmdpark_ReadPacketsPost();

    // What InsertText does once Take refuses: rotate in front of the map line.
    const int rn = static_cast<int>(std::strlen(rot));
    const int mn = static_cast<int>(std::strlen(mapcmd));
    std::memcpy(fake::cmdBuf, rot, static_cast<std::size_t>(rn));
    std::memcpy(fake::cmdBuf + rn, mapcmd, static_cast<std::size_t>(mn));
    fake::CmdCursize() = rn + mn;

    int msec = 1;
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);
    CHECK(g_run.cmdsRun == 2, "level change did not drain (%d)", g_run.cmdsRun);
    CHECK(fake::CmdCursize() == 0, "map line still queued");
    CHECK(cmdpark::g.defers == 0, "level change was moved aside");
    CHECK(cmdpark::ParkBytes() > parked, "backlog was pulled into the map drain");
}

void Test_StrictRunsNothingBetweenTicks() {
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_strict", 1);
    // reserve stays 0: pure strict, far from any boundary.
    g_sim = Sim();
    g_run = Run();
    cmdpark::FreePark();
    cmdpark::g = cmdpark::State();
    fake::ClearCommands();
    fake::SvState() = 2;
    fake::SvsInit() = 1;
    fake::Realtime() = 200000;

    // Far headroom: stock would run this now. Strict moves it aside instead.
    fake::SvTime() = static_cast<std::uint32_t>(fake::Realtime()) + 100;
    fake::QueueCommand(5.0);
    int msec = 1;
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);
    CHECK(g_run.cmdsRun == 0, "strict ran a queued command pre-frame");
    CHECK(fake::CmdCursize() == 0, "queue not moved aside");
    CHECK(cmdpark::ParkBytes() > 0, "moved queue is not in the park");
    CHECK(cmdpark::g.defers == 1, "move not counted (%lld)", cmdpark::g.defers);

    // Another safe spin changes nothing: no pre-frame drip under strict.
    cmdpark_QcommonFrame(msec);
    cmdpark_CbufExecute(&FakeCbufOriginal);
    CHECK(g_run.cmdsRun == 0, "strict dripped between ticks");
    CHECK(cmdpark::ParkBytes() > 0, "park lost bytes between ticks");

    // The fired tick runs it post-tick.
    cmdpark_SvFramePre(msec);
    fake::SvTime() += 100;
    cmdpark_SvFramePost(msec);
    CHECK(g_run.cmdsRun == 1, "post-tick did not run the moved command (%d)", g_run.cmdsRun);
    CHECK(cmdpark::ParkBytes() == 0, "park not empty after post-tick");
}

void Test_NestedDrainRunsWhole() {
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 3);

    g_sim = Sim();
    g_run = Run();
    cmdpark::g = cmdpark::State();
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
            cmdpark_CbufExecute(&FakeCbufOriginal);
            nested = g_run.cmdsRun - before;
        }
    };

    int msec = 1;
    cmdpark_QcommonFrame(msec);
    cmdpark::g.inCbuf = true;
    cmdpark::g.nestedCbuf = 0;
    Cmd::Run();
    cmdpark::g.inCbuf = false;

    CHECK(outer == 1, "outer command did not run");
    CHECK(nested == 2, "nested drain was cut short: %d of 2 commands", nested);
    CHECK(fake::CmdCursize() == 0, "%d bytes left after the nested drain",
          fake::CmdCursize());
    CHECK(fake::CmdWait() == 0, "the feature set cmd_wait");
}

void Test_BehindServerStillDrains() {
    ResetCvars();
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 50);

    // Every game frame overruns a whole tick, so the boundary is always "due".
    // Pre-frame work near it is always moved aside; the post-tick slot runs
    // one chunk per fired tick, so the console and every sofplus timer keep
    // moving instead of going dead.
    Sim sim; sim.sleepMs = 1.0; sim.frameMs = 130.0;
    sim.cmdCostMs = 2.0; sim.cmdsPerTick = 4;
    Run r = RunLoop(sim, 80);

    CHECK(r.cmdsRun > 0, "command buffer starved completely");
    CHECK(r.cmdsRun >= fake::cmdsQueuedTotal / 2,
          "only %d of %d commands ran - the valve is not draining properly",
          r.cmdsRun, fake::cmdsQueuedTotal);
}

void Test_ReserveZeroIsStockScheduling() {
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
    CHECK(cmdpark::g.defers == 0, "held %lld drains with reserve 0",
          cmdpark::g.defers);
}

void Test_HoldArmedRespectsMaster() {
    ResetCvars();
    cmdpark::g = cmdpark::State();
    SetCvar("_sofbuddy_cmdpark_strict", 1);
    SetCvar("_sofbuddy_cmdpark_reserve_ms", 5);
    cmdpark::g.cfg = cmdpark::ReadConfig();
    CHECK(cmdpark::HoldArmed(), "not armed with strict+reserve while enabled");

    SetCvar("_sofbuddy_cmdpark", 0);
    cmdpark::g.cfg = cmdpark::ReadConfig();
    CHECK(!cmdpark::HoldArmed(), "armed with _sofbuddy_cmdpark 0");

    SetCvar("_sofbuddy_cmdpark", 1);
    SetCvar("_sofbuddy_cpuopt", 0);
    cmdpark::g.cfg = cmdpark::ReadConfig();
    CHECK(!cmdpark::HoldArmed(), "armed with _sofbuddy_cpuopt 0");
}

void Test_ShutdownRestoresCvarStrings() {
    fake::Cvar* late = fake::Find("_sofbuddy_cmdpark_cbuf_max");
    CHECK(late != nullptr, "output cvar missing");
    if (!late)
        return;
    char* engineOwned = cmdpark::g_outCbufMax.original;
    CHECK(engineOwned != nullptr, "bind did not capture the engine string");
    CHECK(late->string != engineOwned, "publish did not repoint cvar_t.string");
    CmdPark_Shutdown();
    CHECK(late->string == engineOwned,
          "detach left cvar_t.string pointing into this image");
}

int main() {
    fake::InitImage();
    cmdpark::InitCvars();
    // The two originals production calls directly: drips append through the
    // engine path, post-tick replays run the engine drain.
    detour_Cbuf_AddText::oCbuf_AddText = &FakeCbufAddText;
    detour_Cbuf_Execute::oCbuf_Execute = &FakeCbufOriginal;

    // The engine's `dedicated` cvar, as WinMain reads it.
    fake::DedicatedSlot() = Buddy_GetEngineCvar("dedicated", "1", 0, nullptr);

    Test_ReserveKeepsSmallDrainsOffTheBoundary();
    Test_ReserveDoesNotHarmASaturatedServer();
    Test_DrainsAreNeverSplit();
    Test_StartGateIsBounded();
    Test_ReserveParksEveryInsert();
    Test_LevelChangeDoesNotWaitOnPark();
    Test_StrictRunsNothingBetweenTicks();
    Test_NestedDrainRunsWhole();
    Test_BehindServerStillDrains();
    Test_ReserveZeroIsStockScheduling();
    Test_HoldArmedRespectsMaster();
    Test_ShutdownRestoresCvarStrings();

    if (g_failures == 0)
        std::printf("\nAll cmdpark tests passed.\n");
    else
        std::printf("\n%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
