// Host-side harness for src/features/cpu_optimizations/qpc_timer/*.cpp.
//
// The feature's three translation units are #included below so the tests drive
// the real code (including its file-static state) against:
//   - a synthetic engine image: a heap buffer with valid DOS/NT headers and
//     the curtime global at its real RVA;
//   - a virtual QPC clock that only moves when the harness moves it.
// No server, no Wine.

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

}  // namespace fake

BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
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
constexpr unsigned kRvaCurtime = 0x390D38;

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

std::uint32_t& Curtime() { return *reinterpret_cast<std::uint32_t*>(image + kRvaCurtime); }

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
    (void)msg;
}

#include "../../../src/features/cpu_optimizations/qpc_timer/engine.cpp"
#include "../../../src/features/cpu_optimizations/qpc_timer/cvar.cpp"
#include "../../../src/features/cpu_optimizations/qpc_timer/qpc_timer.cpp"

// ---- test driver -----------------------------------------------------------
int g_failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } while (0)

void SetCvar(const char* name, float v) {
    fake::Cvar* c = fake::Find(name);
    if (c)
        c->value = v;
}

// ---------------------------------------------------------------------------
void Test_QpcOffPassthroughStock() {
    qpctimer::g = qpctimer::State();
    SetCvar("_sofbuddy_qpc", 0);

    static int stockCalls = 0;
    static int stock = 100;
    stockCalls = 0;
    stock = 100;
    auto original = []() -> int { ++stockCalls; return stock++; };

    CHECK(qpc_SysMilliseconds(original) == 100, "first passthrough");
    CHECK(qpc_SysMilliseconds(original) == 101, "second passthrough");
    CHECK(stockCalls == 2, "stock called every time when qpc off (got %d)", stockCalls);

    SetCvar("_sofbuddy_qpc", 1);
}

void Test_SysMillisecondsIsContinuousAndWraps() {
    qpctimer::g = qpctimer::State();
    fake::Curtime() = 0;

    static int stock = 0;
    static int stockCalls = 0;
    stock = 1000;
    stockCalls = 0;
    auto original = []() -> int { ++stockCalls; return stock; };

    CHECK(qpc_SysMilliseconds(original) == 1000, "first return must equal stock");
    CHECK(stockCalls == 1, "stock Sys_Milliseconds must run once to lock");
    CHECK(fake::Curtime() == 1000, "curtime @0x20390D38 not written");

    stock = 9999;  // must be ignored after lock
    fake::AdvanceMs(5);
    CHECK(qpc_SysMilliseconds(original) == 1005, "QPC +5 ms");
    CHECK(stockCalls == 1, "must not mix timeGetTime after lock");
    CHECK(fake::Curtime() == 1005, "curtime not tracking QPC");

    fake::qpc -= 2000;  // 2 ms backward (core migrate)
    // Fixed origin + forward-only clamp: the rewind must not move the clock
    // backwards, and it stays held until QPC catches back up past the hold
    // point (+5 lock, -2 rewind, +1 still short: holds at 1005).
    CHECK(qpc_SysMilliseconds(original) == 1005, "QPC rewind must not go backwards");
    fake::AdvanceMs(1);
    CHECK(qpc_SysMilliseconds(original) == 1005,
          "clock must hold while QPC is still behind the hold point");
    fake::AdvanceMs(2);
    CHECK(qpc_SysMilliseconds(original) == 1006,
          "clock must resume once QPC catches up");

    qpctimer::g = qpctimer::State();
    stock = static_cast<int>(0xFFFFFFFEu);
    stockCalls = 0;
    CHECK(static_cast<std::uint32_t>(qpc_SysMilliseconds(original)) == 0xFFFFFFFEu,
          "lock near DWORD wrap");
    fake::AdvanceMs(5);
    CHECK(static_cast<std::uint32_t>(qpc_SysMilliseconds(original)) == 3u,
          "uint32 wrap (not signed 24.8-day overflow)");
}

int main() {
    fake::InitImage();
    qpctimer::InitCvars();

    Test_QpcOffPassthroughStock();
    Test_SysMillisecondsIsContinuousAndWraps();

    if (g_failures == 0)
        std::printf("\nAll qpc_timer tests passed.\n");
    else
        std::printf("\n%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
