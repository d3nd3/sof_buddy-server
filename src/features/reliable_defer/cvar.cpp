#include "cvar.h"
#include "buddy_import.h"

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
        *reinterpret_cast<char* volatile*>(static_cast<char*>(cv) + kCvarStringOfs) =
            original;
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

OutputCvar g_outQueued;
OutputCvar g_outDripped;
OutputCvar g_outDropped;
OutputCvar g_outCaptureBytes;
OutputCvar g_outOldestWait;
}  // namespace

void RelDef_InitCvars() {
    g_outQueued.Bind("_sofbuddy_reldef_queued");
    g_outDripped.Bind("_sofbuddy_reldef_dripped");
    g_outDropped.Bind("_sofbuddy_reldef_dropped");
    g_outCaptureBytes.Bind("_sofbuddy_reldef_capture_bytes");
    g_outOldestWait.Bind("_sofbuddy_reldef_oldest_wait");
}

void RelDef_PublishGauges(long long queued, long long dripped, long long dropped,
                          int captureBytes, int oldestWait) {
    char t[32];
    FormatI64(t, sizeof(t), queued);
    g_outQueued.Publish(static_cast<float>(queued), t);
    FormatI64(t, sizeof(t), dripped);
    g_outDripped.Publish(static_cast<float>(dripped), t);
    FormatI64(t, sizeof(t), dropped);
    g_outDropped.Publish(static_cast<float>(dropped), t);
    std::snprintf(t, sizeof(t), "%d", captureBytes);
    g_outCaptureBytes.Publish(static_cast<float>(captureBytes), t);
    std::snprintf(t, sizeof(t), "%d", oldestWait);
    g_outOldestWait.Publish(static_cast<float>(oldestWait), t);
}

RelDefPolicy RelDef_ReadPolicy() {
    RelDefPolicy p;
    p.reserveBytes = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_reserve", "256", kCvarFlagArchive, nullptr),
        256.0f));
    p.frameReserveBytes = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_frame_reserve", "0", kCvarFlagArchive, nullptr),
        0.0f));
    p.onePerTick = Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_one_per_tick", "0", kCvarFlagArchive, nullptr),
        0.0f) != 0.0f;
    p.maxQueue = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_max_queue", "32", kCvarFlagArchive, nullptr),
        32.0f));
    p.maxQueueBytes = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_max_queue_bytes", "262144", kCvarFlagArchive,
                           nullptr),
        262144.0f));
    p.frameFirst = Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_frame_first", "1", kCvarFlagArchive, nullptr),
        1.0f) != 0.0f;
    p.maxDripWaitTicks = static_cast<int>(Buddy_ReadCvarValue(
        Buddy_GetEngineCvar("_sofbuddy_reldef_max_drip_wait", "10", kCvarFlagArchive, nullptr),
        10.0f));
    if (p.maxDripWaitTicks < 0)
        p.maxDripWaitTicks = 0;
    return p;
}

extern "C" void RelDef_Shutdown() {
    g_outQueued.Restore();
    g_outDripped.Restore();
    g_outDropped.Restore();
    g_outCaptureBytes.Restore();
    g_outOldestWait.Restore();
}
