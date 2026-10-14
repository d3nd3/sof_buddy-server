// print_guard: bounded formatting for engine print paths (vsprintf into 0x400/
// 0x800 stack buffers) and safe Cbuf_Execute line extraction.

#include "cbuf_guard.h"
#include "cvar.h"
#include "macro_guard.h"
#include "parse_guard.h"
#include "print_guard_logic.h"

#include "detours.h"
#include "log.h"

#include <cstdarg>
#include <cstring>

namespace {

using bprintf_fn = void(__cdecl*)(int, const char*, ...);
using cprintf_fn = void(__cdecl*)(void*, int, const char*, ...);
using clprintf_fn = void(__cdecl*)(void*, void*, int, const char*, ...);
using bcommand_fn = void(__cdecl*)(const char*, ...);
using clientprint_fn = void(__cdecl*)(void*, int, const char*, ...);
using clientloc_fn = void(__cdecl*)(void*, int, int, const char*, ...);
using cbuf_fn = void(__cdecl*)();

bprintf_fn oBroadcast = nullptr;
cprintf_fn oCprintf = nullptr;
clprintf_fn oClprintf = nullptr;
bcommand_fn oBroadcastCmd = nullptr;
clientprint_fn oClientPrint = nullptr;
clientloc_fn oClientLocPrint = nullptr;
cbuf_fn oCbufExecute = nullptr;

void __cdecl hkBroadcast(int level, const char* fmt, ...) {
    if (!oBroadcast)
        return;
    char buf[kPgBroadcastBuf];
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, Pg_BroadcastMax() + 1, fmt, ap);
    va_end(ap);
    oBroadcast(level, "%s", buf);
}

void __cdecl hkCprintf(void* ent, int level, const char* fmt, ...) {
    if (!oCprintf)
        return;
    char buf[kPgClientBuf];
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, Pg_ClientMax() + 1, fmt, ap);
    va_end(ap);
    oCprintf(ent, level, "%s", buf);
}

void __cdecl hkClprintf(void* ent, void* from, int color, const char* fmt, ...) {
    if (!oClprintf)
        return;
    char buf[kPgClientBuf];
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, Pg_ClientMax() + 1, fmt, ap);
    va_end(ap);
    oClprintf(ent, from, color, "%s", buf);
}

void __cdecl hkBroadcastCmd(const char* fmt, ...) {
    if (!oBroadcastCmd)
        return;
    char buf[kPgClientBuf];
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, Pg_ClientMax() + 1, fmt, ap);
    va_end(ap);
    oBroadcastCmd("%s", buf);
}

void __cdecl hkClientPrint(void* cl, int level, const char* fmt, ...) {
    if (!oClientPrint)
        return;
    char buf[kPgClientBuf];
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, Pg_ClientMax() + 1, fmt, ap);
    va_end(ap);
    oClientPrint(cl, level, "%s", buf);
}

void __cdecl hkClientLocPrint(void* cl, int a, int b, const char* fmt, ...) {
    if (!oClientLocPrint)
        return;
    char buf[kPgClientBuf];
    va_list ap;
    va_start(ap, fmt);
    Pg_Format(buf, Pg_ClientMax() + 1, fmt, ap);
    va_end(ap);
    oClientLocPrint(cl, a, b, "%s", buf);
}

char* __cdecl hkMacroExpand(char* text) {
    return Pg_SafeMacroExpand(text);
}

#ifndef SOF_FEATURE_CPU_OPTIMIZATIONS
void hkCbufExecute() {
    Pg_SafeCbufExecute(oCbufExecute);
}
#endif

void Reg(void* addr, void* hook, void** orig, const char* name) {
    if (GetDetourSystem().IsDetourRegistered(name))
        return;
    GetDetourSystem().RegisterDetour(addr, hook, orig, name, DetourModule::SofExe, 0);
}

struct Init {
    Init() {
        Reg(reinterpret_cast<void*>(0x200618d0), reinterpret_cast<void*>(&hkBroadcast),
            reinterpret_cast<void**>(&oBroadcast), "pg_SV_BroadcastPrintf");
        Reg(reinterpret_cast<void*>(0x2005c2e0), reinterpret_cast<void*>(&hkCprintf),
            reinterpret_cast<void**>(&oCprintf), "pg_PF_cprintf");
        Reg(reinterpret_cast<void*>(0x2005c3b0), reinterpret_cast<void*>(&hkClprintf),
            reinterpret_cast<void**>(&oClprintf), "pg_PF_clprintf");
        Reg(reinterpret_cast<void*>(0x200619a0), reinterpret_cast<void*>(&hkBroadcastCmd),
            reinterpret_cast<void**>(&oBroadcastCmd), "pg_SV_BroadcastCommand");
        Reg(reinterpret_cast<void*>(0x200617f0), reinterpret_cast<void*>(&hkClientPrint),
            reinterpret_cast<void**>(&oClientPrint), "pg_SV_ClientPrint");
        Reg(reinterpret_cast<void*>(0x20061860), reinterpret_cast<void*>(&hkClientLocPrint),
            reinterpret_cast<void**>(&oClientLocPrint), "pg_SV_ClientLocPrint");
        Reg(reinterpret_cast<void*>(0x20018d60), reinterpret_cast<void*>(&hkMacroExpand),
            nullptr, "pg_Cmd_MacroExpandString");
#ifndef SOF_FEATURE_CPU_OPTIMIZATIONS
        Reg(reinterpret_cast<void*>(0x20018530), reinterpret_cast<void*>(&hkCbufExecute),
            reinterpret_cast<void**>(&oCbufExecute), "pg_Cbuf_Execute");
#endif
    }
};

const Init g_init;

}  // namespace

void pg_CmdPre(char*& text) {
    if (!text)
        return;
    const int max = Pg_CbufMax();
    const std::size_t n = std::strlen(text);
    if (n < static_cast<std::size_t>(max))
        return;
    static char safe[kPgClientBuf];
    std::memcpy(safe, text, static_cast<std::size_t>(max));
    safe[max] = '\0';
    text = safe;
}

void pg_OnGameDllLoaded(void*) {
    Pg_InitCvars();
    PrintOut(PRINT_LOG, "[print_guard] trampolines: broadcast=%p cprintf=%p\n",
             oBroadcast, oCprintf);
    if (!oBroadcast)
        PrintOut(PRINT_BAD, "[print_guard] SV_BroadcastPrintf hook missing\n");
    Pg_InstallParseGuard();
}
