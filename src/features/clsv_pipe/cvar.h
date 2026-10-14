#pragma once

/** Creates the clsv feature's cvars. Bootstrap-only (GameDllLoaded). */
void Clsv_InitCvars();

/** Master switch (_sofbuddy_clsv). Read live on every command/tick. */
bool Clsv_IsEnabled();

/** Live-tunable pipeline settings (see README for the bandwidth math). */
int Clsv_Lanes();        // _sofbuddy_clsv_lanes   (1..90, default 4)
int Clsv_PerTick();      // _sofbuddy_clsv_per_tick (1..12, default 2)
int Clsv_Interval();     // _sofbuddy_clsv_interval (frames, default 1)
int Clsv_MaxEsc();       // _sofbuddy_clsv_chunk    (default 200)
bool Clsv_AutoSend();    // _sofbuddy_clsv_auto     (default 1)
unsigned Clsv_AckWaitMs();  // _sofbuddy_clsv_ack_wait_ms (default 10000)

/** Download unlock master (_sofbuddy_pipe_dl, default 0). Read live. */
bool DlUnlock_Enabled();

/** Publish the pipe.dl allowed-download gauge (NOSET output). */
void Clsv_SetDlAllowed(long long allowed);

/** Publish delivery gauges (NOSET outputs, written directly). */
void Clsv_SetOutputs(long long sent, long long jobs, long long errors);

/** Detach-time cvar teardown (called from Clsv_Shutdown in clsv.cpp). */
void Clsv_ShutdownCvars();

/** Detach-time teardown: restores the ClientCommand hook, drops jobs, and
 *  hands the engine back ownership of every cvar_t.string this feature
 *  repointed. */
extern "C" {
void Clsv_Shutdown();
}
