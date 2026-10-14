// gamepark (inside profiles): gamepark claim follows admin map changes.
//
// When a signed-in server admin (privilege 0) or map admin (privilege 1)
// changes the map with a sofplus `.rcon` command, they become the signed-in
// gamepark holder and any previous holder's rcon session is logged out.
//
// Deferred causality: this hook runs BEFORE sofplus, so at command time it
// cannot know whether the `.rcon map` will succeed (rcon auth, grants, valid
// map are all sofplus-side). It only records intent and always lets sofplus
// handle the command (never consumes). The SV_Frame Post check acts solely
// when the requested map really loads; failed attempts expire silently.
// Rcon auth and the map change itself stay entirely sofplus-side.
//
// Transfer lines mirror spf_sv_gamepark.func claim flow (see gamepark_logic.h
// for the protocol). All writes go through one Cbuf_AddText blob per chunk so
// they execute as ordinary console commands after the map is up; the gamepark
// 10s check then observes the transferred state. Logout is targeted: only
// rcon slots bound to the OLD claim ip (other than the issuer) are cleared,
// so unrelated rcon sessions are untouched.

#include "gamepark.h"
#include "gamepark_logic.h"
#include "../cvar.h"
#include "../../minigames/minigames_api.h"

#include "buddy_import.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------
constexpr unsigned kEdictClientOfs = 0x74;
constexpr unsigned kClUserinfoOfs = 0xCC;
constexpr unsigned kRvaSvsClients = 0x396EEC;  // svs.clients (same as sv_tracktime)
constexpr unsigned kClientStride = 0xD2AC;
constexpr unsigned kClientStateOfs = 0x00;
constexpr int kCsSpawned = 3;
// in_addr feeding inet_ntoa for _sp_sv_info_client_ip. Verified against the
// spsv sp_sv_info_client decompile (*(in_addr*)(client + 21124) with the same
// 0xD2AC stride and state==3 gate); the userinfo "ip" key is empty on this
// build, so the engine net address is the only native source.
constexpr unsigned kClientNetaddrOfs = 21124;
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet = 8;
constexpr DWORD kPendingTimeoutMs = 60000;  // intent without a map load expires
constexpr std::size_t kCbufChunkCap = 900;  // well under the 0x1000 insert cap

// ---------------------------------------------------------------------------
// Safety helpers (same pattern as profiles.cpp / stats.cpp).
// ---------------------------------------------------------------------------
inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

