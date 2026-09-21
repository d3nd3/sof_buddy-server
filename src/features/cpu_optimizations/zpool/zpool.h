#pragma once

// Internals shared with the host-side test. The overrides themselves
// (zpool_Malloc / zpool_Free) are wired up by hooks.json.

namespace zpool {

/** Resolves the zone chain in the live engine image. */
bool Bind();

/** Same, against an explicit image base - lets the test drive a fake image. */
bool BindAt(unsigned char* base);

/** Returns every cached block to the engine's CRT. */
void Drain();

void SetActive(bool on);
bool Active();

}  // namespace zpool
