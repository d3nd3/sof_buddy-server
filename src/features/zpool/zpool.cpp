// zpool - recycles the engine's zone allocations instead of round-tripping
// the CRT heap for every one.
//
// Why this is worth doing: Z_Malloc/Z_Free are the engine's only allocation
// chokepoint (36 and 56 call sites), and the script path runs several per
// console line - Cmd_TokenizeString Z_Frees and Z_Mallocs one block per
// argument (@0x20018FD8 / @0x200190BD) and Cvar_Set2 frees and re-CopyStrings
// the value on every write. On top of that Z_Malloc zeroes the whole block
// with rep stosd and threads it onto a linked chain. Under Wine each CRT pair
// is an RtlAllocateHeap/RtlFreeHeap round trip behind a lock, which the live
// benchmark put at roughly half of the ~3.1us/op floor that remained after
// hash_lookup.
//
// This is a CACHE, not an arena. Every block is still individually allocated
// through the ENGINE's CRT, so a block still held by the engine after this DLL
// is unloaded - spsv reloads it between games - is an ordinary CRT pointer that
// the restored Z_Free can hand to free() correctly. An arena would make that
// case unfixable, which is why it isn't one.

#include "cvar.h"
#include "engine.h"
#include "zpool.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstring>

namespace zpool {
namespace {

// Full allocations (header included) up to this size are recycled; anything
// larger goes straight through to the CRT, where the round trip is noise
// against the copy cost anyway.
constexpr unsigned kGranularity = 16;
constexpr unsigned kMaxPooled   = 256;
constexpr unsigned kBuckets     = kMaxPooled / kGranularity;   // 16
constexpr unsigned kBucketCap   = 64;                          // <= 256 KB total

void*    g_free[kBuckets][kBucketCap];
unsigned g_freeCount[kBuckets];

void**        g_head     = nullptr;   // z_chain sentinel's `next` = list head
void*         g_sentinel = nullptr;   // &z_chain, what a head node's prev holds
std::int32_t* g_count    = nullptr;
std::int32_t* g_bytes    = nullptr;
bool          g_active   = false;

inline unsigned BucketOf(unsigned full) { return full / kGranularity - 1; }

void* PopFree(unsigned full) {
    unsigned b = BucketOf(full);
    if (g_freeCount[b] == 0)
        return nullptr;
    return g_free[b][--g_freeCount[b]];
}

bool PushFree(unsigned full, void* block) {
    unsigned b = BucketOf(full);
    if (g_freeCount[b] >= kBucketCap)
        return false;
    g_free[b][g_freeCount[b]++] = block;
    return true;
}

}  // namespace

/** Resolves the chain and counters. Returns false if anything looks wrong, in
 *  which case every call below simply forwards to the engine. */
bool BindAt(unsigned char* base) {
    g_sentinel = base + kRvaZChainSentinel;
    // The sentinel's `next` field IS the head pointer the engine keeps at
    // 0x20249E58 - that adjacency is what makes Z_Free's unlink uniform.
    g_head  = reinterpret_cast<void**>(base + kRvaZChainSentinel + kOfsNext);
    g_count = reinterpret_cast<std::int32_t*>(base + kRvaZCount);
    g_bytes = reinterpret_cast<std::int32_t*>(base + kRvaZBytes);

    // The chain must already be initialised: this feature comes up at
    // GameDllLoaded, long after Z_InitMemory.
    if (*g_head == nullptr)
        return false;
    return true;
}


void Drain() {
    for (unsigned b = 0; b < kBuckets; ++b) {
        while (g_freeCount[b] > 0)
            SOF_EP_EngineFree(g_free[b][--g_freeCount[b]]);
    }
}

void SetActive(bool on) { g_active = on; }
bool Active() { return g_active; }

}  // namespace zpool

// ---------------------------------------------------------------------------
// Overrides. These replace Z_Malloc / Z_Free outright, so they reproduce the
// engine's header, linkage and accounting exactly - z_stats_f and Z_Touch walk
// the same chain.
// ---------------------------------------------------------------------------

void* zpool_Malloc(int size, detour_Z_Malloc::tZ_Malloc original) {
    using namespace zpool;
    if (!g_active || size < 0 || !g_head)
        return original(size);

    const unsigned total   = static_cast<unsigned>(size) + kHeaderSize;
    const unsigned rounded = (total + (kGranularity - 1)) & ~(kGranularity - 1);

    void* block = (rounded <= kMaxPooled) ? PopFree(rounded) : nullptr;
    if (!block) {
        block = SOF_EP_EngineMalloc(rounded);
        if (!block)
            return original(size);  // let the engine raise its own Com_Error
    }

    // Callers rely on Z_Malloc handing back zeroed memory, recycled or not.
    std::memset(block, 0, rounded);

    auto* h  = static_cast<ZHeader*>(block);
    h->magic = kMagic;
    h->tag   = 0;
    // Record the size we actually took, so the byte counter stays balanced
    // whichever of us frees it.
    h->size  = rounded;

    void* head = *g_head;
    h->next = head;
    h->prev = g_sentinel;
    *reinterpret_cast<void**>(static_cast<char*>(head) + kOfsPrev) = block;
    *g_head = block;

    ++*g_count;
    *g_bytes += static_cast<std::int32_t>(rounded);

    return static_cast<char*>(block) + kHeaderSize;
}

void zpool_Free(void* ptr, detour_Z_Free::tZ_Free original) {
    using namespace zpool;
    if (!g_active || !ptr || !g_head) {
        original(ptr);
        return;
    }

    auto* h = reinterpret_cast<ZHeader*>(static_cast<char*>(ptr) - kHeaderSize);
    if (h->magic != kMagic) {
        // Not ours to reason about - hand it to the engine, which raises
        // "Z_Free: bad magic" exactly as it always did.
        original(ptr);
        return;
    }

    void* prev = h->prev;
    void* next = h->next;
    *reinterpret_cast<void**>(static_cast<char*>(prev) + kOfsNext) = next;
    *reinterpret_cast<void**>(static_cast<char*>(next) + kOfsPrev) = prev;

    --*g_count;
    *g_bytes -= static_cast<std::int32_t>(h->size);

    const unsigned full = h->size;
    // Poison the magic so a double free still trips the engine's own check
    // instead of silently corrupting the free list.
    h->magic = 0;

    if (full <= kMaxPooled && (full % kGranularity) == 0 && PushFree(full, h))
        return;
    SOF_EP_EngineFree(h);
}

void zpool_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    ZPool_InitCvars();

    if (!ZPool_Enabled()) {
        PrintOutConsole(PRINT_DEV, "[zpool] off (_sofbuddy_zpool 0)\n");
        return;
    }
    if (!zpool::Bind()) {
        PrintOutConsole(PRINT_BAD, "[zpool] zone chain not resolvable - disabled\n");
        return;
    }
    zpool::SetActive(true);
    PrintOutConsole(PRINT_LOG, "[zpool] enabled: recycling zone blocks up to %u bytes\n", 256u);
}

/** Detach hook. Returns every cached block to the engine's CRT: they are not
 *  on the zone chain, so nothing else will ever free them. */
extern "C" void ZPool_Shutdown(void) {
    if (!zpool::Active())
        return;
    zpool::SetActive(false);
    zpool::Drain();
}
