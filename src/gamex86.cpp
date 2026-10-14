/*
 * Server-side gamex86 shim for SOF Buddy.
 *
 * Thin wrapper: loads the stock game logic from base\oldgamex86.dll, forwards
 * GetGameAPI to it, and hosts the data-driven detour pipeline (detours.yaml
 * plus per-feature hooks/pointers/callbacks under src/features/<name>/,
 * see docs/DETOUR_SYSTEM.md).
 */

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "buddy_import.h"
#include "detours.h"
#include "engfuncs.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "log.h"
#include "shared_hook_manager.h"
#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
#include "features/cpu_optimizations/cmdtext_parking/cmdpark.h"
#endif

typedef game_export_t *(*lpfn_GetGameAPI)(game_import_t *);

/* Per-feature detach entry points. Each is compiled only when its feature is
 * enabled in src/features/features.yaml, so both the declaration and the call
 * are gated on the SOF_FEATURE_<NAME> definition CMake derives from that same
 * file. Every one of these has to run before this image is unmapped - spsv
 * FreeLibrary/reloads this DLL between game restarts. */
#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
extern "C" void ClampMonitor_Shutdown(void);
extern "C" void HashLookup_Shutdown(void);
extern "C" void ZPool_Shutdown(void);
extern "C" void TickPacing_Shutdown(void);
extern "C" void CmdPark_Shutdown(void);
extern "C" void CbufInsert_Shutdown(void);
extern "C" void CmdCost_Shutdown(void);
extern "C" void Recvbuf_Shutdown(void);
#endif

#ifdef SOF_FEATURE_STUFFTEXT
/* returns its output cvar_t.string pointers. */
extern "C" void StuffText_Shutdown(void);
#endif
#ifdef SOF_FEATURE_MINIGAMES
extern "C" void Minigames_Shutdown(void);
void lag_NoteGameEvent(const char* tag);
#endif
#ifdef SOF_FEATURE_CLSV_PIPE
extern "C" void Clsv_Shutdown(void);
#endif
#ifdef SOF_FEATURE_PROFILES
extern "C" void Profiles_Shutdown(void);
#endif
#ifdef SOF_FEATURE_RELIABLE_DEFER
extern "C" void RelDef_Shutdown(void);
#endif
#ifdef SOF_FEATURE_SV_TRACKTIME
extern "C" void SvTracktime_Shutdown(void);
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

/* SV_Map calls SV_InitGame only when sv.state is 0. The old hybrid patch
 * removes map's unconditional state reset, so ordinary map/gamemap changes
 * keep this shim loaded. These five cvars are CVAR_LATCH; when one is pending,
 * the stock restart is required (maxclients/maxentities are allocations). */
static constexpr uintptr_t kSvState = 0x203A1F20u;
static constexpr uintptr_t kMapStateReset = 0x20061563u;
static constexpr uintptr_t kSvMapEntry = 0x2005DF20u;

static bool HasLatchedCvar(const char *name, const char *default_value)
{
	void *cv = Buddy_GetEngineCvar(name, default_value, 0, nullptr);
	return cv && *reinterpret_cast<const char *const *>(
		reinterpret_cast<const char *>(cv) + 8);
}

static bool MapNeedsServerRestart()
{
	static const struct {
		const char *name;
		const char *default_value;
	} cvars[] = {
		{"maxclients", "4"},
		{"deathmatch", "0"},
		{"maxentities", "1024"},
		{"skill", "2"},
		{"cheats", "0"},
	};
	for (const auto &cvar : cvars)
		if (HasLatchedCvar(cvar.name, cvar.default_value))
			return true;
	return false;
}

/* Called before the original SV_Map starts. */
extern "C" void __cdecl PrepareMapTransition()
{
	if (MapNeedsServerRestart())
		*reinterpret_cast<volatile int *>(kSvState) = 0;
}

using SvMapFn = void (__cdecl *)(int, char *, int);
extern "C" {
SvMapFn g_origSvMap = nullptr;
}

static bool WriteCode(void *address, const void *bytes, std::size_t size)
{
	DWORD old = 0;
	if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old))
		return false;
	std::memcpy(address, bytes, size);
	VirtualProtect(address, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), address, size);
	return true;
}

/*
 * This must tail-jump to the DetourXS trampoline. A normal C++ call would
 * return into this DLL after SV_Map invokes SV_InitGameProgs and unloads it.
 */
#if defined(__i386__)
extern "C" void __cdecl SvMapCvarHook()
	__attribute__((naked, noinline));
extern "C" void __cdecl SvMapCvarHook()
{
	__asm__(
		"pushal\n"
		"call _PrepareMapTransition\n"
		"popal\n"
		"movl _g_origSvMap, %eax\n"
		"jmp *%eax\n");
}
#else
extern "C" void __cdecl SvMapCvarHook(int attractloop, char *map, int loadgame)
{
	PrepareMapTransition();
	if (g_origSvMap)
		g_origSvMap(attractloop, map, loadgame);
}
#endif

static const struct SvMapCvarHookRegistration {
	SvMapCvarHookRegistration()
	{
		GetDetourSystem().RegisterDetour(
			reinterpret_cast<void *>(kSvMapEntry),
			reinterpret_cast<void *>(&SvMapCvarHook),
			reinterpret_cast<void **>(&g_origSvMap),
			"SvMapCvarHook", DetourModule::SofExe, 0);
	}
} g_svMapCvarHookRegistration;

/* map @ 0x20061563 clears sv.state. Leave that reset suppressed for ordinary
 * map changes; SvMapCvarHook restores the stock restart decision when a
 * latched allocation/game cvar requires it. */
