// stufftext: server-console command that sends svc_stufftext to clients.
//
//   stufftext <target> <command...>
//     target:  client slot (0-based), "all", or a case-insensitive
//              substring of the client's userinfo "name"
//     command: console text stuffed into the client's buffer
//              (a trailing \n is appended when missing)
//
// Registered as an engine console command via Cmd_AddCommand on
// GameDllLoaded, so it works from the dedicated-server console and via
// rcon. Delivery uses the stock order from p_client.cpp:
// gi.WriteByte(svc_stufftext) + gi.WriteString + gi.unicast(ent, true).

#include "cvar.h"
#include "stufftext_cmds.h"
#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace {

// Game DLL layout (same verified constants as ctf_spawn).
constexpr unsigned kEdictStride = 0x464;
constexpr unsigned kEdictClient = 0x74;
constexpr unsigned kEdictInuse = 0x78;
constexpr unsigned kClUserinfo = 0xCC;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr unsigned kRvaMaxclientsCvar = 0x15D9B4;
constexpr unsigned kRvaGEdicts = 0x15CCA0;

constexpr std::size_t kTextBuf = 1024;

long long g_sent = 0;
long long g_errors = 0;

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

HMODULE GameMod() {
    HMODULE h = nullptr;
    if (HMODULE shim = GetModuleHandleA("gamex86.dll")) {
        // Our own image also exports Buddy_GetGameDllHandle; prefer the real
        // stock handle when available so RVAs never resolve against the shim.
        using handle_fn = HMODULE (*)();
        auto fn = reinterpret_cast<handle_fn>(GetProcAddress(shim, "Buddy_GetGameDllHandle"));
        if (fn) {
            HMODULE stock = fn();
            if (stock)
                return stock;
        }
    }
    h = GetModuleHandleA("oldgamex86.dll");
    if (!h) h = GetModuleHandleA("OldGamex86.dll");
    if (!h) h = GetModuleHandleA("OLDGAMEX86.DLL");
    return h;
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000000) return false;
    if (!IsSafeMemoryBlock(reinterpret_cast<const char*>(dos) + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (rva >= nt->OptionalHeader.SizeOfImage) return false;
    if (rva + size > nt->OptionalHeader.SizeOfImage || rva + size < rva) return false;
    return true;
}

int GameMaxClients() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaMaxclientsCvar, sizeof(void*))) return 0;
    void* cv = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaMaxclientsCvar);
    if (!IsSafeMemoryBlock(cv, kCvarValueOfs + sizeof(float))) return 0;
    int v = static_cast<int>(*reinterpret_cast<float*>(static_cast<char*>(cv) + kCvarValueOfs));
    if (v < 1) return 0;
    if (v > 64) return 64;
    return v;
}

char* GameEdicts() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaGEdicts, sizeof(void*))) return nullptr;
    void* edicts = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
    if (!IsValidUserPointer(edicts)) return nullptr;
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

// Case-insensitive substring: nullptr-safe, ASCII only (matches the engine's
// command matching, which is stricmp-based; see hash_lookup README).
bool ContainsNoCase(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle)
        return false;
    for (const char* h = haystack; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
            if (ca != cb)
                break;
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

const char* ClientName(void* edict) {
    if (!IsSafeMemoryBlock(edict, kEdictClient + sizeof(void*)))
        return nullptr;
    void* cl = *reinterpret_cast<void* const*>(static_cast<char*>(edict) + kEdictClient);
    if (!IsSafeMemoryBlock(static_cast<char*>(cl) + kClUserinfo, 64))
        return nullptr;
    const char* userinfo = static_cast<const char*>(cl) + kClUserinfo;
    if (!detour_Info_ValueForKey::oInfo_ValueForKey ||
        !IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Info_ValueForKey::oInfo_ValueForKey)))
        return nullptr;
    const char* name = SOF_EP_Info_ValueForKey(userinfo, "name");
    if (!name || !IsValidUserPointer(name) || !IsSafeMemoryBlock(name, 1))
        return nullptr;
    return name;
}

bool EngineCmdReady() {
    return detour_Cmd_Argc::oCmd_Argc && detour_Cmd_Argv::oCmd_Argv &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argc::oCmd_Argc)) &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argv::oCmd_Argv));
}

// Join argv[first..argc-1] with single spaces into out (truncated), then
// ensure \n termination. Returns false when there is no text at all.
bool BuildStuffText(char* out, std::size_t n, int argc, int first) {
    if (!out || n == 0 || argc <= first)
        return false;
    std::size_t pos = 0;
    for (int i = first; i < argc; ++i) {
        const char* a = SOF_EP_Cmd_Argv(i);
        if (!a || !IsValidUserPointer(a) || !IsSafeMemoryBlock(a, 1))
            continue;
        // Bound the scan: engine args are short console tokens.
        std::size_t len = 0;
        while (len < 512 && a[len])
            ++len;
        if (len == 0)
            continue;
        if (pos > 0 && pos + 1 < n)
            out[pos++] = ' ';
        for (std::size_t k = 0; k < len && pos + 1 < n; ++k)
            out[pos++] = a[k];
    }
    if (pos == 0)
        return false;
    if (out[pos - 1] != '\n' && pos + 1 < n)
        out[pos++] = '\n';
    out[pos] = '\0';
    return true;
}

