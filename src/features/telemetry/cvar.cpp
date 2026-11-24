#include "cvar.h"

#include "buddy_import.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <windows.h>

namespace {

constexpr int kCvarFlagArchive = 1;

// cvar_t layout used by the rest of the server features: +0x00 name,
// +0x04 string, +0x08 latched_string, +0x0C flags, +0x18 value.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;

void* g_cvEnabled = nullptr;
void* g_cvDir = nullptr;
void* g_cvMaxMb = nullptr;
void* g_cvQueueKb = nullptr;
void* g_cvEvents = nullptr;
void* g_cvSampleSec = nullptr;

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

}  // namespace

void* Telemetry_Cvar(const char* name, const char* value, int flags) {
    return Buddy_GetEngineCvar(name, value, flags, nullptr);
}

const char* Telemetry_CvarString(void* cv, const char* fallback) {
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarStringOfs, sizeof(char*)))
        return fallback;
    char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    return (s && Readable(s, 1)) ? s : fallback;
}

float Telemetry_CvarFloat(void* cv, float fallback) {
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarValueOfs, sizeof(float)))
        return fallback;
    return *reinterpret_cast<const float*>(static_cast<char*>(cv) + kCvarValueOfs);
}

int Telemetry_CvarInt(void* cv, int fallback) {
    return static_cast<int>(Telemetry_CvarFloat(cv, static_cast<float>(fallback)));
}

void Telemetry_InitCvars() {
    if (g_cvEnabled)
        return;
    g_cvEnabled = Telemetry_Cvar("_sofbuddy_telemetry", "0", kCvarFlagArchive);
    g_cvDir = Telemetry_Cvar("_sofbuddy_telemetry_dir", "telemetry/spool",
                             kCvarFlagArchive);
    g_cvMaxMb = Telemetry_Cvar("_sofbuddy_telemetry_max_mb", "64",
                               kCvarFlagArchive);
    g_cvQueueKb = Telemetry_Cvar("_sofbuddy_telemetry_queue_kb", "256",
                                 kCvarFlagArchive);
    g_cvEvents = Telemetry_Cvar("_sofbuddy_telemetry_events", "3",
                                kCvarFlagArchive);
    g_cvSampleSec = Telemetry_Cvar("_sofbuddy_telemetry_sample_sec", "30",
                                   kCvarFlagArchive);
}

bool Telemetry_Enabled() {
    return Telemetry_CvarFloat(g_cvEnabled, 0.0f) != 0.0f;
}

const char* Telemetry_Dir() {
    const char* dir = Telemetry_CvarString(g_cvDir, "telemetry/spool");
    return (dir && dir[0]) ? dir : "telemetry/spool";
}

unsigned long long Telemetry_MaxBytes() {
    float mb = Telemetry_CvarFloat(g_cvMaxMb, 64.0f);
    if (mb <= 0.0f)
        return 0;
    if (mb > 4096.0f)
        mb = 4096.0f;
    return static_cast<unsigned long long>(mb * 1024.0f * 1024.0f);
}

std::size_t Telemetry_QueueLimitBytes() {
    float kb = Telemetry_CvarFloat(g_cvQueueKb, 256.0f);
    if (kb < 16.0f)
        kb = 16.0f;
    if (kb > 4096.0f)
        kb = 4096.0f;
    return static_cast<std::size_t>(kb * 1024.0f);
}

int Telemetry_EventsMask() {
    return Telemetry_CvarInt(g_cvEvents, 3);
}

int Telemetry_SampleSeconds() {
    int sec = Telemetry_CvarInt(g_cvSampleSec, 30);
    if (sec < 0)
        sec = 0;
    if (sec > 3600)
        sec = 3600;
    return sec;
}
