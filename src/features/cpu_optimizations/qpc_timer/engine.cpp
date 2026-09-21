#include "engine.h"

#include "buddy_import.h"
#include "log.h"

#include <cstddef>
#include <windows.h>

namespace qpctimer {
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
        return g_engine.curtime != nullptr;
    g_resolved = true;

    HMODULE exe = ExeMod();
    if (!exe || !IsValidModuleRva(exe, kRvaCurtime, sizeof(std::uint32_t))) {
        PrintOut(PRINT_BAD, "[qpc_timer] engine globals not resolvable - feature disabled\n");
        return false;
    }

    g_engine.curtime =
        reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<char*>(exe) + kRvaCurtime);
    return true;
}

const EngineGlobals& Engine() { return g_engine; }

}  // namespace qpctimer
