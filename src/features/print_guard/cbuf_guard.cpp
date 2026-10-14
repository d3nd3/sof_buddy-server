#include "cbuf_guard.h"
#include "cvar.h"
#include "print_guard_logic.h"

#include "generated_detours.h"
#include "generated_engine_pointers.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace {

constexpr unsigned kRvaCmdTextCursize = 0x23F830;
constexpr unsigned kRvaCmdTextData = 0x23F828;
constexpr unsigned kRvaCmdWait = 0x23F838;
constexpr unsigned kRvaCbufQuotes = 0x243848;
HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

template <typename T>
T* Rva(unsigned rva) {
    HMODULE h = ExeMod();
    return h ? reinterpret_cast<T*>(reinterpret_cast<char*>(h) + rva) : nullptr;
}

void ExecString(char* line) {
    detour_Cmd_ExecuteString::hkCmd_ExecuteString(line);
}

}  // namespace

void Pg_SafeCbufExecute(void (*stock_cbuf)()) {
    (void)stock_cbuf;

    auto* cursize = Rva<std::int32_t>(kRvaCmdTextCursize);
    auto** data = Rva<unsigned char*>(kRvaCmdTextData);
    auto* cmdWait = Rva<std::int32_t>(kRvaCmdWait);
    auto* quotes = Rva<std::int32_t>(kRvaCbufQuotes);
    if (!cursize || !data || !cmdWait || !quotes)
        return;

    *quotes = 0;
    char line[kPgClientBuf];

    while (*cursize > 0) {
        const int total = *cursize;
        unsigned char* buf = *data;
        if (!buf) {
            *cursize = 0;
            break;
        }

        int q = 0;
        int len = 0;
        while (len < total) {
            const unsigned char c = buf[len];
            if (c == '"')
                ++q;
            if ((q & 1) == 0 && (c == ';' || c == '\n'))
                break;
            ++len;
        }

        const int copy = len < Pg_CbufMax() ? len : Pg_CbufMax();
        if (copy > 0)
            std::memcpy(line, buf, static_cast<std::size_t>(copy));
        line[copy] = '\0';

        if (len >= total) {
            *cursize = 0;
        } else {
            const int skip = len + 1;
            const int remain = total - skip;
            if (remain > 0)
                std::memmove(buf, buf + skip, static_cast<std::size_t>(remain));
            *cursize = remain > 0 ? remain : 0;
        }

        ExecString(line);

        if (*cmdWait) {
            *cmdWait = 0;
            break;
        }
    }
}

void Pg_SafeCbufExecuteText(char* text) {
    auto* cursize = Rva<std::int32_t>(kRvaCmdTextCursize);
    auto** data = Rva<unsigned char*>(kRvaCmdTextData);
    if (!cursize || !data)
        return;

    const int saved = *cursize;
    unsigned char* heapSave = nullptr;
    if (saved > 0 && *data) {
        heapSave = static_cast<unsigned char*>(std::malloc(saved));
        if (heapSave) {
            std::memcpy(heapSave, *data, static_cast<std::size_t>(saved));
            *cursize = 0;
        }
    }

    if (text && text[0]) {
        char line[kPgClientBuf];
        std::size_t n = std::strlen(text);
        if (n >= static_cast<std::size_t>(Pg_CbufMax()))
            n = static_cast<std::size_t>(Pg_CbufMax());
        std::memcpy(line, text, n);
        line[n] = '\0';
        ExecString(line);
    }

    Pg_SafeCbufExecute(nullptr);

    if (heapSave && saved > 0 && *data) {
        std::memcpy(*data, heapSave, static_cast<std::size_t>(saved));
        *cursize = saved;
    }
    std::free(heapSave);
}
