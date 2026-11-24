// telemetry: native event stream for sof-db-backend.
//
// The game thread only builds a complete JSON line and appends it to a
// bounded in-memory queue. One writer thread owns the spool file; no database,
// DNS, or network call ever runs here. Queue full = drop and count.
//
// Files: <userdir>/<dir>/session-<uuid>.jsonl (dir defaults to telemetry/spool)
// Contract: sof-db-backend docs/EVENT_FORMAT.md and docs/NATIVE_TELEMETRY.md.

#include "cvar.h"

#include "buddy_import.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include "../profiles/profiles_logic.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <deque>
#include <string>
#include <windows.h>
#include <objbase.h>

extern "C" HMODULE Buddy_GetGameDllHandle(void);

namespace {

// ---------------------------------------------------------------------------
// Verified offsets (same ones profiles/stats/server_demo already use).
// ---------------------------------------------------------------------------
constexpr unsigned kRvaGEdicts = 0x15CCA0;
constexpr unsigned kRvaLevelFramenum = 0x15CCD8;
constexpr unsigned kRvaLevelTime = 0x15CCDC;
constexpr unsigned kRvaLevelIntermissiontime = 0x15D1C8;

constexpr unsigned kEdictClientOfs = 0x74;
constexpr unsigned kEdictCtfFlagsOfs = 0x3CC;
constexpr unsigned kClientStateOfs = 0x0;
constexpr unsigned kClientUserinfoOfs = 0xCC;
constexpr unsigned kClientNetnameOfs = 0x2CC;
constexpr unsigned kClientSpectatorOfs = 0x2DC;
constexpr unsigned kClientTeamOfs = 0x324;
constexpr unsigned kClientScoreOfs = 200;
constexpr unsigned kClientEnterFrameOfs = 776;
constexpr unsigned kClientStatsOfs = 164;
constexpr int kStatFrags = 10;
constexpr unsigned kClientNeedBytes = 0x340;
constexpr int kDefaultEdictStride = 0x464;
constexpr int kMaxClients = 64;
constexpr int kClientStateConnected = 3;

constexpr int kEventCombat = 1;
constexpr int kEventFlags = 2;
constexpr int kEventSamples = 4;

// ---------------------------------------------------------------------------
// Safety helpers.
// ---------------------------------------------------------------------------
bool IsSafeMemoryBlock(const void* ptr, std::size_t size) {
    auto addr = reinterpret_cast<std::uintptr_t>(ptr);
    if (!ptr || size == 0 || addr < 0x10000 || addr > 0x7FFFFFFF)
        return false;
    if (addr + size < addr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + size <= end;
}

bool IsExecutableCodeAddress(const void* ptr) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    const DWORD exec_mask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec_mask) != 0;
}

HMODULE GameMod() {
    if (HMODULE h = Buddy_GetGameDllHandle())
        return h;
    if (HMODULE h = GetModuleHandleA("oldgamex86.dll"))
        return h;
    return GetModuleHandleA("OLDGAMEX86.DLL");
}

bool IsValidModuleRva(HMODULE h, unsigned rva, std::size_t size) {
    return h && IsSafeMemoryBlock(reinterpret_cast<char*>(h) + rva, size);
}

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    return GetModuleHandleA("SoF-spsv.exe");
}

constexpr unsigned kRvaCmdFunctions = 0x241840;
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFnOfs = 0x08;

void** CmdFunctionsHead() {
    HMODULE h = ExeMod();
    if (!IsValidModuleRva(h, kRvaCmdFunctions, sizeof(void*)))
        return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(node)) {
        if (!IsSafeMemoryBlock(node, 0x0C))
            break;
        const char* n = *reinterpret_cast<char**>(static_cast<char*>(node) + kCmdNameOfs);
        if (n && IsSafeMemoryBlock(n, 1) && std::strcmp(n, name) == 0)
            return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    if (!name || !name[0] || !fn)
        return;
    void* node = FindCmdNode(name);
    if (node) {
        *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdFnOfs) = fn;
        return;
    }
    if (detour_Cmd_AddCommand::oCmd_AddCommand &&
        IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Cmd_AddCommand::oCmd_AddCommand))) {
        detour_Cmd_AddCommand::oCmd_AddCommand(const_cast<char*>(name), fn);
    }
}

// ---------------------------------------------------------------------------
// Game globals and entity access.
// ---------------------------------------------------------------------------
void* g_gameExport = nullptr;
char* g_edicts = nullptr;
int g_edictStride = 0;

// Cvars bound once in bootstrap context.
void* g_cvMaxClients = nullptr;
void* g_cvHostname = nullptr;
void* g_cvHostport = nullptr;
void* g_cvGamespyport = nullptr;
void* g_cvTimelimit = nullptr;
void* g_cvFraglimit = nullptr;
void* g_cvCtfLoops = nullptr;
void* g_cvDeathmatch = nullptr;
void* g_cvCtfFlagCaptured = nullptr;
void* g_cvMapname = nullptr;

char* Edicts() {
    if (g_edicts)
        return g_edicts;
    if (g_gameExport && IsSafeMemoryBlock(static_cast<char*>(g_gameExport) + 0x60,
                                          sizeof(void*))) {
        char* p = *reinterpret_cast<char**>(static_cast<char*>(g_gameExport) + 0x60);
        if (IsSafeMemoryBlock(p, 1))
            g_edicts = p;
    }
    if (!g_edicts) {
        HMODULE h = GameMod();
        if (IsValidModuleRva(h, kRvaGEdicts, sizeof(void*))) {
            char* p = *reinterpret_cast<char**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
            if (IsSafeMemoryBlock(p, 1))
                g_edicts = p;
        }
    }
    return g_edicts;
}

int EdictStride() {
    if (g_edictStride > 0)
        return g_edictStride;
    if (g_gameExport && IsSafeMemoryBlock(static_cast<char*>(g_gameExport) + 0x64,
                                          sizeof(int))) {
        int n = *reinterpret_cast<int*>(static_cast<char*>(g_gameExport) + 0x64);
        if (n >= 0x100 && n <= 0x1000)
            g_edictStride = n;
    }
    if (g_edictStride <= 0)
        g_edictStride = kDefaultEdictStride;
    return g_edictStride;
}

