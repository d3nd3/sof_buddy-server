// Host-side harness for src/features/clamp_monitor/*.cpp.
//
// Both feature translation units are #included below so the tests can drive
// the real code (including its file-static state) against a synthetic engine
// image: a heap buffer with valid DOS/NT headers and sv.state / sv.time /
// svs.realtime placed at their real RVAs.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "windows.h"
#include "buddy_import.h"
#include "log.h"

// ---- synthetic engine image -----------------------------------------------
namespace fake {

constexpr std::uint32_t kImageSize = 0x400000;
constexpr unsigned kRvaSvState = 0x3A1F20;
constexpr unsigned kRvaSvTime = 0x3A1F28;
constexpr unsigned kRvaSvsRealtime = 0x396DE4;

char* image = nullptr;
DWORD tick = 100000;

void Init() {
    image = static_cast<char*>(std::calloc(1, kImageSize));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.SizeOfImage = kImageSize;
}

std::int32_t& SvState()  { return *reinterpret_cast<std::int32_t*>(image + kRvaSvState); }
std::uint32_t& SvTime()  { return *reinterpret_cast<std::uint32_t*>(image + kRvaSvTime); }
std::uint32_t& Realtime(){ return *reinterpret_cast<std::uint32_t*>(image + kRvaSvsRealtime); }

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

DWORD GetTickCount(void) { return fake::tick; }

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
std::vector<std::string> broadcasts;

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

extern "C" void Buddy_BroadcastPrintf(int, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fake::broadcasts.push_back(buf);
}

extern "C" void PrintOutImpl(int, const char* msg, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    fake::logs.push_back(buf);
}

#include "../../../src/features/clamp_monitor/cvar.cpp"
#include "../../../src/features/clamp_monitor/clamp_monitor.cpp"

// ---- test driver -----------------------------------------------------------
int g_failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } while (0)

float NoopRunFrame(int) { return 0.0f; }

int g_framenum = 0;

/** One SV_Frame(msec), transcribed from the engine (IDA @ 0x2005f5b0 and
 *  0x2005f3f0): accumulate wall time, sleep-or-lowclamp while game time is
 *  ahead, otherwise advance sv.time by one tick, run the game frame, and
 *  highclamp. Returns true when a game frame actually ran. */
bool EngineFrame(int msec) {
    fake::Realtime() += static_cast<std::uint32_t>(msec);
    fake::tick += static_cast<DWORD>(msec);

    // SV_ReadPackets returns here (0x2005F5EF), before the lowclamp test.
    clampmon_ReadPacketsPost();

    if (fake::Realtime() < fake::SvTime()) {                 // SV_Frame
        if (fake::SvTime() - fake::Realtime() > 100)
            fake::Realtime() = fake::SvTime() - 100;         // "sv lowclamp"
        return false;                                        // NET_Sleep
    }

    ++g_framenum;                                            // SV_RunGameFrame
    fake::SvTime() = static_cast<std::uint32_t>(g_framenum) * 100u;
    clampmon_RunFrame(g_framenum, NoopRunFrame);
    if (fake::SvTime() < fake::Realtime())
        fake::Realtime() = fake::SvTime();                   // "sv highclamp"
    return true;
}

/** SpawnServer: wipes sv (sv.time and sv.framenum to 0) and svs.realtime, sets
 *  sv.time to 1000, then runs two ss_loading frames to let entities settle.
 *  Note sv.framenum stays 0 while sv.time is 1000 - so the first real tick
 *  drops sv.time back to 100 and the engine deletes ~900ms plus however long
 *  the map took to load. That is the bogus "clamp" this feature must ignore. */
