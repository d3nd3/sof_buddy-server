// cbuf_insert: make Cbuf_InsertText shift the command buffer in place instead
// of round-tripping it through the zone allocator.
//
// The engine's Cbuf_InsertText (@0x200181D0) is stock Quake 2, with
// Cbuf_AddText inlined into the middle of it:
//
//     temp = Z_Malloc(cursize);      // 0x200181DF  - and Z_Malloc *zeroes*
//     memcpy(temp, data, cursize);   // 0x200181FA  - rep movsd
//     SZ_Clear(&cmd_text);           // 0x20018203
//     ...strlen(text) twice, SZ_Write(text)...      // the inlined AddText
//     SZ_Write(&cmd_text, temp, cursize);           // 0x20018265 - copy back
//     Z_Free(temp);                  // 0x2001826B
//
// With C bytes already queued, that touches C three times - Z_Malloc's
// `rep stosd` zero-fill of C+16 bytes (@0x2001F153), the copy out, and the
// copy back - plus a malloc/free pair and two strlens of `text`. Cbuf_AddText
// by comparison touches only the new text.
//
// It is also quadratic: K inserts against a buffer holding C bytes cost
// O(K*C), because each one moves everything already queued.
//
// The same work done in place:
//
//     memmove(data + len, data, cursize);
//     memcpy(data, text, len);
//     cursize += len;
//
// One pass over C instead of three, no allocator, one strlen instead of two,
// and byte-for-byte the same buffer contents.
//
// Insert semantics are not optional, before anyone asks: text goes at the
// *front* so it runs before what is already queued, which is what makes
// nested script/alias/exec ordering work. Swapping to Cbuf_AddText would
// reorder execution. The point here is to make the insert cheap, not to avoid
// it.
//
// ---------------------------------------------------------------------------
// Off by default, and measured either way. See README - this tree already has
// a recorded case (zpool) of an optimisation that was obviously correct on
// paper and measured to exactly nothing on the live server, and the cost model
// above is dominated by byte traffic whose size nobody here has measured yet.
// ---------------------------------------------------------------------------

#include "cvar.h"
#include "engine.h"

#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace cbufinsert {
namespace {

struct State {
    bool ready = false;
    bool resolved = false;

    double qpcScaleUs = 0.0;   // microseconds per count
    std::int64_t publishAtQpc = 0;
    std::int64_t publishEveryQpc = 0;

    long long inserts = 0;
    long long bytesShifted = 0;
    long long micros = 0;
    long long delegated = 0;
    int maxCursize = 0;
};

State g;

void EnsureReady() {
    if (g.resolved)
        return;
    g.resolved = true;

    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        g.qpcScaleUs = 1000000.0 / static_cast<double>(f.QuadPart);
        g.publishEveryQpc = f.QuadPart / 10;   // publish at most 10x/second
    }
    g.ready = g.qpcScaleUs > 0.0 && EngineReady();
}

std::int64_t Qpc() {
    LARGE_INTEGER c;
    if (!QueryPerformanceCounter(&c))
        return 0;
    return c.QuadPart;
}

}  // namespace
}  // namespace cbufinsert

/** Cbuf_InsertText override.
 *
 *  The fast path runs only when the result provably fits: `cursize + len <
 *  maxsize`. That is stricter than either overflow test the original makes
 *  (the inlined Cbuf_AddText checks `len >= maxsize` against an
 *  already-cleared buffer, and the copy-back leans on SZ_Write), so whenever
 *  this path is taken the original would have succeeded with no message and
 *  the same bytes. Every other case - including every overflow - delegates to
 *  the engine, so the pathological behaviour stays exactly the engine's own
 *  rather than being reimplemented from a guess about what it should be.
 *
 *  Measurement runs in both states so the cvar is a controlled A/B. */
void cbufinsert_InsertText(char* text, detour_Cbuf_InsertText::tCbuf_InsertText original) {
    using namespace cbufinsert;

    EnsureReady();

    const std::int64_t startQpc = g.ready ? Qpc() : 0;
    bool fast = false;

    if (g.ready && text) {
        const std::int32_t cursize = *Engine().cursize;
        const std::int32_t maxsize = *Engine().maxsize;
        const auto len = static_cast<std::int32_t>(std::strlen(text));

        ++g.inserts;
        g.bytesShifted += cursize;
        if (cursize > g.maxCursize)
            g.maxCursize = cursize;

        if (cursize >= 0 && len >= 0 && cursize + len < maxsize && FastPathEnabled()) {
            unsigned char* data = *Engine().data;
            if (data) {
                std::memmove(data + len, data, static_cast<std::size_t>(cursize));
                std::memcpy(data, text, static_cast<std::size_t>(len));
                *Engine().cursize = cursize + len;
                fast = true;
            }
        }
    }

    if (!fast) {
        if (g.ready)
            ++g.delegated;
        if (original)
            original(text);
    }

    if (!g.ready)
        return;

    const std::int64_t endQpc = Qpc();
    g.micros += static_cast<long long>(static_cast<double>(endQpc - startQpc) * g.qpcScaleUs);

    // Publishing five cvars per call would cost more than the function does.
    if (g.publishEveryQpc > 0 && endQpc >= g.publishAtQpc) {
        g.publishAtQpc = endQpc + g.publishEveryQpc;
        SetOutputs(g.inserts, g.bytesShifted, g.maxCursize, g.micros, g.delegated);
    }
}

void cbufinsert_OnGameDllLoaded(void* game_export) {
    using namespace cbufinsert;
    (void)game_export;

    InitCvars();
    EnsureReady();
    PrintOut(PRINT_LOG, "[cbuf_insert] %s, measuring (_sofbuddy_cbuf_insert %d)\n",
             g.ready ? "bound" : "unavailable - delegating every call",
             FastPathEnabled() ? 1 : 0);
}
