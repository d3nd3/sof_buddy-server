// server_demo: record each client's finished player_state_t every server tick.
//
// The capture point is ClientEndServerFrame Post: G_RunFrame has already run
// G_SetStats for the slot, so ps.stats, ps.pmove and the view fields are the
// values the client would have received this tick. The player edict's
// entity_state_t is captured beside it (origin/angles/model/frame/effects),
// which is what a renderer needs to draw the body and the camera separately.
//
// Files are written under <userdir>/<dir>/<name>.sbd. The container format is
// in server_demo_format.h; it is explicitly serialized and versioned, because
// the point is to read it later from a web renderer (and from a replay server)
// without needing SoF's network protocol.
//
// Offsets (verified against retail gamex86.dll and the SoF game source):
//   g_edicts .................... gamex86+0x15CCA0 (or ge->edicts)
//   edict stride ................ 0x464 (or ge->edict_size)
//   edict_t.client .............. ent+0x74
//   gclient_t.ps ................ client+0
//   player_state_t.stats ........ ps+164, short[16]
//   player_state_t size ......... 200
//   entity_state_t .............. ent+0, 116 bytes
//   gclient_t.pers.userinfo ..... client+0xCC (512)
//   gclient_t.pers.netname ...... client+0x2CC (16)
//   gclient_t.pers.spectator .... client+0x2DC (qboolean)
//   level.framenum .............. gamex86+0x15CCD8
//   level.time .................. gamex86+0x15CCDC
//   level.level_name ............ gamex86+0x15CCE0
//   level.mapname ............... gamex86+0x15CD20

#include "cvar.h"
#include "server_demo_format.h"

#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <windows.h>

extern "C" HMODULE Buddy_GetGameDllHandle(void);

namespace {

constexpr unsigned kRvaGEdicts = 0x15CCA0;
constexpr unsigned kRvaLevelFramenum = 0x15CCD8;
constexpr unsigned kRvaLevelTime = 0x15CCDC;
constexpr unsigned kRvaLevelName = 0x15CCE0;
constexpr unsigned kRvaLevelMapName = 0x15CD20;
constexpr unsigned kRvaCmdFunctions = 0x241840;
// SoF.exe server_t.configstrings: SV_FindIndex uses
// `offset unk_203A23B4 + index*64`; SoF.exe base 0x20000000.
constexpr unsigned kRvaConfigStrings = 0x3A23B4;
constexpr unsigned kConfigStringSize = 64;
constexpr int kConfigStringCount = 0xDE3;  // MAX_CONFIGSTRINGS

constexpr int kDefaultEdictStride = 0x464;
constexpr unsigned kEdictClientOfs = 0x74;
constexpr unsigned kClientUserinfoOfs = 0xCC;
constexpr unsigned kClientNetnameOfs = 0x2CC;
constexpr unsigned kClientSpectatorOfs = 0x2DC;
constexpr unsigned kClientNeedBytes = 0x300;
constexpr int kMaxClients = 64;
constexpr std::size_t kFlushThreshold = 64 * 1024;
constexpr int kCmdNameOfs = 0x04;
constexpr int kCmdFnOfs = 0x08;

void* g_gameExport = nullptr;
HMODULE g_gameModule = nullptr;
char* g_edicts = nullptr;
int g_edictStride = 0;
void* g_cvMaxClients = nullptr;
bool g_shuttingDown = false;
std::string g_userdir;
std::string g_gamedir;
std::string g_serverName;

uint32_t g_rosterHash[kMaxClients + 1] = {};
bool g_rosterValid[kMaxClients + 1] = {};

struct RecordingState {
    bool active = false;
    bool ioError = false;
    std::FILE* file = nullptr;
    std::vector<std::uint8_t> buffer;
    std::string path;
    std::string map;
    std::string levelName;
    std::uint32_t firstFrame = 0;
    std::uint32_t lastFrame = 0;
    std::uint64_t clientFrames = 0;
    std::uint32_t maxSlot = 0;
    std::uint64_t bytes = 0;
    unsigned long long maxBytes = 0;
    std::uint32_t startLevelMs = 0;
    std::uint32_t lastLevelMs = 0;
    int interval = 1;
    bool entities = true;
};

RecordingState g_rec;

void StopRecording(std::uint8_t reason);

// ---------------------------------------------------------------------------
// Memory / module validation (same helpers as the other server features).
// ---------------------------------------------------------------------------
HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return nullptr;
}