inline bool IsExecutableCodeAddress(const void* ptr) {
    if (!IsValidUserPointer(ptr))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

bool InfoKeyReady() {
    return detour_Info_ValueForKey::oInfo_ValueForKey &&
           !IsBadReadPtr(reinterpret_cast<void*>(detour_Info_ValueForKey::oInfo_ValueForKey), 1);
}

HMODULE ExeMod() {
    // Resolve the engine by name, not via GetModuleHandleA(nullptr), which
    // would hand back whatever executable hosts the process.
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return nullptr;
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !Readable(h, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const char* base = reinterpret_cast<const char*>(h);
    if (!Readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const unsigned img = nt->OptionalHeader.SizeOfImage;
    return rva < img && rva + size <= img;
}

// Bare dotted-quad address for a spawned slot, byte-identical to what
// sp_sv_info_client stores in _sp_sv_info_client_ip. Never touches userinfo.
bool SlotBareIp(int slot1, char* out, int cap) {
    if (!out || cap <= 0)
        return false;
    out[0] = '\0';
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return false;
    HMODULE h = ExeMod();
    if (!h || !IsValidModuleRva(h, kRvaSvsClients, sizeof(void*)))
        return false;
    const char* base = reinterpret_cast<const char*>(h);
    if (!Readable(base + kRvaSvsClients, sizeof(void*)))
        return false;
    const char* clients = *reinterpret_cast<char* const*>(base + kRvaSvsClients);
    if (!IsValidUserPointer(clients))
        return false;
    const char* client = clients + kClientStride * static_cast<unsigned>(slot1 - 1);
    if (!Readable(client + kClientStateOfs, sizeof(int)))
        return false;
    if (*reinterpret_cast<const int*>(client + kClientStateOfs) != kCsSpawned)
        return false;
    const unsigned char* ip =
        reinterpret_cast<const unsigned char*>(client + kClientNetaddrOfs);
    if (!Readable(ip, 4))
        return false;
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0)
        return false;
    std::string s = gamepark::FormatBareIp(ip[0], ip[1], ip[2], ip[3]);
    if (s.empty() || s.size() + 1 > static_cast<std::size_t>(cap))
        return false;
    std::snprintf(out, static_cast<std::size_t>(cap), "%s", s.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Cvars (_sofbuddy_gamepark master + NOSET gauges).
// ---------------------------------------------------------------------------
struct OutputCvar {
    void* cv = nullptr;
    char* original = nullptr;
    char buf[2][32] = {};
    int live = 0;

    void Bind(const char* name) {
        if (cv)
            return;
        cv = Buddy_GetEngineCvar(name, "0", kCvarFlagNoSet, nullptr);
        if (!cv)
            return;
        original = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    }

    void Publish(float value, const char* text) {
        if (!cv)
            return;
        char* base = static_cast<char*>(cv);
        *reinterpret_cast<volatile float*>(base + kCvarValueOfs) = value;
        char* spare = buf[1 - live];
        std::size_t i = 0;
        for (; text[i] && i + 1 < sizeof(buf[0]); ++i)
            spare[i] = text[i];
        spare[i] = '\0';
        *reinterpret_cast<char* volatile*>(base + kCvarStringOfs) = spare;
        live = 1 - live;
    }
};

void FormatI64(char* out, std::size_t n, long long v) {
    if (n == 0)
        return;
    char digits[24];
    std::size_t d = 0;
    unsigned long long mag =
        v < 0 ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        digits[d++] = static_cast<char>('0' + (mag % 10ull));
        mag /= 10ull;
    } while (mag != 0ull && d < sizeof(digits));
    std::size_t i = 0;
    if (v < 0 && i + 1 < n)
        out[i++] = '-';
    while (d > 0 && i + 1 < n)
        out[i++] = digits[--d];
    out[i] = '\0';
}

OutputCvar g_outTransfers;  // _sofbuddy_gamepark_transfers
OutputCvar g_outErrors;     // _sofbuddy_gamepark_errors
OutputCvar g_outPending;    // _sofbuddy_gamepark_pending
void* g_cvEnabled = nullptr;  // _sofbuddy_gamepark

void Gamepark_InitCvars() {
    g_outTransfers.Bind("_sofbuddy_gamepark_transfers");
    g_outErrors.Bind("_sofbuddy_gamepark_errors");
    g_outPending.Bind("_sofbuddy_gamepark_pending");
    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_gamepark", "1", kCvarFlagArchive, nullptr);
}

// ---------------------------------------------------------------------------
// State: one pending transfer intent (latest wins).
// ---------------------------------------------------------------------------
long long g_transfers = 0;
long long g_errors = 0;

struct Pending {
    bool active = false;
    int slot0 = -1;
    char ip[40] = {};
    char name[40] = {};
    char requested[72] = {};
    char mapAtReq[64] = {};
    long mapCountAtReq = -1;
    DWORD startMs = 0;
};
Pending g_pending;

void RefreshOutputs() {
    char text[32];
    FormatI64(text, sizeof(text), g_transfers);
    g_outTransfers.Publish(static_cast<float>(g_transfers), text);
    FormatI64(text, sizeof(text), g_errors);
    g_outErrors.Publish(static_cast<float>(g_errors), text);
    FormatI64(text, sizeof(text), g_pending.active ? 1 : 0);
    g_outPending.Publish(g_pending.active ? 1.0f : 0.0f, text);
}

void NoteError() {
    ++g_errors;
    RefreshOutputs();
}

// ---------------------------------------------------------------------------
// Engine reads.
// ---------------------------------------------------------------------------

// Engine Cvar_Get creates a missing cvar with the default — the same
// trade-off profiles already makes (e.g. _sp_sv_sound_13). Callers pass
// sentinel defaults ("-1" for the map counter) so absence stays detectable.
std::string EngineCvarString(const char* name, const char* def = "") {
    if (!name || !name[0])
        return def ? def : "";
    void* cv = Buddy_GetEngineCvar(name, def ? def : "", 0, nullptr);
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarStringOfs, sizeof(char*)))
        return def ? def : "";
    char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    if (!s || !IsValidUserPointer(s) || !Readable(s, 1))
        return def ? def : "";
    std::size_t len = 0;
    while (len < 256 && s[len])
        ++len;
    return std::string(s, len);
}

double EngineCvarNumber(const char* name) {
    std::string s = EngineCvarString(name, "0");
    return std::atof(s.c_str());
}

long ReadMapCount() {
    std::string s = EngineCvarString("_sp_sv_info_map_count", "-1");
    if (s.empty() || s == "-1")
        return -1;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return (end && *end == '\0' && v >= 0) ? v : -1;
}

void CurrentMap(char* out, int cap) {
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    std::string s = EngineCvarString("mapname", "");
    int i = 0;
    for (; i < static_cast<int>(s.size()) && i < cap - 1; ++i) {
        char c = s[static_cast<std::size_t>(i)];
        out[i] = (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 32) ? '_' : c;
    }
    out[i] = '\0';
    if (!out[0])
        std::snprintf(out, static_cast<std::size_t>(cap), "unknown");
}

void SlotInfoValue(void* ent, const char* key, char* out, int cap) {
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    if (!ent || !key || !key[0] ||
        !Readable(static_cast<char*>(ent) + kEdictClientOfs, sizeof(void*)))
        return;
    void* client = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClientOfs);
    if (!client || !Readable(static_cast<char*>(client) + kClUserinfoOfs, 64))
        return;
    const char* userinfo = static_cast<const char*>(client) + kClUserinfoOfs;
    if (!Readable(userinfo, 1) || !InfoKeyReady())
        return;
    const char* v = SOF_EP_Info_ValueForKey(userinfo, key);
    if (!v || !IsValidUserPointer(v) || !Readable(v, 1))
        return;
    std::size_t len = 0;
    while (len < 64 && v[len])
        ++len;
    if (len == 0)
        return;
    std::size_t n =
        len >= static_cast<std::size_t>(cap) ? static_cast<std::size_t>(cap - 1) : len;
    for (std::size_t i = 0; i < n; ++i) {
        char c = v[i];
        out[i] = (static_cast<unsigned char>(c) < 32 || c == '"' || c == '\\') ? ' ' : c;
    }
    out[n] = '\0';
}

