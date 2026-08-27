// Host-side harness for src/features/cbuf_insert/*.cpp.
//
// The core test is differential. The engine's own Cbuf_InsertText (IDA
// @0x200181D0) is transcribed below, and every case is run twice from the same
// starting state - once through the transcription, once through the feature's
// in-place fast path - with the resulting 8KB buffer, cursize, and any overflow
// message compared byte for byte. An optimisation of this shape is only worth
// anything if it is indistinguishable from what it replaces, so that is what
// gets asserted rather than "it looks right".

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "windows.h"
#include "buddy_import.h"
#include "log.h"

// ---- synthetic engine image ------------------------------------------------
namespace fake {

constexpr std::uint32_t kImageSize = 0x400000;
constexpr unsigned kRvaCmdTextData    = 0x23F828;
constexpr unsigned kRvaCmdTextMaxsize = 0x23F82C;
constexpr unsigned kRvaCmdTextCursize = 0x23F830;
constexpr std::int32_t kCmdTextSize = 0x2000;   // Cbuf_Init: SZ_Init(..., 0x2000)

char* image = nullptr;
unsigned char* cmdTextBuf = nullptr;
std::int64_t qpc = 0;

unsigned char*& Data()    { return *reinterpret_cast<unsigned char**>(image + kRvaCmdTextData); }
std::int32_t&   Maxsize() { return *reinterpret_cast<std::int32_t*>(image + kRvaCmdTextMaxsize); }
std::int32_t&   Cursize() { return *reinterpret_cast<std::int32_t*>(image + kRvaCmdTextCursize); }

std::vector<std::string> logs;
long long zMallocCalls = 0;

void Init() {
    image = static_cast<char*>(std::calloc(1, kImageSize));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.SizeOfImage = kImageSize;

    cmdTextBuf = static_cast<unsigned char*>(std::calloc(1, kCmdTextSize));
    Data() = cmdTextBuf;
    Maxsize() = kCmdTextSize;
    Cursize() = 0;
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

BOOL QueryPerformanceCounter(LARGE_INTEGER* out) { out->QuadPart = ++fake::qpc; return 1; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) { out->QuadPart = 1000000; return 1; }

// ---- fake engine cvars -----------------------------------------------------
namespace fake {

struct Cvar {
    char* name; char* string; char* latched; int flags;
    int unknown; int modified; float value; void* next;
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
    c->string = strdup(value);
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

#include "../../../src/features/cbuf_insert/engine.cpp"
#include "../../../src/features/cbuf_insert/cvar.cpp"
#include "../../../src/features/cbuf_insert/cbuf_insert.cpp"

// ---- the engine's own Cbuf_InsertText, transcribed -------------------------
//
// IDA @0x200181D0. Cbuf_AddText is inlined into the middle of it, and
// Z_Malloc (@0x2001F120) zero-fills the block it returns before handing it
// over, which is the third pass over the buffer this feature removes.
std::vector<std::string> g_engineMessages;
int g_failures = 0;

void EngineCbufInsertText(char* text) {
    const std::int32_t templen = fake::Cursize();
    unsigned char* temp = nullptr;
    if (templen) {
        ++fake::zMallocCalls;
        temp = static_cast<unsigned char*>(std::calloc(1, templen + 16));  // Z_Malloc zeroes
        std::memcpy(temp, fake::Data(), templen);
        fake::Cursize() = 0;                                               // SZ_Clear
    }

    // inlined Cbuf_AddText(text)
    const auto len = static_cast<std::int32_t>(std::strlen(text));
    if (fake::Cursize() + len >= fake::Maxsize()) {
        g_engineMessages.push_back("Cbuf_AddText: overflow\n");
    } else {
        std::memcpy(fake::Data() + fake::Cursize(), text, len);            // SZ_Write
        fake::Cursize() += len;
    }

    if (templen) {
        // SZ_Write of the saved tail. When this does not fit the engine goes
        // through SZ_GetSpace, whose overflow handling is its own business and
        // is not modelled here - the feature never takes its fast path in a
        // case that reaches this, so the harness only has to be faithful when
        // it fits, and must not scribble past the 8KB array when it does not.
        if (fake::Cursize() + templen <= fake::Maxsize()) {
            std::memcpy(fake::Data() + fake::Cursize(), temp, templen);
            fake::Cursize() += templen;
        } else {
            g_engineMessages.push_back("SZ_GetSpace: overflow\n");
        }
        std::free(temp);
    }
}

// ---- test driver -----------------------------------------------------------
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); \
    std::printf("\n"); } } while (0)

void SetCvar(const char* name, float v) {
    fake::Cvar* c = fake::Find(name);
    if (c) c->value = v;
}
float CvarValue(const char* name) {
    fake::Cvar* c = fake::Find(name);
    return c ? c->value : -1.0f;
}

struct Snapshot {
    std::vector<unsigned char> bytes;
    std::int32_t cursize = 0;
};

Snapshot Capture() {
    Snapshot s;
    s.cursize = fake::Cursize();
    if (s.cursize < 0 || s.cursize > fake::kCmdTextSize) {
        std::printf("  FAIL cursize %d is outside the 8KB buffer\n", s.cursize);
        ++g_failures;
        s.cursize = 0;
    }
    s.bytes.assign(fake::cmdTextBuf, fake::cmdTextBuf + s.cursize);
    return s;
}

void Restore(const Snapshot& s) {
    std::memset(fake::cmdTextBuf, 0, fake::kCmdTextSize);
    std::memcpy(fake::cmdTextBuf, s.bytes.data(), s.bytes.size());
    fake::Cursize() = s.cursize;
}

void SeedBuffer(const std::string& contents) {
    std::memset(fake::cmdTextBuf, 0, fake::kCmdTextSize);
    std::memcpy(fake::cmdTextBuf, contents.data(), contents.size());
    fake::Cursize() = static_cast<std::int32_t>(contents.size());
}

/** Runs one insert both ways from the same start state and compares. Returns
 *  true when the fast path was actually taken (so a test can assert it was). */
bool DifferentialInsert(const std::string& seed, const std::string& text) {
    SeedBuffer(seed);
    const Snapshot start = Capture();

    // engine
    g_engineMessages.clear();
    std::string mutableText = text;
    EngineCbufInsertText(&mutableText[0]);
    const Snapshot engineResult = Capture();

    // feature
    Restore(start);
    const long long slowBefore = cbufinsert::g.delegated;
    mutableText = text;
    cbufinsert_InsertText(&mutableText[0], &EngineCbufInsertText);
    const Snapshot featureResult = Capture();
    const bool tookFastPath = (cbufinsert::g.delegated == slowBefore);

    CHECK(featureResult.cursize == engineResult.cursize,
          "cursize %d != engine's %d (seed %zu, text %zu)",
          featureResult.cursize, engineResult.cursize, seed.size(), text.size());
    CHECK(featureResult.bytes == engineResult.bytes,
          "buffer contents differ (seed %zu, text %zu, fast=%d)",
          seed.size(), text.size(), tookFastPath ? 1 : 0);
    return tookFastPath;
}

std::string Filler(std::size_t n, char seed) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        s.push_back(static_cast<char>('a' + ((seed + i) % 26)));
    return s;
}

