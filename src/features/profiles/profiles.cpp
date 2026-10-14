// profiles: player GUID carried in `team_red_blue` (roster-assigned).
//
// Native port of git-projects/sof-profiles (profiles.func + ext_trigger.func
// + userinfo_rcon.py + export-fire). The .func stack needed an out-of-game
// round-trip per userinfo change:
//
//   userinfo change -> ext_trigger file -> export-fire -> userinfo_rcon.py ->
//   rcon dumpuser -> snapshot_<slot>.cfg -> 200ms timer -> exec -> parse
//
// This feature reads the same `team_red_blue` userinfo key directly in the
// ClientUserinfoChanged / ClientBegin hooks (the engine hands us the
// userinfo string), keeps the same registry + per-slot guid, and
// pushes the same `team_red_blue <guid><bit>-blue|-red` fix back with
// Buddy_StuffText (the bit terminates the leading digit run so AssignTeam's
// atoi() reads it; the prefix text blue-<guid>-0 form would read as blue
// for everyone). No addons, no Python, no timers, no rcon password.
//
// Registry (disk): <user>/sofplus/data/profiles/registry.cfg
//   native "guid privilege nickname hide_tips=N" lines; legacy profiles.func
//   set "~reg_<guid>" "<nickname>" lines are accepted on load.
// Per-slot (memory): one guid, cleared on disconnect — see profiles_logic.h.

#include "cvar.h"
#include "profiles_logic.h"
#include "buddy_import.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

extern "C" BOOLEAN __stdcall SystemFunction036(void* RandomBuffer, unsigned long RandomBufferLength);

extern "C" HMODULE Buddy_GetGameDllHandle(void);

namespace {

// ---------------------------------------------------------------------------
// Engine layout (same verified constants as stufftext / ctf_spawn / clsv).
// ---------------------------------------------------------------------------
constexpr unsigned kEdictStride = 0x464;
constexpr unsigned kEdictClient = 0x74;
constexpr unsigned kEdictInuse = 0x78;
constexpr unsigned kClUserinfo = 0xCC;
constexpr unsigned kClTeam = 0x324;  // gclient resp.team: 0 NOTEAM, 1 TEAM1, 2 TEAM2
constexpr unsigned kCvarValueOfs = 0x18;
constexpr unsigned kRvaMaxclientsCvar = 0x15D9B4;
constexpr unsigned kRvaGEdicts = 0x15CCA0;

constexpr unsigned kExportClientConnectOfs = 0x24;
constexpr unsigned kExportClientBeginOfs = 0x28;
constexpr unsigned kExportClientUserinfoOfs = 0x2C;
constexpr unsigned kExportClientDisconnectOfs = 0x30;
constexpr unsigned kExportClientCommandOfs = 0x34;
constexpr unsigned kExportEdictsOfs = 0x60;
constexpr unsigned kExportEdictSizeOfs = 0x64;

constexpr unsigned kRvaCmdFunctions = 0x241840;
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFnOfs = 0x08;

// ---------------------------------------------------------------------------
// Safety helpers (same pattern as stufftext.cpp / clsv.cpp).
// ---------------------------------------------------------------------------
inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool IsSafeMemoryBlock(const void* ptr, size_t size) {
    if (!IsValidUserPointer(ptr) || size == 0)
        return false;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr + size < addr || addr + size > 0x7FFFFFFF)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + size <= regionEnd);
}

inline bool IsExecutableCodeAddress(const void* ptr) {
    if (!IsValidUserPointer(ptr))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

inline bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

HMODULE GameMod() {
    if (HMODULE shim = GetModuleHandleA("gamex86.dll")) {
        using handle_fn = HMODULE (*)();
        auto fn = reinterpret_cast<handle_fn>(GetProcAddress(shim, "Buddy_GetGameDllHandle"));
        if (fn) {
            if (HMODULE stock = fn())
                return stock;
        }
    }
    if (HMODULE h = GetModuleHandleA("oldgamex86.dll"))
        return h;
    return GetModuleHandleA("OLDGAMEX86.DLL");
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000000)
        return false;
    if (!IsSafeMemoryBlock(reinterpret_cast<const char*>(dos) + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    if (rva >= nt->OptionalHeader.SizeOfImage)
        return false;
    if (rva + size > nt->OptionalHeader.SizeOfImage || rva + size < rva)
        return false;
    return true;
}

int GameMaxClients() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaMaxclientsCvar, sizeof(void*)))
        return 0;
    void* cv = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaMaxclientsCvar);
    if (!IsSafeMemoryBlock(cv, kCvarValueOfs + sizeof(float)))
        return 0;
    int v = static_cast<int>(*reinterpret_cast<float*>(static_cast<char*>(cv) + kCvarValueOfs));
    if (v < 1)
        return 0;
    if (v > profiles::kMaxSlots)
        return profiles::kMaxSlots;
    return v;
}

char* GameEdicts() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaGEdicts, sizeof(void*)))
        return nullptr;
    void* edicts = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
    if (!IsValidUserPointer(edicts))
        return nullptr;
    return static_cast<char*>(edicts);
}

bool EdictInUse(const void* edict) {
    if (!IsSafeMemoryBlock(edict, kEdictInuse + sizeof(int)))
        return false;
    if (!*reinterpret_cast<const int*>(static_cast<const char*>(edict) + kEdictInuse))
        return false;
    if (!IsSafeMemoryBlock(static_cast<const char*>(edict) + kEdictClient, sizeof(void*)))
        return false;
    void* cl = *reinterpret_cast<void* const*>(static_cast<const char*>(edict) + kEdictClient);
    return IsValidUserPointer(cl);
}

void* ClientForEnt(void* ent) {
    if (!IsSafeMemoryBlock(ent, kEdictClient + sizeof(void*)))
        return nullptr;
    void* cl = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClient);
    if (!IsValidUserPointer(cl))
        return nullptr;
    return cl;
}

const char* UserinfoForEnt(void* ent) {
    void* cl = ClientForEnt(ent);
    if (!cl || !IsSafeMemoryBlock(static_cast<char*>(cl) + kClUserinfo, 64))
        return nullptr;
    const char* userinfo = static_cast<const char*>(cl) + kClUserinfo;
    if (!IsSafeMemoryBlock(userinfo, 1))
        return nullptr;
    return userinfo;
}

// Live server team: gclient resp.team (1=blue TEAM1, 2=red TEAM2) -> 0/1.
// Returns -1 when unreadable or NOTEAM.
int LiveTeamBit(void* ent) {
    void* cl = ClientForEnt(ent);
    if (!cl || !IsSafeMemoryBlock(static_cast<char*>(cl) + kClTeam, sizeof(int)))
        return -1;
    int t = *reinterpret_cast<int*>(static_cast<char*>(cl) + kClTeam);
    if (t == 1)
        return 0;
    if (t == 2)
        return 1;
    return -1;
}

int SlotForEdict(void* ent) {
    char* edicts = GameEdicts();
    if (!edicts)
        return -1;
    if (!ent)
        return -1;
    const auto diff = static_cast<char*>(ent) - edicts;
    if (diff > 0 && diff % static_cast<ptrdiff_t>(kEdictStride) == 0) {
        int slot1 = static_cast<int>(diff / static_cast<ptrdiff_t>(kEdictStride));
        int maxClients = GameMaxClients();
        if (slot1 >= 1 && maxClients > 0 && slot1 <= maxClients)
            return slot1 - 1;
    }
    return -1;
}

void* EdictForSlot(int slot0) {
    char* edicts = GameEdicts();
    int maxClients = GameMaxClients();
    if (!IsValidUserPointer(edicts) || maxClients < 1 || slot0 < 0 || slot0 >= maxClients)
        return nullptr;
    char* ent = edicts + static_cast<unsigned>(slot0 + 1) * kEdictStride;
    if (!EdictInUse(ent))
        return nullptr;
    return ent;
}

bool EngineCmdReady() {
    return detour_Cmd_Argc::oCmd_Argc && detour_Cmd_Argv::oCmd_Argv &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argc::oCmd_Argc)) &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argv::oCmd_Argv));
}

bool InfoKeyReady() {
    return detour_Info_ValueForKey::oInfo_ValueForKey &&
           IsExecutableCodeAddress(
               reinterpret_cast<const void*>(detour_Info_ValueForKey::oInfo_ValueForKey));
}

std::string UserinfoTeamRedBlue(const char* userinfo) {
    if (!userinfo || !InfoKeyReady())
        return std::string();
    const char* v = SOF_EP_Info_ValueForKey(userinfo, "team_red_blue");
    if (!v || !IsValidUserPointer(v) || !IsSafeMemoryBlock(v, 1))
        return std::string();
    // Bound the scan: userinfo values are short (MAX_INFO_VALUE 64).
    std::size_t len = 0;
    while (len < 128 && v[len])
        ++len;
    return std::string(v, len);
}

// ---------------------------------------------------------------------------
// Console command registration (repoint-safe, same as stufftext/clsv).
// ---------------------------------------------------------------------------
void** CmdFunctionsHead() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(node)) {
        const char* n = *reinterpret_cast<const char**>(static_cast<char*>(node) + kCmdNameOfs);
        if (n && strcmp(n, name) == 0)
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
    SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

// ---------------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------------
profiles::Registry g_registry;
profiles::SlotState g_slots[profiles::kMaxSlots];
long long g_errors = 0;

using clientconnect_fn = int(__cdecl*)(void*, char*);
using clientbegin_fn = void(__cdecl*)(void*);
using userinfochanged_fn = void(__cdecl*)(void*, char*, int);
using clientdisconnect_fn = void(__cdecl*)(void*);

clientconnect_fn g_origConnect = nullptr;
clientbegin_fn g_origBegin = nullptr;
userinfochanged_fn g_origUserinfo = nullptr;
clientdisconnect_fn g_origDisconnect = nullptr;
void* g_geConnectSlot = nullptr;
void* g_geBeginSlot = nullptr;
void* g_geUserinfoSlot = nullptr;
void* g_geDisconnectSlot = nullptr;
char* g_edictsCache = nullptr;
int g_edictSizeCache = 0;

void RefreshOutputs() {
    long long slots = 0;
    int maxClients = GameMaxClients();
    if (maxClients > 0) {
        for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
            if (g_slots[i].registered)
                ++slots;
        }
    }
    Profiles_SetOutputs(static_cast<long long>(g_registry.Size()), slots, g_errors);
}

