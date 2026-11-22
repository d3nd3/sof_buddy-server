#pragma once

// Settings for the server-side state-demo recorder. All are ARCHIVE and read
// live; there are no output cvars because recording is a console-driven
// action, not a per-frame gauge.

/** Creates the cvars. Bootstrap-only (GameDllLoaded). */
void ServerDemo_InitCvars();

/** `_sofbuddy_svdemo`: master switch. Recording cannot start while this is 0. */
bool ServerDemo_Enabled();

/** `_sofbuddy_svdemo_auto`: start a new file automatically on each map. */
bool ServerDemo_AutoEnabled();

/** `_sofbuddy_svdemo_entities`: include the player edict's entity_state_t. */
bool ServerDemo_EntitiesEnabled();

/** `_sofbuddy_svdemo_interval`: record every Nth server frame (1 = every tick). */
int ServerDemo_Interval();

/** `_sofbuddy_svdemo_max_mb`: per-file size cap in MiB (0 = unlimited). */
unsigned long long ServerDemo_MaxBytes();

/** `_sofbuddy_svdemo_dir`: directory under the engine userdir. Never empty. */
const char* ServerDemo_Dir();