int MaxClients() {
    int n = Telemetry_CvarInt(g_cvMaxClients, 8);
    if (n < 1)
        n = 1;
    if (n > kMaxClients)
        n = kMaxClients;
    return n;
}

char* EntityForSlot(int slot) {
    char* edicts = Edicts();
    const int stride = EdictStride();
    if (!edicts || stride <= 0 || slot < 0 || slot >= MaxClients())
        return nullptr;
    char* ent = edicts + static_cast<unsigned>(slot + 1) * static_cast<unsigned>(stride);
    return IsSafeMemoryBlock(ent, kEdictClientOfs + sizeof(void*)) ? ent : nullptr;
}

int EntitySlot(const void* ent) {
    if (!ent || !IsSafeMemoryBlock(ent, sizeof(int)))
        return -1;
    const int n = *reinterpret_cast<const int*>(ent);
    const int slot = n - 1;
    return (slot >= 0 && slot < kMaxClients) ? slot : -1;
}

void* ClientForEnt(const void* ent) {
    if (!ent || !IsSafeMemoryBlock(static_cast<const char*>(ent) + kEdictClientOfs,
                                   sizeof(void*)))
        return nullptr;
    void* client = *reinterpret_cast<void* const*>(
        static_cast<const char*>(ent) + kEdictClientOfs);
    return IsSafeMemoryBlock(client, kClientNeedBytes) ? client : nullptr;
}

bool ReadInt(const void* p, int* out) {
    if (!IsSafeMemoryBlock(p, sizeof(int)))
        return false;
    *out = *reinterpret_cast<const int*>(p);
    return true;
}

bool ReadShort(const void* p, short* out) {
    if (!IsSafeMemoryBlock(p, sizeof(short)))
        return false;
    *out = *reinterpret_cast<const short*>(p);
    return true;
}

bool ReadByte(const void* p, unsigned char* out) {
    if (!IsSafeMemoryBlock(p, 1))
        return false;
    *out = *reinterpret_cast<const unsigned char*>(p);
    return true;
}

int ReadIntOr(const void* p, int fallback = 0) {
    int v = 0;
    return ReadInt(p, &v) ? v : fallback;
}

short ReadShortOr(const void* p, short fallback = 0) {
    short v = 0;
    return ReadShort(p, &v) ? v : fallback;
}

unsigned char ReadByteOr(const void* p, unsigned char fallback = 0) {
    unsigned char v = 0;
    return ReadByte(p, &v) ? v : fallback;
}

std::uint32_t LevelFrame() {
    HMODULE h = GameMod();
    if (!IsValidModuleRva(h, kRvaLevelFramenum, sizeof(int)))
        return 0;
    return static_cast<std::uint32_t>(
        *reinterpret_cast<const int*>(reinterpret_cast<char*>(h) + kRvaLevelFramenum));
}

float LevelTime() {
    HMODULE h = GameMod();
    if (!IsValidModuleRva(h, kRvaLevelTime, sizeof(float)))
        return 0.0f;
    return *reinterpret_cast<const float*>(reinterpret_cast<char*>(h) + kRvaLevelTime);
}

bool LevelIntermission() {
    HMODULE h = GameMod();
    if (!IsValidModuleRva(h, kRvaLevelIntermissiontime, sizeof(float)))
        return false;
    return *reinterpret_cast<const float*>(
               reinterpret_cast<char*>(h) + kRvaLevelIntermissiontime) != 0.0f;
}

void BindCvars() {
    g_cvMaxClients = Telemetry_Cvar("maxclients", "8", 0);
    g_cvHostname = Telemetry_Cvar("hostname", "", 0);
    g_cvHostport = Telemetry_Cvar("hostport", "0", 0);
    g_cvGamespyport = Telemetry_Cvar("gamespyport", "0", 0);
    g_cvTimelimit = Telemetry_Cvar("timelimit", "0", 0);
    g_cvFraglimit = Telemetry_Cvar("fraglimit", "0", 0);
    g_cvCtfLoops = Telemetry_Cvar("ctf_loops", "0", 0);
    g_cvDeathmatch = Telemetry_Cvar("deathmatch", "0", 0);
    g_cvCtfFlagCaptured = Telemetry_Cvar("ctf_flag_captured", "0", 0);
    g_cvMapname = Telemetry_Cvar("mapname", "", 0);
}