void EngineSpawnServer(int load_duration_ms) {
    fake::SvState() = 0;
    fake::SvTime() = 0;
    fake::Realtime() = 0;
    g_framenum = 0;
    fake::SvTime() = 1000;

    fake::SvState() = 1;                    // ss_loading
    clampmon_RunFrame(0, NoopRunFrame);     // "run two frames to settle"
    clampmon_RunFrame(1, NoopRunFrame);
    fake::SvState() = 2;                    // ss_game

    // Map load is wall time the engine only notices on the next SV_Frame.
    EngineFrame(load_duration_ms);
}

/** Boots a map and runs it until the monitor is past its settle window. */
void EngineStartMap(int load_duration_ms = 0) {
    EngineSpawnServer(load_duration_ms);
    for (int i = 0; i < 6; ++i)
        EngineFrame(100);
}

void SetCvar(const char* name, float v) {
    fake::Cvar* c = fake::Find(name);
    if (c)
        c->value = v;
}

const char* CvarString(const char* name) {
    fake::Cvar* c = fake::Find(name);
    return c ? c->string : "<missing>";
}

float CvarValue(const char* name) {
    fake::Cvar* c = fake::Find(name);
    return c ? c->value : -1.0f;
}

void ResetFeatureState() {
    g_state = MonitorState();
    fake::logs.clear();
    fake::broadcasts.clear();
}

char* g_engineOwnedAvgString = nullptr;

// ---------------------------------------------------------------------------
void Test_ZeroLowclampsIsDistinguishableFromNoData() {
    std::printf("a reported zero says \"measured zero\", not \"never measured\"\n");
    ResetFeatureState();
    EngineStartMap();

    // A healthy server never lowclamps, so the event path never runs. If the
    // counters were only published from there, `_sofbuddy_lowclamps 0` would be
    // indistinguishable from a hook that never fired - which is exactly the
    // question a live server raises when it reports zero.
    for (int i = 0; i < 40; ++i)
        EngineFrame(100);

    CHECK(g_state.lowClamps == 0, "healthy ticks lowclamped %lld times",
          g_state.lowClamps);
    CHECK(g_state.lowChecks >= 40, "only %lld frames were tested", g_state.lowChecks);
    CHECK(CvarValue("_sofbuddy_lowclamp_checks") == static_cast<float>(g_state.lowChecks),
          "_sofbuddy_lowclamp_checks=%.0f but %lld frames were tested",
          CvarValue("_sofbuddy_lowclamp_checks"), g_state.lowChecks);
    CHECK(std::strcmp(CvarString("_sofbuddy_lowclamps"), "0") == 0,
          "_sofbuddy_lowclamps=%s", CvarString("_sofbuddy_lowclamps"));
}

void Test_LowclampIsCountedExactly() {
    std::printf("a lowclamp is counted, with the ms the engine invented\n");
    ResetFeatureState();
    EngineStartMap();

    const long long before = g_state.lowClamps;
    CHECK(before == 0, "a clean map start lowclamped %lld times", before);

    // Shove sv.time ahead of the server clock by more than one whole tick.
    // The engine will jump svs.realtime forward to sv.time - 100, inventing
    // the remainder.
    fake::SvTime() = fake::Realtime() + 250;
    EngineFrame(0);

    CHECK(g_state.lowClamps - before == 1, "counted %lld lowclamps, expected 1",
          g_state.lowClamps - before);
    CHECK(g_state.gainedMs == 150, "gainedMs=%lld, expected 150", g_state.gainedMs);
    CHECK(g_state.worstLow == 150, "worstLow=%d, expected 150", g_state.worstLow);
    CHECK(CvarValue("_sofbuddy_lowclamps") == 1.0f,
          "_sofbuddy_lowclamps=%.0f", CvarValue("_sofbuddy_lowclamps"));
    CHECK(std::strcmp(CvarString("_sofbuddy_lowclamp_gained_ms"), "150") == 0,
          "_sofbuddy_lowclamp_gained_ms=%s", CvarString("_sofbuddy_lowclamp_gained_ms"));
}

