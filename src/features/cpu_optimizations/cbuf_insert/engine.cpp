#include "engine.h"

#include "log.h"

#include <cstddef>
#include <windows.h>

namespace cbufinsert {
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
        return g_engine.cursize != nullptr;
    g_resolved = true;

    HMODULE exe = ExeMod();
    if (!exe ||
        !IsValidModuleRva(exe, kRvaCmdTextData, sizeof(void*)) ||
        !IsValidModuleRva(exe, kRvaCmdTextMaxsize, sizeof(std::int32_t)) ||
        !IsValidModuleRva(exe, kRvaCmdTextCursize, sizeof(std::int32_t))) {
        PrintOut(PRINT_BAD, "[cbuf_insert] cmd_text not resolvable - delegating every call\n");
        return false;
    }

    auto* base = reinterpret_cast<char*>(exe);
    g_engine.data    = reinterpret_cast<unsigned char* const*>(base + kRvaCmdTextData);
    g_engine.maxsize = reinterpret_cast<const std::int32_t*>(base + kRvaCmdTextMaxsize);
    g_engine.cursize = reinterpret_cast<std::int32_t*>(base + kRvaCmdTextCursize);

    // The buffer is an 8KB static array. If maxsize is not what Cbuf_Init sets,
    // this is not the layout these RVAs were read from.
    if (*g_engine.maxsize != 0x2000) {
        PrintOut(PRINT_BAD,
                 "[cbuf_insert] cmd_text.maxsize is %d, expected 8192 - delegating every call\n",
                 static_cast<int>(*g_engine.maxsize));
        g_engine.cursize = nullptr;
        return false;
    }
    return true;
}

const EngineGlobals& Engine() { return g_engine; }

}  // namespace cbufinsert