HMODULE GameMod() {
    if (g_gameModule)
        return g_gameModule;
    HMODULE h = Buddy_GetGameDllHandle();
    if (!h)
        h = GetModuleHandleA("oldgamex86.dll");
    if (!h)
        h = GetModuleHandleA("OLDGAMEX86.DLL");
    g_gameModule = h;
    return h;
}

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

bool IsValidModuleRva(HMODULE h, unsigned rva, std::size_t size) {
    if (!h)
        return false;
    return IsSafeMemoryBlock(reinterpret_cast<char*>(h) + rva, size);
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

void* GameExportPointer(unsigned offset, std::size_t size) {
    if (!g_gameExport || !IsSafeMemoryBlock(static_cast<char*>(g_gameExport) + offset, size))
        return nullptr;
    return static_cast<char*>(g_gameExport) + offset;
}

// ---------------------------------------------------------------------------
// Game globals.
// ---------------------------------------------------------------------------
char* Edicts() {
    if (g_edicts)
        return g_edicts;
    if (void* p = GameExportPointer(0x60, sizeof(void*))) {
        char* edicts = *reinterpret_cast<char**>(p);
        if (IsSafeMemoryBlock(edicts, 1))
            g_edicts = edicts;
    }
    if (!g_edicts) {
        HMODULE h = GameMod();
        if (IsValidModuleRva(h, kRvaGEdicts, sizeof(void*))) {
            char* edicts = *reinterpret_cast<char**>(
                reinterpret_cast<char*>(h) + kRvaGEdicts);
            if (IsSafeMemoryBlock(edicts, 1))
                g_edicts = edicts;
        }
    }
    return g_edicts;
}

int EdictStride() {
    if (g_edictStride > 0)
        return g_edictStride;
    if (void* p = GameExportPointer(0x64, sizeof(int))) {
        const int stride = *reinterpret_cast<int*>(p);
        if (stride >= 0x100 && stride <= 0x1000)
            g_edictStride = stride;
    }
    if (g_edictStride <= 0)
        g_edictStride = kDefaultEdictStride;
    return g_edictStride;
}

int MaxClients() {
    int n = static_cast<int>(Buddy_ReadCvarValue(g_cvMaxClients, 8.0f));
    if (n < 1)
        n = 1;
    if (n > kMaxClients)
        n = kMaxClients;
    return n;
}

void* ClientForEnt(void* ent) {
    if (!ent || !IsSafeMemoryBlock(static_cast<char*>(ent) + kEdictClientOfs, sizeof(void*)))
        return nullptr;
    void* client = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClientOfs);
    if (!client || !IsSafeMemoryBlock(client, kClientNeedBytes))
        return nullptr;
    return client;
}

int SlotForEnt(void* ent) {
    char* edicts = Edicts();
    const int stride = EdictStride();
    if (!ent || !edicts || stride <= 0)
        return 0;
    const auto diff = static_cast<char*>(ent) - edicts;
    if (diff > 0 && diff % stride == 0) {
        const int slot = static_cast<int>(diff / stride);
        if (slot >= 1 && slot <= MaxClients())
            return slot;
    }
    const int n = MaxClients();
    for (int slot = 1; slot <= n; ++slot) {
        if (edicts + static_cast<unsigned>(slot) * static_cast<unsigned>(stride) == ent)
            return slot;
    }
    return 0;
}

std::uint32_t LevelFrame() {
    HMODULE h = GameMod();
    if (!IsValidModuleRva(h, kRvaLevelFramenum, sizeof(int)))
        return 0;
    return static_cast<std::uint32_t>(
        *reinterpret_cast<const int*>(reinterpret_cast<char*>(h) + kRvaLevelFramenum));
}

