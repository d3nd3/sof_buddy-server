// dl_unlock: server-side download unlock for exactly one file.
//
// spsv.dll's my_download blocks every `download` whose path starts with
// "sofplus/" (strnicmp, 8), chained into the engine `download` command
// slot (flat 0x2013A3C4, i.e. engine base + 0x13A3C4) at spsv DLL-load
// time. That keeps server-side scripts private — but it also blocks the
// one file the client needs to bootstrap the pipe listener:
//   sofplus/addons/pipe.func
// (auto-loaded on the client by spf_sc_addons_init, requested with the
// `download` clientcommand).
//
// This module chains the SAME slot one level out: requests for exactly
// that path bypass spsv and reach the true engine original (which still
// enforces `allow_download` and serves from the game dir); everything
// else falls through to whatever the slot held (spsv's filter when
// present). Posture is fail-closed: if the bypass target cannot be
// proven to be executable engine code, nothing is installed and the
// block stays whole.
//
// spsv installs at its DllMain, i.e. before any game GetGameAPI, so the
// normal case is install-once over my_download. A per-frame slot check
// (cheap pointer read) re-installs if anything ever re-patches under us,
// and uninstalls cleanly when `_sofbuddy_pipe_dl` is turned off.

#include "dl_unlock.h"

#include "buddy_import.h"
#include "cvar.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace {

// Engine RVA of the `download` command handler slot (spsv writes flat
// 0x2013A3C4 with a 0x20000000-based engine).
constexpr unsigned kRvaDownloadSlot = 0x13A3C4;
// spsv.dll RVAs (verified in IDA: my_download code, orig_download global
// where spsv saved the engine original it chained).
constexpr unsigned kSpsvMyDownloadRva = 0x175E0;
constexpr unsigned kSpsvOrigGlobalRva = 0x2FC94;

constexpr unsigned long kHealCooldownMs = 5000;

using xcommand_fn = void(__cdecl*)(void);

xcommand_fn g_orig = nullptr;    // chained target for normal paths
xcommand_fn g_bypass = nullptr;  // engine original, allowlisted path only
void** g_slot = nullptr;
bool g_installed = false;
bool g_spsvSeen = false;
long long g_allowed = 0;
unsigned long g_lastHealMs = 0;
bool g_haveTriedHeal = false;

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
    const DWORD execMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

inline bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

HMODULE SpsvMod() {
    if (HMODULE h = GetModuleHandleA("spsv.dll"))
        return h;
    return GetModuleHandleA("SPSV.DLL");
}

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000000)
        return false;
    if (!IsSafeMemoryBlock(reinterpret_cast<const char*>(dos) + dos->e_lfanew,
                           sizeof(IMAGE_NT_HEADERS)))
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

bool InsideModule(HMODULE h, const void* p) {
    if (!h || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    auto base = reinterpret_cast<uintptr_t>(h);
    auto addr = reinterpret_cast<uintptr_t>(p);
    return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
}

bool EngineCmdReady() {
    return detour_Cmd_Argc::oCmd_Argc && detour_Cmd_Argv::oCmd_Argv &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argc::oCmd_Argc)) &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argv::oCmd_Argv));
}

void __cdecl DlUnlock_Download_f();

// ---- ghoul-table auto-registration --------------------------------------
// The client's connect-time precache walk requests every non-empty
// CS_GHOULFILES entry verbatim ("%s", no prefix/extension mangling) and
// stops at the first empty slot, so the entry must be registered compactly
// (first-free, never gapped) and re-registered every map: configstrings
// are wiped on map spawn. SV_GhoulFileIndex does exactly that (idempotent:
// returns the existing index when already present), which is why the raw
// table pokes are not used here.

constexpr unsigned kRvaSvConfigstrings = 0x3A2374;
constexpr int kCsMapChecksum = 6;
constexpr int kCsGhoulFiles = 1497;
constexpr int kMaxGhoulFiles = 2048;
constexpr int kMaxQpath = 64;
constexpr unsigned long kGhoulErrCooldownMs = 30000;

char g_lastMap[kMaxQpath + 1] = {};
bool g_ghoulRegistered = false;
int g_ghoulIndex = 0;
unsigned long g_ghoulErrMs = 0;

char* SvConfigstrings() {
    HMODULE h = ExeMod();
    if (!h || !IsValidModuleRva(h, kRvaSvConfigstrings, kMaxQpath))
        return nullptr;
    char* base = reinterpret_cast<char*>(h) + kRvaSvConfigstrings;
    if (!Readable(base, kMaxQpath))
        return nullptr;
    return base;
}