void SyncLoadedProfiles() {
    for (int i = 0; i < profiles::kMaxSlots; ++i) {
        profiles::SlotState& st = g_slots[i];
        if (!st.registered || st.guid.empty())
            continue;
        int privilege = profiles::kUnspecifiedPrivilege;
        if (g_registry.LookupPrivilege(st.guid, &privilege))
            st.privilege = privilege;
        else {
            st.registered = false;
            st.nickname.clear();
            st.privilege = profiles::kUnspecifiedPrivilege;
        }
    }
}

void NoteError() {
    ++g_errors;
    RefreshOutputs();
}

bool SystemRandomBytes(unsigned char* out, std::size_t n);

// Mint a fresh 24-digit guid, retrying on (astronomically unlikely) registry
// collision. Returns "" after bounded retries.
std::string MintFreshGuid() {
    for (int attempt = 0; attempt < 32; ++attempt) {
        unsigned char bytes[profiles::kIdentityDigits];
        if (!SystemRandomBytes(bytes, sizeof(bytes))) {
            static bool seeded = false;
            if (!seeded) {
                std::srand(GetTickCount() ^ 0x9E3779B9u);
                seeded = true;
            }
            const DWORD tick = GetTickCount();
            for (std::size_t i = 0; i < sizeof(bytes); ++i)
                bytes[i] = static_cast<unsigned char>((std::rand() & 0xFF) ^ ((tick >> (i % 4)) & 0xFF));
        }
        std::size_t i = 0;
        std::string guid = profiles::MintIdentity([&]() -> unsigned {
            return bytes[i++ % sizeof(bytes)];
        });
        if (!profiles::GuidValid(guid))
            continue;
        if (g_registry.Lookup(guid, nullptr))
            continue;  // collision: retry
        return guid;
    }
    return std::string();
}

// Registry file: <user>/sofplus/data/profiles/registry.cfg (same location
// profiles.func used via sp_sc_cvar_save relative to sofplus/data/).
void RegistryFsPath(char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    const char* user = Buddy_FS_Userdir();
    if (!user || !user[0])
        user = "User";
    std::snprintf(out, cap, "%s/sofplus/data/profiles/registry.cfg", user);
}

bool Profiles_SaveRegistry() {
    char path[512];
    RegistryFsPath(path, sizeof(path));
    if (!path[0])
        return false;
    Buddy_FS_CreatePath(path);
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        Buddy_DebugPrintf("profiles: cannot open '%s' for save\n", path);
        return false;
    }
    std::string text = g_registry.Save();
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok)
        Buddy_DebugPrintf("profiles: save to '%s' failed\n", path);
    return ok;
}

void NoteLegacyRegistry() {
    int legacy = 0;
    for (const auto& kv : g_registry.GuidToNick()) {
        if (profiles::GuidDeprecated(kv.first))
            ++legacy;
    }
    if (legacy > 0)
        Buddy_DebugPrintf(
            "[profiles] registry holds %d deprecated 62-digit guid(s); they "
            "keep working, but new identities are 24-digit (profile_register)\n",
            legacy);
}

bool Profiles_LoadRegistry() {
    // Prefer the engine FS (searches user dir + paks), fall back to the
    // userdir path directly.
    void* buf = nullptr;
    int len = Buddy_FS_LoadFile("sofplus/data/profiles/registry.cfg", &buf, false);
    if (len >= 0 && buf) {
        std::string text(static_cast<const char*>(buf), static_cast<std::size_t>(len));
        Buddy_FS_FreeFile(buf);
        g_registry.Clear();
        g_registry.Load(text);
        SyncLoadedProfiles();
        RefreshOutputs();
        NoteLegacyRegistry();
        return true;
    }
    char path[512];
    RegistryFsPath(path, sizeof(path));
    FILE* f = path[0] ? std::fopen(path, "rb") : nullptr;
    if (!f)
        return false;
    std::string text;
    char chunk[4096];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        text.append(chunk, n);
    std::fclose(f);
    g_registry.Clear();
    g_registry.Load(text);
    SyncLoadedProfiles();
    RefreshOutputs();
    NoteLegacyRegistry();
    return true;
}

// OS random bytes via RtlGenRandom (advapi32, already linked). Resolved once
// via GetProcAddress so a missing export degrades to the rand() fallback
// instead of a load-time import failure under Wine.
bool SystemRandomBytes(unsigned char* out, std::size_t n) {
    if (!out || n == 0 || n > 256)
        return false;
    using RtlGenRandomFn = BOOLEAN(__stdcall*)(void*, unsigned long);
    static RtlGenRandomFn fn = nullptr;
    static bool lookedUp = false;
    if (!lookedUp) {
        lookedUp = true;
        if (HMODULE h = GetModuleHandleA("advapi32.dll"))
            fn = reinterpret_cast<RtlGenRandomFn>(GetProcAddress(h, "SystemFunction036"));
    }
    if (fn && fn(out, static_cast<unsigned long>(n)))
        return true;
    return false;
}

bool ParsePrivilege(const char* text, int* privilege) {
    if (!text || !privilege)
        return false;
    if (std::strcmp(text, "0") == 0)
        *privilege = profiles::kServerAdmin;
    else if (std::strcmp(text, "1") == 0)
        *privilege = profiles::kMapAdmin;
    else if (std::strcmp(text, "2") == 0)
        *privilege = profiles::kNormalUser;
    else
        return false;
    return true;
}

const char* PrivilegeText(int privilege) {
    if (privilege == profiles::kUnspecifiedPrivilege)
        return "none";
    return privilege == profiles::kServerAdmin ? "server" :
        privilege == profiles::kMapAdmin ? "map" : "user";
}

// Join argv[first..last-1] with single spaces (for nicknames with spaces).
void JoinArgvRange(int first, int last, char* out, int cap) {
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    if (!EngineCmdReady())
        return;
    for (int i = first; i < last; ++i) {
        const char* a = SOF_EP_Cmd_Argv(i);
        if (!a || !*a)
            continue;
        if (out[0]) {
            if (static_cast<int>(std::strlen(out) + 1) >= cap)
                return;
            std::strcat(out, " ");
        }
        std::size_t need = std::strlen(out) + std::strlen(a);
        if (need >= static_cast<std::size_t>(cap))
            return;
        std::strcat(out, a);
    }
}

// Join argv[first..argc-1] with single spaces.
void JoinArgvFrom(int first, char* out, int cap) {
    JoinArgvRange(first, EngineCmdReady() ? SOF_EP_Cmd_Argc() : first, out, cap);
}

// Console-directed print for admin command output. PrintOut routes through
// gi.dprintf (gated on developer 1 — invisible otherwise) and gi.bprintf
// would spam every player; gi.cprintf with NULL ent is the engine's
// server-console channel (stock precedent: g_cmds.cpp, dm_ctf.cpp),
// visible on console/rcon with no developer mode needed.
constexpr int kPrintConsoleHigh = 2;  // PRINT_HIGH (level is irrelevant for NULL ent)

void ConsolePrint(const char* fmt, ...) {
    if (!fmt)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Buddy_ClientPrintf(nullptr, kPrintConsoleHigh, "%s", buf);
}

// Push the slot guid + team to the client. Returns "pushed", "printed"
// (manual mode) or "failed" (no team). Mirrors fn_push_checked/fn_push_slot.
std::string PushChecked(int slot0, int team) {
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return "failed";
    if (team != 0 && team != 1)
        return "failed";
    profiles::SlotState& st = g_slots[slot0];
    if (st.guid.empty() || !profiles::GuidValid(st.guid))
        return "failed";
    void* ent = EdictForSlot(slot0);
    if (!ent)
        return "failed";
    std::string full = profiles::TeamRedBlueForClient(st.guid, team);
    st.team = team;
    st.registered = true;
    if (Profiles_UseStufftext()) {
        std::string cmd = "team_red_blue " + full + "\n";
        st.restored = full;
        st.carrierPending = true;
        if (!Buddy_StuffText(ent, cmd.c_str())) {
            st.restored.clear();
            st.carrierPending = false;
            return "failed";
        }
        ConsolePrint("[profiles] pushed %s to slot %d\n", full.c_str(), slot0);
        RefreshOutputs();
        return "pushed";
    }
    ConsolePrint("run: stufftext %d team_red_blue %s\n", slot0, full.c_str());
    RefreshOutputs();
    return "printed";
}

