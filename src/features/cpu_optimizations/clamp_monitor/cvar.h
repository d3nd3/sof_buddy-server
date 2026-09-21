#pragma once

/** Creates the monitor's cvars. Bootstrap-only (GameDllLoaded). */
void ClampMonitor_InitCvars();

/** Notify threshold in ms (admin-tunable): log via dprintf. 0 = off. */
void* ClampMonitor_NotifyCvar();

/** Verbose lowclamp context lines via dprintf. 0 = off. */
void* ClampMonitor_LowDebugCvar();

/** Rolling window in seconds (admin-tunable). */
void* ClampMonitor_WindowCvar();

/** Broadcast threshold in ms (admin-tunable): 0 = disabled. When the rolling
 *  average meets/exceeds this, broadcast a lag notice to all clients. */
void* ClampMonitor_BroadcastThresholdCvar();

/** Minimum seconds between broadcast notices (admin-tunable). */
void* ClampMonitor_BroadcastIntervalCvar();

/** Publish counter / rolling average / last tick's loss / cumulative lost ms.
 *  Counters are `long long` so the printed cvar string stays exact; cvar_t
 *  only has a float `value`, so that half loses precision past 2^24. */
void ClampMonitor_SetOutputs(long long highclamps, float avg_ms, int last_ms, long long lost_ms);

/** Publish the lowclamp side: how many times the engine has jumped svs.realtime
 *  *forward* to sv.time - 100, how many SV_Frames were tested to find that out,
 *  the total ms it invented, and the worst single jump. Separate from
 *  SetOutputs because these are sampled on a different hook - lowclamp frames
 *  never reach G_RunFrame.
 *
 *  `checks` is not decoration: it is what makes a reported zero mean "measured
 *  zero" rather than "never measured". */
void ClampMonitor_SetLowOutputs(long long lowclamps, long long checks,
                                long long gained_ms, int worst_ms);

/** Quit the server on a strict violation (highclamp/lowclamp with strict on).
 *  Default 0: log one console line and keep running. */
bool ClampMonitor_StrictExit();

/** Append every recorded clamp to sof_buddy-highclamps.txt
 *  (`[timestamp] hostport=<port> <reason>`), independently of strict.
 *  Default 0 = off. */
bool ClampMonitor_LogFile();

/** The engine's own hostport value, for the violation log. "?" when unknown. */
const char* ClampMonitor_Hostport();

/** Shim-log the clamp gauges (strict-mode session summary). */
void ClampMonitor_LogSessionSummary();

/** Detach-time teardown (extern "C" for the shim's DllMain path): hands the
 *  engine back ownership of every cvar_t.string this feature repointed at its
 *  own storage. Mandatory - spsv FreeLibrary/reloads this DLL, and the engine
 *  keeps the cvar_t forever. */
extern "C" {
void ClampMonitor_Shutdown();
}
