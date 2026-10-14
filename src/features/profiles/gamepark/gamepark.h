#pragma once

// gamepark (inside profiles): gamepark claim follows admin map changes.
//
// When a signed-in server admin (privilege 0) or map admin (privilege 1)
// changes the map with a sofplus `.rcon` command, the transfer records intent
// at command time and acts only when the requested map really loads: the
// issuer becomes the signed-in gamepark holder
// (`_spf_sv_gamepark_claimed*` point at them) and any previous holder's rcon
// session (`_spf_sv_rcon_username/ip/commands_$slot`) is logged out. Failed
// attempts (no rcon auth, ungranted command, bad map) never reach a map load,
// so they transfer nothing. This header is the narrow seam minigames calls
// into — the rest lives in gamepark.cpp + gamepark_logic.h.

/// GameDllLoaded entry (wired via gamepark/callbacks/callbacks.json).
void gamepark_OnGameDllLoaded(void* gameExport);

/// SV_Frame Post entry (wired via gamepark/hooks/hooks.json). Completes a
/// pending transfer once the requested map loads; drops stale pendings
/// (timeout, or the map changed to something else). Cheap when idle: one
/// pending-flag check per frame, plus a mapname read only while pending.
void gamepark_SvFramePost(int msec);

/// ClientCommand observer. Runs BEFORE sofplus (outer spsv detour runs
/// first, same as the stats filter). Records transfer intent for
/// `.rcon map|gamemap <name>` issued by privilege 0/1 slots and always
/// returns false so sofplus handles the command itself — rcon auth, grants,
/// and the actual map change stay entirely sofplus-side.
/// slot1 is 1-based (0 when unresolvable). words/splitN cover the say-box
/// (".rcon map q2dm1") form; the direct console form is read from argv.
bool Gamepark_HandleClientCommand(void* ent, int slot1, const char* cmd, int splitN,
                                  const char* const* words);

/// Master switch (_sofbuddy_gamepark, default 1). When off, nothing is
/// recorded and no transfer ever fires.
bool Gamepark_IsEnabled();
