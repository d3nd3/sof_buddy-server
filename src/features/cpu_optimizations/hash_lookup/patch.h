#pragma once

namespace hashlookup {

/** Verifies every patch site against the bytes it expects and, only if all of
 *  them match, redirects each engine scan loop into its stub. All-or-nothing:
 *  a single mismatch leaves the engine completely untouched. */
bool Install();

/** Puts the original bytes back and drops the index. Must run before this DLL
 *  is unmapped - spsv FreeLibrary/reloads it between games, and a jmp left
 *  pointing at a freed image is a guaranteed crash. */
void Revert();

}  // namespace hashlookup