// Immediate restore on userinfo hook using live server team (no snapshot
// wait). Mirrors fn_try_restore_live + fn_restore_stash_push dedup.
bool TryRestoreLive(int slot0, const profiles::TrbParsed& cur) {
    if (!Profiles_IsEnabled() || slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    profiles::SlotState& st = g_slots[slot0];
    if (st.guid.empty())
        return false;
    if (st.intCvar) {
        void* ent = EdictForSlot(slot0);
        const int live = ent ? LiveTeamBit(ent) : -1;
        int team = profiles::IntCvarTeam(cur.raw, st.guid, st.team, live);
        if (profiles::PackNeedsLiveResync(cur.raw, st.guid, live))
            team = live;
        if (team != 0 && team != 1)
            return false;
        std::string pack = profiles::IntCvarPackDecimal(st.guid, team);
        if (profiles::RestoreTargetPresent(cur.raw, pack)) {
            st.restored = pack;
            st.carrierPending = false;
            return false;
        }
        if (st.carrierPending)
            st.carrierPending = false;
        std::string rc = PushChecked(slot0, team);
        return rc == "pushed" || rc == "printed";
    }
    int wrapBit = -1;
    if (profiles::WrapMatchesGuid(cur.raw, st.guid, &wrapBit)) {
        st.team = wrapBit;
        st.restored = profiles::BuildTeamRedBlue(st.guid, wrapBit);
        st.carrierPending = false;
        return false;
    }
    std::string nick;
    if (!g_registry.Lookup(st.guid, &nick))
        return false;
    // If the current userinfo already carries the slot guid, nothing
    // to do (also covers the post-push echo).
    if (cur.valid && cur.identity == st.guid) {
        st.restored = profiles::BuildTeamRedBlue(st.guid, cur.team);
        st.carrierPending = false;
        return false;
    }
    void* ent = EdictForSlot(slot0);
    if (!ent)
        return false;
    int team = profiles::ResolveRestoreTeam(LiveTeamBit(ent), cur.team, st.team);
    if (team != 0 && team != 1)
        return false;
    std::string full = profiles::BuildTeamRedBlue(st.guid, team);
    if (profiles::RestoreTargetPresent(cur.raw, full)) {
        st.restored = full;
        st.carrierPending = false;
        return false;
    }
    if (cur.valid) {
        // Current userinfo holds a *different* valid guid: only re-push
        // when the slot is registered. A guest guid is left alone.
        if (!st.registered)
            return false;
    }
    if (st.carrierPending)
        st.carrierPending = false;
    std::string rc = PushChecked(slot0, team);
    if (rc == "pushed")
        st.team = team;
    return rc == "pushed" || rc == "printed";
}

int FindSlotByGuid(const std::string& guid);

// Live display name for a slot: userinfo name, falling back to the given nick.
std::string SlotLiveName(int slot0, const std::string& fallback) {
    std::string live = fallback;
    void* ent = (slot0 >= 0 && slot0 < profiles::kMaxSlots) ? EdictForSlot(slot0) : nullptr;
    const char* ui = ent ? UserinfoForEnt(ent) : nullptr;
    if (ui && InfoKeyReady()) {
        const char* v = SOF_EP_Info_ValueForKey(ui, "name");
        if (v && IsValidUserPointer(v) && IsSafeMemoryBlock(v, 1)) {
            std::size_t len = 0;
            while (len < 64 && v[len])
                ++len;
            if (len > 0)
                live.assign(v, len);
        }
    }
    return live;
}

void RoleStyle(int privilege, const char** roleOut, const char** colorOut) {
    if (privilege == profiles::kServerAdmin) {
        *roleOut = "Admin";
        *colorOut = "\006";  // P_PURPLE reads magenta/pink in clients
    } else if (privilege == profiles::kMapAdmin) {
        *roleOut = "MapAdmin";
        *colorOut = "\007";  // P_CYAN
    } else {
        *roleOut = "Player";
        *colorOut = "\003";  // P_GREEN
    }
}

// Styled presence line, shared by sign-in and sign-out: body yellow
// throughout, only [Role] takes the privilege color. Yellow is re-asserted
// after each name so embedded name colors cannot bleed white into the rest
// of the line. No brackets around names — only the role tag is wrapped.
void AnnouncePresence(const std::string& live, const std::string& nick, int privilege,
                      bool signingIn) {
    if (nick.empty())
        return;
    const char* role = "Player";
    const char* roleColor = "\003";
    RoleStyle(privilege, &role, &roleColor);
    constexpr char kBody[] = "\004";   // P_YELLOW
    constexpr char kReset[] = "\001";  // P_WHITE
    Buddy_BroadcastPrintf(2, "%s%s%s signed %s as %s[%s]%s%s%s. %s!%s\n", kBody, live.c_str(),
                          kBody, signingIn ? "in" : "out", roleColor, role, kBody, nick.c_str(),
                          kBody, signingIn ? "Welcome" : "Goodbye", kReset);
}

// Sign-in detection runs as early as the handshake (userinfo updates stream
// in while connecting), but the welcome must wait until the player is fully
// in the world: announce now when begun, else remember it for ClientBegin.
void WelcomePlayerMaybe(int slot0);

// Server-wide welcome on a fresh sign-in (not on every userinfo sighting).
// Fires sp_sv_sound_broadcast 13 when the sofplus sound slot holds
// misc/talk.wav.
void WelcomePlayer(int slot0) {
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return;
    const profiles::SlotState& st = g_slots[slot0];
    if (st.nickname.empty())
        return;
    AnnouncePresence(SlotLiveName(slot0, st.nickname), st.nickname, st.privilege, true);
    void* cv = Buddy_GetEngineCvar("_sp_sv_sound_13", "", 0, nullptr);
    const char* configured = "";
    if (cv && Readable(static_cast<char*>(cv) + 0x04, sizeof(char*))) {
        char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + 0x04);
        if (s && IsValidUserPointer(s) && IsSafeMemoryBlock(s, 1))
            configured = s;
    }
    if (std::strcmp(configured, "misc/talk.wav") != 0)
        return;
    auto add = detour_Cbuf_AddText::oCbuf_AddText;
    if (!add)
        return;
    char cmd[] = "sp_sv_sound_broadcast 13\n";
    add(cmd);
}

void WelcomePlayerMaybe(int slot0) {
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return;
    profiles::SlotState& st = g_slots[slot0];
    if (st.begun) {
        WelcomePlayer(slot0);
        return;
    }
    st.welcomePending = true;
}

// Bind a valid, roster-known userinfo read to the slot. Mirrors
// fn_userinfo_read's known-guid branch + fn_bind_slot.
// A guid already on another connected slot is left where it is.
void BindSlot(int slot0, const profiles::TrbParsed& cur) {
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots || !cur.valid)
        return;
    std::string nick;
    if (!g_registry.Lookup(cur.identity, &nick))
        return;
    int owner = FindSlotByGuid(cur.identity);
    if (owner >= 0 && owner != slot0)
        return;
    profiles::SlotState& st = g_slots[slot0];
    // A team switch collapses the carrier and briefly unregisters the slot;
    // the echo must not look like a fresh sign-in. Only a new identity
    // announces.
    const bool isNewIdentity = st.guid != cur.identity;
    int privilege = profiles::kUnspecifiedPrivilege;
    g_registry.LookupPrivilege(cur.identity, &privilege);
    st.team = cur.team;
    st.nickname = nick;
    st.registered = true;
    st.privilege = privilege;
    st.offered = false;
    if (st.guid != cur.identity) {
        st.guid = cur.identity;
        st.restored.clear();  // new identity => new push target allowed
        if (profiles::GuidDeprecated(cur.identity))
            Buddy_DebugPrintf(
                "[profiles] slot %d signed in with deprecated 62-digit guid; "
                "it keeps working, but mint a fresh 24-digit one with "
                "profile_register <nickname> (then profile_del the old guid)\n",
                slot0);
    }
    if (isNewIdentity)
        WelcomePlayerMaybe(slot0);
}

// Packed CVAR_INT value, or the truncated full-guid push that reveals one.
// On a pack, cur becomes that guid and its low bit. Returns true either way.
bool NoteIntCvar(int slot0, profiles::TrbParsed& cur) {
    profiles::SlotState& st = g_slots[slot0];
    int bit = -1;
    if (!st.guid.empty() && profiles::IntCvarMatchesGuid(cur.raw, st.guid, &bit)) {
        st.intCvar = true;
        cur.identity = st.guid;
        cur.team = bit;
        cur.valid = true;
        std::string nick;
        if (g_registry.Lookup(st.guid, &nick))
            BindSlot(slot0, cur);
        return true;
    }
    if (!st.guid.empty() && profiles::IntCvarTruncation(cur.raw, st.guid, &bit)) {
        st.intCvar = true;
        return true;
    }
    if (!st.guid.empty())
        return false;
    std::uint32_t fold = 0;
    int team = -1;
    std::string found;
    if (profiles::IntCvarSplit(cur.raw, &fold, &team)) {
        for (const auto& kv : g_registry.GuidToNick()) {
            if (profiles::GuidFold23(kv.first) != fold)
                continue;
            if (!found.empty()) {
                found.clear();
                break;
            }
            found = kv.first;
        }
    }
    // Full-guid push collapsed by float32. The slot guid was cleared on
    // restart, so match the stored decimal to the one roster guid.
    if (found.empty() &&
        !profiles::MatchRosterTruncation(cur.raw, g_registry.GuidToNick(), &found, &team))
        return false;
    if (found.empty())
        return false;
    st.intCvar = true;
    cur.identity = found;
    cur.team = team;
    cur.valid = true;
    std::string nick;
    if (g_registry.Lookup(found, &nick))
        BindSlot(slot0, cur);
    return true;
}

// Full userinfo observation: called from Begin (reads live userinfo) and
// UserinfoChanged (uses the hook's userinfo* directly). Mirrors
// fn_userinfo_changed + fn_userinfo_read without the snapshot delay.
void ObserveUserinfo(int slot0, const char* userinfo, bool restoreAttempted = false) {
    if (!Profiles_IsEnabled() || slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return;
    std::string trb = userinfo ? UserinfoTeamRedBlue(userinfo) : std::string();
    profiles::TrbParsed cur = profiles::ParseTeamRedBlue(trb);
    profiles::SlotState& st = g_slots[slot0];
    if (st.carrierPending && profiles::RestoreTargetPresent(trb, st.restored))
        st.carrierPending = false;
    if (cur.valid) {
        std::string nick;
        if (g_registry.Lookup(cur.identity, &nick)) {
            BindSlot(slot0, cur);
        } else {
            // Valid guid, unknown to roster: guest. Remember it only when the
            // slot has no guid yet, so a roster identity already on the slot
            // is not replaced. Do not mark registered.
            st.team = cur.team;
            st.nickname.clear();
            st.registered = false;
            if (st.guid.empty())
                st.guid = cur.identity;
        }
    } else if (NoteIntCvar(slot0, cur)) {
        if (!restoreAttempted)
            TryRestoreLive(slot0, cur);
    } else {
        // Bare 0/1 or garbage: do not assign guid. Maybe re-push from it.
        if (cur.team == 0 || cur.team == 1)
            st.team = cur.team;
        st.registered = false;
        if (!restoreAttempted)
            TryRestoreLive(slot0, cur);
    }
    RefreshOutputs();
}

void ClearSlot(int slot0) {
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return;
    g_slots[slot0] = profiles::SlotState();
    RefreshOutputs();
}

int FindSlotByGuid(const std::string& guid) {
    if (guid.empty())
        return -1;
    int maxClients = GameMaxClients();
    for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
        if (g_slots[i].guid == guid && EdictForSlot(i))
            return i;
    }
    return -1;
}

