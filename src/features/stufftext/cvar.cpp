#include "cvar.h"
#include "buddy_import.h"
#include "stufftext_reconnect.h"

#include <cstddef>
#include <cstdio>

namespace {

constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet = 8;

struct OutputCvar {
    void* cv = nullptr;
    char* original = nullptr;
    char buf[2][32] = {};
    int live = 0;

    void Bind(const char* name) {
        if (cv)
            return;
        cv = Buddy_GetEngineCvar(name, "0", kCvarFlagNoSet, nullptr);
        if (!cv)
            return;
        original = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    }

    void Publish(float value, const char* text) {
        if (!cv)
            return;
        char* base = static_cast<char*>(cv);
        *reinterpret_cast<volatile float*>(base + kCvarValueOfs) = value;
        char* spare = buf[1 - live];
        std::size_t i = 0;
        for (; text[i] && i + 1 < sizeof(buf[0]); ++i)
            spare[i] = text[i];
        spare[i] = '\0';
        *reinterpret_cast<char* volatile*>(base + kCvarStringOfs) = spare;
        live = 1 - live;
    }

    void Restore() {
        if (!cv || !original)
            return;
        *reinterpret_cast<char* volatile*>(static_cast<char*>(cv) + kCvarStringOfs) = original;
        cv = nullptr;
    }
};

void FormatI64(char* out, std::size_t n, long long v) {
    if (n == 0)
        return;
    char digits[24];
    std::size_t d = 0;
    unsigned long long mag = v < 0 ? 0ull - static_cast<unsigned long long>(v)
                                   : static_cast<unsigned long long>(v);
    do {
        digits[d++] = static_cast<char>('0' + (mag % 10ull));
        mag /= 10ull;
    } while (mag != 0ull && d < sizeof(digits));
    std::size_t i = 0;
    if (v < 0 && i + 1 < n)
        out[i++] = '-';
    while (d > 0 && i + 1 < n)
        out[i++] = digits[--d];
    out[i] = '\0';
}

OutputCvar g_outSent;    // _sofbuddy_stufftext_sent
OutputCvar g_outErrors;  // _sofbuddy_stufftext_errors

void* g_cvEnabled = nullptr;  // _sofbuddy_stufftext

}  // namespace

void StuffText_InitCvars() {
    g_outSent.Bind("_sofbuddy_stufftext_sent");
    g_outErrors.Bind("_sofbuddy_stufftext_errors");
    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_stufftext", "1", kCvarFlagArchive, nullptr);
}

void* StuffText_EnabledCvar() {
    return g_cvEnabled;
}

bool StuffText_IsEnabled() {
    if (!g_cvEnabled)
        return true;
    return Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
}

void StuffText_SetOutputs(long long sent, long long errors) {
    char text[32];
    FormatI64(text, sizeof(text), sent);
    g_outSent.Publish(static_cast<float>(sent), text);
    FormatI64(text, sizeof(text), errors);
    g_outErrors.Publish(static_cast<float>(errors), text);
}

void StuffText_Shutdown() {
    // Park a mid-flight reconnect chain first: restores attractloop so the
    // server never keeps refusing connects after this DLL goes away.
    StuffReconnect_OnDetach();
    g_outSent.Restore();
    g_outErrors.Restore();
}
