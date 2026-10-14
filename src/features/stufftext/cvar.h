#pragma once

/** Creates the stufftext feature's cvars. Bootstrap-only (GameDllLoaded). */
void StuffText_InitCvars();

/** Master switch cvar (_sofbuddy_stufftext). Read live on every command. */
void* StuffText_EnabledCvar();

/** True when the stufftext server command is enabled (fail-open). */
bool StuffText_IsEnabled();

/** Publish delivery counters (NOSET outputs, written directly). */
void StuffText_SetOutputs(long long sent, long long errors);

/** Detach-time teardown: hands the engine back ownership of every
 *  cvar_t.string this feature repointed. Mandatory - spsv
 *  FreeLibrary/reloads this DLL (see clamp_monitor/cvar.cpp). */
extern "C" {
void StuffText_Shutdown();
}