// Resolve a roster reference: full guid, short login number, or nickname.
// errOut describes misses (a login number shared by several profiles must be
// replaced with the full guid).
bool ResolveRosterRef(const std::string& input, std::string* guidOut, std::string* nickOut,
                      std::string* errOut = nullptr) {
    auto fail = [&](const std::string& e) {
        if (errOut)
            *errOut = e;
        return false;
    };
    if (input.empty())
        return fail("missing target");
    if (profiles::GuidValid(input)) {
        std::string nick;
        if (!g_registry.Lookup(input, &nick))
            return fail("not in roster");
        if (guidOut)
            *guidOut = input;
        if (nickOut)
            *nickOut = nick;
        return true;
    }
    if (profiles::IsLoginNumber(input)) {
        std::string found, nick;
        if (!profiles::LookupGuidByFold(g_registry.GuidToNick(), input, &found))
            return fail("login number not in roster, or matches several (use full guid)");
        if (!g_registry.Lookup(found, &nick))
            return fail("not in roster");
        if (guidOut)
            *guidOut = found;
        if (nickOut)
            *nickOut = nick;
        return true;
    }
    std::string guid, nick;
    if (!g_registry.FindByNickname(input, &guid))
        return fail("not in roster");
    if (!g_registry.Lookup(guid, &nick))
        return fail("not in roster");
    if (guidOut)
        *guidOut = guid;
    if (nickOut)
        *nickOut = nick;
    return true;
}

// Small number for display; falls back to the full guid when unfodable.
std::string SmallGuid(const std::string& guid) {
    std::string num = profiles::GuidFoldDecimal(guid);
    return num.empty() ? guid : num;
}

// Resolve profile_signout target: slot number, roster guid, login number,
// or nickname.
int ResolveSignoutSlot(const std::string& target, std::string* errOut) {
    if (target.empty()) {
        if (errOut)
            *errOut = "missing target";
        return -1;
    }
    if (profiles::GuidValid(target)) {
        int slot = FindSlotByGuid(target);
        if (slot < 0 && errOut)
            *errOut = "guid not on any slot";
        return slot;
    }
    if (profiles::IsLoginNumber(target)) {
        std::string found;
        if (!profiles::LookupGuidByFold(g_registry.GuidToNick(), target, &found)) {
            if (errOut)
                *errOut = "login number not on any slot, or matches several (use full guid)";
            return -1;
        }
        int slot = FindSlotByGuid(found);
        if (slot < 0 && errOut)
            *errOut = "guid not on any slot";
        return slot;
    }
    char* end = nullptr;
    long slot = std::strtol(target.c_str(), &end, 10);
    if (end && *end == '\0') {
        int maxClients = GameMaxClients();
        if (slot == maxClients)
            slot = maxClients - 1;
        if (slot >= 0 && slot < maxClients) {
            if (!EdictForSlot(static_cast<int>(slot))) {
                if (errOut)
                    *errOut = "slot empty";
                return -1;
            }
            return static_cast<int>(slot);
        }
    }
    std::string guid;
    if (!g_registry.FindByNickname(target, &guid)) {
        if (errOut)
            *errOut = "not in roster";
        return -1;
    }
    int found = FindSlotByGuid(guid);
    if (found < 0 && errOut)
        *errOut = "nickname not on any slot";
    return found;
}

