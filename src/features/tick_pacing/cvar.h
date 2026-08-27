#pragma once

namespace tickpace {

/** Tunables, read once per engine iteration (Qcommon_frame). Every one of them
 *  is live-settable from the console, so a server can back the feature out
 *  without a restart. */
struct Config {
    bool  enabled       = false;  // _sofbuddy_tickpace
    float spinMs        = 0.0f;   // _sofbuddy_tickpace_spin_ms
    float reserveMs     = 3.0f;   // _sofbuddy_tickpace_reserve_ms
    float deferMaxMs    = 200.0f; // _sofbuddy_tickpace_defer_max_ms
    bool  dedicated     = false;  // engine `dedicated` cvar, snapshotted here
};

/** Creates every cvar. Bootstrap-only (GameDllLoaded). */
void InitCvars();

/** Snapshot of the tunables. Cheap (four pointer derefs). */
Config ReadConfig();

/** Publishes the output cvars. Called at most once per server tick. */
void SetOutputs(float lateAvgMs, float lateMaxMs, long long saved,
                float cbufMaxMs, long long defers);

}  // namespace tickpace

/** Detach-time teardown (extern "C" for the shim's DllMain path, like
 *  ClampMonitor_Shutdown): hands the engine back ownership of every
 *  cvar_t.string this feature repointed at its own storage. Mandatory - spsv
 *  FreeLibrary/reloads this DLL and the engine keeps the cvar_t forever. */
extern "C" {
void TickPacing_Shutdown();
}
