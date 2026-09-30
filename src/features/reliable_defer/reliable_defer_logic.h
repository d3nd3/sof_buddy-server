#pragma once

#include "engine.h"
#include "reliable_defer_wire.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

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

// End offset of the last complete message in [p, p+n), or 0.
// Stops at the first incomplete or opaque opcode — never guesses a cut
// inside a message (that cut is "Illegible server message" on the client).
// 0x24/0x25/0x27 are length-prefixed: SP_Print plants them with SZ_GetSpace,
// and the counted payload may contain 0x0B and NUL.
// Layouts match CL_ParseServerMessage (SoF.exe 0x2000ee30 / sof-bin 0x80ca8a8).
// MSG_ReadString @ 0x2001E3B0 stops on 0x00 and 0xFF.
inline int RelDef_CStrEnd(const std::uint8_t* p, int n, int i) {
    return RelDef_WireCStrEnd(p, n, i);
}

// svc_ric (clientRICBuf::ReadRICs): count, then that many records.
// Low nibble is the type. Types 0-4 carry one arg; its size is the low
// two bits of the high nibble (0..3 => 1..4 bytes). Type 5 and unknown
// types carry nothing. Payload is binary, so do not stop on NUL.
inline int RelDef_RicEnd(const std::uint8_t* p, int n, int i) {
    if (i >= n)
        return -1;
    const int count = p[i++];
    for (int r = 0; r < count; ++r) {
        if (i >= n)
            return -1;
        const int raw = p[i++];
        if ((raw & 0x0f) > 4)
            continue;
        const int nbytes = ((raw >> 4) & 3) + 1;
        if (i + nbytes > n)
            return -1;
        i += nbytes;
    }
    return i;
}

// svc_equip @ CL_ParseServerMessage case 0x6 → sub_20001FD0: sub-byte 1 then
// three (count + count×(MSG_ReadString + ReadLong)) sections; other sub-bytes
// are opcode + sub only.
inline int RelDef_EquipEnd(const std::uint8_t* p, int n, int i) {
    if (i >= n)
        return -1;
    if (p[i] != 1) {
        // Other sub-types are opcode + sub only (sub_20001FD0).
        return i + 1;
    }
    ++i;
    for (int s = 0; s < 3; ++s) {
        if (i >= n)
            return -1;
        int count = p[i++];
        for (int r = 0; r < count; ++r) {
            i = RelDef_CStrEnd(p, n, i);
            if (i < 0 || i + 4 > n)
                return -1;
            i += 4;
        }
    }
    return i;
}

// svc_playernamecols case 0x21: count, then count×(color byte; if color<0
// signed, start+end bytes else one index byte).
inline int RelDef_PlayerNameColsEnd(const std::uint8_t* p, int n, int i) {
    if (i >= n)
        return -1;
    const int total = p[i++];
    for (int c = 0; c < total; ++c) {
        if (i >= n)
            return -1;
        const signed char color = static_cast<signed char>(p[i++]);
        if (color >= 0) {
            if (i >= n)
                return -1;
            ++i;
        } else if (i + 2 > n) {
            return -1;
        } else {
            i += 2;
        }
    }
    return i;
}

inline bool RelDef_IsOpaquePacket(const std::uint8_t* p, int n) {
    if (!p || n <= 0)
        return false;
    switch (p[0]) {
    case 0x01: case 0x05:
    case 0x14: case 0x15: case 0x16: case 0x17:
        return true;
    default:
        return false;
    }
}

