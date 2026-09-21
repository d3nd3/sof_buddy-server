#pragma once

#include <windows.h>

namespace tickpace {

// WinMain @0x20066300 (dedicated path only) does `push 1 / call ds:Sleep`
// at 0x2006639D/0x2006639F and returns to 0x200663A5. On hosts with a coarse
// Sleep quantum that 1ms sleep can cost a whole quantum, waking the loop up
// after the tick boundary it was supposed to hit.
//
// The old answer was SpinToBoundary: busy-wait inside SvFramePre. That holds
// the main thread for up to 20ms *after* Cbuf_Execute and *before*
// SV_ReadPackets, so client packets queue the whole time and arrive clumped.
// This gate does what the request actually wants instead: when the tick is
// due within `_sofbuddy_tickpace_spin_ms`, WinMain's Sleep(1) is skipped and
// the loop iterates as usual - packets keep being read every iteration.
//
// Only that one call site is ever skipped (checked via return address), and
// only while a map runs on a dedicated server. Three independent reasons, in
// order: drain-boundary one-shot (`_sofbuddy_tickpace`), parked backlog
// (`cmdtext_parking` armed), spin window (`_sofbuddy_tickpace &&
// _sofbuddy_tickpace_spin_ms` > 0). An empty park sleeps again.

void SleepGate_Install();
void SleepGate_Remove();

// Pure decision half of the gate (no _ReturnAddress inside, so the host
// harness can drive it). `retaddr` is the caller's return address.
bool SleepGate_ShouldSkip(DWORD ms, void* retaddr);

// The return address the gate recognises (module base + WinMain Sleep site).
// Null when the engine image is not resolved.
void* SleepGate_WinMainSleepRet();

}  // namespace tickpace