std::uint32_t LevelTimeMs() {
    HMODULE h = GameMod();
    if (!IsValidModuleRva(h, kRvaLevelTime, sizeof(float)))
        return 0;
    const float t = *reinterpret_cast<const float*>(reinterpret_cast<char*>(h) + kRvaLevelTime);
    if (!(t > 0.0f))
        return 0;
    const double ms = static_cast<double>(t) * 1000.0 + 0.5;
    if (ms > 0xffffffffu)
        return 0xffffffffu;
    return static_cast<std::uint32_t>(ms);
}

std::string ReadGameString(unsigned rva, std::size_t cap) {
    HMODULE h = GameMod();
    if (!IsValidModuleRva(h, rva, cap))
        return std::string();
    const char* p = reinterpret_cast<const char*>(h) + rva;
    std::size_t n = 0;
    while (n < cap && p[n])
        ++n;
    return std::string(p, n);
}

std::string LevelName() {
    return ReadGameString(kRvaLevelName, 64);
}

std::string LevelMapName() {
    return ReadGameString(kRvaLevelMapName, 64);
}

// ---------------------------------------------------------------------------
// Engine cvar strings / filenames.
// ---------------------------------------------------------------------------
std::string CvarStringByName(const char* name, const char* fallback) {
    void* cv = Buddy_GetEngineCvar(name, fallback, 0, nullptr);
    if (!cv || !IsSafeMemoryBlock(static_cast<char*>(cv) + 0x04, sizeof(char*)))
        return std::string(fallback ? fallback : "");
    const char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + 0x04);
    if (!s || !IsSafeMemoryBlock(s, 1))
        return std::string(fallback ? fallback : "");
    std::size_t n = 0;
    while (n < 512 && s[n])
        ++n;
    return std::string(s, n);
}

void SanitizeName(const char* in, char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    std::size_t j = 0;
    if (in) {
        for (std::size_t i = 0; in[i] && j + 1 < cap; ++i) {
            const char c = in[i];
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            out[j++] = safe ? c : '_';
        }
    }
    out[j] = '\0';
    if (j == 0)
        std::snprintf(out, cap, "demo");
    if (out[0] == '.')
        out[0] = '_';
}

std::string MapBaseName() {
    std::string map = LevelMapName();
    const std::size_t slash = map.find_last_of("/\\");
    if (slash != std::string::npos)
        map = map.substr(slash + 1);
    const std::size_t dot = map.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        map = map.substr(0, dot);
    if (map.empty())
        map = "demo";
    return map;
}