void NoteResult(bool ok) {
    if (ok)
        ++g_sent;
    else
        ++g_errors;
    StuffText_SetOutputs(g_sent, g_errors);
}

int ResolveSlot(const char* target, int maxClients) {
    if (!target || maxClients < 1)
        return -2;
    char* end = nullptr;
    long v = std::strtol(target, &end, 10);
    if (!end || *end != '\0')
        return -2;  // not numeric
    if (v == maxClients)  // allow 1-based entnum for the last slot too
        return maxClients - 1;
    if (v < 0 || v >= maxClients)
        return -3;  // numeric but out of range
    return static_cast<int>(v);
}

}  // namespace

// Shared delivery accounting for the reconnect chain (see
// stufftext_reconnect.h): every stuff the chain sends goes through here so
// the sent/errors outputs stay exact. Global linkage; forwards to the
// TU-internal counters above.
void StuffText_NoteDelivery(bool ok) {
    if (ok)
        ++g_sent;
    else
        ++g_errors;
    StuffText_SetOutputs(g_sent, g_errors);
}

// Engine xcommand_t: void (__cdecl*)(void). Runs on the engine thread during
// Cbuf_Execute - the same context stock game code calls gi.unicast from.
extern "C" void __cdecl stufftext_Command_f() {
    if (!StuffText_IsEnabled()) {
        Buddy_DebugPrintf("stufftext is disabled (_sofbuddy_stufftext 0)\n");
        ++g_errors;
        StuffText_SetOutputs(g_sent, g_errors);
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("stufftext: engine arg accessors unavailable\n");
        NoteResult(false);
        return;
    }

    const int argc = SOF_EP_Cmd_Argc();
    if (argc < 3) {
        Buddy_DebugPrintf("usage: stufftext <slot|all|name> <command...>\n");
        NoteResult(false);
        return;
    }

    const char* target = SOF_EP_Cmd_Argv(1);
    if (!target || !IsValidUserPointer(target) || !IsSafeMemoryBlock(target, 1)) {
        NoteResult(false);
        return;
    }

    char text[kTextBuf];
    if (!BuildStuffText(text, sizeof(text), argc, 2)) {
        Buddy_DebugPrintf("stufftext: nothing to send\n");
        NoteResult(false);
        return;
    }

    char* edicts = GameEdicts();
    const int maxClients = GameMaxClients();
    if (!IsValidUserPointer(edicts) || maxClients < 1) {
        Buddy_DebugPrintf("stufftext: server not ready\n");
        NoteResult(false);
        return;
    }

    if (_stricmp(target, "all") == 0) {
        int delivered = 0;
        for (int slot = 0; slot < maxClients; ++slot) {
            char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
            if (!EdictInUse(edict))
                continue;
            if (Buddy_StuffText(edict, text))
                ++delivered;
        }
        if (delivered > 0) {
            g_sent += delivered;
            StuffText_SetOutputs(g_sent, g_errors);
            PrintOut(PRINT_LOG, "[stufftext] stuffed %d client(s): %s", delivered, text);
        } else {
            Buddy_DebugPrintf("stufftext: no clients connected\n");
            NoteResult(false);
        }
        return;
    }

    int slot = ResolveSlot(target, maxClients);
    if (slot >= 0) {
        char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
        if (!EdictInUse(edict)) {
            Buddy_DebugPrintf("stufftext: slot %d is not in use\n", slot);
            NoteResult(false);
            return;
        }
        if (Buddy_StuffText(edict, text)) {
            NoteResult(true);
            PrintOut(PRINT_LOG, "[stufftext] stuffed slot %d: %s", slot, text);
        } else {
            Buddy_DebugPrintf("stufftext: delivery to slot %d failed\n", slot);
            NoteResult(false);
        }
        return;
    }
    if (slot == -3) {
        Buddy_DebugPrintf("stufftext: slot out of range (0-%d, or all)\n", maxClients - 1);
        NoteResult(false);
        return;
    }

    // Name substring match (first in-use hit wins).
    for (int i = 0; i < maxClients; ++i) {
        char* edict = edicts + static_cast<unsigned>(i + 1) * kEdictStride;
        if (!EdictInUse(edict))
            continue;
        const char* name = ClientName(edict);
        if (name && ContainsNoCase(name, target)) {
            if (Buddy_StuffText(edict, text)) {
                NoteResult(true);
                PrintOut(PRINT_LOG, "[stufftext] stuffed '%s' (slot %d): %s", name, i, text);
            } else {
                Buddy_DebugPrintf("stufftext: delivery to '%s' failed\n", name);
                NoteResult(false);
            }
            return;
        }
    }
    Buddy_DebugPrintf("stufftext: no client matching '%s'\n", target);
    NoteResult(false);
}

void stufftext_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    StuffText_InitCvars();
    StuffText_SetOutputs(g_sent, g_errors);
    StuffText_RegisterConsoleCommands();
}
