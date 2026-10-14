#pragma once

namespace recvbuf {

/** Creates the cvars. Bootstrap-only (GameDllLoaded). */
void InitCvars();

/** _sofbuddy_recvbuf_kb: requested SO_RCVBUF in KiB, live-readable so it can
 *  be A/B'd without a restart (applies to newly opened sockets immediately,
 *  and to existing sockets the next time the sweep runs). 0 = leave the
 *  engine alone entirely. Gated on the _sofbuddy_cpuopt master. */
int RequestedBytes();

/** Publishes the gauges (actual getsockopt value after the kernel clamps,
 *  and how many sockets were touched). */
void SetOutputs(int actualBytes, int sockets);

}  // namespace recvbuf

/** Detach-time teardown (extern "C" for the shim's DllMain path): hands the
 *  engine back ownership of every cvar_t.string this feature repointed at its
 *  own storage. */
extern "C" {
void Recvbuf_Shutdown();
}