// Unbind a slot: clear the slot guid and collapse client team_red_blue.
bool SignoutSlot(int slot0) {
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    void* ent = EdictForSlot(slot0);
    if (!ent)
        return false;
    profiles::SlotState& st = g_slots[slot0];
    std::string wasGuid = st.guid;
    std::string wasNick = st.nickname;
    int wasPrivilege = st.privilege;
    // Only a registered slot was ever welcomed: guest-remembered guids go
    // quiet, so every Goodbye pairs with an earlier Welcome.
    bool wasSignedIn = st.registered && !wasGuid.empty();
    std::string wasLive = SlotLiveName(slot0, wasNick);
    // begun tracks the connection, not the binding: a mid-game re-signin
    // must still announce immediately (no new Begin is coming).
    const bool wasBegun = st.begun;
    int team = profiles::ResolveRestoreTeam(LiveTeamBit(ent), st.team, st.team);
    ClearSlot(slot0);
    g_slots[slot0].begun = wasBegun;
    if (team == 0 || team == 1) {
        if (Profiles_UseStufftext()) {
            char cmd[24];
            std::snprintf(cmd, sizeof(cmd), "team_red_blue %d\n", team);
            if (!Buddy_StuffText(ent, cmd))
                return false;
        } else {
            ConsolePrint("run: stufftext %d team_red_blue %d\n", slot0, team);
        }
    }
    if (!wasGuid.empty()) {
        ConsolePrint("[profiles] slot %d signed out guid %s\n", slot0, SmallGuid(wasGuid).c_str());
        if (wasSignedIn)
            AnnouncePresence(wasLive, wasNick, wasPrivilege, false);
    } else {
        ConsolePrint("[profiles] slot %d signed out\n", slot0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// game_export_t hooks (ge slot swap, same technique as minigames/clsv for
// ClientCommand). Only slots no other feature touches.
// ---------------------------------------------------------------------------
int __cdecl HkClientConnect(void* ent, char* userinfo) {
    int rc = 1;
    if (g_origConnect)
        rc = g_origConnect(ent, userinfo);
    // No state yet: edict may not be inuse. Observe on Begin instead.
    return rc;
}

void __cdecl HkClientBegin(void* ent) {
    if (g_origBegin)
        g_origBegin(ent);
    if (!Profiles_IsEnabled() || !ent)
        return;
    int slot = SlotForEdict(ent);
    if (slot < 0 || slot >= profiles::kMaxSlots)
        return;
    g_slots[slot].begun = true;
    const char* userinfo = UserinfoForEnt(ent);
    ObserveUserinfo(slot, userinfo);
    // Anyone who signed in while connecting gets welcomed now that they are
    // fully in the world.
    profiles::SlotState& st = g_slots[slot];
    if (st.welcomePending && st.registered && !st.guid.empty()) {
        st.welcomePending = false;
        WelcomePlayer(slot);
    }
}

// Truncation of <guid><bit>. Write the packed decimal (23 guid bits, team
// in bit 0) over it so this AssignTeam reads the team. The client is
// stuffed that same decimal afterwards.
void ApplyIntCvarBit(int slot, char* userinfo) {
    if (slot < 0 || !userinfo || g_slots[slot].guid.empty())
        return;
    std::string trb = UserinfoTeamRedBlue(userinfo);
    int bit = -1;
    if (profiles::IntCvarMatchesGuid(trb, g_slots[slot].guid, &bit)) {
        g_slots[slot].intCvar = true;
        return;
    }
    if (!profiles::IntCvarTruncation(trb, g_slots[slot].guid, &bit))
        return;
    g_slots[slot].intCvar = true;
    int team = g_slots[slot].team;
    if (team != 0 && team != 1)
        team = bit;
    if (team != 0 && team != 1)
        return;
    std::string pack = profiles::IntCvarPackDecimal(g_slots[slot].guid, team);
    const char* key = "\\team_red_blue\\";
    char* hit = std::strstr(userinfo, key);
    if (!hit)
        return;
    char* val = hit + std::strlen(key);
    char* end = std::strchr(val, '\\');
    if (!end)
        end = val + std::strlen(val);
    std::size_t tail = std::strlen(end);
    if (static_cast<std::size_t>(val - userinfo) + pack.size() + tail >= 512)
        return;
    std::memmove(val + pack.size(), end, tail + 1);
    std::memcpy(val, pack.data(), pack.size());
}

void __cdecl HkClientUserinfoChanged(void* ent, char* userinfo, int notFirst) {
    (void)notFirst;
    int slot = -1;
    if (Profiles_IsEnabled() && ent) {
        slot = SlotForEdict(ent);
        if (slot < 0) {
            // Fallback: scan for the edict (stale g_edicts cache across reloads).
            int maxClients = GameMaxClients();
            for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i)
                if (EdictForSlot(i) == ent) { slot = i; break; }
        }
        if (slot >= 0 && userinfo && IsValidUserPointer(userinfo) && IsSafeMemoryBlock(userinfo, 1))
            ApplyIntCvarBit(slot, userinfo);
    }
    if (g_origUserinfo)
        g_origUserinfo(ent, userinfo, notFirst);
    if (slot < 0)
        return;
    // Prefer the hook's userinfo* (authoritative, no struct read needed);
    // fall back to the live struct copy when the pointer is bad.
    const char* ui = nullptr;
    if (userinfo && IsValidUserPointer(userinfo) && IsSafeMemoryBlock(userinfo, 1))
        ui = userinfo;
    else
        ui = UserinfoForEnt(ent);
    // Immediate restore first (same order as fn_userinfo_changed), then the
    // audit/bind observation.
    profiles::SlotState& st = g_slots[slot];
    bool restoreAttempted = false;
    if (!st.guid.empty()) {
        std::string trbNow = ui ? UserinfoTeamRedBlue(ui) : std::string();
        profiles::TrbParsed curNow = profiles::ParseTeamRedBlue(trbNow);
        restoreAttempted = TryRestoreLive(slot, curNow);
    }
    ObserveUserinfo(slot, ui, restoreAttempted);
}

void __cdecl HkClientDisconnect(void* ent) {
    if (g_origDisconnect)
        g_origDisconnect(ent);
    if (!ent)
        return;
    int slot = SlotForEdict(ent);
    // Edict may already be freed: scan by pointer identity before giving up.
    if (slot < 0) {
        char* edicts = GameEdicts();
        if (edicts) {
            int maxClients = GameMaxClients();
            for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
                char* cand = edicts + static_cast<unsigned>(i + 1) * kEdictStride;
                if (cand == ent) {
                    slot = i;
                    break;
                }
            }
        }
    }
    if (slot >= 0)
        ClearSlot(slot);
}

bool HookSlot(void* gameExport, unsigned ofs, void* hk, void** origOut, void** slotOut,
              const char* name) {
    if (!gameExport || !hk || !origOut || !slotOut)
        return false;
    if (!Readable(static_cast<char*>(gameExport) + ofs, sizeof(void*))) {
        PrintOut(PRINT_BAD, "[profiles] %s hook skipped: unreadable ge table\n", name);
        return false;
    }
    void** slot = reinterpret_cast<void**>(static_cast<char*>(gameExport) + ofs);
    void* target = *slot;
    if (!target || !IsExecutableCodeAddress(target)) {
        PrintOut(PRINT_BAD, "[profiles] %s hook skipped: bad target\n", name);
        return false;
    }
    *origOut = target;
    *slotOut = slot;
    *slot = hk;
    return true;
}

void InstallGameHooks(void* gameExport) {
    if (!gameExport)
        return;
    if (!Readable(static_cast<char*>(gameExport) + kExportEdictsOfs, sizeof(void*)) ||
        !Readable(static_cast<char*>(gameExport) + kExportEdictSizeOfs, sizeof(int))) {
        PrintOut(PRINT_BAD, "[profiles] hooks skipped: unreadable ge table\n");
        return;
    }
    char* edicts = *reinterpret_cast<char**>(static_cast<char*>(gameExport) + kExportEdictsOfs);
    int edictSize =
        *reinterpret_cast<int*>(static_cast<char*>(gameExport) + kExportEdictSizeOfs);
    if (edicts && edictSize > 0) {
        g_edictsCache = edicts;
        g_edictSizeCache = edictSize;
    }
    int hooked = 0;
    if (!g_origConnect &&
        HookSlot(gameExport, kExportClientConnectOfs, reinterpret_cast<void*>(&HkClientConnect),
                 reinterpret_cast<void**>(&g_origConnect), &g_geConnectSlot, "ClientConnect"))
        ++hooked;
    if (!g_origBegin &&
        HookSlot(gameExport, kExportClientBeginOfs, reinterpret_cast<void*>(&HkClientBegin),
                 reinterpret_cast<void**>(&g_origBegin), &g_geBeginSlot, "ClientBegin"))
        ++hooked;
    if (!g_origUserinfo &&
        HookSlot(gameExport, kExportClientUserinfoOfs,
                 reinterpret_cast<void*>(&HkClientUserinfoChanged),
                 reinterpret_cast<void**>(&g_origUserinfo), &g_geUserinfoSlot,
                 "ClientUserinfoChanged"))
        ++hooked;
    if (!g_origDisconnect &&
        HookSlot(gameExport, kExportClientDisconnectOfs,
                 reinterpret_cast<void*>(&HkClientDisconnect),
                 reinterpret_cast<void**>(&g_origDisconnect), &g_geDisconnectSlot,
                 "ClientDisconnect"))
        ++hooked;
    PrintOut(PRINT_LOG, "[profiles] hooked %d Client slots (ge+0x24/0x28/0x2c/0x30)\n", hooked);
}

void RestoreGameHooks() {
    if (g_origConnect && g_geConnectSlot && Readable(g_geConnectSlot, sizeof(void*)))
        *reinterpret_cast<void**>(g_geConnectSlot) = reinterpret_cast<void*>(g_origConnect);
    if (g_origBegin && g_geBeginSlot && Readable(g_geBeginSlot, sizeof(void*)))
        *reinterpret_cast<void**>(g_geBeginSlot) = reinterpret_cast<void*>(g_origBegin);
    if (g_origUserinfo && g_geUserinfoSlot && Readable(g_geUserinfoSlot, sizeof(void*)))
        *reinterpret_cast<void**>(g_geUserinfoSlot) = reinterpret_cast<void*>(g_origUserinfo);
    if (g_origDisconnect && g_geDisconnectSlot && Readable(g_geDisconnectSlot, sizeof(void*)))
        *reinterpret_cast<void**>(g_geDisconnectSlot) = reinterpret_cast<void*>(g_origDisconnect);
    g_origConnect = nullptr;
    g_origBegin = nullptr;
    g_origUserinfo = nullptr;
    g_origDisconnect = nullptr;
    g_geConnectSlot = nullptr;
    g_geBeginSlot = nullptr;
    g_geUserinfoSlot = nullptr;
    g_geDisconnectSlot = nullptr;
}

// ---------------------------------------------------------------------------
// Server-console commands (profile_*). Engine xcommand_t: void (__cdecl*)(void).
extern "C" void __cdecl profile_Register_f();
extern "C" void __cdecl profile_Import_f();
extern "C" void __cdecl profile_SetPrivilege_f();
extern "C" void __cdecl profile_Del_f();
extern "C" void __cdecl profile_Query_f();
extern "C" void __cdecl profile_Signin_f();
extern "C" void __cdecl profile_Signout_f();
extern "C" void __cdecl profile_GetSlotById_f();
extern "C" void __cdecl profile_GetSlotByNick_f();
extern "C" void __cdecl profile_Save_f();
extern "C" void __cdecl profile_Load_f();

extern "C" void __cdecl profile_Register_f() {
    if (!Profiles_IsEnabled()) {
        Buddy_DebugPrintf("profiles is disabled (_sofbuddy_profiles 0)\n");
        NoteError();
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("profiles: engine arg accessors unavailable\n");
        NoteError();
        return;
    }
    int argc = SOF_EP_Cmd_Argc();
    if (argc < 2) {
        Buddy_DebugPrintf("usage: profile_register <nickname> (mints a fresh guid)\n");
        NoteError();
        return;
    }
    const char* a1 = SOF_EP_Cmd_Argv(1);
    if (!a1 || !IsValidUserPointer(a1) || !IsSafeMemoryBlock(a1, 1)) {
        NoteError();
        return;
    }
    char nick[256];
    JoinArgvRange(1, argc, nick, sizeof(nick));
    if (!nick[0]) {
        Buddy_DebugPrintf("profile_register: nickname is required\n");
        NoteError();
        return;
    }
    if (profiles::GuidValid(a1) || profiles::GuidValid(nick)) {
        Buddy_DebugPrintf("profile_register: to keep an existing guid, use profile_import <guid24> <nickname>\n");
        NoteError();
        return;
    }
    std::string guid = MintFreshGuid();
    if (guid.empty()) {
        Buddy_DebugPrintf("profile_register: mint failed, retry\n");
        NoteError();
        return;
    }
    std::string err;
    if (!g_registry.Add(guid, nick, &err)) {
        Buddy_DebugPrintf("profile_register: %s\n", err.c_str());
        NoteError();
        return;
    }
    Profiles_SaveRegistry();
    RefreshOutputs();
    PrintOut(PRINT_LOG, "[profiles] registry guid %s privilege user nickname %s (minted)\n",
             SmallGuid(guid).c_str(), nick);
}

extern "C" void __cdecl profile_Import_f() {
    if (!Profiles_IsEnabled()) {
        Buddy_DebugPrintf("profiles is disabled (_sofbuddy_profiles 0)\n");
        NoteError();
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("profiles: engine arg accessors unavailable\n");
        NoteError();
        return;
    }
    int argc = SOF_EP_Cmd_Argc();
    if (argc < 3) {
        Buddy_DebugPrintf("usage: profile_import <guid24> <nickname>\n");
        NoteError();
        return;
    }
    const char* a1 = SOF_EP_Cmd_Argv(1);
    if (!a1 || !IsValidUserPointer(a1) || !IsSafeMemoryBlock(a1, 1)) {
        NoteError();
        return;
    }
    char nick[256];
    JoinArgvRange(2, argc, nick, sizeof(nick));
    if (!profiles::GuidValid(a1) || !nick[0]) {
        Buddy_DebugPrintf("usage: profile_import <guid24> <nickname>\n");
        NoteError();
        return;
    }
    std::string err;
    if (!g_registry.Add(a1, nick, &err)) {
        Buddy_DebugPrintf("profile_import: %s\n", err.c_str());
        NoteError();
        return;
    }
    Profiles_SaveRegistry();
    RefreshOutputs();
    int privilege = profiles::kNormalUser;
    g_registry.LookupPrivilege(a1, &privilege);
    PrintOut(PRINT_LOG, "[profiles] registry guid %s privilege %s nickname %s (imported)\n",
             SmallGuid(a1).c_str(), PrivilegeText(privilege), nick);
    if (profiles::GuidDeprecated(a1))
        PrintOut(PRINT_LOG,
                 "[profiles] imported guid is a deprecated 62-digit one (kept "
                 "working); prefer profile_register for a fresh 24-digit guid\n");
}

extern "C" void __cdecl profile_SetPrivilege_f() {
    if (!Profiles_IsEnabled()) {
        Buddy_DebugPrintf("profiles is disabled (_sofbuddy_profiles 0)\n");
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("profiles: engine arg accessors unavailable\n");
        return;
    }
    const int argc = SOF_EP_Cmd_Argc();
    int privilege = profiles::kUnspecifiedPrivilege;
    if (argc < 3 || !ParsePrivilege(SOF_EP_Cmd_Argv(argc - 1), &privilege)) {
        Buddy_DebugPrintf("usage: profile_set_privilege <guid/number/nickname> <0|1|2>\n");
        return;
    }
    char target[256];
    JoinArgvRange(1, argc - 1, target, sizeof(target));
    std::string guid, nick, err;
    if (!ResolveRosterRef(target, &guid, &nick, &err)) {
        Buddy_DebugPrintf("profile_set_privilege: '%s' %s\n", target, err.c_str());
        return;
    }
    if (!g_registry.SetPrivilege(guid, privilege, &err)) {
        Buddy_DebugPrintf("profile_set_privilege: %s\n", err.c_str());
        return;
    }
    for (int i = 0; i < profiles::kMaxSlots; ++i) {
        if (g_slots[i].guid == guid)
            g_slots[i].privilege = privilege;
    }
    Profiles_SaveRegistry();
    PrintOut(PRINT_LOG, "[profiles] %s privilege %s nickname %s\n", SmallGuid(guid).c_str(),
             PrivilegeText(privilege), nick.c_str());
}

extern "C" void __cdecl profile_Del_f() {
    if (!Profiles_IsEnabled()) {
        Buddy_DebugPrintf("profiles is disabled (_sofbuddy_profiles 0)\n");
        NoteError();
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("profiles: engine arg accessors unavailable\n");
        NoteError();
        return;
    }
    int argc = SOF_EP_Cmd_Argc();
    if (argc < 2) {
        Buddy_DebugPrintf("usage: profile_del <guid24>/<number>/<nickname>\n");
        NoteError();
        return;
    }
    // Accept: del <guid> | del <guid> "" | del "" <nick> | del <nick...>.
    // When two tokens are given, prefer guid when the first validates.
    std::string a1 = SOF_EP_Cmd_Argv(1);
    char rest[256];
    JoinArgvFrom(2, rest, sizeof(rest));
    std::string guid, nick;
    if (profiles::GuidValid(a1)) {
        guid = a1;
    } else if (profiles::IsLoginNumber(a1)) {
        std::string found;
        if (!profiles::LookupGuidByFold(g_registry.GuidToNick(), a1, &found)) {
            Buddy_DebugPrintf("profile_del: login number not in roster, or matches several "
                              "(use full guid)\n");
            NoteError();
            return;
        }
        guid = found;
    } else if (!a1.empty() && (a1 != "\"\"" && a1 != "''")) {
        // First token is not a guid: treat whole tail as nickname.
        char all[256];
        JoinArgvFrom(1, all, sizeof(all));
        nick = all;
    }
    if (!rest[0]) {
        // Single-arg form already classified above.
    } else if (!guid.empty()) {
        // del <guid> <maybe-nick>: guid wins (mirrors .func guid branch).
    } else if (nick.empty()) {
        nick = rest;
    }
    if (guid.empty() && nick.empty()) {
        Buddy_DebugPrintf("usage: profile_del <guid24>/<number>/<nickname>\n");
        NoteError();
        return;
    }
    std::string err;
    // Single nickname arg that cleans to a guid lookup miss reports cleanly.
    if (guid.empty() && !nick.empty()) {
        std::string g;
        if (!g_registry.FindByNickname(nick, &g)) {
            // Try raw guid (e.g. quoted "") fallthrough already handled; this
            // is a genuine miss.
            if (!g_registry.Remove("", nick, &err)) {
                Buddy_DebugPrintf("profile_del: %s\n", err.c_str());
                NoteError();
                return;
            }
            Profiles_SaveRegistry();
            RefreshOutputs();
            PrintOut(PRINT_LOG, "[profiles] registry removed nickname %s\n", nick.c_str());
            return;
        }
        guid = g;
        nick.clear();
    }
    if (!g_registry.Remove(guid, nick, &err)) {
        Buddy_DebugPrintf("profile_del: %s\n", err.c_str());
        NoteError();
        return;
    }
    Profiles_SaveRegistry();
    RefreshOutputs();
    PrintOut(PRINT_LOG, "[profiles] registry removed id %s\n", SmallGuid(guid).c_str());
}

enum class SigninBindResult { kOk, kTaken, kNoTeam, kPushFailed };

// Shared bind tail for server-console signin and player `.login`: attach the
// roster entry to the slot and push its carrier. slot is 0-based.
SigninBindResult BindSlotToRoster(void* ent, int slot, const std::string& guid,
                                  const std::string& nick) {
    if (!ent || slot < 0 || slot >= profiles::kMaxSlots)
        return SigninBindResult::kPushFailed;
    int owner = FindSlotByGuid(guid);
    if (owner >= 0 && owner != slot)
        return SigninBindResult::kTaken;
    profiles::SlotState& st = g_slots[slot];
    const bool isNewIdentity = st.guid != guid;
    st.guid = guid;
    st.nickname = nick;
    g_registry.LookupPrivilege(guid, &st.privilege);
    st.restored.clear();
    st.carrierPending = false;
    const char* ui = UserinfoForEnt(ent);
    std::string currentTrb = ui ? UserinfoTeamRedBlue(ui) : std::string();
    int currentBit = -1;
    if (profiles::IntCvarMatchesGuid(currentTrb, st.guid, &currentBit) ||
        profiles::IntCvarTruncation(currentTrb, st.guid, &currentBit))
        st.intCvar = true;
    int team = LiveTeamBit(ent);
    if (team != 0 && team != 1)
        team = st.team;
    if (team != 0 && team != 1) {
        profiles::TrbParsed cur = profiles::ParseTeamRedBlue(ui ? UserinfoTeamRedBlue(ui) : "");
        team = cur.team;
    }
    if (team != 0 && team != 1)
        return SigninBindResult::kNoTeam;
    if (PushChecked(slot, team) == "failed")
        return SigninBindResult::kPushFailed;
    if (isNewIdentity)
        WelcomePlayerMaybe(slot);
    return SigninBindResult::kOk;
}

// Tell the player on ent their login number and how to reuse it. Console
// print (not centerprint): it stays in their console to copy from. The
// number is the guid fold without the team bit: fewer characters to store
// and type than the full guid. Pink (P_PURPLE) with a console: prefix so it
// reads as an official server notice.
void TellPlayerGuid(void* ent, const std::string& guid) {
    if (!ent || guid.empty())
        return;
    const std::string num = profiles::GuidFoldDecimal(guid);
    if (num.empty())
        return;
    constexpr char kLoginPink[] = "\006";
    Buddy_ClientPrintf(ent, kPrintConsoleHigh,
                       "%sconsole: Your login number is %s - write it down: .login %s to sign "
                       "yourself in.\n",
                       kLoginPink, num.c_str(), num.c_str());
}

extern "C" void __cdecl profile_Signin_f() {
    if (!Profiles_IsEnabled()) {
        Buddy_DebugPrintf("profiles is disabled (_sofbuddy_profiles 0)\n");
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("profiles: engine arg accessors unavailable\n");
        return;
    }
    int argc = SOF_EP_Cmd_Argc();
    if (argc < 2) {
        Buddy_DebugPrintf("usage: profile_signin <slot>\n");
        Buddy_DebugPrintf("       profile_signin <slot> <guid/number/nickname> (push a roster entry)\n");
        return;
    }
    char* end = nullptr;
    long slot = std::strtol(SOF_EP_Cmd_Argv(1), &end, 10);
    if (!end || *end != '\0') {
        Buddy_DebugPrintf("usage: profile_signin <slot>\n");
        Buddy_DebugPrintf("       profile_signin <slot> <guid/number/nickname> (push a roster entry)\n");
        return;
    }
    int maxClients = GameMaxClients();
    if (slot == maxClients)
        slot = maxClients - 1;
    if (slot < 0 || slot >= maxClients) {
        Buddy_DebugPrintf("profile_signin: slot out of range (0-%d)\n", maxClients - 1);
        return;
    }
    void* ent = EdictForSlot(static_cast<int>(slot));
    if (!ent) {
        Buddy_DebugPrintf("profile_signin: slot empty\n");
        return;
    }
    char idArg[256];
    JoinArgvFrom(2, idArg, sizeof(idArg));
    if (!idArg[0]) {
        // Auto variation: re-read whatever guid the client's userinfo carries
        // and bind it if roster-known. Credential handoff only on a fresh
        // transition, like the welcome broadcast.
        profiles::SlotState& before = g_slots[slot];
        const std::string wasGuid = before.guid;
        ObserveUserinfo(static_cast<int>(slot), UserinfoForEnt(ent));
        profiles::SlotState& st = g_slots[slot];
        Buddy_DebugPrintf("[profiles] slot %d: guid=%s privilege=%s team=%d registered=%d\n",
                         static_cast<int>(slot), SmallGuid(st.guid).c_str(), PrivilegeText(st.privilege), st.team,
                         st.registered ? 1 : 0);
        if (st.registered && !st.guid.empty() && wasGuid != st.guid)
            TellPlayerGuid(ent, st.guid);
        return;
    }
    // Explicit variation: push a roster entry to the slot. Accepts a guid,
    // a short login number, or a nickname (so no guid copy-paste after
    // profile_register).
    std::string guid, nick, resolveErr;
    if (!ResolveRosterRef(idArg, &guid, &nick, &resolveErr)) {
        Buddy_DebugPrintf("profile_signin: '%s' %s (profile_register to mint, profile_import to keep)\n",
                          idArg, resolveErr.c_str());
        NoteError();
        return;
    }
    if (profiles::GuidDeprecated(guid))
        Buddy_DebugPrintf(
            "[profiles] profile_signin: 62-digit guids are deprecated (kept "
            "working); prefer profile_register for a fresh 24-digit guid\n");
    const profiles::SlotState& before = g_slots[slot];
    const bool freshSignin = before.guid != guid;
    switch (BindSlotToRoster(ent, static_cast<int>(slot), guid, nick)) {
        case SigninBindResult::kTaken: {
            int owner = FindSlotByGuid(guid);
            ConsolePrint("profile_signin: %s already on slot %d\n", nick.c_str(), owner);
            NoteError();
            return;
        }
        case SigninBindResult::kNoTeam:
            Buddy_DebugPrintf("profile_signin: no team data yet\n");
            NoteError();
            return;
        case SigninBindResult::kPushFailed:
            Buddy_DebugPrintf("profile_signin: push failed\n");
            NoteError();
            return;
        case SigninBindResult::kOk:
            break;
    }
    if (freshSignin)
        TellPlayerGuid(ent, guid);
}

extern "C" void __cdecl profile_Signout_f() {
    if (!Profiles_IsEnabled()) {
        Buddy_DebugPrintf("profiles is disabled (_sofbuddy_profiles 0)\n");
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("profiles: engine arg accessors unavailable\n");
        return;
    }
    if (SOF_EP_Cmd_Argc() < 2) {
        Buddy_DebugPrintf("usage: profile_signout <slot>/<guid>/<number>/<nickname>|all\n");
        return;
    }
    char target[256];
    JoinArgvFrom(1, target, sizeof(target));
    std::string lower = target;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "all") {
        int maxClients = GameMaxClients();
        int cleared = 0;
        std::string who;
        for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
            if (g_slots[i].guid.empty() || !EdictForSlot(i))
                continue;
            std::string tag = g_slots[i].nickname.empty()
                                  ? SmallGuid(g_slots[i].guid)
                                  : g_slots[i].nickname;
            if (SignoutSlot(i)) {
                ++cleared;
                if (!who.empty())
                    who += ", ";
                char item[96];
                std::snprintf(item, sizeof(item), "%d:%s", i, tag.c_str());
                who += item;
            }
        }
        Buddy_DebugPrintf("profile_signout: signed out %d slot(s)%s%s\n", cleared,
                          who.empty() ? "" : ": ", who.c_str());
        return;
    }
    std::string err;
    int slot = ResolveSignoutSlot(target, &err);
    if (slot < 0) {
        Buddy_DebugPrintf("profile_signout: %s\n", err.empty() ? "not found" : err.c_str());
        NoteError();
        return;
    }
    if (!SignoutSlot(slot)) {
        Buddy_DebugPrintf("profile_signout: failed on slot %d\n", slot);
        NoteError();
    }
}

