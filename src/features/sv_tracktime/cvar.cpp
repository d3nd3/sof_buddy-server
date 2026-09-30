#include "cvar.h"
#include "buddy_import.h"

#include <cstddef>
#include <cstdio>

namespace {

// cvar_t layout (IDA, Cvar_Set2 @ 0x20021d70 in SoF.exe): +0x00 name,
// +0x04 string, +0x08 latched_string, +0x0C flags, +0x18 value, +0x1C next.
// SoF's `value` sits at +0x18, not stock Quake 2's +0x14.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;

constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet = 8;

constexpr float kDefaultTolerance = 20.0f;
constexpr int kDefaultWindow = 256;
constexpr int kDefaultMinSamples = 30;

/** One output cvar, published by writing cvar_t directly.
 *
 *  Why not gi.cvar_setvalue: engine cvar API calls are documented to crash on
 *  some Wine builds when made from inside frame-path hooks (see
 *  buddy_import.h), and these are published from the SV_ClientThink hook.
 *
 *  Both fields must be written - the engine's console print of a cvar reads
 *  cvar_t.string, not .value - and `string` is double-buffered: format into the
 *  spare buffer, then swap the pointer with one aligned store, so a reader can
 *  never see a half-written string.
 *
 *  `original` is the engine-allocated string we displaced; it goes back on
 *  detach, because the engine owns the cvar_t for the life of the process and
 *  Cvar_Set2 does Z_Free(var->string). */
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

/** 64-bit decimal by hand: this DLL builds against msvcrt, whose printf does
 *  not reliably take the "ll" modifier, and a counter must never print as
 *  garbage in the console. */
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

OutputCvar g_outFrames;    // _sofbuddy_tracktime_frames
OutputCvar g_outSuspect;   // _sofbuddy_tracktime_suspect
OutputCvar g_outWorstDrift;  // _sofbuddy_tracktime_worst_drift

void* g_cvEnabled = nullptr;     // _sofbuddy_tracktime
void* g_cvTolerance = nullptr;  // _sofbuddy_tracktime_tolerance (percent)
void* g_cvWindow = nullptr;     // _sofbuddy_tracktime_window (samples)
void* g_cvMinSamples = nullptr; // _sofbuddy_tracktime_min (samples)

}  // namespace

void SvTracktime_InitCvars() {
    g_outFrames.Bind("_sofbuddy_tracktime_frames");
    g_outSuspect.Bind("_sofbuddy_tracktime_suspect");
    g_outWorstDrift.Bind("_sofbuddy_tracktime_worst_drift");

    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_tracktime", "1", kCvarFlagArchive, nullptr);
    if (!g_cvTolerance)
        g_cvTolerance =
            Buddy_GetEngineCvar("_sofbuddy_tracktime_tolerance", "20", kCvarFlagArchive, nullptr);
    if (!g_cvWindow)
        g_cvWindow =
            Buddy_GetEngineCvar("_sofbuddy_tracktime_window", "256", kCvarFlagArchive, nullptr);
    if (!g_cvMinSamples)
        g_cvMinSamples =
            Buddy_GetEngineCvar("_sofbuddy_tracktime_min", "30", kCvarFlagArchive, nullptr);
}

namespace {

float ReadF(void* cv, float dflt) {
    if (!cv)
        return dflt;
    return *reinterpret_cast<volatile float*>(static_cast<char*>(cv) + kCvarValueOfs);
}

}  // namespace

bool SvTracktime_Enabled() {
    if (!g_cvEnabled)
        SvTracktime_InitCvars();
    if (!g_cvEnabled)
        return true;  // fail open: measuring costs nothing
    return ReadF(g_cvEnabled, 1.0f) != 0.0f;
}

float SvTracktime_Tolerance() {
    const float v = ReadF(g_cvTolerance, kDefaultTolerance);
    // 0 is meaningful (call out any deviation at all); only NaN/negative falls
    // back to the default.
    return (v >= 0.0f) ? v : kDefaultTolerance;
}

int SvTracktime_Window() {
    const int v = static_cast<int>(ReadF(g_cvWindow, static_cast<float>(kDefaultWindow)));
    return (v > 0) ? v : kDefaultWindow;
}

int SvTracktime_MinSamples() {
    const int v = static_cast<int>(ReadF(g_cvMinSamples, static_cast<float>(kDefaultMinSamples)));
    return (v > 0) ? v : kDefaultMinSamples;
}

void SvTracktime_SetOutputs(long long frames, int suspect_slots, double worst_drift) {
    char text[32];

    FormatI64(text, sizeof(text), frames);
    g_outFrames.Publish(static_cast<float>(frames), text);

    std::snprintf(text, sizeof(text), "%d", suspect_slots);
    g_outSuspect.Publish(static_cast<float>(suspect_slots), text);

    std::snprintf(text, sizeof(text), "%.1f", worst_drift);
    g_outWorstDrift.Publish(static_cast<float>(worst_drift), text);
}

extern "C" void SvTracktime_Shutdown() {
    g_outFrames.Restore();
    g_outSuspect.Restore();
    g_outWorstDrift.Restore();
    g_cvEnabled = nullptr;
    g_cvTolerance = nullptr;
    g_cvWindow = nullptr;
    g_cvMinSamples = nullptr;
}
