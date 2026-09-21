#pragma once

// Called from cbuf_insert's Cbuf_InsertText override (only one override allowed).
namespace cmdpark {
bool Take(char* text);  // true if parked; caller must not write cmd_text

// Side-store backlog, in bytes. What the Sleep-skip gate (tick_pacing) reads
// at WinMain Sleep time: >0 means previous ticks' parked payload is still
// waiting for a drip. Same thread (main loop), plain read.
int ParkBytes();

// True when parking is armed (strict or reserve_ms > 0). The Sleep-skip gate
// only fast-drains a non-empty park while this holds.
bool HoldArmed();
}
