/*
 * Server-side gamex86 shim for SOF Buddy.
 * Loads the stock game logic from base\oldgamex86.dll and forwards GetGameAPI.
 * Detours and feature hooks: detours.yaml plus per-feature hooks.json and callbacks.json under src/features.
 * (see docs/DETOUR_SYSTEM.md).
 */

#include <windows.h>
#include <cstring>

#include "detours.h"
#include "engfuncs.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "shared_hook_manager.h"

static HMODULE g_hShimModule = nullptr;
static HMODULE g_hGameDll = nullptr;
static lpfn_GetGameApi g_pfnGetGameApi = nullptr;

/* game_export_t: apiversion, Init, Shutdown, ... — only need the prefix to wrap Shutdown. */
struct game_export_head {
	int apiversion;
	void (*Init)(void);
	void (*Shutdown)(void);
};

static void (*g_origShutdown)(void);
static bool s_lifetimeInit = false;
static bool s_gameDetoursActive = false;

static void Buddy_OnGameShutdown(void)
{
	if (g_origShutdown)
		g_origShutdown();
	GetDetourSystem().RemoveGameDetours();
	s_gameDetoursActive = false;
}

static BOOL LoadOriginalGameDll(HINSTANCE shimModule)
{
	char path[MAX_PATH];
	char *slash;

	// Resolve next to this DLL (Base\gamex86.dll -> Base\oldgamex86.dll). Using the
	// main exe directory + "base\\..." breaks dedicated servers whose exe is not the
	// game root, and breaks Wine when the folder is "Base" but we spelled "base".
	if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(shimModule), path, MAX_PATH))
		return FALSE;

	slash = strrchr(path, '\\');
	if (!slash)
		slash = strrchr(path, '/');
	if (!slash)
		return FALSE;
	slash[1] = '\0';

	if (strlen(path) + strlen("oldgamex86.dll") >= MAX_PATH)
		return FALSE;
	strcat(path, "oldgamex86.dll");

	g_hGameDll = LoadLibraryA(path);
	if (!g_hGameDll)
		return FALSE;

	g_pfnGetGameApi = (lpfn_GetGameApi)GetProcAddress(g_hGameDll, "GetGameAPI");
	return g_pfnGetGameApi != nullptr;
}

extern "C" game_export_t *GetGameApi(game_import_t *import)
{
	// Do not LoadLibrary the real game DLL from DllMain — loader lock breaks that on
	// Windows/Wine and DllMain returns FALSE -> engine reports "failed to load game DLL".
	if (!g_pfnGetGameApi) {
		if (!g_hShimModule)
			return nullptr;
		if (!LoadOriginalGameDll(reinterpret_cast<HINSTANCE>(g_hShimModule)))
			return nullptr;
	}

	game_export_t *ge = g_pfnGetGameApi(import);

	game_export_head *head = reinterpret_cast<game_export_head *>(ge);
	if (head->Shutdown != Buddy_OnGameShutdown) {
		g_origShutdown = head->Shutdown;
		head->Shutdown = Buddy_OnGameShutdown;
	}

	if (!s_lifetimeInit) {
		s_lifetimeInit = true;
		GetDetourSystem().ProcessDeferredRegistrations();
		RegisterAllFeatureHooks();
		RegisterPointerOnlyFunctions_SofExe();
		RegisterPointerOnlyFunctions_RefDll();
		RegisterPointerOnlyFunctions_PlayerDll();
		RegisterPointerOnlyFunctions_Unknown();
	}
	if (!s_gameDetoursActive) {
		s_gameDetoursActive = true;
		RegisterPointerOnlyFunctions_GameDll();
		EnginePointers_Bind();
		GetDetourSystem().ApplyGameDetours();
		SharedHookManager::Instance().DispatchHook<void *>(
			"GameDllLoaded", SharedHookPhase::Post, static_cast<void *>(ge));
	}

	return ge;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;

	if (reason == DLL_PROCESS_ATTACH) {
		g_hShimModule = reinterpret_cast<HMODULE>(inst);
		return TRUE;
	}
	if (reason == DLL_PROCESS_DETACH)
		GetDetourSystem().RemoveGameDetours();

	return TRUE;
}