void PublishFoundSlot(int found) {
    // Canonical internal output (_sb_internal_ by policy) + legacy aliases so
    // old configs/scripts keep working.
    void* cv = Buddy_GetEngineCvar("_sb_internal_profile_found_slot", "-1", 0, nullptr);
    if (cv)
        Buddy_SetEngineCvarValue("_sb_internal_profile_found_slot", static_cast<float>(found));
    cv = Buddy_GetEngineCvar("_profile_found_slot", "-1", 0, nullptr);
    if (cv)
        Buddy_SetEngineCvarValue("_profile_found_slot", static_cast<float>(found));
    cv = Buddy_GetEngineCvar("_prof_found_slot", "-1", 0, nullptr);
    if (cv)
        Buddy_SetEngineCvarValue("_prof_found_slot", static_cast<float>(found));
}

extern "C" void __cdecl profile_GetSlotById_f() {
    if (!EngineCmdReady())
        return;
    int argc = SOF_EP_Cmd_Argc();
    if (argc != 2) {
        Buddy_DebugPrintf("usage: profile_get_slot_by_id <guid/number>\n");
        return;
    }
    const char* id = SOF_EP_Cmd_Argv(1);
    int found = -1;
    if (id && *id) {
        std::string guid = id;
        if (!profiles::GuidValid(guid) && profiles::IsLoginNumber(guid)) {
            std::string resolved;
            if (profiles::LookupGuidByFold(g_registry.GuidToNick(), guid, &resolved))
                guid = resolved;
        }
        int maxClients = GameMaxClients();
        for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
            if (g_slots[i].guid == guid) {
                found = i;
                break;
            }
        }
    }
    PublishFoundSlot(found);
    Buddy_DebugPrintf("[profiles] profile_get_slot_by_id %s -> %d\n", id ? id : "", found);
}

