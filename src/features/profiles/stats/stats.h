#pragma once

// stats (inside profiles): admin-vouched per-player match statistics.
//
// The .upload_match request is the trust signal: only a signed-in server
// admin (profiles privilege 0) may trigger it, and the resulting scoreboard
// is treated as clean match data. This header is the narrow seam minigames
// calls into — the rest lives in stats.cpp + stats_logic.h.

/// GameDllLoaded entry (wired via stats/callbacks/callbacks.json).
void stats_OnGameDllLoaded(void* gameExport);

/// SV_Frame Post entry (wired via stats/hooks/hooks.json). Flushes a pending
/// upload once the map reaches intermission; drops stale pendings on map
/// change. Cheap: one bool + one float read when idle.
void stats_SvFramePost(int msec);

/// Ensure the mg_stats menu page is registered (idempotent). Called from the
/// minigames ClientCommand path and GameDllLoaded, like other pages.
void stats_EnsureRegistered();

/// Per-frame maintenance: closes the stats page when the slot loses its
/// admin profile (mirrors cmds/cvars pages). Called from MaintainLayoutClient.
void stats_MaintainForSlot(int slot1);

/// ClientCommand filter. Runs BEFORE sofplus (outer spsv detour runs first,
/// so our check wins). Returns true when the word was consumed (blocked or
/// menu-handled) and sofplus must NOT see it; false to let the original
/// sofplus .upload_match / .cancel_upload run (admin path — our pending is
/// recorded first, sofplus still does its discord screenshot + scheduling).
/// slot1 is 1-based (0 when unresolvable). words/splitN cover the say-box
/// ".upload_match" form; cmd covers the direct console form.
bool Stats_HandleClientCommand(void* ent, int slot1, const char* cmd, int splitN,
                               const char* const* words);

/// Master switch (_sofbuddy_stats, default 1). When off, the filter passes
/// everything through and no stats are recorded.
bool Stats_IsEnabled();

/// Lifetime totals for one 0-based slot's roster identity. False (with
/// outputs zeroed) when stats are off, the slot has no identity, or no match
/// was ever recorded for it. Used by the .mg landing so each player row can
/// carry its main stats.
bool Stats_LifetimeForSlot(int slot0, long long* matches, long long* frags);

/// Aggregate counters for the landing summary line. Zeroed when off.
void Stats_Summary(long long* matches, long long* players);
