#pragma once

// cvars for sv_tracktime. Settings are ARCHIVE and read live; the three
// gauges are NOSET outputs the feature publishes (see README).

/** Creates the cvars. Bootstrap-only (GameDllLoaded). */
void SvTracktime_InitCvars();

/** Master switch (default 1). */
bool SvTracktime_Enabled();

/** Allowed |drift| in percent before a slot is called out (default 20).
 *  0 means "call out any deviation". */
float SvTracktime_Tolerance();

/** Rolling window depth in samples (default 256 = kRing). */
int SvTracktime_Window();

/** Samples required in the window before any verdict (default 30). */
int SvTracktime_MinSamples();

/** Publish the gauges: usercmds tracked since boot (spawned slots only), how
 *  many slots are currently called out, and the worst |drift| among them. */
void SvTracktime_SetOutputs(long long frames, int suspect_slots, double worst_drift);

/** Hands the engine back every cvar_t.string this feature repointed at its own
 *  storage. Mandatory: spsv FreeLibrary/reloads this DLL and the engine keeps
 *  the cvar_t forever. */
extern "C" void SvTracktime_Shutdown();
