#include "cvar.h"

#include "buddy_import.h"

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace {

constexpr int kCvarFlagArchive = 1;

// cvar_t layout used by the rest of the server features: +0x00 name,
// +0x04 string, +0x08 latched_string, +0x0C flags, +0x18 value.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;

void* g_cvEnabled = nullptr;
void* g_cvAuto = nullptr;
void* g_cvEntities = nullptr;
void* g_cvInterval = nullptr;
void* g_cvMaxMb = nullptr;
void* g_cvDir = nullptr;

bool Readable(const void* ptr, std::size_t size) {
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

const char* CvarString(void* cv) {
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarStringOfs, sizeof(char*)))
        return "";
    char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    return (s && Readable(s, 1)) ? s : "";
}

bool CvarBool(void* cv, bool fallback) {
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarValueOfs, sizeof(float)))
        return fallback;
    const float v = *reinterpret_cast<const float*>(static_cast<char*>(cv) + kCvarValueOfs);
    return v != 0.0f;
}

float CvarFloat(void* cv, float fallback) {
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarValueOfs, sizeof(float)))
        return fallback;
    return *reinterpret_cast<const float*>(static_cast<char*>(cv) + kCvarValueOfs);
}

}  // namespace

void ServerDemo_InitCvars() {
    if (g_cvEnabled)
        return;
    g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_svdemo", "1", kCvarFlagArchive, nullptr);
    g_cvAuto = Buddy_GetEngineCvar("_sofbuddy_svdemo_auto", "0", kCvarFlagArchive, nullptr);
    g_cvEntities =
        Buddy_GetEngineCvar("_sofbuddy_svdemo_entities", "1", kCvarFlagArchive, nullptr);
    g_cvInterval =
        Buddy_GetEngineCvar("_sofbuddy_svdemo_interval", "1", kCvarFlagArchive, nullptr);
    g_cvMaxMb = Buddy_GetEngineCvar("_sofbuddy_svdemo_max_mb", "64", kCvarFlagArchive, nullptr);
    g_cvDir = Buddy_GetEngineCvar("_sofbuddy_svdemo_dir", "demos", kCvarFlagArchive, nullptr);
}

// The getters deliberately do *not* call Cvar_Get. They run from
// ClientEndServerFrame Post, and the engine cvar API is not safe from the
// frame path on every Wine build (see buddy_import.h). OnGameDllLoaded is the
// only place that binds them; an unbound cvar falls back to its default.

bool ServerDemo_Enabled() {
    // Fail closed: an unbound recording feature must not start writing files.
    return CvarBool(g_cvEnabled, false);
}

bool ServerDemo_AutoEnabled() {
    return CvarBool(g_cvAuto, false);
}

bool ServerDemo_EntitiesEnabled() {
    return CvarBool(g_cvEntities, true);
}

int ServerDemo_Interval() {
    int n = static_cast<int>(CvarFloat(g_cvInterval, 1.0f));
    if (n < 1)
        n = 1;
    if (n > 600)
        n = 600;
    return n;
}

unsigned long long ServerDemo_MaxBytes() {
    float mb = CvarFloat(g_cvMaxMb, 64.0f);
    if (mb <= 0.0f)
        return 0;
    if (mb > 4096.0f)
        mb = 4096.0f;
    return static_cast<unsigned long long>(mb * 1024.0f * 1024.0f);
}

const char* ServerDemo_Dir() {
    const char* dir = CvarString(g_cvDir);
    return (dir && dir[0]) ? dir : "demos";
}
