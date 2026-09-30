#pragma once

/** Creates the profiles feature's cvars. Bootstrap-only (GameDllLoaded). */
void Profiles_InitCvars();

/** Master switch cvar (_sofbuddy_profiles). Read live on every hook/command. */
bool Profiles_IsEnabled();

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
