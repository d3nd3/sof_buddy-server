#pragma once

/*
 * Opaque Quake2-style game API types — full definitions live in the stock game DLL.
 * Keeps the shim buildable without pulling in the entire engine SDK tree.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct game_import_s game_import_t;
typedef struct game_export_s game_export_t;

#ifdef __cplusplus
}
#endif

typedef game_export_t *(*lpfn_GetGameApi)(game_import_t *import);
