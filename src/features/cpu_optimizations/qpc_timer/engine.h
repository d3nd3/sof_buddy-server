#pragma once

// ---------------------------------------------------------------------------
// Engine state this feature reads: the `curtime` global written by
// Sys_Milliseconds @0x20055930.
//
// SoF.exe / SoF-spsv.exe share the same engine .text and the image is not
// relocatable (it always lands at 0x20000000), but we still express this as
// an RVA and add the real module base at bind time - it costs nothing and stays
// correct if an image ever does get relocated.
//
// Stock (`0x20055930`):
//   if (!initialized) sys_ms_base = timeGetTime() & 0xFFFF0000;
//                                                     (write @0x20055951)
//   curtime = timeGetTime() - sys_ms_base;            (write @0x20055961)
// `sys_ms_base` is `0x20390C30`, `initialized` is `0x20390D40` (the only
// xrefs; an earlier draft used `0x20390D44` / `0x20390D48` and never ran).
// sys_ms_base itself is only used inside that function; we write curtime.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace qpctimer {

constexpr unsigned kRvaCurtime = 0x390D38;  // int curtime @0x20390D38

struct EngineGlobals {
    volatile std::uint32_t* curtime = nullptr;  // last Sys_Milliseconds
};

/** Resolves and validates the engine image once. False forever after (with a
 *  single log line) if the image can't be validated - the feature then stays
 *  completely inert (stock clock passthrough). */
bool EngineReady();

/** Only valid once EngineReady() has returned true. */
const EngineGlobals& Engine();

}  // namespace qpctimer