static void KeepMapInProcess()
{
	auto *map_site = reinterpret_cast<unsigned char *>(kMapStateReset);
	static const unsigned char map_orig[10] = {
		0xC7, 0x05, 0x20, 0x1F, 0x3A, 0x20, 0x00, 0x00, 0x00, 0x00};
	static const unsigned char nops[10] = {
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	if (std::memcmp(map_site, map_orig, sizeof(map_orig)) != 0 &&
		std::memcmp(map_site, nops, sizeof(nops)) != 0) {
		PrintOut(PRINT_BAD, "map reload skip: unexpected bytes at 0x20061563\n");
		return;
	}

	if (std::memcmp(map_site, nops, sizeof(nops)) != 0 &&
		!WriteCode(map_site, nops, sizeof(nops))) {
		return;
	}
}

static void RestoreMapInProcess()
{
	auto *map_site = reinterpret_cast<unsigned char *>(kMapStateReset);
	static const unsigned char map_orig[10] = {
		0xC7, 0x05, 0x20, 0x1F, 0x3A, 0x20, 0x00, 0x00, 0x00, 0x00};
	static const unsigned char nops[10] = {
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	if (std::memcmp(map_site, nops, sizeof(nops)) == 0)
		WriteCode(map_site, map_orig, sizeof(map_orig));
}

/* dm (deathmatch_class_self @ oldgamex86 + 0x15C4D8). ExitLevel calls
 * vtable+0x30, ClearFlagCount: zeroes red/blue captures, ctf_loops_count and
 * ctf_flag_captured. That function also writes CS_CTF_BLUE_STAT / RED_STAT
 * (8, 9) as "d", which is the dropped-flag HUD, not "at home". Flags spawned
 * this level already published "h". Put "h" back after the count reset. */
static void ClearFlagCaptures()
{
	HMODULE game = GetModuleHandleA("oldgamex86.dll");
	if (!game)
		return;
	auto *dm = *reinterpret_cast<void **>(
		reinterpret_cast<unsigned char *>(game) + 0x15C4D8);
	if (!dm)
		return;
	auto **vt = *reinterpret_cast<void ***>(dm);
	reinterpret_cast<void (__thiscall *)(void *)>(vt[12])(dm);
	Buddy_Configstring(8, "h");
	Buddy_Configstring(9, "h");
}

using SpawnEntitiesFn = void (__cdecl *)(char *, const char *, char *);
static SpawnEntitiesFn g_origSpawnEntities = nullptr;

static void __cdecl SpawnEntities_ClearCaptures(char *map, const char *ents, char *spawn)
{
	if (g_origSpawnEntities)
		g_origSpawnEntities(map, ents, spawn);
#ifdef SOF_FEATURE_MINIGAMES
	lag_NoteGameEvent("mapspawn");
#endif
	ClearFlagCaptures();
}

static const struct SpawnEntitiesHook {
	SpawnEntitiesHook()
	{
		GetDetourSystem().RegisterDetour(
			reinterpret_cast<void *>(0xBDB50),
			reinterpret_cast<void *>(&SpawnEntities_ClearCaptures),
			reinterpret_cast<void **>(&g_origSpawnEntities),
			"SpawnEntities_ClearCaptures", DetourModule::GameDll, 0);
	}
} g_spawnEntitiesHook;

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
#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
		cmdpark::SetMapRestartProbe(&MapNeedsServerRestart);
#endif
		RegisterAllFeatureHooks();
		RegisterPointerOnlyFunctions_SofExe();
		RegisterPointerOnlyFunctions_RefDll();
		RegisterPointerOnlyFunctions_PlayerDll();
		RegisterPointerOnlyFunctions_Unknown();
		RegisterPointerOnlyFunctions_GameDll();
		EnginePointers_Bind();

		GetDetourSystem().ApplyExeDetours();
		KeepMapInProcess();
		GetDetourSystem().ApplyGameDetours();
		SharedHookManager::Instance().DispatchHook<void *>(
			"GameDllLoaded", SharedHookPhase::Post, static_cast<void *>(ge));
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
		RestoreMapInProcess();
		GetDetourSystem().RemoveAllDetours();
#ifdef SOF_FEATURE_CPU_OPTIMIZATIONS
		ZPool_Shutdown();
		HashLookup_Shutdown();
		ClampMonitor_Shutdown();
		TickPacing_Shutdown();
		CmdPark_Shutdown();
		CbufInsert_Shutdown();
		CmdCost_Shutdown();
		Recvbuf_Shutdown();
#endif
#ifdef SOF_FEATURE_STUFFTEXT
		StuffText_Shutdown();
#endif
#ifdef SOF_FEATURE_CLSV_PIPE
		// Before Minigames_Shutdown: both features swap the same
		// game_export_t ClientCommand slot (ge+0x34), installed
		// minigames-then-clsv, so unwind LIFO to end on the stock pointer.
		Clsv_Shutdown();
#endif
#ifdef SOF_FEATURE_MINIGAMES
		Minigames_Shutdown();
#endif
#ifdef SOF_FEATURE_PROFILES
		Profiles_Shutdown();
#endif
#ifdef SOF_FEATURE_RELIABLE_DEFER
		RelDef_Shutdown();
#endif
#ifdef SOF_FEATURE_SV_TRACKTIME
		SvTracktime_Shutdown();
#endif
		g_hGameDll = nullptr;
		g_pfnGetGameAPI = nullptr;
	}
	return TRUE;
}
