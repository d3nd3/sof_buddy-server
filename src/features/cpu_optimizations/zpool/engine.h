#pragma once

// ---------------------------------------------------------------------------
// The engine's zone allocator (SoF.exe / SoF-spsv.exe), read out of
// Z_Malloc @0x2001F120 and Z_Free @0x2001EBC0.
//
// Z_Malloc(size) allocates size + 0x10 through the CRT, zeroes the whole
// block, fills a 16-byte header, links it at the front of a sentinel-
// terminated doubly-linked chain, bumps two counters, and returns hdr + 0x10:
//
//     +0x00  prev    (void*)   - the previous node, or &z_chain for the head
//     +0x04  next    (void*)   - the next node
//     +0x08  magic   (uint16)  - 0x1D1D, checked by Z_Free
//     +0x0A  tag     (uint16)  - always written 0 by the engine
//     +0x0C  size    (uint32)  - the FULL allocation, i.e. request + 0x10
//     +0x10  payload           - what the caller gets
//
// The chain sentinel lives at 0x20249E54 and its `next` field *is* the head
// pointer at 0x20249E58, which is why Z_Free's `prev->next = next` correctly
// updates the head without special-casing it. z_stats_f and Z_Touch walk this
// chain, so the pool reproduces the layout and linkage exactly.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace zpool {

constexpr unsigned kRvaZChainSentinel = 0x249E54;  // z_chain (its +4 is the head)
constexpr unsigned kRvaZCount         = 0x249634;  // live allocation count
constexpr unsigned kRvaZBytes         = 0x249E70;  // live byte total

constexpr unsigned kHeaderSize = 0x10;
constexpr unsigned kOfsPrev    = 0x00;
constexpr unsigned kOfsNext    = 0x04;
constexpr unsigned kOfsMagic   = 0x08;
constexpr unsigned kOfsTag     = 0x0A;
constexpr unsigned kOfsSize    = 0x0C;

constexpr std::uint16_t kMagic = 0x1D1D;

struct ZHeader {
    void*         prev;
    void*         next;
    std::uint16_t magic;
    std::uint16_t tag;
    std::uint32_t size;   // full allocation, header included
};

static_assert(sizeof(ZHeader) == kHeaderSize, "zone header must be 16 bytes");

}  // namespace zpool