void Test_ExactlyOneHundredBehindIsNotAClamp() {
    std::printf("the normal post-tick deficit of exactly 100ms is not a clamp\n");
    ResetFeatureState();
    EngineStartMap();

    // `cmp edx, 64h / jbe` (0x2005F61D): 100 returns, 101 clamps. A tick that
    // fires exactly on its boundary leaves precisely 100, every single time -
    // counting that would report a clamp on every well-behaved tick.
    fake::SvTime() = fake::Realtime() + 100;
    EngineFrame(0);
    CHECK(g_state.lowClamps == 0, "100ms behind counted as a clamp");

    fake::SvTime() = fake::Realtime() + 101;
    EngineFrame(0);
    CHECK(g_state.lowClamps == 1, "101ms behind was not counted");
    CHECK(g_state.gainedMs == 1, "gainedMs=%lld, expected 1", g_state.gainedMs);
}

void Test_MapChangeLowclampIsNotCounted() {
    std::printf("the map-change lowclamp is settle-window noise, not a stat\n");
    ResetFeatureState();

    // SpawnServer leaves sv.time at 1000 and svs.realtime at 0, so the first
    // SV_Frame of every map is ~900ms "behind" and lowclamps once. That is
    // map loading, not starvation - the same discontinuity the highclamp side
    // already skips.
    EngineStartMap(30);
    CHECK(g_state.lowClamps == 0, "map change counted %lld lowclamps",
          g_state.lowClamps);
    CHECK(g_state.gainedMs == 0, "map change invented %lld ms", g_state.gainedMs);
}

void Test_LowclampIgnoredOutsideSsGame() {
    std::printf("nothing is counted while sv.state is not ss_game\n");
    ResetFeatureState();
    EngineStartMap();

    fake::SvState() = 1;                 // ss_loading
    fake::SvTime() = fake::Realtime() + 5000;
    clampmon_ReadPacketsPost();
    CHECK(g_state.lowClamps == 0, "counted a clamp at ss_loading");

    fake::SvState() = 0;                 // ss_dead
    clampmon_ReadPacketsPost();
    CHECK(g_state.lowClamps == 0, "counted a clamp at ss_dead");
}

void Test_MapChangeSpikeIsNotCounted() {
    std::printf("map-change spike is not reported as starvation\n");
    ResetFeatureState();
    EngineSpawnServer(/*load_duration_ms=*/4000);
    for (int i = 0; i < 20; ++i)
        EngineFrame(100);   // perfectly healthy ticks

    CHECK(CvarValue("_sofbuddy_highclamps") == 0.0f,
          "highclamps=%g, expected 0", CvarValue("_sofbuddy_highclamps"));
    CHECK(CvarValue("_sofbuddy_clamp_lost_ms") == 0.0f,
          "lost_ms=%g, expected 0", CvarValue("_sofbuddy_clamp_lost_ms"));
    CHECK(fake::broadcasts.empty(), "unexpected broadcast: %s",
          fake::broadcasts.empty() ? "" : fake::broadcasts[0].c_str());
}

void Test_LoadFramesAreNotTicks() {
    std::printf("ss_loading frames never enter the window\n");
    ResetFeatureState();
    fake::SvState() = 1;
    fake::SvTime() = 1000;
    fake::Realtime() = 9999;             // would look like a 8999ms clamp
    clampmon_RunFrame(0, NoopRunFrame);
    clampmon_RunFrame(1, NoopRunFrame);
    CHECK(g_state.highClamps == 0, "highClamps=%lld, expected 0", g_state.highClamps);
    CHECK(g_state.count == 0, "window has %d samples, expected 0", g_state.count);
}

void Test_SustainedOverloadCountsEveryTick() {
    std::printf("sustained overload counts a clamp on every tick\n");
    ResetFeatureState();
    EngineStartMap();
    const long long before = g_state.highClamps;
    for (int i = 0; i < 30; ++i)
        EngineFrame(150);                // 50ms behind every tick

    CHECK(g_state.highClamps - before == 30,
          "counted %lld clamps over 30 overloaded ticks", g_state.highClamps - before);
    CHECK(g_state.lostMs == 30 * 50, "lostMs=%lld, expected 1500", g_state.lostMs);
    CHECK(g_state.lastLost == 50, "lastLost=%d, expected 50", g_state.lastLost);
}