void MakeAutoName(char* out, std::size_t cap) {
    char map[64];
    SanitizeName(MapBaseName().c_str(), map, sizeof(map));
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::snprintf(out, cap, "%s_%04u%02u%02u_%02u%02u%02u", map,
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
}

void BuildRecordingPath(const char* requested, char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    char name[80];
    if (requested && requested[0])
        SanitizeName(requested, name, sizeof(name));
    else
        MakeAutoName(name, sizeof(name));

    const char* user = g_userdir.empty() ? "." : g_userdir.c_str();
    const char* dir = ServerDemo_Dir();
    char file_name[96];
    const std::size_t n = std::strlen(name);
    if (n > 4 && std::strcmp(name + n - 4, ".sbd") == 0)
        std::snprintf(file_name, sizeof(file_name), "%s", name);
    else
        std::snprintf(file_name, sizeof(file_name), "%s.sbd", name);
    // FS_CreatePath (gi slot 96) writes temporary NULs into its argument, so
    // this must be a mutable buffer, not std::string::c_str().
    std::snprintf(out, cap, "%s/%s/%s", user, dir, file_name);
}

// ---------------------------------------------------------------------------
// File writer.
// ---------------------------------------------------------------------------
bool FlushBuffer() {
    if (!g_rec.file || g_rec.buffer.empty())
        return true;
    const std::size_t n =
        std::fwrite(g_rec.buffer.data(), 1, g_rec.buffer.size(), g_rec.file);
    g_rec.bytes += n;
    if (n != g_rec.buffer.size()) {
        // Keep only the unwritten tail. A later flush (or the END attempt)
        // must not duplicate the prefix that did reach the file.
        g_rec.buffer.erase(g_rec.buffer.begin(),
                           g_rec.buffer.begin() + static_cast<std::ptrdiff_t>(n));
        g_rec.ioError = true;
        if (!g_shuttingDown)
            Buddy_DebugPrintf("[svdemo] write failed: %s\n", g_rec.path.c_str());
        return false;
    }
    g_rec.buffer.clear();
    return true;
}

bool AppendChunk(const std::vector<std::uint8_t>& chunk) {
    if (!g_rec.active)
        return false;
    if (g_rec.maxBytes && g_rec.bytes + g_rec.buffer.size() + chunk.size() > g_rec.maxBytes) {
        StopRecording(svdemo::kEndSizeCap);
        return false;
    }
    g_rec.buffer.insert(g_rec.buffer.end(), chunk.begin(), chunk.end());
    if (g_rec.buffer.size() >= kFlushThreshold && !FlushBuffer()) {
        StopRecording(svdemo::kEndError);
        return false;
    }
    return true;
}

void WriteConfigStringsToRecording() {
    HMODULE exe = ExeMod();
    if (!IsValidModuleRva(exe, kRvaConfigStrings, kConfigStringSize)) {
        Buddy_DebugPrintf("[svdemo] configstrings unavailable; map assets unresolved\n");
        return;
    }
    char* base = reinterpret_cast<char*>(exe) + kRvaConfigStrings;
    const bool wholeArray =
        IsSafeMemoryBlock(base, static_cast<std::size_t>(kConfigStringCount) * kConfigStringSize);
    int written = 0;
    for (int i = 0; i < kConfigStringCount; ++i) {
        char* s = base + static_cast<unsigned>(i) * kConfigStringSize;
        if (!wholeArray && !IsSafeMemoryBlock(s, kConfigStringSize))
            continue;
        std::size_t n = 0;
        while (n < kConfigStringSize && s[n])
            ++n;
        if (n == 0)
            continue;
        svdemo::ConfigString cs;
        cs.index = static_cast<std::uint16_t>(i);
        cs.value.assign(s, n);
        std::vector<std::uint8_t> chunk;
        chunk.reserve(6 + n);
        svdemo::WriteConfigString(chunk, cs);
        if (!AppendChunk(chunk))
            break;
        ++written;
    }
    Buddy_DebugPrintf("[svdemo] captured %d configstring(s)\n", written);
}

void ResetRecordingState() {
    g_rec.active = false;
    g_rec.ioError = false;
    g_rec.file = nullptr;
    g_rec.buffer.clear();
    g_rec.path.clear();
    g_rec.map.clear();
    g_rec.levelName.clear();
    g_rec.firstFrame = 0;
    g_rec.lastFrame = 0;
    g_rec.clientFrames = 0;
    g_rec.maxSlot = 0;
    g_rec.bytes = 0;
    g_rec.maxBytes = 0;
    g_rec.startLevelMs = 0;
    g_rec.lastLevelMs = 0;
    g_rec.interval = 1;
    g_rec.entities = true;
}

const char* EndReasonName(std::uint8_t reason) {
    switch (reason) {
        case svdemo::kEndStopped:
            return "stopped";
        case svdemo::kEndMapChange:
            return "map change";
        case svdemo::kEndDisabled:
            return "disabled";
        case svdemo::kEndSizeCap:
            return "size cap";
        case svdemo::kEndShutdown:
            return "shutdown";
        case svdemo::kEndError:
            return "error";
        default:
            return "unknown";
    }
}

void StopRecording(std::uint8_t reason) {
    if (!g_rec.active && !g_rec.file)
        return;

    const bool wasActive = g_rec.active;
    const std::string path = g_rec.path;
    const std::uint64_t frames = g_rec.clientFrames;
    const std::uint64_t bytes = g_rec.bytes + g_rec.buffer.size();

    g_rec.active = false;
    if (g_rec.file) {
        if (wasActive) {
            svdemo::EndInfo end;
            end.first_frame = g_rec.firstFrame;
            end.last_frame = g_rec.lastFrame;
            end.client_frames = static_cast<std::uint32_t>(g_rec.clientFrames);
            end.max_clients = g_rec.maxSlot;
            end.duration_ms = g_rec.lastLevelMs >= g_rec.startLevelMs
                                  ? g_rec.lastLevelMs - g_rec.startLevelMs
                                  : 0;
            end.reason = g_rec.ioError ? static_cast<std::uint8_t>(svdemo::kEndError)
                                       : reason;
            std::vector<std::uint8_t> tail;
            svdemo::WriteEnd(tail, end);
            g_rec.buffer.insert(g_rec.buffer.end(), tail.begin(), tail.end());
        }
        FlushBuffer();
        std::fclose(g_rec.file);
        g_rec.file = nullptr;
    }

    if (wasActive && !g_shuttingDown) {
        Buddy_DebugPrintf("[svdemo] stopped (%s): %s - %u client-frames, %u bytes\n",
                          EndReasonName(reason), path.c_str(),
                          static_cast<unsigned>(frames),
                          static_cast<unsigned>(bytes));
    }
    ResetRecordingState();
}

bool StartRecording(const char* requested, bool automatic) {
    if (g_rec.active || g_rec.file)
        return false;
    if (!ServerDemo_Enabled()) {
        Buddy_DebugPrintf("[svdemo] recorder is disabled (_sofbuddy_svdemo 0)\n");
        return false;
    }
    const std::string map = LevelMapName();
    if (map.empty()) {
        Buddy_DebugPrintf("[svdemo] no level loaded; cannot start\n");
        return false;
    }

    char path_buf[512];
    BuildRecordingPath(requested, path_buf, sizeof(path_buf));
    if (!path_buf[0]) {
        Buddy_DebugPrintf("[svdemo] could not build a recording path\n");
        return false;
    }
    Buddy_FS_CreatePath(path_buf);
    std::FILE* file = std::fopen(path_buf, "wb");
    if (!file) {
        Buddy_DebugPrintf("[svdemo] cannot open %s\n", path_buf);
        return false;
    }
    const std::string path(path_buf);

    ResetRecordingState();
    g_rec.file = file;
    g_rec.path = path;
    g_rec.map = map;
    g_rec.levelName = LevelName();
    g_rec.entities = ServerDemo_EntitiesEnabled();
    g_rec.interval = ServerDemo_Interval();
    g_rec.maxBytes = ServerDemo_MaxBytes();
    g_rec.firstFrame = LevelFrame();
    g_rec.lastFrame = g_rec.firstFrame;
    g_rec.startLevelMs = LevelTimeMs();
    g_rec.lastLevelMs = g_rec.startLevelMs;
    g_rec.maxSlot = 0;

    svdemo::FileInfo info;
    info.flags = g_rec.entities ? svdemo::kChunkFlagHasEntity : svdemo::kChunkFlagNone;
    info.max_clients = static_cast<std::uint32_t>(MaxClients());
    info.tick_ms = 100;
    info.created_unix = static_cast<std::uint32_t>(std::time(nullptr));
    info.map_name = map;
    info.level_name = g_rec.levelName;
    info.gamedir = g_gamedir;
    info.server_name = g_serverName;
    std::vector<std::uint8_t> header;
    svdemo::WriteFileHeader(header, info);
    g_rec.buffer.insert(g_rec.buffer.end(), header.begin(), header.end());

    if (!FlushBuffer()) {
        std::fclose(g_rec.file);
        ResetRecordingState();
        return false;
    }

    std::memset(g_rosterHash, 0, sizeof(g_rosterHash));
    std::memset(g_rosterValid, 0, sizeof(g_rosterValid));
    g_rec.active = true;
    WriteConfigStringsToRecording();
    if (!g_rec.active)
        return false;
    Buddy_DebugPrintf("[svdemo] recording%s: %s (map %s, every %d tick(s)%s)\n",
                      automatic ? " (auto)" : "", path.c_str(), map.c_str(),
                      g_rec.interval, g_rec.entities ? ", entity state" : "");
    return true;
}

// ---------------------------------------------------------------------------
// Roster.
// ---------------------------------------------------------------------------
std::string FixedString(const char* src, std::size_t cap) {
    if (!src)
        return std::string();
    std::size_t n = 0;
    while (n < cap && src[n])
        ++n;
    return std::string(src, n);
}

std::uint32_t HashRoster(const std::string& name, const std::string& userinfo,
                         std::uint8_t flags) {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 16777619u;
        }
    };
    mix(name.data(), name.size());
    mix(userinfo.data(), userinfo.size());
    mix(&flags, sizeof(flags));
    return h ? h : 1u;
}

