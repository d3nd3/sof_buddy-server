#pragma once

// Tightens COM_Parse's com_token bound: at most 255 chars stored
// (indices 0..254) with the NUL at [255], so the terminator can never land
// at [256] — one past the 256-byte buffer. See parse_guard.cpp.
bool Pg_InstallParseGuard();