bool GhoulPointerReady() {
    return detour_SV_GhoulFileIndex::oSV_GhoulFileIndex &&
           IsExecutableCodeAddress(
               reinterpret_cast<const void*>(detour_SV_GhoulFileIndex::oSV_GhoulFileIndex));
}

// SV_FindIndex Com_Errors the server when the range is full; pre-scan so a
// full ghoul table degrades to a log line instead of a crash.
bool GhoulSlotFree(char* strings) {
    for (int i = 1; i < kMaxGhoulFiles; ++i) {
        char* slot =
            strings + static_cast<unsigned>(kCsGhoulFiles + i) * kMaxQpath;
        if (!Readable(slot, kMaxQpath))
            return false;
        if (slot[0] == '\0')
            return true;
    }
    return false;
}

void GhoulEnsureRegistered() {
    char* strings = SvConfigstrings();
    if (!strings) {
        g_ghoulRegistered = false;
        return;
    }
    char* cs = strings + static_cast<unsigned>(kCsMapChecksum) * kMaxQpath;
    if (!Readable(cs, kMaxQpath) || cs[0] == '\0') {
        g_ghoulRegistered = false;  // between maps: re-register on next spawn
        return;
    }
    if (g_ghoulRegistered && std::strncmp(g_lastMap, cs, kMaxQpath) == 0)
        return;
    const unsigned long now = GetTickCount();
    if (!GhoulPointerReady() || !GhoulSlotFree(strings)) {
        if (static_cast<unsigned long>(now - g_ghoulErrMs) >= kGhoulErrCooldownMs) {
            g_ghoulErrMs = now;
            PrintOut(PRINT_BAD,
                     "[pipe_dl] ghoul registration deferred (%s)\n",
                     GhoulPointerReady() ? "table full" : "SV_GhoulFileIndex unavailable");
        }
        return;
    }
    const int idx = SOF_EP_SV_GhoulFileIndex(const_cast<char*>(dlunlock::kPipePath));
    if (idx < kCsGhoulFiles + 1 || idx >= kCsGhoulFiles + kMaxGhoulFiles) {
        PrintOut(PRINT_BAD, "[pipe_dl] SV_GhoulFileIndex returned bad index %d\n", idx);
        return;
    }
    std::memcpy(g_lastMap, cs, kMaxQpath);
    g_lastMap[kMaxQpath] = '\0';
    g_ghoulRegistered = true;
    g_ghoulIndex = idx;
    PrintOut(PRINT_LOG, "[pipe_dl] registered '%s' at ghoul index %d (map %s)\n",
             dlunlock::kPipePath, idx, g_lastMap);
}

bool InstallNow() {    HMODULE eng = ExeMod();
    if (!eng || !IsValidModuleRva(eng, kRvaDownloadSlot, sizeof(void*))) {
        PrintOut(PRINT_BAD, "[pipe_dl] install skipped: engine download slot unreadable\n");
        return false;
    }
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(eng) + kRvaDownloadSlot);
    if (!Readable(slot, sizeof(void*))) {
        PrintOut(PRINT_BAD, "[pipe_dl] install skipped: slot pointer unreadable\n");
        return false;
    }
    void* cur = *slot;
    if (cur == reinterpret_cast<void*>(&DlUnlock_Download_f)) {
        g_installed = true;
        return true;
    }

    void* orig = cur;
    void* bypass = cur;
    bool spsv = false;
    if (HMODULE spsvMod = SpsvMod()) {
        if (IsValidModuleRva(spsvMod, kSpsvMyDownloadRva, 16) &&
            IsValidModuleRva(spsvMod, kSpsvOrigGlobalRva, sizeof(void*))) {
            void* myDl =
                reinterpret_cast<char*>(spsvMod) + kSpsvMyDownloadRva;
            if (cur == myDl) {
                spsv = true;
                void** g = reinterpret_cast<void**>(reinterpret_cast<char*>(spsvMod) +
                                                    kSpsvOrigGlobalRva);
                if (!Readable(g, sizeof(void*))) {
                    PrintOut(PRINT_BAD,
                             "[pipe_dl] install refused: spsv orig global unreadable "
                             "(fail closed, block intact)\n");
                    return false;
                }
                void* engOrig = *g;
                if (!IsExecutableCodeAddress(engOrig) || InsideModule(spsvMod, engOrig)) {
                    PrintOut(PRINT_BAD,
                             "[pipe_dl] install refused: spsv orig target not engine code "
                             "(fail closed, block intact)\n");
                    return false;
                }
                bypass = engOrig;  // exact-path requests skip my_download
                orig = myDl;       // everything else keeps spsv's filter
            }
        }
    }
    if (!IsExecutableCodeAddress(orig) || !IsExecutableCodeAddress(bypass)) {
        PrintOut(PRINT_BAD, "[pipe_dl] install refused: chained target not executable\n");
        return false;
    }
    // Writable .data table (spsv's BeginPatches writes it with no protection
    // change); read back to confirm the write landed.
    *slot = reinterpret_cast<void*>(&DlUnlock_Download_f);
    if (*slot != reinterpret_cast<void*>(&DlUnlock_Download_f)) {
        PrintOut(PRINT_BAD, "[pipe_dl] install failed: slot write did not stick\n");
        return false;
    }
    g_slot = slot;
    g_orig = reinterpret_cast<xcommand_fn>(orig);
    g_bypass = reinterpret_cast<xcommand_fn>(bypass);
    g_spsvSeen = spsv;
    g_installed = true;
    PrintOut(PRINT_LOG,
             "[pipe_dl] installed (slot=%p orig=%p bypass=%p spsv=%d): only '%s' bypasses\n",
             slot, orig, bypass, spsv ? 1 : 0, dlunlock::kPipePath);
    return true;
}

