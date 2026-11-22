#pragma once

#include <cstdio>
#include <cstdarg>

// Engine stack buffers (retail disassembly SoF.exe / SoF-spsv.exe @ 0x20000000).
constexpr int kPgBroadcastBuf = 0x800;  // SV_BroadcastPrintf
constexpr int kPgClientBuf = 0x400;     // PF_cprintf, PF_clprintf, SV_ClientPrint*, SV_BroadcastCommand
constexpr int kPgBroadcastChars = kPgBroadcastBuf - 1;
constexpr int kPgClientChars = kPgClientBuf - 1;

// SoFPlus scripter defaults: max #~cvar payload after macro expand onto the command line.
// 1023-char line minus the fixed command+quoting overhead.
// Client slots are 0-99, so the prefix must reserve two digits:
//   sp_sv_print_broadcast "<payload>"       -> 24 bytes
//   sp_sv_print_client <0-99> "<payload>"   -> 24 bytes (two-digit slot)
constexpr int kPgSpSvPrintBroadcastPrefix = 24;
constexpr int kPgSpSvPrintClientPrefix = 24;
constexpr int kPgSofplusBroadcastChars = kPgClientChars - kPgSpSvPrintBroadcastPrefix;  // 999
constexpr int kPgSofplusClientChars = kPgClientChars - kPgSpSvPrintClientPrefix;        // 999

inline void Pg_Format(char* dst, int cap, const char* fmt, va_list ap) {
    if (!dst || cap <= 0)
        return;
    if (!fmt) {
        dst[0] = '\0';
        return;
    }
    vsnprintf(dst, static_cast<size_t>(cap), fmt, ap);
}