void MaybeWriteRoster(void* client, int slot, std::uint32_t frame, std::uint32_t timeMs) {
    const std::string name = FixedString(static_cast<char*>(client) + kClientNetnameOfs, 16);
    const std::string userinfo = FixedString(static_cast<char*>(client) + kClientUserinfoOfs, 512);
    const std::uint8_t spectator =
        *reinterpret_cast<const std::uint8_t*>(static_cast<char*>(client) + kClientSpectatorOfs)
            ? svdemo::kClientFlagSpectator
            : svdemo::kClientFlagNone;
    const std::uint32_t hash = HashRoster(name, userinfo, spectator);
    if (g_rosterValid[slot] && g_rosterHash[slot] == hash)
        return;

    svdemo::RosterEntry entry;
    entry.frame = frame;
    entry.time_ms = timeMs;
    entry.slot = static_cast<std::uint8_t>(slot);
    entry.flags = spectator;
    entry.name = name;
    entry.userinfo = userinfo;
    std::vector<std::uint8_t> chunk;
    chunk.reserve(24 + name.size() + userinfo.size());
    svdemo::WriteRoster(chunk, entry);
    if (!AppendChunk(chunk))
        return;
    g_rosterHash[slot] = hash;
    g_rosterValid[slot] = true;
}

// ---------------------------------------------------------------------------
// Capture.
// ---------------------------------------------------------------------------
void CaptureClient(void* ent) {
    if (!g_rec.active || !ent)
        return;
    void* client = ClientForEnt(ent);
    if (!client)
        return;
    const int slot = SlotForEnt(ent);
    if (slot < 1 || slot > kMaxClients)
        return;

    const std::uint32_t frame = LevelFrame();
    const std::uint32_t timeMs = LevelTimeMs();
    if (timeMs != g_rec.lastLevelMs)
        g_rec.lastLevelMs = timeMs;

    // Roster changes are recorded even on skipped frames, so a name/model
    // change is never lost just because the interval did not align.
    MaybeWriteRoster(client, slot, frame, timeMs);

    if (g_rec.interval > 1 && (frame % static_cast<std::uint32_t>(g_rec.interval)) != 0)
        return;

    svdemo::ClientFrame cf;
    cf.frame = frame;
    cf.time_ms = timeMs;
    cf.slot = static_cast<std::uint8_t>(slot);
    cf.flags = svdemo::kClientFlagNone;
    std::memcpy(&cf.ps, static_cast<char*>(client), sizeof(cf.ps));
    // These are process-local pointers in the live game; never write them.
    cf.ps.gun = 0;
    cf.ps.bod = 0;
    if (*reinterpret_cast<const std::uint8_t*>(static_cast<char*>(client) + kClientSpectatorOfs))
        cf.flags |= svdemo::kClientFlagSpectator;

    if (g_rec.entities) {
        if (IsSafeMemoryBlock(ent, sizeof(cf.es))) {
            std::memcpy(&cf.es, ent, sizeof(cf.es));
            cf.flags |= svdemo::kClientFlagHasEntity;
        }
    }

    std::vector<std::uint8_t> chunk;
    chunk.reserve(12 + sizeof(cf.ps) + (g_rec.entities ? sizeof(cf.es) : 0));
    svdemo::WriteClientFrame(chunk, cf);
    if (!AppendChunk(chunk))
        return;

    if (g_rec.clientFrames == 0)
        g_rec.firstFrame = frame;
    ++g_rec.clientFrames;
    g_rec.lastFrame = frame;
    if (static_cast<std::uint32_t>(slot) > g_rec.maxSlot)
        g_rec.maxSlot = static_cast<std::uint32_t>(slot);
}