extern "C" void __cdecl profile_GetSlotByNick_f() {
    if (!EngineCmdReady())
        return;
    int argc = SOF_EP_Cmd_Argc();
    if (argc < 2) {
        Buddy_DebugPrintf("usage: profile_get_slot_by_nick <nickname>\n");
        return;
    }
    char nick[256];
    JoinArgvFrom(1, nick, sizeof(nick));
    int found = -1;
    if (nick[0]) {
        std::string guid;
        if (g_registry.FindByNickname(nick, &guid)) {
            int maxClients = GameMaxClients();
            for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
                if (g_slots[i].guid == guid) {
                    found = i;
                    break;
                }
            }
        }
    }
    PublishFoundSlot(found);
    Buddy_DebugPrintf("[profiles] profile_get_slot_by_nick %s -> %d\n", nick, found);
}

extern "C" void __cdecl profile_Save_f() {
    if (Profiles_SaveRegistry())
        PrintOut(PRINT_LOG, "[profiles] registry saved\n");
    else {
        Buddy_DebugPrintf("profiles: save failed\n");
        NoteError();
    }
}

extern "C" void __cdecl profile_Load_f() {
    if (Profiles_LoadRegistry())
        PrintOut(PRINT_LOG, "[profiles] registry loaded (%u entries)\n",
                 static_cast<unsigned>(g_registry.Size()));
    else
        Buddy_DebugPrintf("profiles: no registry file yet\n");
    RefreshOutputs();
}

extern "C" void __cdecl profile_Query_f() {
    if (!Profiles_IsEnabled()) {
        ConsolePrint("profiles is disabled (_sofbuddy_profiles 0)\n");
        return;
    }
    int maxClients = GameMaxClients();
    if (maxClients < 1) {
        ConsolePrint("profiles: server not ready\n");
        return;
    }
    int nOk = 0, nBad = 0, nFix = 0, nManual = 0, nGuest = 0, nPending = 0;
    for (int i = 0; i < maxClients && i < profiles::kMaxSlots; ++i) {
        void* ent = EdictForSlot(i);
        if (!ent)
            continue;
        const char* ui = UserinfoForEnt(ent);
        profiles::TrbParsed cur =
            profiles::ParseTeamRedBlue(ui ? UserinfoTeamRedBlue(ui) : std::string());
        if (!cur.valid)
            NoteIntCvar(i, cur);
        if (cur.valid && g_slots[i].intCvar) {
            int live = ent ? LiveTeamBit(ent) : -1;
            if (live != cur.team)
                cur.valid = false;
        }
        std::string audit = "guest";
        std::string auditNick;
        if (cur.valid) {
            if (g_registry.Lookup(cur.identity, &auditNick)) {
                int owner = FindSlotByGuid(cur.identity);
                if (owner >= 0 && owner != i) {
                    ConsolePrint("slot %d rejected: %s already on slot %d\n", i,
                                 auditNick.c_str(), owner);
                    ++nGuest;
                    continue;
                }
                audit = "ok";
                BindSlot(i, cur);
            } else {
                audit = "guest";
            }
        } else {
            profiles::SlotState& st = g_slots[i];
            if (!st.guid.empty() && g_registry.Lookup(st.guid, &auditNick))
                audit = "wrong";
            else
                audit = "guest";
        }
        const char* cvarInt = g_slots[i].intCvar ? " CVAR_INT" : "";
        if (audit == "ok") {
            g_slots[i].registered = true;
            int privilege = profiles::kUnspecifiedPrivilege;
            g_registry.LookupPrivilege(cur.identity, &privilege);
            g_slots[i].privilege = privilege;
            ConsolePrint("[profiles] slot %d ok%s privilege %s guid %s nickname %s\n", i, cvarInt,
                         PrivilegeText(privilege), SmallGuid(cur.identity).c_str(), auditNick.c_str());
            ++nOk;
        } else if (audit == "wrong") {
            // nBad counts EVERY wrong slot; nFix/nManual/nPending partition
            // it — each wrong slot ends in exactly one of those outcomes.
            ++nBad;
            profiles::SlotState& st = g_slots[i];
            std::string trbNow = ui ? UserinfoTeamRedBlue(ui) : std::string();
            ConsolePrint("slot %d wrong%s: %s guid %s\n", i, cvarInt, trbNow.c_str(),
                           SmallGuid(st.guid).c_str());
            void* e2 = EdictForSlot(i);
            int team = st.intCvar
                ? profiles::IntCvarTeam(trbNow, st.guid, st.team, e2 ? LiveTeamBit(e2) : -1)
                : profiles::ResolveRestoreTeam(e2 ? LiveTeamBit(e2) : -1, cur.team, st.team);
            if (team != 0 && team != 1) {
                ++nPending;
                continue;
            }
            std::string rc = PushChecked(i, team);
            if (rc == "pushed")
                ++nFix;
            else if (rc == "printed")
                ++nManual;
            else
                ++nPending;
        } else {
            std::string trbNow = ui ? UserinfoTeamRedBlue(ui) : std::string();
            ConsolePrint("slot %d guest%s: %s\n", i, cvarInt, trbNow.c_str());
            ++nGuest;
        }
    }
    RefreshOutputs();
    ConsolePrint("[profiles] query: %d ok, %d wrong, %d pushed, %d manual, %d guests, %d pending\n",
                 nOk, nBad, nFix, nManual, nGuest, nPending);
}

}  // namespace

bool Profiles_ClientLogin(void* ent, int slot1, const char* guidArg) {
    if (!Profiles_IsEnabled())
        return false;
    if (!ent || slot1 < 1 || slot1 > profiles::kMaxSlots) {
        if (ent)
            Buddy_ClientPrintf(ent, kPrintConsoleHigh, ".login: cannot resolve player slot\n");
        return true;
    }
    std::string arg = guidArg ? guidArg : "";
    while (!arg.empty() &&
           std::isspace(static_cast<unsigned char>(arg.front())))
        arg.erase(arg.begin());
    while (!arg.empty() &&
           std::isspace(static_cast<unsigned char>(arg.back())))
        arg.pop_back();
    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"')
        arg = arg.substr(1, arg.size() - 2);
    std::string guid, nick;
    if (profiles::GuidValid(arg)) {
        guid = arg;
        if (!g_registry.Lookup(guid, &nick)) {
            Buddy_ClientPrintf(ent, kPrintConsoleHigh,
                               ".login: guid not in roster (ask an admin to register you)\n");
            return true;
        }
    } else if (!profiles::IsLoginNumber(arg)) {
        Buddy_ClientPrintf(ent, kPrintConsoleHigh, "usage: .login <login number or guid>\n");
        return true;
    } else if (!profiles::LookupGuidByFold(g_registry.GuidToNick(), arg, &guid) ||
               !g_registry.Lookup(guid, &nick)) {
        Buddy_ClientPrintf(ent, kPrintConsoleHigh,
                           ".login: login number not in roster, or matches several profiles "
                           "(use your full guid instead)\n");
        return true;
    }
    switch (BindSlotToRoster(ent, slot1 - 1, guid, nick)) {
        case SigninBindResult::kTaken: {
            int owner = FindSlotByGuid(guid);
            Buddy_ClientPrintf(ent, kPrintConsoleHigh, ".login: %s already on slot %d\n",
                               nick.c_str(), owner);
            return true;
        }
        case SigninBindResult::kNoTeam:
            Buddy_ClientPrintf(ent, kPrintConsoleHigh, ".login: no team data yet\n");
            return true;
        case SigninBindResult::kPushFailed:
            Buddy_ClientPrintf(ent, kPrintConsoleHigh, ".login: push failed\n");
            return true;
        case SigninBindResult::kOk:
            break;
    }
    // No success print here: the broadcast welcome already confirms the
    // sign-in, and a second line reads as a duplicate.
    return true;
}

bool Profiles_ClientLogout(void* ent, int slot1) {
    if (!Profiles_IsEnabled())
        return false;
    if (!ent || slot1 < 1 || slot1 > profiles::kMaxSlots) {
        if (ent)
            Buddy_ClientPrintf(ent, kPrintConsoleHigh, ".logout: cannot resolve player slot\n");
        return true;
    }
    const int slot0 = slot1 - 1;
    if (g_slots[slot0].guid.empty()) {
        Buddy_ClientPrintf(ent, kPrintConsoleHigh, ".logout: not signed in\n");
        return true;
    }
    // SignoutSlot clears the binding, collapses the carrier, and broadcasts
    // the goodbye; no extra line here, for the same no-duplicate reason.
    SignoutSlot(slot0);
    return true;
}

