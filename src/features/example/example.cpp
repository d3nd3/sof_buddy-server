#include "generated_engine_pointers.h"
#include "log.h"

#include <windows.h>

void example_OnGameDllLoaded(void* game_export)
{
	(void)game_export;
	PrintOut(PRINT_LOG, "[example] GameDllLoaded (sof_buddy-server)\n");
	SOF_EP_Com_DPrintf("[sof_buddy-server] example: GameDllLoaded\n");
}
