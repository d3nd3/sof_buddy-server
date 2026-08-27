#pragma once

namespace cbufinsert {

/** Creates the cvars. Bootstrap-only (GameDllLoaded). */
void InitCvars();

/** _sofbuddy_cbuf_insert: 1 = in-place shift, 0 = delegate to the engine.
 *  Read per call (one pointer deref) so it can be A/B'd on a live server
 *  without a restart. Measurement runs either way. */
bool FastPathEnabled();

/** Publishes the counters. Throttled by the caller to ~10Hz. */
void SetOutputs(long long inserts, long long bytesShifted, int maxCursize,
                long long micros, long long delegated);

}  // namespace cbufinsert

/** Detach-time teardown (extern "C" for the shim's DllMain path): hands the
 *  engine back ownership of every cvar_t.string this feature repointed at its
 *  own storage. */
extern "C" {
void CbufInsert_Shutdown();
}
