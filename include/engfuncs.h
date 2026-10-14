#pragma once

/*
 * Quake2-style game API types. The shim only needs the game_export_t prefix;
 * the full layouts live in the stock game DLL / engine SDK.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct game_import_s game_import_t;

typedef struct game_export_s {
	int apiversion;
	void (*Init)(void);
	void (*Shutdown)(void);
} game_export_t;

#ifdef __cplusplus
}
#endif

typedef game_export_t *(*lpfn_GetGameAPI)(game_import_t *import);
