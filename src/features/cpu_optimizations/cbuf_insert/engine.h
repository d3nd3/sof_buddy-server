#pragma once

// ---------------------------------------------------------------------------
// The console command buffer (SoF.exe / SoF-spsv.exe, base 0x20000000).
//
// cmd_text is a stock Quake 2 sizebuf_t over an 8KB static array:
// Cbuf_Init @0x20018160 is `SZ_Init(&cmd_text, &cmd_text_buf, 0x2000)`.
//
//   sizebuf_t: +0x00 allowoverflow  +0x04 overflowed  +0x08 data
//              +0x0C maxsize        +0x10 cursize     +0x14 readcount
//
// so with cmd_text at 0x2023F820 the three fields below fall where
// Cbuf_AddText/Cbuf_InsertText/Cbuf_Execute are seen reading them
// (0x2023F82C as maxsize @0x20018222, 0x2023F830 as cursize @0x20018530).
// cmd_wait @0x2023F838 sits just past readcount - a separate global, not part
// of the struct.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace cbufinsert {

constexpr unsigned kRvaCmdTextData    = 0x23F828;  // byte *cmd_text.data
constexpr unsigned kRvaCmdTextMaxsize = 0x23F82C;  // int   cmd_text.maxsize
constexpr unsigned kRvaCmdTextCursize = 0x23F830;  // int   cmd_text.cursize

struct EngineGlobals {
    unsigned char* const*   data    = nullptr;  // read only; the engine owns it
    const std::int32_t*     maxsize = nullptr;
    std::int32_t*           cursize = nullptr;
};

/** Resolves and validates the engine image once. False forever after (with a
 *  single log line) if it cannot be validated - the feature then delegates
 *  every call and touches nothing. */
bool EngineReady();
const EngineGlobals& Engine();

}  // namespace cbufinsert
