#include "engine.h"

#include "buddy_import.h"
#include "log.h"

#include <cstddef>
#include <windows.h>

namespace tickpace {
namespace {

EngineGlobals g_engine;
bool g_resolved = false;

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

bool IsSafeMemoryBlock(const void* ptr, std::size_t size) {
    auto addr = reinterpret_cast<std::uintptr_t>(ptr);
    if (!ptr || size == 0 || addr + size < addr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    return addr + size <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (!IsSafeMemoryBlock(dos, sizeof(IMAGE_DOS_HEADER)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!IsSafeMemoryBlock(nt, sizeof(IMAGE_NT_HEADERS)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    return rva <= nt->OptionalHeader.SizeOfImage && rva + size <= nt->OptionalHeader.SizeOfImage;
}

}  // namespace

bool EngineReady() {
    if (g_resolved)
        return g_engine.svsRealtime != nullptr;
    g_resolved = true;

    HMODULE exe = ExeMod();
    if (!exe ||
        !IsValidModuleRva(exe, kRvaSvState, sizeof(std::int32_t)) ||
        !IsValidModuleRva(exe, kRvaSvTime, sizeof(std::uint32_t)) ||
        !IsValidModuleRva(exe, kRvaSvsRealtime, sizeof(std::uint32_t)) ||
        !IsValidModuleRva(exe, kRvaSvsInitialized, sizeof(std::int32_t)) ||
        !IsValidModuleRva(exe, kRvaDedicated, sizeof(void*)) ||
        !IsValidModuleRva(exe, kRvaCmdTextCursize, sizeof(std::int32_t))) {
        PrintOut(PRINT_BAD, "[tick_pacing] engine globals not resolvable - feature disabled\n");
        return false;
    }

    auto* base = reinterpret_cast<char*>(exe);
    g_engine.svState        = reinterpret_cast<volatile const std::int32_t*>(base + kRvaSvState);
    g_engine.svTime         = reinterpret_cast<volatile const std::uint32_t*>(base + kRvaSvTime);
    g_engine.svsRealtime    = reinterpret_cast<volatile std::uint32_t*>(base + kRvaSvsRealtime);
    g_engine.svsInitialized = reinterpret_cast<volatile const std::int32_t*>(base + kRvaSvsInitialized);
    g_engine.dedicated      = reinterpret_cast<void* const*>(base + kRvaDedicated);
    g_engine.cmdTextCursize = reinterpret_cast<volatile const std::int32_t*>(base + kRvaCmdTextCursize);
    return true;
}

const EngineGlobals& Engine() { return g_engine; }

bool ServerRunning() {
    return g_engine.svState && *g_engine.svState == kSvStateGame &&
           g_engine.svsInitialized && *g_engine.svsInitialized != 0;
}

bool CommandsQueued() {
    return g_engine.cmdTextCursize && *g_engine.cmdTextCursize != 0;
}

bool IsDedicated() {
    if (!g_engine.dedicated)
        return false;
    void* cv = *g_engine.dedicated;
    return cv && Buddy_ReadCvarValue(cv, 0.0f) != 0.0f;
}

}  // namespace tickpace
