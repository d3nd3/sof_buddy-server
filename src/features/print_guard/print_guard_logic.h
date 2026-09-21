#pragma once

#include <cstdio>
#include <cstdarg>

// Engine stack buffers (IDA SoF.exe / SoF-spsv.exe @ 0x20000000).
constexpr int kPgBroadcastBuf = 0x800;  // SV_BroadcastPrintf
constexpr int kPgClientBuf = 0x400;     // PF_cprintf, PF_clprintf, SV_ClientPrint*, SV_BroadcastCommand
constexpr int kPgBroadcastChars = kPgBroadcastBuf - 1;
constexpr int kPgClientChars = kPgClientBuf - 1;

// SoFPlus scripter defaults: max #~cvar payload after macro expand onto the command line.
// 1023-char line minus fixed prefix (IDA spsv.dll sp_sv_print_* handlers).
constexpr int kPgSpSvPrintBroadcastPrefix = 24;  // sp_sv_print_broadcast "
constexpr int kPgSpSvPrintClientPrefix = 23;       // sp_sv_print_client 0 " (slot 0-9)
constexpr int kPgSofplusBroadcastChars = kPgClientChars - kPgSpSvPrintBroadcastPrefix;  // 999
constexpr int kPgSofplusClientChars = kPgClientChars - kPgSpSvPrintClientPrefix;        // 1000

inline void Pg_Format(char* dst, int cap, const char* fmt, va_list ap) {
    if (!dst || cap <= 0)
        return;
    if (!fmt) {
        dst[0] = '\0';
        return;
    }
    vsnprintf(dst, static_cast<size_t>(cap), fmt, ap);
}