// ---------------------------------------------------------------------------
// UUID, time and JSON.
// ---------------------------------------------------------------------------
std::string NewUuid() {
    GUID g = {};
    char buf[40] = {};
    if (CoCreateGuid(&g) == S_OK) {
        std::snprintf(
            buf, sizeof(buf),
            "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    } else {
        static volatile LONG counter = 0;
        const unsigned long a = static_cast<unsigned long>(GetTickCount());
        const unsigned long b = static_cast<unsigned long>(
            InterlockedIncrement(&counter));
        std::snprintf(buf, sizeof(buf),
                      "%08lx-0000-4000-8000-%04lx%08lx", a, b & 0xffffu,
                      a & 0xffffffffu);
    }
    return buf;
}

std::string UtcNow() {
    SYSTEMTIME st = {};
    GetSystemTime(&st);
    char buf[40] = {};
    std::snprintf(buf, sizeof(buf),
                  "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds);
    return buf;
}

std::string JsonEscape(const char* raw) {
    std::string out;
    if (!raw)
        return out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(raw); *p; ++p) {
        const unsigned char c = *p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    char esc[7];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string JsonString(const char* raw) {
    return "\"" + JsonEscape(raw) + "\"";
}

std::string JsonString(const std::string& raw) {
    return JsonString(raw.c_str());
}

std::string FixedString(const char* src, std::size_t cap) {
    if (!src)
        return std::string();
    std::size_t n = 0;
    while (n < cap && src[n])
        ++n;
    return std::string(src, n);
}

class JsonObj {
public:
    JsonObj() { s_ = "{"; }

    void AddRaw(const char* key, const std::string& value) {
        Prefix(key);
        s_ += value;
    }
    void AddStr(const char* key, const std::string& value) {
        Prefix(key);
        s_ += JsonString(value);
    }
    void AddInt(const char* key, long long value) {
        Prefix(key);
        s_ += std::to_string(value);
    }
    void AddBool(const char* key, bool value) {
        Prefix(key);
        s_ += value ? "true" : "false";
    }
    void AddFloat(const char* key, double value, int decimals = 3) {
        Prefix(key);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
        s_ += buf;
    }
    std::string Done() const { return s_ + "}"; }

private:
    void Prefix(const char* key) {
        if (!empty_)
            s_ += ",";
        empty_ = false;
        s_ += JsonString(key);
        s_ += ":";
    }

    std::string s_;
    bool empty_ = true;
};

// ---------------------------------------------------------------------------
// Session and match state.
// ---------------------------------------------------------------------------
bool g_initialized = false;
bool g_sessionStarted = false;
std::string g_sessionUuid;
std::string g_userdir;
std::string g_spoolDir;
unsigned long long g_maxBytes = 0;
std::size_t g_queueLimitBytes = 0;
std::uint64_t g_seq = 0;
std::uint64_t g_mapSerial = 0;

struct MapState {
    bool loaded = false;
    bool started = false;
    bool ended = false;
    std::string map;
    std::string gameType;
    std::string matchUuid;
    float loadTime = 0.0f;
    int timelimit = 0;
    int fraglimit = 0;
    int ctfLoops = 0;
    int deathmatch = 0;
    int maxclients = 0;
    int blueCaps = 0;
    int redCaps = 0;
    int lastCappingSlot = -1;
    float lastSampleTime = 0.0f;
};

MapState g_map;

struct SlotState {
    bool active = false;
    std::uint64_t mapSerial = 0;
    int lastTeam = -1;
    int deaths = 0;
    int suicides = 0;
    int teamkills = 0;
    int flagsCaptured = 0;
    int flagsRecovered = 0;
};

SlotState g_slots[kMaxClients];

const char* SideName(int team) {
    return team == 1 ? "blue" : team == 2 ? "red" : "";
}

const char* GameTypeFor(int deathmatch) {
    return deathmatch == 4 ? "CTF" : "DM";
}

// ---------------------------------------------------------------------------
// Bounded queue and writer thread.
// ---------------------------------------------------------------------------
CRITICAL_SECTION g_queueLock;
bool g_queueLockInit = false;
std::deque<std::string> g_queue;
std::size_t g_queueBytes = 0;
bool g_stopping = false;
HANDLE g_writerThread = nullptr;
std::FILE* g_spoolFile = nullptr;
std::string g_spoolPath;
unsigned long long g_spoolBytes = 0;
int g_spoolIndex = 0;
volatile LONG g_writerError = 0;
volatile LONG g_dropped = 0;

std::string BuildSpoolPath(int index) {
    char name[160];
    if (index <= 0) {
        std::snprintf(name, sizeof(name), "session-%s.jsonl", g_sessionUuid.c_str());
    } else {
        std::snprintf(name, sizeof(name), "session-%s-%d.jsonl",
                      g_sessionUuid.c_str(), index);
    }
    return g_userdir + "/" + g_spoolDir + "/" + name;
}

bool OpenSpool() {
    if (g_spoolFile)
        return true;
    g_spoolPath = BuildSpoolPath(g_spoolIndex);
    g_spoolFile = std::fopen(g_spoolPath.c_str(), "ab");
    if (!g_spoolFile) {
        InterlockedExchange(&g_writerError, 1);
        return false;
    }
    g_spoolBytes = 0;
    return true;
}

void CloseSpool() {
    if (g_spoolFile) {
        std::fflush(g_spoolFile);
        std::fclose(g_spoolFile);
        g_spoolFile = nullptr;
    }
}

bool WriteBatch(const std::deque<std::string>& batch) {
    if (batch.empty())
        return true;
    if (!OpenSpool())
        return false;
    for (const std::string& line : batch) {
        if (g_maxBytes > 0 && g_spoolBytes + line.size() > g_maxBytes) {
            CloseSpool();
            ++g_spoolIndex;
            if (!OpenSpool())
                return false;
        }
        if (std::fwrite(line.data(), 1, line.size(), g_spoolFile) != line.size()) {
            InterlockedExchange(&g_writerError, 1);
            CloseSpool();
            return false;
        }
        g_spoolBytes += line.size();
    }
    std::fflush(g_spoolFile);
    return true;
}

DWORD WINAPI WriterThreadProc(LPVOID) {
    for (;;) {
        std::deque<std::string> batch;
        bool stop = false;
        EnterCriticalSection(&g_queueLock);
        stop = g_stopping;
        batch.swap(g_queue);
        g_queueBytes = 0;
        LeaveCriticalSection(&g_queueLock);

        if (!batch.empty())
            WriteBatch(batch);
        if (stop && batch.empty())
            break;
        if (batch.empty())
            Sleep(50);
    }
    CloseSpool();
    return 0;
}

void StartWriter() {
    if (g_writerThread)
        return;
    g_stopping = false;
    InterlockedExchange(&g_writerError, 0);
    InterlockedExchange(&g_dropped, 0);
    g_spoolIndex = 0;
    CloseSpool();
    g_writerThread = CreateThread(nullptr, 0, WriterThreadProc, nullptr, 0, nullptr);
    if (!g_writerThread) {
        InterlockedExchange(&g_writerError, 1);
        Buddy_DebugPrintf("[telemetry] writer thread creation failed\n");
    }
}

void StopWriter() {
    if (!g_writerThread)
        return;
    EnterCriticalSection(&g_queueLock);
    g_stopping = true;
    LeaveCriticalSection(&g_queueLock);

    const DWORD wait = WaitForSingleObject(g_writerThread, 5000);
    if (wait != WAIT_OBJECT_0)
        InterlockedExchange(&g_writerError, 1);
    CloseHandle(g_writerThread);
    g_writerThread = nullptr;
    g_stopping = false;
    CloseSpool();

    EnterCriticalSection(&g_queueLock);
    g_queue.clear();
    g_queueBytes = 0;
    LeaveCriticalSection(&g_queueLock);
}

void EnqueueLine(const std::string& line) {
    if (!g_writerThread || line.empty())
        return;
    if (!Telemetry_Enabled())
        return;
    EnterCriticalSection(&g_queueLock);
    if (g_queueLimitBytes > 0 &&
        g_queueBytes + line.size() > g_queueLimitBytes) {
        InterlockedIncrement(&g_dropped);
        LeaveCriticalSection(&g_queueLock);
        return;
    }
    g_queue.push_back(line);
    g_queueBytes += line.size();
    LeaveCriticalSection(&g_queueLock);
}

bool EventEnabled(int bit) {
    return (Telemetry_EventsMask() & bit) != 0;
}

std::string CurrentMatchUuid() {
    return (g_map.started && !g_map.ended) ? g_map.matchUuid : std::string();
}

std::string EventLine(const char* type, const std::string& data,
                      const std::string& matchUuid = std::string(),
                      const std::string& playerGuid = std::string(),
                      int slot = -1) {
    std::string line;
    line.reserve(256 + data.size());
    line += "{\"uuid\":\"";
    line += NewUuid();
    line += "\",\"type\":\"";
    line += type;
    line += "\",\"occurred_at\":\"";
    line += UtcNow();
    line += "\",\"seq\":";
    line += std::to_string(++g_seq);
    line += ",\"session_uuid\":";
    line += JsonString(g_sessionUuid);
    if (!matchUuid.empty()) {
        line += ",\"match_uuid\":";
        line += JsonString(matchUuid);
    }
    if (!playerGuid.empty()) {
        line += ",\"player_guid\":";
        line += JsonString(playerGuid);
    }
    if (slot >= 0) {
        line += ",\"slot\":";
        line += std::to_string(slot);
    }
    line += ",\"data\":";
    line += data;
    line += "}\n";
    return line;
}

void Emit(const char* type, const std::string& data,
          const std::string& matchUuid = std::string(),
          const std::string& playerGuid = std::string(), int slot = -1) {
    EnqueueLine(EventLine(type, data, matchUuid, playerGuid, slot));
}

// ---------------------------------------------------------------------------
// Player snapshot.
// ---------------------------------------------------------------------------
struct PlayerSnapshot {
    int slot = -1;
    std::string name;
    std::string guid;
    int team = 0;
    bool spectator = false;
    int frags = 0;
    int score = 0;
    int deaths = 0;
    int suicides = 0;
    int teamkills = 0;
    int flagsCaptured = 0;
    int flagsRecovered = 0;
    int playtimeSec = 0;
    int ctfFlags = 0;
    std::uint32_t levelFrame = 0;
};

const char* LookupUserinfo(const char* userinfo, const char* key) {
    if (!userinfo || !key)
        return "";
    if (detour_Info_ValueForKey::oInfo_ValueForKey &&
        IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Info_ValueForKey::oInfo_ValueForKey))) {
        const char* v = SOF_EP_Info_ValueForKey(const_cast<char*>(userinfo), key);
        if (v && IsSafeMemoryBlock(v, 1))
            return v;
        return "";
    }
    // Minimal fallback: \key\value pairs.
    const std::size_t keyLen = std::strlen(key);
    const char* p = userinfo;
    while (*p) {
        if (*p++ != '\\')
            continue;
        const char* k = p;
        while (*p && *p != '\\')
            ++p;
        if (static_cast<std::size_t>(p - k) != keyLen ||
            std::strncmp(k, key, keyLen) != 0)
            continue;
        if (*p == '\\')
            ++p;
        const char* v = p;
        while (*p && *p != '\\')
            ++p;
        static char value[256];
        const std::size_t n = static_cast<std::size_t>(p - v);
        const std::size_t copy = n < sizeof(value) - 1 ? n : sizeof(value) - 1;
        std::memcpy(value, v, copy);
        value[copy] = '\0';
        return value;
    }
    return "";
}

std::string GuidFromUserinfo(const char* userinfo) {
    const char* raw = LookupUserinfo(userinfo, "team_red_blue");
    if (!raw || !raw[0])
        return std::string();
    profiles::TrbParsed parsed = profiles::ParseTeamRedBlue(raw);
    return parsed.valid ? parsed.identity : std::string();
}

bool SnapshotSlot(int slot, bool requireConnected, PlayerSnapshot* out) {
    if (!out || slot < 0 || slot >= MaxClients())
        return false;
    char* ent = EntityForSlot(slot);
    void* client = ClientForEnt(ent);
    if (!client)
        return false;
    if (requireConnected &&
        ReadIntOr(static_cast<char*>(client) + kClientStateOfs, -1) !=
            kClientStateConnected)
        return false;

    PlayerSnapshot p;
    p.slot = slot;
    p.name = FixedString(static_cast<char*>(client) + kClientNetnameOfs, 16);
    const char* userinfo = static_cast<char*>(client) + kClientUserinfoOfs;
    p.guid = GuidFromUserinfo(userinfo);
    if (p.name.empty()) {
        const char* n = LookupUserinfo(userinfo, "name");
        p.name = (n && n[0]) ? n : "player";
    }
    p.team = ReadIntOr(static_cast<char*>(client) + kClientTeamOfs, 0);
    if (p.team != 1 && p.team != 2)
        p.team = 0;
    p.spectator =
        ReadByteOr(static_cast<char*>(client) + kClientSpectatorOfs, 0) != 0;
    p.frags = static_cast<int>(
        ReadShortOr(static_cast<char*>(client) + kClientStatsOfs +
                        kStatFrags * static_cast<int>(sizeof(short)),
                    0));
    p.score = ReadIntOr(static_cast<char*>(client) + kClientScoreOfs, 0);
    p.playtimeSec = 0;
    const int enterFrame = ReadIntOr(
        static_cast<char*>(client) + kClientEnterFrameOfs, 0);
    p.levelFrame = LevelFrame();
    if (enterFrame > 0 && p.levelFrame >= static_cast<std::uint32_t>(enterFrame)) {
        p.playtimeSec = static_cast<int>(
            (p.levelFrame - static_cast<std::uint32_t>(enterFrame)) / 10u);
    }
    p.ctfFlags = ReadIntOr(static_cast<char*>(ent) + kEdictCtfFlagsOfs, 0);

    if (g_slots[slot].mapSerial == g_mapSerial) {
        p.deaths = g_slots[slot].deaths;
        p.suicides = g_slots[slot].suicides;
        p.teamkills = g_slots[slot].teamkills;
        p.flagsCaptured = g_slots[slot].flagsCaptured;
        p.flagsRecovered = g_slots[slot].flagsRecovered;
    }
    *out = p;
    return true;
}

bool HasConnectedClient() {
    for (int slot = 0; slot < MaxClients(); ++slot) {
        PlayerSnapshot p;
        if (SnapshotSlot(slot, true, &p) && !p.spectator)
            return true;
    }
    return false;
}

int CountPlayers(int* nonSpectators, int* spectators, int* bestFrags) {
    int players = 0;
    int specs = 0;
    int topFrags = 0;
    for (int slot = 0; slot < MaxClients(); ++slot) {
        PlayerSnapshot p;
        if (!SnapshotSlot(slot, true, &p))
            continue;
        if (p.spectator) {
            ++specs;
        } else {
            ++players;
            if (p.frags > topFrags)
                topFrags = p.frags;
        }
    }
    if (nonSpectators)
        *nonSpectators = players;
    if (spectators)
        *spectators = specs;
    if (bestFrags)
        *bestFrags = topFrags;
    return players;
}

// ---------------------------------------------------------------------------
// Event emitters.
// ---------------------------------------------------------------------------
void EmitSessionStart() {
    JsonObj d;
    d.AddStr("build_version", "sof_buddy-server");
    d.AddStr("feature_profile", "telemetry");
    d.AddStr("hostname", Telemetry_CvarString(g_cvHostname, ""));
    const int hostport = Telemetry_CvarInt(g_cvHostport, 0);
    const int gamespyport = Telemetry_CvarInt(g_cvGamespyport, 0);
    if (hostport > 0)
        d.AddInt("game_port", hostport);
    if (gamespyport > 0)
        d.AddInt("gamespy_port", gamespyport);
    Emit("session_start", d.Done());
}

void EmitSessionEnd() {
    JsonObj d;
    d.AddStr("reason", "dll_unload");
    Emit("session_end", d.Done());
}

void EnsureSession() {
    if (g_sessionStarted || !g_initialized)
        return;
    if (!Telemetry_Enabled())
        return;
    StartWriter();
    if (!g_writerThread)
        return;
    if (g_sessionUuid.empty())
        g_sessionUuid = NewUuid();
    EmitSessionStart();
    g_sessionStarted = true;
}

void EmitMapStart() {
    JsonObj d;
    d.AddStr("map", g_map.map);
    d.AddStr("game_type", g_map.gameType);
    d.AddStr("hostname", Telemetry_CvarString(g_cvHostname, ""));
    d.AddInt("maxclients", g_map.maxclients);
    d.AddInt("timelimit", g_map.timelimit);
    d.AddInt("fraglimit", g_map.fraglimit);
    d.AddInt("ctf_loops", g_map.ctfLoops);
    d.AddInt("deathmatch", g_map.deathmatch);
    Emit("map_start", d.Done(), g_map.matchUuid);
}

void EnsureMapStarted() {
    if (!g_map.loaded || g_map.started || g_map.ended)
        return;
    EnsureSession();
    if (!g_sessionStarted)
        return;
    EmitMapStart();
    g_map.started = true;
    g_map.lastSampleTime = LevelTime();
}

void EmitPlayerStats(const std::string& matchUuid) {
    for (int slot = 0; slot < MaxClients(); ++slot) {
        PlayerSnapshot p;
        if (!SnapshotSlot(slot, true, &p))
            continue;
        JsonObj d;
        d.AddStr("name", p.name);
        d.AddStr("team", SideName(p.team));
        d.AddInt("team_number", p.team);
        d.AddBool("spectator", p.spectator);
        d.AddInt("frags", p.frags);
        d.AddInt("deaths", p.deaths);
        d.AddInt("suicides", p.suicides);
        d.AddInt("teamkills", p.teamkills);
        d.AddInt("score", p.score);
        d.AddInt("flags_captured", p.flagsCaptured);
        d.AddInt("flags_recovered", p.flagsRecovered);
        d.AddInt("playtime_sec", p.playtimeSec);
        d.AddStr("source", "native_telemetry");
        Emit("player_match_stats", d.Done(), matchUuid, p.guid, slot);
    }
}

void EmitMapEnd(const char* hint) {
    if (!g_map.loaded || !g_map.started || g_map.ended)
        return;
    g_map.ended = true;

    int players = 0;
    int specs = 0;
    int maxFrags = 0;
    CountPlayers(&players, &specs, &maxFrags);

    const float now = LevelTime();
    const bool flaglimit =
        Telemetry_CvarFloat(g_cvCtfFlagCaptured, 0.0f) != 0.0f ||
        (g_map.ctfLoops > 0 &&
         (g_map.blueCaps >= g_map.ctfLoops || g_map.redCaps >= g_map.ctfLoops));

    const char* reason = "map_change";
    if (hint && (std::strcmp(hint, "abandoned") == 0 ||
                 std::strcmp(hint, "server_shutdown") == 0 ||
                 std::strcmp(hint, "dll_unload") == 0)) {
        reason = hint;
    } else if (flaglimit) {
        reason = "flaglimit";
    } else if (g_map.fraglimit > 0 && maxFrags >= g_map.fraglimit) {
        reason = "fraglimit";
    } else if (g_map.timelimit > 0 &&
               now >= static_cast<float>(g_map.timelimit) * 60.0f - 1.0f) {
        reason = "timelimit";
    }

    const char* winner = "";
    if (g_map.blueCaps > g_map.redCaps)
        winner = "blue";
    else if (g_map.redCaps > g_map.blueCaps)
        winner = "red";
    else if (std::strcmp(reason, "timelimit") == 0 ||
             std::strcmp(reason, "fraglimit") == 0 ||
             std::strcmp(reason, "map_change") == 0)
        winner = "draw";

    JsonObj d;
    d.AddStr("map", g_map.map);
    d.AddStr("game_type", g_map.gameType);
    d.AddStr("end_reason", reason);
    d.AddStr("winner_side", winner);
    d.AddInt("blue_caps", g_map.blueCaps);
    d.AddInt("red_caps", g_map.redCaps);
    d.AddInt("score_blue", g_map.blueCaps);
    d.AddInt("score_red", g_map.redCaps);
    d.AddInt("capping_slot", g_map.lastCappingSlot);
    d.AddInt("ctf_loops", g_map.ctfLoops);
    d.AddInt("timelimit", g_map.timelimit);
    d.AddInt("fraglimit", g_map.fraglimit);
    d.AddInt("players", players);
    d.AddInt("spectators", specs);
    d.AddFloat("duration_sec", now > g_map.loadTime ? now - g_map.loadTime : 0.0f, 1);
    d.AddStr("source", "native_telemetry");

    const std::string matchUuid = g_map.matchUuid;
    Emit("map_end", d.Done(), matchUuid);
    EmitPlayerStats(matchUuid);
}

void EmitPlayerConnect(const PlayerSnapshot& p) {
    JsonObj d;
    d.AddStr("name", p.name);
    d.AddStr("team", SideName(p.team));
    d.AddInt("team_number", p.team);
    d.AddBool("spectator", p.spectator);
    d.AddStr("guid", p.guid);
    Emit("player_connect", d.Done(), CurrentMatchUuid(), p.guid, p.slot);
}

void EmitPlayerDisconnect(const PlayerSnapshot& p) {
    JsonObj d;
    d.AddStr("name", p.name);
    d.AddStr("team", SideName(p.team));
    d.AddInt("team_number", p.team);
    d.AddBool("spectator", p.spectator);
    d.AddStr("guid", p.guid);
    d.AddInt("frags", p.frags);
    d.AddInt("deaths", p.deaths);
    d.AddInt("suicides", p.suicides);
    d.AddInt("teamkills", p.teamkills);
    d.AddInt("score", p.score);
    d.AddInt("flags_captured", p.flagsCaptured);
    d.AddInt("flags_recovered", p.flagsRecovered);
    d.AddInt("playtime_sec", p.playtimeSec);
    Emit("player_disconnect", d.Done(), CurrentMatchUuid(), p.guid, p.slot);
}

void EmitTeamChange(const PlayerSnapshot& p, int fromTeam) {
    JsonObj d;
    d.AddStr("name", p.name);
    d.AddInt("from_team", fromTeam);
    d.AddInt("to_team", p.team);
    d.AddStr("from_side", SideName(fromTeam));
    d.AddStr("to_side", SideName(p.team));
    Emit("team_change", d.Done(), CurrentMatchUuid(), p.guid, p.slot);
}

void EmitFlagCapture(const PlayerSnapshot& p, int flagTeam) {
    JsonObj d;
    d.AddStr("team", SideName(p.team));
    d.AddStr("flag_team", SideName(flagTeam));
    d.AddInt("team_number", p.team);
    d.AddInt("flag_team_number", flagTeam);
    d.AddInt("blue_caps", g_map.blueCaps);
    d.AddInt("red_caps", g_map.redCaps);
    d.AddStr("name", p.name);
    Emit("flag_capture", d.Done(), CurrentMatchUuid(), p.guid, p.slot);
}

void EmitPlayerDeath(const PlayerSnapshot& victim, const PlayerSnapshot* killer,
                     int mod, bool teamkill, bool suicide) {
    JsonObj d;
    d.AddInt("victim_slot", victim.slot);
    d.AddStr("victim_name", victim.name);
    d.AddStr("victim_team", SideName(victim.team));
    d.AddInt("killer_slot", killer ? killer->slot : -1);
    d.AddStr("killer_name", killer ? killer->name : "");
    d.AddStr("killer_team", killer ? SideName(killer->team) : "");
    d.AddInt("mod", mod);
    d.AddBool("teamkill", teamkill);
    d.AddBool("suicide", suicide);
    Emit("player_death", d.Done(), CurrentMatchUuid(), victim.guid, victim.slot);
}

void EmitPlayerSample(const PlayerSnapshot& p) {
    JsonObj d;
    d.AddStr("name", p.name);
    d.AddStr("team", SideName(p.team));
    d.AddInt("team_number", p.team);
    d.AddBool("spectator", p.spectator);
    d.AddInt("frags", p.frags);
    d.AddInt("deaths", p.deaths);
    d.AddInt("suicides", p.suicides);
    d.AddInt("teamkills", p.teamkills);
    d.AddInt("score", p.score);
    d.AddInt("flags_captured", p.flagsCaptured);
    d.AddInt("flags_recovered", p.flagsRecovered);
    d.AddInt("playtime_sec", p.playtimeSec);
    d.AddStr("source", "native_telemetry");
    Emit("player_sample", d.Done(), CurrentMatchUuid(), p.guid, p.slot);
}

void EmitSamples() {
    for (int slot = 0; slot < MaxClients(); ++slot) {
        PlayerSnapshot p;
        if (SnapshotSlot(slot, true, &p))
            EmitPlayerSample(p);
    }
}

void EmitSelfTest() {
    const std::string match = NewUuid();

    JsonObj ms;
    ms.AddStr("map", "test/selftest");
    ms.AddStr("game_type", "CTF");
    ms.AddStr("hostname", "telemetry selftest");
    ms.AddInt("maxclients", 8);
    ms.AddInt("timelimit", 0);
    ms.AddInt("fraglimit", 0);
    ms.AddInt("ctf_loops", 1);
    ms.AddInt("deathmatch", 4);
    Emit("map_start", ms.Done(), match);

    JsonObj pc;
    pc.AddStr("name", "TelemetrySelfTest");
    pc.AddStr("team", "blue");
    pc.AddInt("team_number", 1);
    pc.AddBool("spectator", false);
    Emit("player_connect", pc.Done(), match, "", 0);

    JsonObj fc;
    fc.AddStr("team", "blue");
    fc.AddStr("flag_team", "red");
    fc.AddInt("team_number", 1);
    fc.AddInt("flag_team_number", 2);
    fc.AddInt("blue_caps", 1);
    fc.AddInt("red_caps", 0);
    fc.AddStr("name", "TelemetrySelfTest");
    Emit("flag_capture", fc.Done(), match, "", 0);

    JsonObj pd;
    pd.AddInt("victim_slot", 0);
    pd.AddStr("victim_name", "TelemetrySelfTest");
    pd.AddStr("victim_team", "blue");
    pd.AddInt("killer_slot", -1);
    pd.AddStr("killer_name", "");
    pd.AddStr("killer_team", "");
    pd.AddInt("mod", 0);
    pd.AddBool("teamkill", false);
    pd.AddBool("suicide", true);
    Emit("player_death", pd.Done(), match, "", 0);

    JsonObj pms;
    pms.AddStr("name", "TelemetrySelfTest");
    pms.AddStr("team", "blue");
    pms.AddInt("team_number", 1);
    pms.AddBool("spectator", false);
    pms.AddInt("frags", 1);
    pms.AddInt("deaths", 1);
    pms.AddInt("suicides", 1);
    pms.AddInt("teamkills", 0);
    pms.AddInt("score", 1);
    pms.AddInt("flags_captured", 1);
    pms.AddInt("flags_recovered", 0);
    pms.AddInt("playtime_sec", 60);
    pms.AddStr("source", "native_telemetry");
    Emit("player_match_stats", pms.Done(), match, "", 0);

    JsonObj me;
    me.AddStr("map", "test/selftest");
    me.AddStr("game_type", "CTF");
    me.AddStr("end_reason", "flaglimit");
    me.AddStr("winner_side", "blue");
    me.AddInt("blue_caps", 1);
    me.AddInt("red_caps", 0);
    me.AddInt("score_blue", 1);
    me.AddInt("score_red", 0);
    me.AddInt("capping_slot", 0);
    me.AddInt("ctf_loops", 1);
    me.AddInt("timelimit", 0);
    me.AddInt("fraglimit", 0);
    me.AddInt("players", 1);
    me.AddInt("spectators", 0);
    me.AddFloat("duration_sec", 60.0, 1);
    me.AddStr("source", "native_telemetry");
    Emit("map_end", me.Done(), match);
}

void __cdecl Telemetry_SelfTest_f() {
    EnsureSession();
    if (!g_sessionStarted) {
        Buddy_DebugPrintf("[telemetry] selftest: set _sofbuddy_telemetry 1 first\n");
        return;
    }
    EmitSelfTest();
    Buddy_DebugPrintf("[telemetry] selftest events queued\n");
}

// ---------------------------------------------------------------------------
// Game hooks.
// ---------------------------------------------------------------------------
void OnGameDllLoadedImpl(void* gameExport) {
    g_gameExport = gameExport;
    g_edicts = nullptr;
    g_edictStride = 0;

    Telemetry_InitCvars();
    BindCvars();

    g_initialized = true;
    g_sessionStarted = false;
    g_sessionUuid = NewUuid();
    g_seq = 0;
    g_mapSerial = 0;
    g_map = MapState();
    for (auto& s : g_slots)
        s = SlotState();

    const char* user = Buddy_FS_Userdir();
    g_userdir = (user && user[0]) ? user : "User";
    g_spoolDir = Telemetry_Dir();
    g_maxBytes = Telemetry_MaxBytes();
    g_queueLimitBytes = Telemetry_QueueLimitBytes();

    if (!g_queueLockInit) {
        InitializeCriticalSection(&g_queueLock);
        g_queueLockInit = true;
    }

    // Directory creation is an engine call: do it here, on the game thread.
    // FS_CreatePath may write temporary NULs into its argument, so pass a
    // mutable buffer, not std::string::c_str().
    char firstPath[512] = {};
    std::snprintf(firstPath, sizeof(firstPath), "%s/%s/session-%s.jsonl",
                  g_userdir.c_str(), g_spoolDir.c_str(),
                  g_sessionUuid.c_str());
    Buddy_FS_CreatePath(firstPath);

    InstallCommand("telemetry_selftest",
                   reinterpret_cast<void*>(&Telemetry_SelfTest_f));

    if (Telemetry_Enabled())
        EnsureSession();
}

void ShutdownImpl() {
    if (!g_initialized)
        return;

    if (g_map.loaded && g_map.started && !g_map.ended)
        EmitMapEnd("server_shutdown");
    if (g_sessionStarted)
        EmitSessionEnd();

    StopWriter();

    const LONG dropped = InterlockedCompareExchange(&g_dropped, 0, 0);
    if (dropped > 0)
        Buddy_DebugPrintf("[telemetry] dropped %ld event(s): queue full\n",
                          dropped);
    if (InterlockedCompareExchange(&g_writerError, 0, 0) != 0)
        Buddy_DebugPrintf("[telemetry] writer reported an I/O error; "
                          "spool may be incomplete\n");

    if (g_queueLockInit) {
        DeleteCriticalSection(&g_queueLock);
        g_queueLockInit = false;
    }
    g_initialized = false;
    g_sessionStarted = false;
    g_sessionUuid.clear();
    g_map = MapState();
    g_gameExport = nullptr;
    g_edicts = nullptr;
    g_edictStride = 0;
}

// Called from the core SpawnEntities detour in gamex86.cpp.
void OnSpawnEntitiesImpl(const char* mapname) {
    if (!g_initialized)
        return;
    EnsureSession();

    if (g_map.loaded && g_map.started && !g_map.ended)
        EmitMapEnd("abandoned");

    const char* map = (mapname && mapname[0])
                          ? mapname
                          : Telemetry_CvarString(g_cvMapname, "unknown");
    if (!map || !map[0])
        map = "unknown";

    g_map = MapState();
    g_map.loaded = true;
    g_map.map = map;
    g_map.deathmatch = Telemetry_CvarInt(g_cvDeathmatch, 0);
    g_map.gameType = GameTypeFor(g_map.deathmatch);
    g_map.matchUuid = NewUuid();
    g_map.loadTime = LevelTime();
    g_map.timelimit = Telemetry_CvarInt(g_cvTimelimit, 0);
    g_map.fraglimit = Telemetry_CvarInt(g_cvFraglimit, 0);
    g_map.ctfLoops = Telemetry_CvarInt(g_cvCtfLoops, 0);
    g_map.maxclients = MaxClients();
    g_map.blueCaps = 0;
    g_map.redCaps = 0;
    g_map.lastCappingSlot = -1;
    g_map.lastSampleTime = g_map.loadTime;

    ++g_mapSerial;
    for (auto& s : g_slots) {
        s = SlotState();
        s.mapSerial = 0;
    }
}

void ClientBeginPostImpl(void* ent) {
    const int slot = EntitySlot(ent);
    if (slot < 0)
        return;
    PlayerSnapshot p;
    if (!SnapshotSlot(slot, true, &p))
        return;

    if (!p.spectator)
        EnsureMapStarted();

    g_slots[slot].active = true;
    g_slots[slot].mapSerial = g_mapSerial;
    g_slots[slot].lastTeam = p.team;
    g_slots[slot].deaths = 0;
    g_slots[slot].suicides = 0;
    g_slots[slot].teamkills = 0;
    g_slots[slot].flagsCaptured = 0;
    g_slots[slot].flagsRecovered = 0;
    EmitPlayerConnect(p);
}

void ClientUserinfoPostImpl(void* ent, char*, int) {
    const int slot = EntitySlot(ent);
    if (slot < 0 || !g_slots[slot].active ||
        g_slots[slot].mapSerial != g_mapSerial)
        return;
    PlayerSnapshot p;
    if (!SnapshotSlot(slot, true, &p))
        return;
    if (p.team != 0 && p.team != g_slots[slot].lastTeam) {
        const int from = g_slots[slot].lastTeam;
        g_slots[slot].lastTeam = p.team;
        EmitTeamChange(p, from);
    }
}

void ClientDisconnectPreImpl(void*& ent) {
    const int slot = EntitySlot(ent);
    if (slot < 0 || !g_slots[slot].active)
        return;
    PlayerSnapshot p;
    if (SnapshotSlot(slot, false, &p))
        EmitPlayerDisconnect(p);
    g_slots[slot] = SlotState();
}

void PlayerDiePostImpl(void* self, void*, void* attacker, int mod, float*) {
    const int victimSlot = EntitySlot(self);
    if (victimSlot < 0)
        return;
    PlayerSnapshot victim;
    if (!SnapshotSlot(victimSlot, true, &victim))
        return;

    const int killerSlot = attacker && attacker != self ? EntitySlot(attacker) : -1;
    PlayerSnapshot killer;
    const bool hasKiller =
        killerSlot >= 0 && SnapshotSlot(killerSlot, true, &killer);
    const bool suicide = !hasKiller;
    const bool teamkill =
        hasKiller && victim.team != 0 && victim.team == killer.team;

    EnsureMapStarted();
    if (g_slots[victimSlot].mapSerial == g_mapSerial) {
        ++g_slots[victimSlot].deaths;
        if (suicide)
            ++g_slots[victimSlot].suicides;
        if (teamkill && killerSlot >= 0 &&
            g_slots[killerSlot].mapSerial == g_mapSerial)
            ++g_slots[killerSlot].teamkills;
    }

    if (EventEnabled(kEventCombat))
        EmitPlayerDeath(victim, hasKiller ? &killer : nullptr, mod, teamkill,
                        suicide);
}

void CtfBaseTouchImpl(void* self, void* other, void* plane, void* surf,
                      detour_ctf_base_touch::tctf_base_touch original) {
    if (!original)
        return;
    if (!self || !other) {
        original(self, other, plane, surf);
        return;
    }

    const int before = ReadIntOr(static_cast<char*>(other) + kEdictCtfFlagsOfs, 0);
    const int slot = EntitySlot(other);
    PlayerSnapshot p;
    const bool havePlayer = slot >= 0 && SnapshotSlot(slot, true, &p);

    original(self, other, plane, surf);

    if (before == 0 || !havePlayer || p.team == 0 || p.team == before)
        return;

    // The enemy flag was carried to the player's own base and reset. This is
    // the same moment SoFPlus logs `flag`, but our counter is not allowed to
    // miss the deciding cap.
    EnsureMapStarted();
    if (p.team == 1)
        ++g_map.blueCaps;
    else if (p.team == 2)
        ++g_map.redCaps;
    g_map.lastCappingSlot = slot;
    if (g_slots[slot].mapSerial == g_mapSerial)
        ++g_slots[slot].flagsCaptured;

    if (EventEnabled(kEventFlags))
        EmitFlagCapture(p, before);
}

void SvFramePostImpl(int) {
    if (!g_initialized)
        return;
    EnsureSession();
    if (!g_sessionStarted || !g_map.loaded || g_map.ended)
        return;
    // ClientBegin is the normal map-start trigger. This also covers a client
    // that was already connected when the feature loaded (or a bot) without
    // inventing a map row for an empty rotation.
    if (!g_map.started && HasConnectedClient())
        EnsureMapStarted();
    if (!g_map.started)
        return;

    if (LevelIntermission()) {
        EmitMapEnd("intermission");
        return;
    }

    const int sampleSec = Telemetry_SampleSeconds();
    if (sampleSec > 0 && EventEnabled(kEventSamples)) {
        const float now = LevelTime();
        if (now - g_map.lastSampleTime >= static_cast<float>(sampleSec)) {
            g_map.lastSampleTime = now;
            EmitSamples();
        }
    }

    if (InterlockedCompareExchange(&g_writerError, 0, 0) != 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Buddy_DebugPrintf("[telemetry] spool writer error; check telemetry dir\n");
        }
    }
}

}  // namespace

