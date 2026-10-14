// Audit harness: one Cmd_AddCommand per hardening point (V1..V6) + state.
// Each handler audits a candidate stufftext string and prints ALLOW/BLOCK.
// Nothing here is sent to clients; use the real `stufftext` command for that.
//
// Console quoting matters: the engine splits unquoted ';' before our handler
// runs. Quote the candidate and escape control bytes:
//   stufftext_audit_v1 "password foo\x3bbar\n"
// Escapes: \n -> LF, \r -> CR, \\ -> backslash, \xHH -> byte.

#include "stufftext_audit.h"

#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace {

bool g_attackLoop = false;
bool g_inPrecache = false;
bool g_openQuote = false;

inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool IsSafeMemoryBlock(const void* ptr, size_t size) {
    if (!IsValidUserPointer(ptr) || size == 0)
        return false;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr + size < addr || addr + size > 0x7FFFFFFF)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    uintptr_t end =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + size <= end);
}

inline bool IsExec(const void* p) {
    if (!IsValidUserPointer(p))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD m = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                    PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & m) != 0;
}

bool CmdReady() {
    return detour_Cmd_Args::oCmd_Args &&
           IsExec(reinterpret_cast<const void*>(detour_Cmd_Args::oCmd_Args));
}

// Raw candidate from Cmd_Args (preserves spacing/quotes), unescaped into out.
// Returns false when no candidate was given.
bool FetchCandidate(char* out, size_t cap, size_t* outLen) {
    if (!CmdReady() || !out || cap == 0)
        return false;
    const char* raw = SOF_EP_Cmd_Args();
    if (!raw || !IsValidUserPointer(raw) || !IsSafeMemoryBlock(raw, 1))
        return false;
    size_t rawLen = 0;
    while (rawLen < 2048 && raw[rawLen])
        ++rawLen;
    if (rawLen == 0)
        return false;
    *outLen = StuffAudit_Unescape(out, cap, raw, rawLen);
    return *outLen > 0;
}

void Report(const char* tag, const StuffAuditResult& r, const char* s,
            size_t len) {
    // Log printable form with visible escapes so LF/CR don't split the log.
    char show[256];
    size_t p = 0;
    for (size_t i = 0; i < len && p + 4 < sizeof(show); ++i) {
        char c = s[i];
        if (c == '\n') {
            show[p++] = '\\';
            show[p++] = 'n';
        } else if (c == '\r') {
            show[p++] = '\\';
            show[p++] = 'r';
        } else if (c >= 32 && c < 127) {
            show[p++] = c;
        } else {
            show[p++] = '.';
        }
    }
    show[p] = '\0';
    Buddy_DebugPrintf("[%s] %s (%s) len=%u : %s\n", tag,
                      r.allow ? "ALLOW" : "BLOCK", r.reason,
                      static_cast<unsigned>(len), show);
    PrintOut(PRINT_LOG, "[%s] %s (%s): %s\n", tag,
             r.allow ? "ALLOW" : "BLOCK", r.reason, show);
}

}  // namespace

extern "C" void __cdecl stufftext_audit_v1_f() {
    char buf[1100];
    size_t n = 0;
    if (!FetchCandidate(buf, sizeof(buf), &n)) {
        Buddy_DebugPrintf("usage: stufftext_audit_v1 \"<candidate>\"\n");
        return;
    }
    Report("audit_v1_baseline", StuffAudit_V1(buf, n, g_attackLoop), buf, n);
}

extern "C" void __cdecl stufftext_audit_v2_f() {
    char buf[1100];
    size_t n = 0;
    if (!FetchCandidate(buf, sizeof(buf), &n)) {
        Buddy_DebugPrintf("usage: stufftext_audit_v2 \"<candidate>\"\n");
        return;
    }
    Report("audit_v2_strict",
           StuffAudit_V2_Strict(buf, n, g_attackLoop), buf, n);
}

extern "C" void __cdecl stufftext_audit_v3_f() {
    char buf[1100];
    size_t n = 0;
    if (!FetchCandidate(buf, sizeof(buf), &n)) {
        Buddy_DebugPrintf(
            "usage: stufftext_audit_v3 \"<candidate>\" (quote state persists; "
            "see stufftext_audit_state)\n");
        return;
    }
    Report("audit_v3_quote",
           StuffAudit_V3_QuoteCarry(buf, n, g_attackLoop, &g_openQuote), buf,
           n);
}

extern "C" void __cdecl stufftext_audit_v4_f() {
    char buf[1100];
    size_t n = 0;
    if (!FetchCandidate(buf, sizeof(buf), &n)) {
        Buddy_DebugPrintf("usage: stufftext_audit_v4 \"<candidate>\"\n");
        return;
    }
    Report("audit_v4_stategate",
           StuffAudit_V4_StateGated(buf, n, g_attackLoop, g_inPrecache), buf,
           n);
}

extern "C" void __cdecl stufftext_audit_v5_f() {
    char buf[1100];
    size_t n = 0;
    if (!FetchCandidate(buf, sizeof(buf), &n)) {
        Buddy_DebugPrintf("usage: stufftext_audit_v5 \"<candidate>\"\n");
        return;
    }
    Report("audit_v5_check", StuffAudit_V5_Check(buf, n), buf, n);
}

extern "C" void __cdecl stufftext_audit_v6_f() {
    char buf[1100];
    size_t n = 0;
    if (!FetchCandidate(buf, sizeof(buf), &n)) {
        Buddy_DebugPrintf("usage: stufftext_audit_v6 \"<candidate>\"\n");
        return;
    }
    Report("audit_v6_canon", StuffAudit_V6_Canonical(buf, n), buf, n);
}

extern "C" void __cdecl stufftext_audit_state_f() {
    // No args: show flags. Args: attack 0|1, precache 0|1, quote_reset.
    if (!detour_Cmd_Argc::oCmd_Argc ||
        !IsExec(reinterpret_cast<const void*>(detour_Cmd_Argc::oCmd_Argc)) ||
        !detour_Cmd_Argv::oCmd_Argv ||
        !IsExec(reinterpret_cast<const void*>(detour_Cmd_Argv::oCmd_Argv))) {
        Buddy_DebugPrintf("audit_state: engine arg accessors unavailable\n");
        return;
    }
    int argc = SOF_EP_Cmd_Argc();
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* k = SOF_EP_Cmd_Argv(i);
        const char* v = SOF_EP_Cmd_Argv(i + 1);
        if (!k || !v || !IsValidUserPointer(k) || !IsValidUserPointer(v))
            continue;
        if (strcmp(k, "attack") == 0)
            g_attackLoop = (v[0] == '1');
        else if (strcmp(k, "precache") == 0)
            g_inPrecache = (v[0] == '1');
    }
    for (int i = 1; i < argc; ++i) {
        const char* a = SOF_EP_Cmd_Argv(i);
        if (a && IsValidUserPointer(a) && strcmp(a, "quote_reset") == 0)
            g_openQuote = false;
    }
    Buddy_DebugPrintf(
        "[audit_state] attackLoop=%d inPrecache=%d openQuote=%d\n",
        g_attackLoop ? 1 : 0, g_inPrecache ? 1 : 0,
        g_openQuote ? 1 : 0);
}

