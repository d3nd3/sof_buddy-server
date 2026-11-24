#pragma once

#include <cstddef>
#include <cstdint>

// Telemetry cvars. All are ARCHIVE and read live; the writer queue limit is
// read once at init because it sizes the in-memory bound.

/** Creates the telemetry cvars. Bootstrap-only (GameDllLoaded). */
void Telemetry_InitCvars();

/** `_sofbuddy_telemetry`: master switch. */
bool Telemetry_Enabled();

/** `_sofbuddy_telemetry_dir`: spool directory under the engine userdir. */
const char* Telemetry_Dir();

/** `_sofbuddy_telemetry_max_mb`: per-file cap in MiB (0 = unlimited). */
unsigned long long Telemetry_MaxBytes();

/** `_sofbuddy_telemetry_queue_kb`: bounded in-memory queue size. */
std::size_t Telemetry_QueueLimitBytes();

/** `_sofbuddy_telemetry_events`: bitmask for optional event families. */
int Telemetry_EventsMask();

/** `_sofbuddy_telemetry_sample_sec`: player sample cadence (0 = off). */
int Telemetry_SampleSeconds();

/** Bind an engine cvar. Bootstrap-only; do not call from a frame path. */
void* Telemetry_Cvar(const char* name, const char* value, int flags);

/** Safe reads from a cvar bound by Telemetry_Cvar. */
const char* Telemetry_CvarString(void* cv, const char* fallback);
float Telemetry_CvarFloat(void* cv, float fallback);
int Telemetry_CvarInt(void* cv, int fallback);
