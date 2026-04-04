#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Call once from GetGameApi(import) before the game runs. */
void Buddy_BindGameImport(void* import);

/** 1 if _sofbuddy_custom_respawn is non-zero (custom CTF spawn), else 0 (stock). */
int Buddy_CustomRespawnEnabled(void);

#ifdef __cplusplus
}
#endif