void Test_RollingAverageDecays() {
    std::printf("rolling average rises under load and decays back to zero\n");
    ResetFeatureState();
    SetCvar("_sofbuddy_clamp_window", 2.0f);   // 20 ticks
    EngineStartMap();

    for (int i = 0; i < 20; ++i)
        EngineFrame(140);                      // 40ms lost per tick
    CHECK(g_state.avgMs > 39.0f && g_state.avgMs < 41.0f,
          "avg under load = %.2f, expected ~40", g_state.avgMs);

    for (int i = 0; i < 20; ++i)
        EngineFrame(100);                      // healthy again
    CHECK(g_state.avgMs == 0.0f, "avg after recovery = %.2f, expected 0", g_state.avgMs);
}

void Test_WindowCvarIsHonoured() {
    std::printf("_sofbuddy_clamp_window changes the averaging span\n");
    ResetFeatureState();
    EngineStartMap();
    for (int i = 0; i < 10; ++i)
        EngineFrame(200);                      // 10 ticks at 100ms lost
    for (int i = 0; i < 10; ++i)
        EngineFrame(100);                      // 10 healthy ticks

    SetCvar("_sofbuddy_clamp_window", 1.0f);   // last 10 ticks -> all healthy
    EngineFrame(100);
    CHECK(g_state.avgMs == 0.0f, "1s window avg = %.2f, expected 0", g_state.avgMs);

    // 2.1s must round to 21 ticks, not truncate to 20: the window then spans
    // 12 healthy ticks and 9 of the burst -> 900/21.
    SetCvar("_sofbuddy_clamp_window", 2.1f);
    EngineFrame(100);
    CHECK(g_state.avgMs > 42.0f && g_state.avgMs < 43.5f,
          "2.1s window avg = %.2f, expected ~42.86", g_state.avgMs);
}

void Test_CountersPrintExactly() {
    std::printf("counter cvars print exact integers, not %%g scientific notation\n");
    ResetFeatureState();
    ClampMonitor_SetOutputs(1234567, 12.5f, 42, 9876543210LL);
    CHECK(std::strcmp(CvarString("_sofbuddy_highclamps"), "1234567") == 0,
          "highclamps string = \"%s\"", CvarString("_sofbuddy_highclamps"));
    CHECK(std::strcmp(CvarString("_sofbuddy_clamp_lost_ms"), "9876543210") == 0,
          "lost_ms string = \"%s\"", CvarString("_sofbuddy_clamp_lost_ms"));
    CHECK(std::strcmp(CvarString("_sofbuddy_clamp_last"), "42") == 0,
          "last string = \"%s\"", CvarString("_sofbuddy_clamp_last"));
    CHECK(std::strcmp(CvarString("_sofbuddy_clamp_avg"), "12.50") == 0,
          "avg string = \"%s\"", CvarString("_sofbuddy_clamp_avg"));
}

void Test_ShutdownReturnsEngineStrings() {
    std::printf("shutdown hands cvar_t.string back to the engine\n");
    ResetFeatureState();
    fake::Cvar* avg = fake::Find("_sofbuddy_clamp_avg");
    char* engine_owned = g_engineOwnedAvgString;
    ClampMonitor_SetOutputs(1, 2.0f, 3, 4);
    CHECK(avg->string != engine_owned, "publish did not repoint cvar_t.string");
    ClampMonitor_Shutdown();
    CHECK(avg->string == engine_owned,
          "shutdown left cvar_t.string pointing into this module");
    ClampMonitor_InitCvars();   // re-bind, as a DLL reload would
}