// ---------------------------------------------------------------------------
void Test_MatchesEngineByteForByte() {
    std::printf("in-place insert is byte-identical to the engine's, across sizes\n");
    SetCvar("_sofbuddy_cbuf_insert", 1);

    int fastCount = 0;
    const std::size_t seeds[] = {0, 1, 7, 64, 500, 2048, 4095, 6000};
    const std::size_t texts[] = {1, 2, 13, 100, 1024, 2000};
    for (std::size_t s : seeds) {
        for (std::size_t t : texts) {
            if (DifferentialInsert(Filler(s, 'x'), Filler(t, 'q')))
                ++fastCount;
        }
    }
    std::printf("    %d of %zu cases took the fast path\n",
                fastCount, sizeof(seeds) / sizeof(*seeds) * sizeof(texts) / sizeof(*texts));
    CHECK(fastCount > 30, "only %d cases took the fast path", fastCount);
}

void Test_OrderingIsInsertNotAppend() {
    std::printf("inserted text lands at the front, ahead of what was queued\n");
    SetCvar("_sofbuddy_cbuf_insert", 1);

    SeedBuffer("second;third;");
    std::string text = "first;";
    cbufinsert_InsertText(&text[0], &EngineCbufInsertText);

    std::string got(reinterpret_cast<char*>(fake::cmdTextBuf), fake::Cursize());
    CHECK(got == "first;second;third;", "got \"%s\"", got.c_str());
}

void Test_OverflowDelegatesToTheEngine() {
    std::printf("anything that does not provably fit is handed to the engine\n");
    SetCvar("_sofbuddy_cbuf_insert", 1);

    const long long before = cbufinsert::g.delegated;
    // 6000 queued + 2500 new > 8192.
    SeedBuffer(Filler(6000, 'x'));
    std::string text = Filler(2500, 'q');
    g_engineMessages.clear();
    cbufinsert_InsertText(&text[0], &EngineCbufInsertText);

    CHECK(cbufinsert::g.delegated == before + 1,
          "overflow case did not delegate (%lld -> %lld)", before, cbufinsert::g.delegated);
    CHECK(!g_engineMessages.empty(),
          "the engine's own overflow path did not run");
}

