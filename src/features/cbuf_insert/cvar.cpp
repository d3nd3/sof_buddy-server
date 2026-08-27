#include "cvar.h"

#include "buddy_import.h"

#include <cstddef>
#include <cstdio>

namespace cbufinsert {
namespace {

// Verified cvar_t layout (IDA, Cvar_Set2 @ 0x20021d70): name +0x00,
// string +0x04, latched +0x08, flags +0x0C, modified +0x14, value +0x18.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs  = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet   = 8;

/** Output cvar published by writing cvar_t directly - engine cvar API calls
 *  are documented to crash on some Wine builds from frame-path hooks, and
 *  Cbuf_InsertText is squarely on that path. Double-buffered string, restored
 *  on detach. Same shape as clamp_monitor/cvar.cpp, deliberately duplicated
 *  rather than shared: features here are self-contained folders, and one
 *  feature's detach must not strand another's strings. */
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

/** Hand-rolled: this DLL builds against msvcrt, whose printf does not reliably
 *  understand "ll", and "%g" turns anything past ~1e6 into "1.23457e+06" in
 *  the console and in sofplus string compares. */
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

OutputCvar g_outInserts;    // _sofbuddy_cbuf_inserts
OutputCvar g_outBytes;      // _sofbuddy_cbuf_insert_bytes
OutputCvar g_outMax;        // _sofbuddy_cbuf_insert_max
OutputCvar g_outMicros;     // _sofbuddy_cbuf_insert_us
OutputCvar g_outDelegated;  // _sofbuddy_cbuf_insert_slow

void* g_cvEnabled = nullptr;

}  // namespace

void InitCvars() {
    g_outInserts.Bind("_sofbuddy_cbuf_inserts");
    g_outBytes.Bind("_sofbuddy_cbuf_insert_bytes");
    g_outMax.Bind("_sofbuddy_cbuf_insert_max");
    g_outMicros.Bind("_sofbuddy_cbuf_insert_us");
    g_outDelegated.Bind("_sofbuddy_cbuf_insert_slow");

    if (!g_cvEnabled)
        // Off by default on purpose. See README: this tree already has one
        // recorded case (zpool) of an optimisation that was obviously right on
        // paper and measured to exactly nothing. Measurement runs in both
        // states, so flipping this is a controlled A/B on your own server.
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_cbuf_insert", "0", kCvarFlagArchive, nullptr);
}

bool FastPathEnabled() {
    return Buddy_ReadCvarValue(g_cvEnabled, 0.0f) != 0.0f;
}

void SetOutputs(long long inserts, long long bytesShifted, int maxCursize,
                long long micros, long long delegated) {
    char text[32];

    FormatI64(text, sizeof(text), inserts);
    g_outInserts.Publish(static_cast<float>(inserts), text);

    FormatI64(text, sizeof(text), bytesShifted);
    g_outBytes.Publish(static_cast<float>(bytesShifted), text);

    std::snprintf(text, sizeof(text), "%d", maxCursize);
    g_outMax.Publish(static_cast<float>(maxCursize), text);

    FormatI64(text, sizeof(text), micros);
    g_outMicros.Publish(static_cast<float>(micros), text);

    FormatI64(text, sizeof(text), delegated);
    g_outDelegated.Publish(static_cast<float>(delegated), text);
}

}  // namespace cbufinsert

extern "C" void CbufInsert_Shutdown() {
    cbufinsert::g_outInserts.Restore();
    cbufinsert::g_outBytes.Restore();
    cbufinsert::g_outMax.Restore();
    cbufinsert::g_outMicros.Restore();
    cbufinsert::g_outDelegated.Restore();
}