// ---------------------------------------------------------------------------
// Word matching (dot-optional, case-insensitive — same as stats filter).
// ---------------------------------------------------------------------------
bool SameNoCase(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool WordMatches(const char* word, const char* name) {
    if (!word || !word[0] || !name || !name[0])
        return false;
    if (SameNoCase(word, name))
        return true;
    if (word[0] == '.' && SameNoCase(word + 1, name))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Transfer.
// ---------------------------------------------------------------------------
void QueueCbufLines(const std::vector<std::string>& lines) {
    auto add = detour_Cbuf_AddText::oCbuf_AddText;
    if (!add || !IsExecutableCodeAddress(reinterpret_cast<const void*>(add))) {
        Buddy_DebugPrintf("[gamepark] Cbuf unavailable, transfer dropped\n");
        NoteError();
        return;
    }
    std::string chunk;
    auto flush = [&]() {
        if (chunk.empty())
            return;
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", chunk.c_str());
        chunk.clear();
        add(tmp);
    };
    for (const std::string& ln : lines) {
        if (chunk.size() + ln.size() + 1 >= kCbufChunkCap)
            flush();
        chunk += ln;
        chunk += "\n";
    }
    flush();
}

void DoTransfer() {
    Pending p = g_pending;
    g_pending = Pending();
    RefreshOutputs();
    if (!MgSlotSpawned(p.slot0 + 1)) {
        Buddy_DebugPrintf("[gamepark] issuer slot %d gone, transfer dropped\n", p.slot0);
        return;
    }
    double duration = EngineCvarNumber("_spf_sv_gamepark_duration");
    if (!(duration > 0)) {
        Buddy_DebugPrintf("[gamepark] no gamepark duration configured, transfer dropped\n");
        return;
    }
    double claimed = EngineCvarNumber("_spf_sv_gamepark_claimed");
    if (!(claimed > 0)) {
        // Unclaimed (0) or resetting (-1): nobody holds it, so there is no
        // previous user to log out and no claim to take over.
        Buddy_DebugPrintf("[gamepark] server not claimed, transfer dropped\n");
        return;
    }
    std::string oldIp = EngineCvarString("_spf_sv_gamepark_claimed_ip", "");
    std::string hostBase = EngineCvarString("_spf_sv_gamepark_hostname", "");
    std::vector<gamepark::RconSlot> rss;
    const int n = MgMaxClients();
    for (int slot1 = 1; slot1 <= n && slot1 <= kMgMaxSlots; ++slot1) {
        if (!MgSlotSpawned(slot1))
            continue;
        char key[64];
        std::snprintf(key, sizeof(key), "_spf_sv_rcon_ip_%d", slot1 - 1);
        std::string rip = EngineCvarString(key, "");
        if (rip.empty())
            continue;
        gamepark::RconSlot rs;
        rs.slot0 = slot1 - 1;
        rs.ip = rip;
        rss.push_back(rs);
    }
    gamepark::TransferInput in;
    in.senderSlot0 = p.slot0;
    in.senderIp = p.ip;
    in.oldClaimedIp = oldIp;
    in.hostnameBase = hostBase;
    in.rconSlots = rss;
    std::vector<std::string> lines = gamepark::BuildTransferLines(in);
    if (lines.empty()) {
        NoteError();
        return;
    }
    unsigned loggedOut = 0;
    if (!oldIp.empty()) {
        for (const auto& rs : rss) {
            if (rs.slot0 != p.slot0 && rs.ip == oldIp)
                ++loggedOut;
        }
    }
    QueueCbufLines(lines);
    ++g_transfers;
    RefreshOutputs();
    Buddy_DebugPrintf("[gamepark] claim transferred to slot %d (%s); %u previous session(s) logged out\n",
                      p.slot0, p.name[0] ? p.name : "?", loggedOut);
}

// ---------------------------------------------------------------------------
// Console command registration (repoint-safe, same as profiles/stats).
// ---------------------------------------------------------------------------
HMODULE ExeModForCmd() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

void** CmdFunctionsHead() {
    HMODULE h = ExeModForCmd();
    if (!h)
        return nullptr;
    constexpr unsigned kRvaCmdFunctions = 0x241840;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(node)) {
        const char* n = *reinterpret_cast<const char**>(static_cast<char*>(node) + 0x04);
        if (n && std::strcmp(n, name) == 0)
            return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    void* node = FindCmdNode(name);
    if (node) {
        *reinterpret_cast<void**>(static_cast<char*>(node) + 0x08) = fn;
        return;
    }
    SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

void ConsolePrint(const char* fmt, ...) {
    if (!fmt)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Buddy_ClientPrintf(nullptr, 2, "%s", buf);
}

extern "C" void __cdecl gamepark_Status_f();

extern "C" void __cdecl gamepark_Status_f() {
    if (!Gamepark_IsEnabled()) {
        Buddy_DebugPrintf("gamepark is disabled (_sofbuddy_gamepark 0)\n");
        return;
    }
    ConsolePrint("[gamepark] %lld transfers, pending=%d\n", g_transfers,
                 g_pending.active ? 1 : 0);
    if (g_pending.active) {
        const DWORD age = GetTickCount() - g_pending.startMs;
        ConsolePrint("[gamepark] pending: slot %d (%s) -> %s\n", g_pending.slot0,
                     g_pending.name[0] ? g_pending.name : "?", g_pending.requested);
        ConsolePrint("[gamepark] pending age %us\n", static_cast<unsigned>(age / 1000u));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public seam (gamepark.h).
// ---------------------------------------------------------------------------
bool Gamepark_IsEnabled() {
    if (!g_cvEnabled)
        return true;
    return Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
}

void gamepark_OnGameDllLoaded(void* gameExport) {
    (void)gameExport;
    Gamepark_InitCvars();
    g_pending = Pending();
    RefreshOutputs();
    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        IsBadReadPtr(reinterpret_cast<void*>(detour_Cmd_AddCommand::oCmd_AddCommand), 1)) {
        PrintOut(PRINT_BAD, "[gamepark] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }
    InstallCommand("gamepark_status", reinterpret_cast<void*>(&gamepark_Status_f));
    PrintOut(PRINT_LOG, "[gamepark] ready\n");
}

void gamepark_SvFramePost(int msec) {
    (void)msec;
    if (!Gamepark_IsEnabled() || !g_pending.active)
        return;
    char cur[64] = {};
    CurrentMap(cur, sizeof(cur));
    const long curCount = ReadMapCount();
    const bool countKnown = g_pending.mapCountAtReq >= 0 && curCount >= 0;
    const bool changed = (countKnown && curCount != g_pending.mapCountAtReq) ||
                         (std::strcmp(cur, g_pending.mapAtReq) != 0);
    if (!changed) {
        if (GetTickCount() - g_pending.startMs > kPendingTimeoutMs) {
            Buddy_DebugPrintf("[gamepark] pending rcon map %s expired, dropping\n",
                              g_pending.requested);
            g_pending = Pending();
            RefreshOutputs();
        }
        return;
    }
    if (!gamepark::MapsEqualFold(g_pending.requested, cur)) {
        // The map went somewhere else (rotation, another admin, vote):
        // this intent did not cause it.
        Buddy_DebugPrintf("[gamepark] map went to %s (wanted %s), dropping pending\n", cur,
                          g_pending.requested);
        g_pending = Pending();
        RefreshOutputs();
        return;
    }
    DoTransfer();
}

bool Gamepark_HandleClientCommand(void* ent, int slot1, const char* cmd, int splitN,
                                  const char* const* words) {
    // Resolve the head word: say-box ".rcon map q2dm1" arrives split
    // (words[0]); direct console arrives as cmd.
    const char* head = nullptr;
    if (splitN >= 1 && words && words[0] && words[0][0])
        head = words[0];
    else if (cmd && cmd[0])
        head = cmd;
    else
        return false;
    if (!WordMatches(head, "rcon"))
        return false;
    if (!Gamepark_IsEnabled() || !Profiles_IsEnabled())
        return false;
    if (slot1 < 1 || slot1 > kMgMaxSlots || !ent)
        return false;  // unattributable: leave it entirely to sofplus
    // Subcommand + argument. Say form: words[1]/words[2]. Direct form:
    // MgTake resolved cmd with argv base 0 here, so argv(1)/(2) apply.
    const char* sub = nullptr;
    const char* arg = nullptr;
    if (splitN >= 1 && words) {
        sub = (splitN >= 2) ? words[1] : nullptr;
        arg = (splitN >= 3) ? words[2] : nullptr;
    } else {
        sub = Buddy_ClientArgv(1);
        arg = Buddy_ClientArgv(2);
        if (sub && !sub[0])
            sub = nullptr;
        if (arg && !arg[0])
            arg = nullptr;
    }
    if (!gamepark::IsMapChangeSubcommand(sub))
        return false;
    if (!arg)
        return false;  // usage error stays sofplus-side
    if (!Profiles_IsLevel01Admin(slot1 - 1))
        return false;  // not our principal: rcon auth stays sofplus-side
    // Engine net address, NOT userinfo: the "ip" userinfo key is empty on
    // this build. Same bytes sp_sv_info_client feeds inet_ntoa, so the
    // claim ip and rcon-session matching stay apples-to-apples.
    char ip[40] = {};
    if (!SlotBareIp(slot1, ip, sizeof(ip))) {
        Buddy_DebugPrintf("[gamepark] slot %d has no engine address, not recording\n",
                          slot1 - 1);
        return false;
    }
    const std::string cleanIp = gamepark::SanitizeIp(ip);
    if (cleanIp.empty())
        return false;
    const std::string mapArg = gamepark::SanitizeMapArg(arg);
    if (mapArg.empty())
        return false;
    char live[40] = {};
    SlotInfoValue(ent, "name", live, sizeof(live));
    // Record intent (latest wins) and let sofplus run the command: auth,
    // grants, and the map change itself stay sofplus-side. The transfer
    // fires only if this exact map loads (see gamepark_SvFramePost).
    g_pending.active = true;
    g_pending.slot0 = slot1 - 1;
    std::snprintf(g_pending.ip, sizeof(g_pending.ip), "%s", cleanIp.c_str());
    std::snprintf(g_pending.name, sizeof(g_pending.name), "%s", live);
    std::snprintf(g_pending.requested, sizeof(g_pending.requested), "%s", mapArg.c_str());
    CurrentMap(g_pending.mapAtReq, sizeof(g_pending.mapAtReq));
    g_pending.mapCountAtReq = ReadMapCount();
    g_pending.startMs = GetTickCount();
    RefreshOutputs();
    Buddy_DebugPrintf("[gamepark] slot %d (%s) rcon %s %s: transfer armed\n", g_pending.slot0,
                      g_pending.name[0] ? g_pending.name : "?", sub, mapArg.c_str());
    return false;  // never consumes: the .rcon command always runs in sofplus
}
