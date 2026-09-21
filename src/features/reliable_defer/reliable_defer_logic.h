#pragma once

#include "engine.h"

#include <cstddef>
#include <cstdint>

constexpr int kRelDefMsgInlineReserve = 16;  // Netchan_Setup: message.maxsize = buffersize - 16
constexpr int kRelDefWireHdrBytes = 8;       // seq+ack on server→client (qport non-zero after connect)

enum RelDefAction : std::uint8_t {
    RELDEF_WRITE_NOW = 0,
    RELDEF_QUEUE = 1,
    RELDEF_DROP = 2,
};

struct RelDefPolicy {
    int reserveBytes = 256;
    int frameReserveBytes = 0;  // 0 = auto (buffersize/2, matches engine soft cap)
    bool onePerTick = false;
    int maxQueue = 32;
    int maxQueueBytes = 65536;
    bool frameFirst = true;    // hold fat blobs so the snapshot keeps flowing
    int maxDripWaitTicks = 10;  // starvation escape for held blobs (frames)
};

// Limits derived from engine `buffersize` (NET_Config: 1400 MP, 16384 SP).
struct RelDefLimits {
    int buffersize = 0;
    int msgMaxsize = 0;     // buffersize - 16
    int softCap = 0;        // buffersize / 2 (configstrings, ghoul reliable, baselines)
    int frameReserve = 0;   // bytes left for frame+datagram on wire
    int maxDripBytes = 0;   // max reliable blob to drip this tick
    int stagingReserve = 0; // headroom inside message staging
};

inline RelDefLimits RelDef_ComputeLimits(int buffersize, const RelDefPolicy& p) {
    RelDefLimits l;
    if (buffersize <= 0)
        return l;
    l.buffersize = buffersize;
    l.msgMaxsize = buffersize - kRelDefMsgInlineReserve;
    l.softCap = buffersize / 2;
    l.frameReserve = p.frameReserveBytes > 0 ? p.frameReserveBytes : l.softCap;
    if (l.frameReserve > buffersize - kRelDefWireHdrBytes)
        l.frameReserve = buffersize - kRelDefWireHdrBytes;
    l.maxDripBytes = buffersize - kRelDefWireHdrBytes - l.frameReserve;
    if (l.maxDripBytes < 0)
        l.maxDripBytes = 0;
    const int scaled = l.msgMaxsize / 8;
    l.stagingReserve = p.reserveBytes > scaled ? p.reserveBytes : scaled;
    if (l.stagingReserve > l.msgMaxsize)
        l.stagingReserve = l.msgMaxsize;
    return l;
}

inline RelDefAction RelDef_Classify(int msgCursize, int incomingLen, int reliableLength,
                                    int queueCount, int queueBytes,
                                    const RelDefLimits& lim, const RelDefPolicy& p,
                                    int clientState = kCsSpawned) {
    if (incomingLen <= 0)
        return RELDEF_WRITE_NOW;
    // Connect/spawn handshake: match stock (no capture). Defer only after spawned.
    if (clientState >= 0 && clientState < kCsSpawned)
        return RELDEF_WRITE_NOW;
    if (lim.msgMaxsize > 0 && incomingLen > lim.msgMaxsize)
        return RELDEF_DROP;
    if (reliableLength > 0)
        return RELDEF_QUEUE;
    if (queueCount > 0)
        return RELDEF_QUEUE;
    if (p.onePerTick && msgCursize > 0)
        return RELDEF_QUEUE;
    if (lim.msgMaxsize > 0 && msgCursize + incomingLen > lim.msgMaxsize - lim.stagingReserve)
        return RELDEF_QUEUE;
    if (p.maxQueue > 0 && queueCount >= p.maxQueue)
        return RELDEF_DROP;
    if (p.maxQueueBytes > 0 && queueBytes + incomingLen > p.maxQueueBytes)
        return RELDEF_DROP;
    // With one_per_tick, never write the first byte of a message straight into
    // staging — svc_print / svc_welcomeprint are 2–3 SZ_Writes; byte 1 must not
    // bypass capture while bytes 2–3 defer (that split corrupts the client).
    if (p.onePerTick)
        return RELDEF_QUEUE;
    return RELDEF_WRITE_NOW;
}

inline bool RelDef_CanDrip(int reliableLength, int msgCursize, int blobLen,
                           const RelDefLimits& lim, bool frameFirst = false,
                           int oldestWaitTicks = 0, int maxWaitTicks = 0) {
    if (reliableLength != 0 || blobLen <= 0)
        return false;
    const int total = msgCursize + blobLen;
    // Only cap staging size. maxDripBytes is wire guidance for stock (may drop
    // unreliable); blocking drip here deadlocks connect bursts > maxDripBytes.
    if (lim.msgMaxsize > 0 && total > lim.msgMaxsize)
        return false;
    if (frameFirst && lim.maxDripBytes > 0 && total > lim.maxDripBytes) {
        // Frame-first: a fat blob would eat this tick's snapshot
        // (Netchan_Transmit drops the whole unreliable frame when the packet
        // is full). Hold it — unless it has waited past the starvation bound,
        // in which case send it anyway and sacrifice one frame.
        if (maxWaitTicks <= 0 || oldestWaitTicks < maxWaitTicks)
            return false;
    }
    return true;
}

inline void RelDef_CaptureAppend(std::uint8_t* dst, int& dstLen, int dstCap,
                                 const void* src, int srcLen) {
    if (!dst || !src || srcLen <= 0 || dstLen < 0 || dstCap <= dstLen)
        return;
    const int n = srcLen < dstCap - dstLen ? srcLen : dstCap - dstLen;
    if (n <= 0)
        return;
    const auto* s = static_cast<const std::uint8_t*>(src);
    for (int i = 0; i < n; ++i)
        dst[dstLen++] = s[i];
}

// Lockstep bypass for binary multi-write messages: svc_download (0x13) and
// svc_sp_print_data_1/_data_2 (0x24/0x25) are Byte(op) + Short + Byte|Short +
// SZ_Write(raw binary), written atomically in one engine call. The payload
// is binary (fake 0x0B/NULs) and each unit must hit staging stock-immediate
// and atomic — capture/split/delay corrupts it.
// phase: 0 = off, 3..2 = header bypasses remaining, 1 = data-or-new-message
// (a short "file not found" message has no data: the next non-SZ write
// disarms and classifies normally).
enum RelDefDlOutcome : std::uint8_t {
    RELDEF_DL_NOW = 0,
    RELDEF_DL_CLASSIFY = 1,
};

inline bool RelDef_IsLockstepOp(unsigned char b) {
    return b == 0x13 || b == 0x24 || b == 0x25;
}

inline RelDefDlOutcome RelDef_DlStep(int& phase, bool isBulk) {
    if (phase > 1) {
        --phase;
        return RELDEF_DL_NOW;
    }
    phase = 0;
    return isBulk ? RELDEF_DL_NOW : RELDEF_DL_CLASSIFY;
}