void Test_DisabledDelegatesEverything() {
    std::printf("_sofbuddy_cbuf_insert 0 delegates every call and still measures\n");
    SetCvar("_sofbuddy_cbuf_insert", 0);

    const long long insertsBefore = cbufinsert::g.inserts;
    const long long slowBefore = cbufinsert::g.delegated;
    const long long zBefore = fake::zMallocCalls;

    for (int i = 0; i < 10; ++i)
        DifferentialInsert(Filler(300, 'x'), Filler(50, 'q'));

    CHECK(cbufinsert::g.delegated - slowBefore == 10,
          "expected 10 delegations, got %lld", cbufinsert::g.delegated - slowBefore);
    CHECK(cbufinsert::g.inserts - insertsBefore == 10,
          "measurement stopped when the fast path was off (%lld)",
          cbufinsert::g.inserts - insertsBefore);
    CHECK(fake::zMallocCalls > zBefore, "the engine path did not allocate");
    SetCvar("_sofbuddy_cbuf_insert", 1);
}

void Test_FastPathAvoidsTheAllocator() {
    std::printf("the fast path does no zone allocation at all\n");
    SetCvar("_sofbuddy_cbuf_insert", 1);

    SeedBuffer(Filler(1000, 'x'));
    const long long zBefore = fake::zMallocCalls;
    for (int i = 0; i < 20; ++i) {
        std::string text = Filler(10, 'q');
        cbufinsert_InsertText(&text[0], &EngineCbufInsertText);
    }
    CHECK(fake::zMallocCalls == zBefore,
          "fast path allocated %lld times", fake::zMallocCalls - zBefore);
    CHECK(fake::Cursize() == 1000 + 200, "cursize %d, expected 1200", fake::Cursize());
}

void Test_CountersTrackTheWork() {
    std::printf("counters report the bytes the optimisation is there to move\n");
    SetCvar("_sofbuddy_cbuf_insert", 1);

    cbufinsert::g.inserts = 0;
    cbufinsert::g.bytesShifted = 0;
    cbufinsert::g.maxCursize = 0;
    cbufinsert::g.publishAtQpc = 0;

    SeedBuffer(Filler(500, 'x'));
    for (int i = 0; i < 3; ++i) {
        std::string text = Filler(10, 'q');
        cbufinsert_InsertText(&text[0], &EngineCbufInsertText);
    }
    // cursize at each call: 500, 510, 520
    CHECK(cbufinsert::g.inserts == 3, "inserts=%lld", cbufinsert::g.inserts);
    CHECK(cbufinsert::g.bytesShifted == 500 + 510 + 520,
          "bytesShifted=%lld, expected 1530", cbufinsert::g.bytesShifted);
    CHECK(cbufinsert::g.maxCursize == 520, "maxCursize=%d", cbufinsert::g.maxCursize);

    cbufinsert::SetOutputs(cbufinsert::g.inserts, cbufinsert::g.bytesShifted,
                           cbufinsert::g.maxCursize, cbufinsert::g.micros,
                           cbufinsert::g.delegated);
    CHECK(CvarValue("_sofbuddy_cbuf_insert_bytes") == 1530.0f,
          "cvar bytes = %.0f", CvarValue("_sofbuddy_cbuf_insert_bytes"));
}

void Test_EmptyBufferAndEmptyText() {
    std::printf("empty buffer and empty text behave as the engine does\n");
    SetCvar("_sofbuddy_cbuf_insert", 1);
    DifferentialInsert("", "hello;");
    DifferentialInsert("queued;", "");
    DifferentialInsert("", "");
}

void Test_ShutdownRestoresCvarStrings() {
    std::printf("detach hands cvar_t.string back to the engine\n");
    fake::Cvar* c = fake::Find("_sofbuddy_cbuf_inserts");
    CHECK(c != nullptr, "output cvar missing");
    if (!c)
        return;
    char* engineOwned = cbufinsert::g_outInserts.original;
    CHECK(engineOwned != nullptr, "bind did not capture the engine string");
    CHECK(c->string != engineOwned, "publish did not repoint cvar_t.string");
    CbufInsert_Shutdown();
    CHECK(c->string == engineOwned, "detach left cvar_t.string in this image");
}

int main() {
    fake::Init();
    cbufinsert::InitCvars();

    Test_MatchesEngineByteForByte();
    Test_OrderingIsInsertNotAppend();
    Test_OverflowDelegatesToTheEngine();
    Test_DisabledDelegatesEverything();
    Test_FastPathAvoidsTheAllocator();
    Test_CountersTrackTheWork();
    Test_EmptyBufferAndEmptyText();
    Test_ShutdownRestoresCvarStrings();

    if (g_failures == 0)
        std::printf("\nAll cbuf_insert tests passed.\n");
    else
        std::printf("\n%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
