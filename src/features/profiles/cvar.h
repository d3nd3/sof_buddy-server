#pragma once

/** Creates the profiles feature's cvars. Bootstrap-only (GameDllLoaded). */
void Profiles_InitCvars();

/** Master switch cvar (_sofbuddy_profiles). Read live on every hook/command. */
bool Profiles_IsEnabled();

/** 0-based slot has a roster profile signed in. When profiles are off, every slot passes. */
bool Profiles_SlotActive(int slot0);

/** Returns 0 server, 1 map, 2 user, or -1 for no signed-in profile. */
int Profiles_SlotPrivilege(int slot0);

/** Compatibility alias for older callers. */
int Profiles_SlotAdminLevel(int slot0);

/** Only privilege 0 can use the current server command/cvar pages. */
bool Profiles_IsLevel0Admin(int slot0);

/** Privilege 0 or 1: server admins plus map admins. Gates features map
 *  admins are trusted with (match-stats upload). Normal users (2) and
 *  guests (no profile) fail. */
bool Profiles_IsLevel01Admin(int slot0);

/** Per-profile menu preference, persisted in registry.cfg. */
bool Profiles_SlotHideTips(int slot0);
bool Profiles_SetSlotHideTips(int slot0, bool hide);

/** Per-profile scoreboard preference, persisted in registry.cfg.
 *  Off (default): score opens vanilla, score+use opens the new board.
 *  On: score opens the new board, score+use opens vanilla. */
bool Profiles_SlotSwapScoreboard(int slot0);
bool Profiles_SetSlotSwapScoreboard(int slot0, bool swap);

/** Player self-service login (`.login <guid>` in the client console,
 *  forwarded as say). Binds the caller's own slot when the guid is in the
 *  roster; prints usage/errors privately to the player, never to chat.
 *  Always consumes the word when profiles is on. slot1 is 1-based. */
bool Profiles_ClientLogin(void* ent, int slot1, const char* guid);

/** Player self-service logout (`.logout` in the client console). Unbinds
 *  the caller's own slot (with the usual goodbye broadcast); tells them
 *  privately when there was nothing to clear. Always consumes the word
 *  when profiles is on. slot1 is 1-based. */
bool Profiles_ClientLogout(void* ent, int slot1);

/** index-th signed-in slot (0-based). Writes registry nickname; optional live userinfo name. */
bool Profiles_SignedInAt(int index, int* slot0, char* nickname, int nicknameCap,
                         char* slotName = nullptr, int slotNameCap = 0);

/** Full guid + registry nickname for one 0-based slot. False when the slot
 *  has no roster identity. Used by the stats sub-feature (same DLL) to key
 *  per-player rows without duplicating slot state. */
bool Profiles_SlotGuid(int slot0, char* guidOut, int guidCap);
bool Profiles_SlotNickname(int slot0, char* nickOut, int nickCap);

/** Stufftext delivery switch (_sofbuddy_profiles_stufftext, default 1).
 *  0 = print the fix line instead of stuffing (manual mode, mirrors
 *  _prof_use_stufftext 0 in profiles.func). */
bool Profiles_UseStufftext();

/** Publish gauges (NOSET outputs, written directly). */
void Profiles_SetOutputs(long long registered, long long slots, long long errors);

/** Detach-time teardown: hands the engine back ownership of every
 *  cvar_t.string this feature repointed. */
void Profiles_ShutdownCvars();

/** Detach-time teardown: restores game_export_t slots and drops state. */
extern "C" {
void Profiles_Shutdown();
}