// Core entry points called from gamex86.cpp (C linkage).
extern "C" void Telemetry_Shutdown(void) {
    ShutdownImpl();
}

extern "C" void Telemetry_OnSpawnEntities(const char* mapname) {
    OnSpawnEntitiesImpl(mapname);
}

// Generated hook callbacks use the same C++ linkage as the registration
// header's declarations.
void Telemetry_OnGameDllLoaded(void* gameExport) {
    OnGameDllLoadedImpl(gameExport);
}

void Telemetry_ClientBeginPost(void* ent) {
    ClientBeginPostImpl(ent);
}

void Telemetry_ClientUserinfoPost(void* ent, char* userinfo, int notFirst) {
    ClientUserinfoPostImpl(ent, userinfo, notFirst);
}

void Telemetry_ClientDisconnectPre(void*& ent) {
    ClientDisconnectPreImpl(ent);
}

void Telemetry_PlayerDiePost(void* self, void* inflictor, void* attacker,
                             int damage, float* point) {
    PlayerDiePostImpl(self, inflictor, attacker, damage, point);
}

void Telemetry_CtfBaseTouch(void* self, void* other, void* plane, void* surf,
                            detour_ctf_base_touch::tctf_base_touch original) {
    CtfBaseTouchImpl(self, other, plane, surf, original);
}

void Telemetry_SvFramePost(int msec) {
    SvFramePostImpl(msec);
}