inline int RelDef_MsgEnd(const std::uint8_t* p, int n, int i) {
    if (!p || i < 0 || i >= n)
        return -1;
    const int op = p[i++];
    auto need = [&](int k) { return i + k <= n ? i + k : -1; };
    auto str = [&]() { return RelDef_CStrEnd(p, n, i); };
    switch (op) {
    case 0x02: case 0x0D: case 0x11:          // layout, stufftext, centerprint: string
        return str();
    case 0x0B:                                // print: level + string
        if (i >= n) return -1;
        ++i;
        return str();
    case 0x0C:                                // nameprint: client, team, string
        if (need(2) < 0) return -1;
        i += 2;
        return str();
    case 0x0F: {                              // index + inline ID, or -1 + string
        if (need(2) < 0) return -1;
        i += 2;
        if (need(2) < 0) return -1;
        const auto id = static_cast<std::int16_t>(p[i] | (p[i + 1] << 8));
        i += 2;
        return id >= 0 ? i : str();
    }
    case 0x20:                                // cinprint: short, short, byte, string
        if (need(5) < 0) return -1;
        i += 5;
        return str();
    case 0x08:                                // disconnect: ReadWord (reason id)
    case 0x12: case 0x22: case 0x23:          // caption / sp_print / rem cs: short
        return need(2);
    case 0x06:
        return RelDef_EquipEnd(p, n, i);
    case 0x1E:                                // rebuild_pred_inv: 640-byte inven_c
        return need(640);
    case 0x1F:                                // countdown: ReadLong
        return need(4);
    case 0x21:
        return RelDef_PlayerNameColsEnd(p, n, i);
    case 0x24: case 0x27: {                   // sp_print_data_1 / obit: short + byte n + n
        // SP_Print copies the whole packet via SZ_GetSpace, so it shows up
        // in capture only after absorb. Payload is binary (0x0B, NUL).
        if (need(3) < 0) return -1;
        const int count = p[i + 2];
        i += 3;
        return need(count);
    }
    case 0x25: {                              // sp_print_data_2: short + short n + n
        if (need(4) < 0) return -1;
        const int count = p[i + 2] | (p[i + 3] << 8);
        i += 4;
        return need(count);
    }
    case 0x19:                                // damagetexture: short + byte
        return need(3);
    case 0x1D:                                // restart_predn: byte
        return need(1);
    case 0x1A: {                              // ghoul reliable: short n + n bytes
        // Payload is a bit packet (string table). A 0x0B or NUL inside it
        // is not a message boundary.
        if (need(2) < 0) return -1;
        const int count = p[i] | (p[i + 1] << 8);
        i += 2;
        return need(count);
    }
    case 0x1C:                                // ric: count + records
        return RelDef_RicEnd(p, n, i);
    case 0x0E:
        return RelDef_WireServerDataEnd(p, n, i);
    case 0x10:
        return RelDef_WireSpawnBaselineEnd(p, n, i);
    case 0x13:
        return RelDef_WireDownloadEnd(p, n, i);
    case 0x04:
        return RelDef_WireSoundInfoEnd(p, n, i);
    case 0x0A:
        return RelDef_WireSoundEnd(p, n, i);
    case 0x18:
        return RelDef_WireCulledEventEnd(p, n, i);
    case 0x1B:
        return RelDef_WireCountedShortEnd(p, n, i);
    case 0x07: case 0x09: case 0x26: case 0x28:  // nop, reconnect, welcome, force
        return i;
    case 0x01: case 0x05: case 0x14: case 0x15: case 0x16: case 0x17:
        return -1;  // frame / TE / effect — wrong lane or unbounded
    default:
        return -1;
    }
}

inline int RelDef_LastCompleteEnd(const std::uint8_t* p, int n) {
    if (!p || n <= 0)
        return 0;
    int pos = 0, last = 0;
    while (pos < n) {
        const int end = RelDef_MsgEnd(p, n, pos);
        if (end <= pos)
            break;
        pos = last = end;
    }
    return last;
}

inline bool RelDef_EndsAtMessageBoundary(const std::uint8_t* p, int n) {
    return n <= 0 || (p && RelDef_LastCompleteEnd(p, n) == n);
}

// How much of a capture may be queued. A tail that does not open with an
// opcode is dropped: it would ride out after a finished svc_layout and the
// client would report "Illegible server message (Last command was svc_layout)".
inline int RelDef_SealEnd(const std::uint8_t* p, int n) {
    if (!p || n <= 0)
        return 0;
    const int end = RelDef_LastCompleteEnd(p, n);
    if (end >= n)
        return n;
    if (end <= 0) {
        if (p[0] < 1 || p[0] > 0x28)
            return 0;
        const int msgEnd = RelDef_MsgEnd(p, n, 0);
        return (msgEnd > 0 && msgEnd <= n) ? n : 0;
    }
    if (p[end] < 1 || p[end] > 0x28)
        return end;
    const int msgEnd = RelDef_MsgEnd(p, n, end);
    return (msgEnd > end && msgEnd <= n) ? n : end;
}