void Test_BroadcastThresholdAndRateLimit() {
    std::printf("broadcast fires at threshold and respects its interval\n");
    ResetFeatureState();
    SetCvar("_sofbuddy_clamp_window", 2.0f);
    SetCvar("_sofbuddy_clamp_broadcast_ms", 20.0f);
    SetCvar("_sofbuddy_clamp_broadcast_interval", 30.0f);
    EngineStartMap();

    for (int i = 0; i < 10; ++i)
        EngineFrame(105);                      // 5ms lost: under threshold
    CHECK(fake::broadcasts.empty(), "broadcast fired below threshold");

    for (int i = 0; i < 20; ++i)
        EngineFrame(200);                      // 100ms lost: well over
    CHECK(fake::broadcasts.size() == 1, "expected 1 broadcast, got %zu",
          fake::broadcasts.size());

    for (int i = 0; i < 20; ++i)               // 2s more, still inside 30s
        EngineFrame(200);
    CHECK(fake::broadcasts.size() == 1, "rate limit breached: %zu broadcasts",
          fake::broadcasts.size());

    fake::tick += 31000;                        // past the interval
    EngineFrame(200);
    CHECK(fake::broadcasts.size() == 2, "expected a second broadcast, got %zu",
          fake::broadcasts.size());
    SetCvar("_sofbuddy_clamp_broadcast_ms", 0.0f);
}

void Test_NotifyLogThreshold() {
    std::printf("notify threshold gates the shim log line\n");
    ResetFeatureState();
    SetCvar("_sofbuddy_clamp_notify_ms", 30.0f);
    EngineStartMap();

    EngineFrame(120);                           // 20ms lost: below threshold
    CHECK(fake::logs.empty(), "logged below threshold: %s", fake::logs.empty() ? "" : fake::logs[0].c_str());

    EngineFrame(500);                           // 400ms lost
    CHECK(fake::logs.size() == 1, "expected 1 log line, got %zu", fake::logs.size());
    CHECK(fake::logs.size() == 1 && fake::logs[0].find("400 ms deleted") != std::string::npos,
          "log line was: %s", fake::logs.empty() ? "" : fake::logs[0].c_str());
    SetCvar("_sofbuddy_clamp_notify_ms", 5.0f);
}

void Test_FreezeTest() {
    std::printf("SIGSTOP-style freeze reports one clamp of the frozen duration\n");
    ResetFeatureState();
    EngineStartMap();
    const long long before = g_state.highClamps;

    EngineFrame(1000);                          // 1s stall -> 900ms deleted
    CHECK(g_state.highClamps - before == 1, "expected 1 clamp, got %lld",
          g_state.highClamps - before);
    CHECK(g_state.lastLost == 900, "lastLost=%d, expected 900", g_state.lastLost);

    for (int i = 0; i < 5; ++i)
        EngineFrame(100);
    CHECK(g_state.highClamps - before == 1, "clamps kept counting after the freeze: %lld",
          g_state.highClamps - before);
}

int main() {
    fake::Init();
    ClampMonitor_InitCvars();
    g_engineOwnedAvgString = fake::Find("_sofbuddy_clamp_avg")->string;

    Test_LoadFramesAreNotTicks();
    Test_ZeroLowclampsIsDistinguishableFromNoData();
    Test_LowclampIsCountedExactly();
    Test_ExactlyOneHundredBehindIsNotAClamp();
    Test_MapChangeLowclampIsNotCounted();
    Test_LowclampIgnoredOutsideSsGame();
    Test_MapChangeSpikeIsNotCounted();
    Test_SustainedOverloadCountsEveryTick();
    Test_RollingAverageDecays();
    Test_WindowCvarIsHonoured();
    Test_CountersPrintExactly();
    Test_ShutdownReturnsEngineStrings();
    Test_BroadcastThresholdAndRateLimit();
    Test_NotifyLogThreshold();
    Test_FreezeTest();

    std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures != 0;
}