// ---------------------------------------------------------------------------
// Commands.
// ---------------------------------------------------------------------------
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
    void* node = FindCmdNode(name);
    if (node) {
        *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdFnOfs) = fn;
        return;
    }
    if (detour_Cmd_AddCommand::oCmd_AddCommand)
        SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

std::string JoinArgs(int argc) {
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const char* arg =
            detour_Cmd_Argv::oCmd_Argv ? SOF_EP_Cmd_Argv(i) : nullptr;
        if (!arg)
            continue;
        if (!out.empty())
            out.push_back(' ');
        out += arg;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public lifecycle.
// ---------------------------------------------------------------------------
extern "C" void __cdecl svdemo_Start_f();
extern "C" void __cdecl svdemo_Stop_f();
extern "C" void __cdecl svdemo_Status_f();
extern "C" void __cdecl svdemo_Mark_f();
extern "C" void __cdecl svdemo_Flush_f();
extern "C" void __cdecl svdemo_Roster_f();

void ServerDemo_OnGameDllLoaded(void* gameExport) {
    g_gameExport = gameExport;
    g_gameModule = nullptr;
    g_edicts = nullptr;
    g_edictStride = 0;
    g_cvMaxClients = nullptr;
    g_shuttingDown = false;
    g_userdir.clear();
    g_gamedir.clear();
    g_serverName.clear();

    ServerDemo_InitCvars();
    // Bind everything the frame path needs here, in bootstrap context. Reading
    // cvar values later is safe; calling Cvar_Get from the frame path is not.
    g_cvMaxClients = Buddy_GetEngineCvar("maxclients", "8", 0, nullptr);
    const char* userdir = Buddy_FS_Userdir();
    if (userdir && userdir[0])
        g_userdir = userdir;
    g_gamedir = CvarStringByName("gamedir", "");
    g_serverName = CvarStringByName("hostname", "");

    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        !IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Cmd_AddCommand::oCmd_AddCommand))) {
        PrintOut(PRINT_BAD, "[svdemo] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }
    InstallCommand("svdemo_start", reinterpret_cast<void*>(&svdemo_Start_f));
    InstallCommand("svdemo_stop", reinterpret_cast<void*>(&svdemo_Stop_f));
    InstallCommand("svdemo_status", reinterpret_cast<void*>(&svdemo_Status_f));
    InstallCommand("svdemo_mark", reinterpret_cast<void*>(&svdemo_Mark_f));
    InstallCommand("svdemo_flush", reinterpret_cast<void*>(&svdemo_Flush_f));
    InstallCommand("svdemo_roster", reinterpret_cast<void*>(&svdemo_Roster_f));
    PrintOut(PRINT_LOG, "[svdemo] state-demo recorder ready (format v%u)\n",
             static_cast<unsigned>(svdemo::kVersion));
}

void ServerDemo_ClientEndServerFramePost(void* ent) {
    if (!ent)
        return;

    // Idle path: no allocation, no map string, no VirtualQuery when recording
    // is off and auto-start is not requested. This hook runs for every client
    // on every server tick.
    if (!g_rec.active) {
        if (!ServerDemo_Enabled() || !ServerDemo_AutoEnabled())
            return;
    } else if (!ServerDemo_Enabled()) {
        StopRecording(svdemo::kEndDisabled);
        return;
    }

    const std::string map = LevelMapName();
    if (g_rec.active && (map.empty() || map != g_rec.map))
        StopRecording(svdemo::kEndMapChange);

    if (!g_rec.active) {
        if (!ServerDemo_AutoEnabled() || map.empty())
            return;
        if (!StartRecording(nullptr, true))
            return;
    }

    CaptureClient(ent);
}

extern "C" void __cdecl svdemo_Start_f() {
    if (!detour_Cmd_Argc::oCmd_Argc)
        return;
    const int argc = SOF_EP_Cmd_Argc();
    const char* name =
        argc >= 2 && detour_Cmd_Argv::oCmd_Argv ? SOF_EP_Cmd_Argv(1) : nullptr;
    if (g_rec.active) {
        Buddy_DebugPrintf("[svdemo] already recording: %s\n", g_rec.path.c_str());
        return;
    }
    StartRecording(name, false);
}

extern "C" void __cdecl svdemo_Stop_f() {
    if (!g_rec.active) {
        Buddy_DebugPrintf("[svdemo] not recording\n");
        return;
    }
    StopRecording(svdemo::kEndStopped);
}

extern "C" void __cdecl svdemo_Status_f() {
    if (!g_rec.active) {
        Buddy_DebugPrintf("[svdemo] idle (master %d, auto %d, dir %s)\n",
                          ServerDemo_Enabled() ? 1 : 0, ServerDemo_AutoEnabled() ? 1 : 0,
                          ServerDemo_Dir());
        return;
    }
    Buddy_DebugPrintf(
        "[svdemo] recording %s\n"
        "[svdemo]   map=%s level=%s frames=%u..%u client-frames=%u bytes=%u max-slot=%u\n"
        "[svdemo]   interval=%d entities=%d level-ms=%u\n",
        g_rec.path.c_str(), g_rec.map.c_str(), g_rec.levelName.c_str(),
        static_cast<unsigned>(g_rec.firstFrame), static_cast<unsigned>(g_rec.lastFrame),
        static_cast<unsigned>(g_rec.clientFrames),
        static_cast<unsigned>(g_rec.bytes + g_rec.buffer.size()),
        static_cast<unsigned>(g_rec.maxSlot), g_rec.interval, g_rec.entities ? 1 : 0,
        static_cast<unsigned>(g_rec.lastLevelMs));
}

extern "C" void __cdecl svdemo_Mark_f() {
    if (!g_rec.active) {
        Buddy_DebugPrintf("[svdemo] not recording\n");
        return;
    }
    if (!detour_Cmd_Argc::oCmd_Argc)
        return;
    const int argc = SOF_EP_Cmd_Argc();
    std::string text = JoinArgs(argc);
    if (text.empty())
        text = "mark";
    if (text.size() > 1024)
        text.resize(1024);
    svdemo::Event event;
    event.frame = LevelFrame();
    event.time_ms = LevelTimeMs();
    event.kind = svdemo::kEventMark;
    event.text = text;
    std::vector<std::uint8_t> chunk;
    svdemo::WriteEvent(chunk, event);
    if (AppendChunk(chunk))
        Buddy_DebugPrintf("[svdemo] mark @ %u ms: %s\n", static_cast<unsigned>(event.time_ms),
                          text.c_str());
}

extern "C" void __cdecl svdemo_Flush_f() {
    if (!g_rec.active) {
        Buddy_DebugPrintf("[svdemo] not recording\n");
        return;
    }
    if (FlushBuffer())
        Buddy_DebugPrintf("[svdemo] flushed (%u bytes)\n", static_cast<unsigned>(g_rec.bytes));
    else
        StopRecording(svdemo::kEndError);
}

extern "C" void __cdecl svdemo_Roster_f() {
    if (!g_rec.active) {
        Buddy_DebugPrintf("[svdemo] not recording\n");
        return;
    }
    std::memset(g_rosterValid, 0, sizeof(g_rosterValid));
    Buddy_DebugPrintf("[svdemo] roster will be re-emitted on the next client frame\n");
}

extern "C" void ServerDemo_Shutdown() {
    g_shuttingDown = true;
    StopRecording(svdemo::kEndShutdown);
    g_gameExport = nullptr;
    g_gameModule = nullptr;
    g_edicts = nullptr;
    g_edictStride = 0;
    g_cvMaxClients = nullptr;
}