// A MSG_WriteString continues only a packet whose retail parser reads a string.
inline bool RelDef_StringContinues(const std::uint8_t* p, int n) {
    if (!p || n <= 0)
        return false;
    const int end = RelDef_LastCompleteEnd(p, n);
    if (end >= n)
        return false;
    switch (p[end]) {
    case 0x02: case 0x06: case 0x0B: case 0x0C:
    case 0x0D: case 0x0E: case 0x0F: case 0x11: case 0x20:
        return RelDef_MsgEnd(p, n, end) < 0;
    default:
        return false;
    }
}

inline void RelDef_CaptureAppend(std::uint8_t* dst, int& dstLen, int dstCap,
                                 const void* src, int srcLen) {
    if (!dst || !src || srcLen <= 0 || dstLen < 0 || dstCap <= dstLen)
        return;
    const int n = srcLen < dstCap - dstLen ? srcLen : dstCap - dstLen;
    if (n <= 0)
        return;
    std::memcpy(dst + dstLen, src, static_cast<std::size_t>(n));
    dstLen += n;
}

// Lockstep bypass for svc_download (0x13): Byte(op) + Short + Byte +
// SZ_Write(raw binary), written atomically in one engine call. SP_Print
// (0x24/0x25) ships via SZ_GetSpace+SZ_Write — RelDef_MsgEnd keeps it whole.
// phase: 0 = off, 3..2 = header bypasses remaining, 1 = data-or-new-message
// (a short "file not found" message has no data: the next non-SZ write
// disarms and classifies normally).
enum RelDefDlOutcome : std::uint8_t {
    RELDEF_DL_NOW = 0,
    RELDEF_DL_CLASSIFY = 1,
};

inline bool RelDef_IsLockstepOp(unsigned char b) {
    return b == 0x13;
}

// svc_ghoulreliable: MSG_WriteByte(0x1A) then SZ_GetSpace of the short and
// the body into the same mailbox. The opcode has to land in that buffer;
// capturing it alone makes the client unpack a header with no body
// (Ghoul :StringTable underflowed).
inline bool RelDef_WriteThroughOp(unsigned char b) {
    return b == 0x1A;
}

// SV_Map broadcasts these as one SZ_Write (opcode + text + NUL) while the
// client is still spawned, then flushes before SpawnServer. "changing" is
// what makes the client run "menu loading"; without it the load screen
// never starts and "cmd begin" has nothing to finish.
inline bool RelDef_IsLevelChange(const std::uint8_t* p, int n) {
    auto at = [&](const char* s, int sn) {
        if (!p || n != sn + 1 || p[0] != 0x0D)
            return false;
        for (int i = 0; i < sn; ++i)
            if (p[i + 1] != static_cast<unsigned char>(s[i]))
                return false;
        return true;
    };
    return at("changing\n", 10) || at("reconnect\n", 11);
}

// True when this mailbox is exactly a level-change packet, or one was
// appended after other mail already sitting there.
inline bool RelDef_HasLevelChange(const std::uint8_t* p, int n) {
    if (RelDef_IsLevelChange(p, n))
        return true;
    if (n > 11 && RelDef_IsLevelChange(p + (n - 11), 11))
        return true;
    return n > 12 && RelDef_IsLevelChange(p + (n - 12), 12);
}

// SV_Map flushes while the client is still spawned, so the snapshot is parsed
// after "changing" and spends the client's one-shot menu close on the old
// servercount. Drop spawned to connected for that send: the engine then
// transmits the mailbox with no snapshot. Caller restores spawned afterwards.
inline bool RelDef_SuppressSnapshot(int clientState, bool hasLevelChange) {
    return clientState >= kCsSpawned && hasLevelChange;
}

inline RelDefDlOutcome RelDef_DlStep(int& phase, bool isBulk) {
    if (phase > 1) {
        --phase;
        return RELDEF_DL_NOW;
    }
    phase = 0;
    return isBulk ? RELDEF_DL_NOW : RELDEF_DL_CLASSIFY;
}