void profiles_AssignTeam(void* thisp, void* ent, char* userinfo,
                         detour_orig_AssignTeam::torig_AssignTeam original) {
    if (Profiles_IsEnabled() && userinfo && InfoKeyReady()) {
        std::string trb = UserinfoTeamRedBlue(userinfo);
        int slot = ent ? SlotForEdict(ent) : -1;
        if (slot >= 0 && slot < profiles::kMaxSlots) {
            profiles::SlotState& st = g_slots[slot];
            int desiredTeam = st.team;
            int parsedTeam = -1;
            if (st.intCvar)
                profiles::IntCvarMatchesGuid(trb, st.guid, &parsedTeam);
            else {
                profiles::TrbParsed parsed = profiles::ParseTeamRedBlue(trb);
                if (parsed.valid)
                    parsedTeam = parsed.team;
            }
            if (parsedTeam == 0 || parsedTeam == 1)
                desiredTeam = parsedTeam;
            bool carrierTarget = profiles::RestoreTargetPresent(trb, st.restored);
            if (!carrierTarget && st.intCvar)
                carrierTarget = profiles::IntCvarMatchesGuid(trb, st.guid, &parsedTeam);
            if (!carrierTarget) {
                profiles::TrbParsed parsed = profiles::ParseTeamRedBlue(trb);
                carrierTarget = parsed.valid && parsed.identity == st.guid;
                if (carrierTarget)
                    parsedTeam = parsed.team;
            }
            if (parsedTeam == 0 || parsedTeam == 1)
                desiredTeam = parsedTeam;
            // The carrier echo is not a team-selection request. Avoid
            // AssignTeam's userinfo suicide/rewrite path when the live team
            // already matches the carrier.
            if (!st.guid.empty() &&
                carrierTarget &&
                profiles::SuppressCarrierAssign(
                    carrierTarget, LiveTeamBit(ent), desiredTeam))
                return;
            // A valid carrier naming a different profile is stale (a menu
            // re-sending a remembered value): hold the slot team instead of
            // switching on its bit. The post-hook restore re-pushes this
            // slot's own pack, so no flip-flop.
            if (profiles::ForeignCarrier(trb, st.guid, st.registered))
                return;
        }
        // Bare 0/1 is a collapsed userinfo: skip stock so it does not
        // team-kill before the guid is pushed back. A packed CVAR_INT
        // value can itself be 0 or 1 (guid fold 0); that one still runs.
        if (profiles::TeamRedBlueIsBareBit(trb)) {
            int bit = -1;
            bool packed = slot >= 0 && slot < profiles::kMaxSlots &&
                profiles::IntCvarMatchesGuid(trb, g_slots[slot].guid, &bit);
            if (slot >= 0 && slot < profiles::kMaxSlots &&
                !g_slots[slot].guid.empty() && !packed)
                return;
        }
    }
    if (original)
        original(thisp, ent, userinfo);
}

void profiles_OnGameDllLoaded(void* gameExport) {
    Profiles_InitCvars();
    Profiles_LoadRegistry();
    RefreshOutputs();
    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        !IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Cmd_AddCommand::oCmd_AddCommand))) {
        PrintOut(PRINT_BAD, "[profiles] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }
    InstallCommand("profile_register", reinterpret_cast<void*>(&profile_Register_f));
    InstallCommand("profile_import", reinterpret_cast<void*>(&profile_Import_f));
    InstallCommand("profile_set_privilege", reinterpret_cast<void*>(&profile_SetPrivilege_f));
    InstallCommand("profile_set_level", reinterpret_cast<void*>(&profile_SetPrivilege_f));
    InstallCommand("profile_del", reinterpret_cast<void*>(&profile_Del_f));
    InstallCommand("profile_query", reinterpret_cast<void*>(&profile_Query_f));
    InstallCommand("profile_signin", reinterpret_cast<void*>(&profile_Signin_f));
    InstallCommand("profile_signout", reinterpret_cast<void*>(&profile_Signout_f));
    InstallCommand("profile_get_slot_by_id", reinterpret_cast<void*>(&profile_GetSlotById_f));
    InstallCommand("profile_get_slot_by_nick", reinterpret_cast<void*>(&profile_GetSlotByNick_f));
    InstallCommand("profile_save", reinterpret_cast<void*>(&profile_Save_f));
    InstallCommand("profile_load", reinterpret_cast<void*>(&profile_Load_f));
    InstallGameHooks(gameExport);
    PrintOut(PRINT_LOG, "[profiles] server commands registered (registry %u entries)\n",
             static_cast<unsigned>(g_registry.Size()));
}

bool Profiles_SlotActive(int slot0) {
    if (!Profiles_IsEnabled())
        return true;
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    return st.registered && profiles::GuidValid(st.guid);
}

int Profiles_SlotPrivilege(int slot0) {
    if (!Profiles_IsEnabled())
        return profiles::kServerAdmin;
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return profiles::kUnspecifiedPrivilege;
    const profiles::SlotState& st = g_slots[slot0];
    return st.registered && profiles::GuidValid(st.guid)
        ? st.privilege : profiles::kUnspecifiedPrivilege;
}

int Profiles_SlotAdminLevel(int slot0) {
    return Profiles_SlotPrivilege(slot0);
}

bool Profiles_IsLevel0Admin(int slot0) {
    return Profiles_SlotPrivilege(slot0) == profiles::kServerAdmin;
}

bool Profiles_IsLevel01Admin(int slot0) {
    const int privilege = Profiles_SlotPrivilege(slot0);
    return privilege == profiles::kServerAdmin || privilege == profiles::kMapAdmin;
}

bool Profiles_SlotGuid(int slot0, char* guidOut, int guidCap) {
    if (guidOut && guidCap > 0)
        guidOut[0] = '\0';
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots || !guidOut || guidCap < 2)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    if (!st.registered || !profiles::GuidValid(st.guid))
        return false;
    std::size_t n = st.guid.size();
    if (n >= static_cast<std::size_t>(guidCap))
        n = static_cast<std::size_t>(guidCap - 1);
    if (n)
        std::memcpy(guidOut, st.guid.data(), n);
    guidOut[n] = '\0';
    return true;
}

bool Profiles_SlotNickname(int slot0, char* nickOut, int nickCap) {
    if (nickOut && nickCap > 0)
        nickOut[0] = '\0';
    if (slot0 < 0 || slot0 >= profiles::kMaxSlots || !nickOut || nickCap < 2)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    if (!st.registered || !profiles::GuidValid(st.guid) || st.nickname.empty())
        return false;
    std::string display = profiles::DisplayNickname(st.nickname);
    std::size_t n = display.size();
    if (n >= static_cast<std::size_t>(nickCap))
        n = static_cast<std::size_t>(nickCap - 1);
    if (n)
        std::memcpy(nickOut, display.data(), n);
    nickOut[n] = '\0';
    return n > 0;
}

bool Profiles_SlotHideTips(int slot0) {
    if (!Profiles_IsEnabled() || slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    if (!st.registered || !profiles::GuidValid(st.guid))
        return false;
    bool hide = false;
    g_registry.LookupHideTips(st.guid, &hide);
    return hide;
}

bool Profiles_SetSlotHideTips(int slot0, bool hide) {
    if (!Profiles_IsEnabled() || slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    if (!st.registered || !profiles::GuidValid(st.guid))
        return false;
    if (!g_registry.SetHideTips(st.guid, hide))
        return false;
    return Profiles_SaveRegistry();
}

bool Profiles_SlotSwapScoreboard(int slot0) {
    if (!Profiles_IsEnabled() || slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    if (!st.registered || !profiles::GuidValid(st.guid))
        return false;
    bool swap = false;
    g_registry.LookupSwapScore(st.guid, &swap);
    return swap;
}

bool Profiles_SetSlotSwapScoreboard(int slot0, bool swap) {
    if (!Profiles_IsEnabled() || slot0 < 0 || slot0 >= profiles::kMaxSlots)
        return false;
    const profiles::SlotState& st = g_slots[slot0];
    if (!st.registered || !profiles::GuidValid(st.guid))
        return false;
    if (!g_registry.SetSwapScore(st.guid, swap))
        return false;
    return Profiles_SaveRegistry();
}

static void CopyDisplayField(const char* src, char* out, int cap) {
    if (!out || cap < 2) {
        if (out && cap > 0)
            out[0] = '\0';
        return;
    }
    std::string display = profiles::DisplayNickname(src ? src : "");
    std::size_t n = display.size();
    if (n >= static_cast<std::size_t>(cap))
        n = static_cast<std::size_t>(cap - 1);
    if (n)
        std::memcpy(out, display.data(), n);
    out[n] = '\0';
}

static void LiveSlotName(int slot0, char* out, int cap) {
    if (out && cap > 0)
        out[0] = '\0';
    void* ent = EdictForSlot(slot0);
    const char* ui = ent ? UserinfoForEnt(ent) : nullptr;
    if (!ui || !InfoKeyReady())
        return;
    const char* name = SOF_EP_Info_ValueForKey(const_cast<char*>(ui), "name");
    CopyDisplayField(name, out, cap);
}

bool Profiles_SignedInAt(int index, int* slot0, char* nickname, int nicknameCap,
                         char* slotName, int slotNameCap) {
    if (index < 0 || !slot0 || !nickname || nicknameCap < 2)
        return false;
    nickname[0] = '\0';
    if (slotName && slotNameCap > 0)
        slotName[0] = '\0';
    int seen = 0;
    for (int i = 0; i < profiles::kMaxSlots; ++i) {
        const profiles::SlotState& st = g_slots[i];
        if (!st.registered || !profiles::GuidValid(st.guid))
            continue;
        if (seen++ != index)
            continue;
        *slot0 = i;
        CopyDisplayField(st.nickname.c_str(), nickname, nicknameCap);
        if (!nickname[0])
            std::snprintf(nickname, static_cast<std::size_t>(nicknameCap), "slot %d", i);
        if (slotName && slotNameCap >= 2)
            LiveSlotName(i, slotName, slotNameCap);
        return true;
    }
    return false;
}

extern "C" void Profiles_Shutdown() {
    RestoreGameHooks();
    g_edictsCache = nullptr;
    g_edictSizeCache = 0;
    for (int i = 0; i < profiles::kMaxSlots; ++i)
        g_slots[i] = profiles::SlotState();
    Profiles_ShutdownCvars();
}
