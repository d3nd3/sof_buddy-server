#pragma once

namespace qpctimer {

void InitCvars();

/** `_sofbuddy_qpc` (default 1): QPC-backed Sys_Milliseconds. `0` passes the
 *  stock timeGetTime clock straight through. Independent of
 *  `_sofbuddy_cpuopt` / `_sofbuddy_tickpace` on purpose, so flipping those
 *  never jumps the clock both features share. */
bool UseQpcClock();

}  // namespace qpctimer
