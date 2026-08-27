#include "cvar.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <windows.h>

namespace {

inline bool IsValidCodeAddress(const void* ptr) {
    if (!ptr)
        return false;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000 || addr > 0x7FFFFFFF)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

}  // namespace

void example_OnGameDllLoaded(void* game_export)
{
	(void)game_export;
	Example_InitCvars();
	PrintOut(PRINT_LOG, "[example] GameDllLoaded (sof_buddy-server)\n");

	void* fn = reinterpret_cast<void*>(detour_Com_DPrintf::oCom_DPrintf);
	if (IsValidCodeAddress(fn)) {
		SOF_EP_Com_DPrintf("[sof_buddy-server] example: GameDllLoaded\n");
	}
}
