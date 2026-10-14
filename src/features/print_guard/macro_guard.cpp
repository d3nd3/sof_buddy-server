#include "cvar.h"
#include "print_guard_logic.h"

#include <cstring>

#ifndef PG_CDECL
#if defined(_WIN32) || defined(__MINGW32__)
#define PG_CDECL __cdecl
#else
#define PG_CDECL
#endif
#endif

namespace {

constexpr unsigned kAbsCOM_Parse = 0x20055470;
constexpr unsigned kAbsCvarVariableString = 0x20021760;

using parse_fn = char*(PG_CDECL*)(char**);
using cvarstr_fn = char*(PG_CDECL*)(const char*);

parse_fn COM_Parse() {
    return reinterpret_cast<parse_fn>(kAbsCOM_Parse);
}

cvarstr_fn CvarStr() {
    return reinterpret_cast<cvarstr_fn>(kAbsCvarVariableString);
}

thread_local char g_out[kPgClientBuf];
thread_local int g_depth = 0;

int Append(char* dst, int pos, const char* src, int cap) {
    if (!src || pos >= cap - 1)
        return pos;
    while (*src && pos < cap - 1)
        dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

}  // namespace

char* Pg_SafeMacroExpand(char* text) {
    if (!text || !text[0])
        return text;
    if (!std::strchr(text, '#') && !std::strchr(text, '$'))
        return text;
    if (g_depth >= 8)
        return text;
    ++g_depth;

    int pos = 0;
    bool quote = false;
    char* p = text;
    const parse_fn parse = COM_Parse();
    const cvarstr_fn cvar = CvarStr();
    if (!parse || !cvar) {
        --g_depth;
        return text;
    }

    const int max = Pg_MacroMax();
    const int bufCap = max + 1;
    while (*p && pos < max) {
        const char c = *p;
        if (c == '"') {
            quote = !quote;
            g_out[pos++] = c;
            ++p;
            continue;
        }
        if (!quote && (c == '#' || c == '$')) {
            const bool hash = (c == '#');
            char* cur = p + 1;
            char* tok = parse(&cur);
            if (!tok || !tok[0]) {
                g_out[pos++] = c;
                ++p;
                continue;
            }
            const char* val = cvar(tok);
            if (!val)
                val = "";
            if (hash) {
                if (pos < max)
                    g_out[pos++] = '"';
                pos = Append(g_out, pos, val, bufCap);
                if (pos < max)
                    g_out[pos++] = '"';
            } else {
                pos = Append(g_out, pos, val, bufCap);
            }
            p = cur;
            continue;
        }
        g_out[pos++] = c;
        ++p;
    }
    g_out[pos] = '\0';
    --g_depth;
    return g_out;
}
