/*
 * Server-side gamex86 shim for SOF Buddy.
 *
 * Thin wrapper: loads the stock game logic from base\oldgamex86.dll, forwards
 * GetGameAPI to it, and hosts the data-driven detour pipeline (detours.yaml
 * plus per-feature hooks/pointers/callbacks under src/features/<name>/,
 * see docs/DETOUR_SYSTEM.md).
 */

#include <windows.h>
#include <cstring>

#include "buddy_import.h"
#include "detours.h"
#include "log.h"
#include "engfuncs.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "shared_hook_manager.h"

typedef game_export_t *(*lpfn_GetGameAPI)(game_import_t *);

/* Per-feature detach entry points. Each is compiled only when its feature is
 * enabled in src/features/features.yaml, so both the declaration and the call
 * are gated on the SOF_FEATURE_<NAME> definition CMake derives from that same
 * file. Every one of these has to run before this image is unmapped - spsv
 * FreeLibrary/reloads this DLL between game restarts. */
#ifdef SOF_FEATURE_CLAMP_MONITOR
/* returns every cvar_t.string it repointed to the engine, which owns the
 * cvar_t past this DLL generation. */
extern "C" void ClampMonitor_Shutdown(void);
#endif

#ifdef SOF_FEATURE_HASH_LOOKUP
/* puts the engine's own scan-loop bytes back. The JMPs it writes point into
 * this image. */
extern "C" void HashLookup_Shutdown(void);
#endif

#ifdef SOF_FEATURE_ZPOOL
/* hands every cached zone block back to the engine's CRT. */
extern "C" void ZPool_Shutdown(void);
#endif

#ifdef SOF_FEATURE_TICK_PACING
/* returns its cvar_t.string pointers. */
extern "C" void TickPacing_Shutdown(void);
#endif

#ifdef SOF_FEATURE_CBUF_INSERT
/* ditto. */
extern "C" void CbufInsert_Shutdown(void);
#endif

static HMODULE g_hShim = nullptr;
static HMODULE g_hGameDll = nullptr;
static lpfn_GetGameAPI g_pfnGetGameAPI = nullptr;

/*
 * The stock gamex86.dll exports exactly one symbol: GetGameAPI.
 * Anything else in the export table (GetGameApi, Buddy_*, ...) means the
 * candidate is another shim build - loading it would re-enter this file and
 * recurse until the stack overflows.
 */
static bool ExportsOnlyGetGameAPI(HMODULE mod)
{
	const auto *dos = static_cast<const IMAGE_DOS_HEADER *>(static_cast<const void *>(mod));
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;

	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
		reinterpret_cast<const char *>(mod) + dos->e_lfanew);
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
		return false;

	const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	const DWORD size = nt->OptionalHeader.SizeOfImage;
	if (!dir.Size || dir.VirtualAddress > size || dir.VirtualAddress + dir.Size > size)
		return false;

	const char *base = static_cast<const char *>(static_cast<const void *>(mod));
	const auto *exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
	const auto *names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
	if (!exports->NumberOfNames ||
	    exports->AddressOfNames > size ||
	    exports->AddressOfNames + exports->NumberOfNames * sizeof(DWORD) > size)
		return false;

	bool hasGetGameAPI = false;
	for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
		if (names[i] >= size)
			return false;
		if (strcmp(base + names[i], "GetGameAPI") == 0)
			hasGetGameAPI = true;
		else
			return false;
	}
	return hasGetGameAPI;
}

/* Load base\oldgamex86.dll (next to this shim) and bind its GetGameAPI. */
static bool LoadStockGameDll()
{
	char path[MAX_PATH] = {0};
	if (!g_hShim || !GetModuleFileNameA(g_hShim, path, MAX_PATH))
		return false;

	char *slash = strrchr(path, '\\');
	if (!slash)
		return false;
	strcpy(slash + 1, "oldgamex86.dll");

	HMODULE mod = LoadLibraryA(path);
	if (!ExportsOnlyGetGameAPI(mod)) {
		if (mod)
			FreeLibrary(mod);
		return false;
	}

	g_hGameDll = mod;
	g_pfnGetGameAPI = reinterpret_cast<lpfn_GetGameAPI>(
		reinterpret_cast<void *>(GetProcAddress(mod, "GetGameAPI")));
	return true;
}

static game_export_t *ForwardGetGameAPI(game_import_t *import)
{
	static bool busy = false; /* re-entrancy brake */
	static bool bootstrapped = false;

	if (busy || !import)
		return nullptr;

	Buddy_BindGameImport(import);

	if ((!g_pfnGetGameAPI && !LoadStockGameDll()) || !g_pfnGetGameAPI)
		return nullptr;

	busy = true;
	game_export_t *ge = g_pfnGetGameAPI(import);
	busy = false;

	if (!ge || ge->apiversion <= 0 || ge->apiversion > 100)
		return nullptr;

	/* Bootstrap once per DLL generation. spsv may FreeLibrary/reload us between
	 * game restarts; DllMain detach strips the patches so the next generation
	 * finds clean code (and no JMP into freed trampolines). */
	if (!bootstrapped) {
		bootstrapped = true;

		GetDetourSystem().ProcessDeferredRegistrations();
		RegisterAllFeatureHooks();
		RegisterPointerOnlyFunctions_SofExe();
		RegisterPointerOnlyFunctions_RefDll();
		RegisterPointerOnlyFunctions_PlayerDll();
		RegisterPointerOnlyFunctions_Unknown();
		RegisterPointerOnlyFunctions_GameDll();
		EnginePointers_Bind();

		PrintOut(PRINT_LOG, "[shim] bootstrap: applying detours\n");
		GetDetourSystem().ApplyExeDetours();
		GetDetourSystem().ApplyGameDetours();
		SharedHookManager::Instance().DispatchHook<void *>(
			"GameDllLoaded", SharedHookPhase::Post, static_cast<void *>(ge));
		PrintOut(PRINT_LOG, "[shim] bootstrap complete\n");
	}

	return ge;
}

extern "C" __declspec(dllexport) game_export_t *__cdecl GetGameAPI(game_import_t *import)
{
	return ForwardGetGameAPI(import);
}

extern "C" __declspec(dllexport) HMODULE Buddy_GetGameDllHandle(void)
{
	return g_hGameDll;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		g_hShim = inst;
	else if (reason == DLL_PROCESS_DETACH && g_hGameDll)
	{
		/* spsv FreeLibrary/reloads this DLL across game restarts. Undo our
		 * patches now, otherwise the next generation inherits JMPs into
		 * trampolines freed with this instance. */
		GetDetourSystem().RemoveAllDetours();
#ifdef SOF_FEATURE_ZPOOL
		ZPool_Shutdown();
#endif
#ifdef SOF_FEATURE_HASH_LOOKUP
		HashLookup_Shutdown();
#endif
#ifdef SOF_FEATURE_CLAMP_MONITOR
		ClampMonitor_Shutdown();
#endif
#ifdef SOF_FEATURE_TICK_PACING
		TickPacing_Shutdown();
#endif
#ifdef SOF_FEATURE_CBUF_INSERT
		CbufInsert_Shutdown();
#endif
		g_hGameDll = nullptr;
		g_pfnGetGameAPI = nullptr;
	}
	return TRUE;
}