void UninstallNow(const char* why) {
    if (!g_installed)
        return;
    if (g_slot && Readable(g_slot, sizeof(void*)) &&
        *g_slot == reinterpret_cast<void*>(&DlUnlock_Download_f) && g_orig &&
        IsExecutableCodeAddress(reinterpret_cast<const void*>(g_orig))) {
        *g_slot = reinterpret_cast<void*>(g_orig);
    }
    g_installed = false;
    g_slot = nullptr;
    g_orig = nullptr;
    g_bypass = nullptr;
    PrintOut(PRINT_LOG, "[pipe_dl] uninstalled (%s)\n", why ? why : "?");
}

void __cdecl DlUnlock_Download_f() {
    const char* path = nullptr;
    if (EngineCmdReady() && SOF_EP_Cmd_Argc() >= 2) {
        const char* a = SOF_EP_Cmd_Argv(1);
        if (a && IsSafeMemoryBlock(a, 1))
            path = a;
    }
    xcommand_fn target = g_orig;
    if (path && dlunlock::AllowPath(path)) {
        target = g_bypass;
        ++g_allowed;
        Clsv_SetDlAllowed(g_allowed);
        PrintOut(PRINT_LOG, "[pipe_dl] allowed '%s' (%lld total)\n", path, g_allowed);
    }
    if (target && IsExecutableCodeAddress(reinterpret_cast<const void*>(target)))
        target();
    else
        PrintOut(PRINT_BAD, "[pipe_dl] no valid chained target; request dropped\n");
}

}  // namespace

void DlUnlock_Tick() {
    if (!DlUnlock_Enabled()) {
        UninstallNow("master cvar off");
        return;
    }
    if (g_installed && g_slot && Readable(g_slot, sizeof(void*)) &&
        *g_slot == reinterpret_cast<void*>(&DlUnlock_Download_f)) {
        GhoulEnsureRegistered();  // healthy slot: keep the per-map entry live
        return;
    }
    const unsigned long now = GetTickCount();
    if (g_haveTriedHeal && static_cast<unsigned long>(now - g_lastHealMs) < kHealCooldownMs)
        return;
    g_haveTriedHeal = true;
    g_lastHealMs = now;
    InstallNow();
}

void DlUnlock_Shutdown() {
    UninstallNow("detach");
}

void DlUnlock_Status() {
    char ghoul[16];
    if (g_ghoulRegistered)
        std::snprintf(ghoul, sizeof(ghoul), "%d", g_ghoulIndex);
    else
        std::snprintf(ghoul, sizeof(ghoul), "-");
    Buddy_DebugPrintf("[pipe_dl] enabled=%d installed=%d spsv=%d allowed=%lld ghoul=%s\n",
                      DlUnlock_Enabled() ? 1 : 0, g_installed ? 1 : 0,
                      g_spsvSeen ? 1 : 0, g_allowed, ghoul);
}
