// reliable_defer: queue server→client reliable staging (netchan.message).
// Hooks SZ_Write + MSG_Write* and guards Netchan_Transmit. Within a frame, all
// client appends for one slot accumulate in a capture; blobs are cut only at
// complete message boundaries (RelDef_LastCompleteEnd). A cut mid-message
// makes the next blob start on payload bytes ("Illegible server message").

#include "cvar.h"
#include "engine.h"
#include "reliable_defer_logic.h"

#include "buddy_import.h"
#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include <windows.h>

namespace reldef_internal {

constexpr int kMaxSlots = 64;

struct SlotQueue {
    std::deque<std::vector<std::uint8_t>> blobs;
    std::deque<int> enqueued;  // sv_framenum when each blob was queued
    int bytes = 0;
};

struct State {
    bool ready = false;
    bool enabled = true;
    bool disabling = false;
    int maxClients = 8;
    RelDefPolicy policy;
    RelDefLimits limits;
    char* clientsBase = nullptr;
    void* msgSb[kMaxSlots] = {};
    SlotQueue slot[kMaxSlots];
    int captureSlot = 0;
    std::vector<std::uint8_t> capture;
    // Drip's own oSZ_Write bypass depth.
    int bypass = 0;
    // Per-slot lockstep bypass phase (see RelDef_DlStep): binary multi-write
    // messages started at a message boundary skip classification entirely.
    int dlPhase[kMaxSlots] = {};
    bool holdSnapshot[kMaxSlots] = {};
    long long deferred = 0;
    long long queued = 0;
    long long dripped = 0;
    long long dropped = 0;
    long long captured = 0;
};

State g;

std::uintptr_t EngineBase() {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
}

template <typename T>
T* Rva(unsigned rva) {
    return reinterpret_cast<T*>(EngineBase() + rva);
}

int EngineBuffersize() {
    int* p = Rva<int>(kRvaBuffersize);
    if (p && *p > 0)
        return *p;
    // NET_Config @ SV_InitGame: maxclients==1 → 16384, else 1400
    void* cv = Buddy_GetEngineCvar("maxclients", "8", 0, nullptr);
    const int mc = static_cast<int>(Buddy_ReadCvarValue(cv, 8.0f));
    return mc == 1 ? 16384 : 1400;
}

int CaptureMax() {
    return g.limits.msgMaxsize > 0 ? g.limits.msgMaxsize : 1384;
}

void PreserveCapture();
bool PendingEmpty();

void RefreshCache() {
    void* cv = Buddy_GetEngineCvar("_sofbuddy_reldef", "1", 1, nullptr);
    const bool requested = Buddy_ReadCvarValue(cv, 1.0f) != 0.0f;
    if (requested) {
        g.disabling = false;
        g.enabled = true;
    } else if (g.ready && (g.enabled || g.disabling)) {
        if (!g.disabling)
            PreserveCapture();
        g.disabling = true;
        g.enabled = true;  // drain in order before switching off
    } else {
        g.enabled = false;
    }
    g.policy = RelDef_ReadPolicy();
    g.limits = RelDef_ComputeLimits(EngineBuffersize(), g.policy);
    void* mcv = Buddy_GetEngineCvar("maxclients", "8", 0, nullptr);
    const int n = static_cast<int>(Buddy_ReadCvarValue(mcv, 8.0f));
    g.maxClients = n > 0 && n <= kMaxSlots ? n : 8;
    g.clientsBase = *Rva<char*>(kRvaSvsClients);
    for (int slot = 1; slot <= kMaxSlots; ++slot) {
        if (slot > g.maxClients || !g.clientsBase)
            g.msgSb[slot - 1] = nullptr;
        else
            g.msgSb[slot - 1] = g.clientsBase +
                                static_cast<unsigned>(slot - 1) * kClientStride +
                                kClientMessageOfs;
    }
}

char* ClientBase(int slot1) {
    if (slot1 < 1 || slot1 > kMaxSlots || !g.clientsBase)
        return nullptr;
    return g.clientsBase + static_cast<unsigned>(slot1 - 1) * kClientStride;
}

int SlotFromPointer(void* ptr, unsigned offset) {
    if (!ptr || !g.clientsBase)
        return -1;
    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(g.clientsBase) + offset;
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(ptr);
    if (value < base)
        return -1;
    const std::uintptr_t delta = value - base;
    if (delta % kClientStride)
        return -1;
    const int slot = static_cast<int>(delta / kClientStride) + 1;
    return slot >= 1 && slot <= g.maxClients &&
                   g.msgSb[slot - 1] == ptr
               ? slot
               : -1;
}

int SlotFromMessage(void* sb) {
    return SlotFromPointer(sb, kClientMessageOfs);
}

int ReadInt(const char* base, unsigned ofs) {
    return *reinterpret_cast<const int*>(base + ofs);
}

int EngineFramenum() {
    int* p = Rva<int>(kRvaSvFramenum);
    return p ? *p : 0;
}

bool QueuePush(int slot1, const void* data, int len, bool bounded = true) {
    if (slot1 < 1 || slot1 > kMaxSlots || len <= 0 || !data)
        return false;
    SlotQueue& q = g.slot[slot1 - 1];
    if (bounded && g.policy.maxQueue > 0 &&
        static_cast<int>(q.blobs.size()) >= g.policy.maxQueue) {
        ++g.dropped;
        return false;
    }
    if (bounded && g.policy.maxQueueBytes > 0 &&
        q.bytes + len > g.policy.maxQueueBytes) {
        ++g.dropped;
        return false;
    }
    std::vector<std::uint8_t> blob(static_cast<std::size_t>(len));
    std::memcpy(blob.data(), data, static_cast<std::size_t>(len));
    q.bytes += len;
    q.blobs.push_back(std::move(blob));
    q.enqueued.push_back(EngineFramenum());
    ++g.queued;
    return true;
}

bool QueuePushRaw(int slot1, const void* data, int len) {
    return QueuePush(slot1, data, len, false);
}

void ClearSlotPending(int slot1) {
    if (g.captureSlot == slot1) {
        g.capture.clear();
        g.captureSlot = 0;
    }
    if (slot1 >= 1 && slot1 <= kMaxSlots) {
        g.slot[slot1 - 1].blobs.clear();
        g.slot[slot1 - 1].enqueued.clear();
        g.slot[slot1 - 1].bytes = 0;
    }
}

void FlushCaptureToMessage(int slot1) {
    if (g.captureSlot != slot1 || g.capture.empty())
        return;
    char* cl = ClientBase(slot1);
    if (!cl || !detour_SZ_Write::oSZ_Write)
        return;
    char* msg = cl + kClientMessageOfs;
    ++g.bypass;
    detour_SZ_Write::oSZ_Write(
        msg, g.capture.data(), static_cast<int>(g.capture.size()));
    --g.bypass;
    g.capture.clear();
    g.captureSlot = 0;
}

void RescueLevelChange(int slot1);

RelDefAction ClassifySlot(int slot1, int incomingLen) {
    char* cl = ClientBase(slot1);
    if (!cl)
        return RELDEF_WRITE_NOW;
    const int state = ReadInt(cl, kClientStateOfs);
    if (state < kCsSpawned) {
        // SpawnServer drops the client to connected before the next send.
        // A changing/reconnect that a drip pushed back onto the queue would
        // be deleted here, and the client would never open the load menu.
        RescueLevelChange(slot1);
        ClearSlotPending(slot1);
        return RELDEF_WRITE_NOW;
    }
    char* msg = cl + kClientMessageOfs;
    SlotQueue& q = g.slot[slot1 - 1];
    return RelDef_Classify(ReadInt(msg, kSzCursize), incomingLen,
                           ReadInt(cl, kClientReliableLenOfs),
                           static_cast<int>(q.blobs.size()), q.bytes, g.limits, g.policy,
                           state);
}

void FlushCapture() {
    if (g.captureSlot <= 0 || g.capture.empty())
        return;
    const int slot = g.captureSlot;
    const int n = static_cast<int>(g.capture.size());
    const int end = RelDef_SealEnd(g.capture.data(), n);
    if (end > 0) {
        if (QueuePush(slot, g.capture.data(), end))
            ++g.deferred;
        else
            Buddy_DebugPrintf("reldef: drop capture slot %d len %d\n", slot, end);
    }
    if (end < n)
        Buddy_DebugPrintf("reldef: drop tail slot %d len %d\n", slot, n - end);
    g.capture.clear();
    g.captureSlot = 0;
}

void PreserveCapture() {
    if (g.captureSlot <= 0 || g.capture.empty())
        return;
    const int slot = g.captureSlot;
    if (!QueuePushRaw(slot, g.capture.data(), static_cast<int>(g.capture.size())))
        return;
    g.capture.clear();
    g.captureSlot = 0;
}

bool PendingEmpty() {
    if (g.captureSlot > 0 && !g.capture.empty())
        return false;
    for (const SlotQueue& q : g.slot)
        if (!q.blobs.empty())
            return false;
    return true;
}

void ClearMessageStaging(char* msg) {
    if (!msg)
        return;
    *reinterpret_cast<int*>(msg + kSzCursize) = 0;
    *reinterpret_cast<unsigned char*>(msg + kSzOverflowed) = 0;
}

// Pull staged bytes back into capture so a defer never splits svc_* mid-message.
// Staging always starts at a message boundary (cleared on transmit/absorb),
// but it may END mid-message (opcode+level staged NOW, string deferred next).
// Capture-first order: any existing capture bytes are older than staging;
// staging can only be non-empty here via unhooked direct writes (SZ_GetSpace,
// MSG_WriteLong), which are newer.
void AbsorbMessageIntoCapture(int slot1) {
    char* cl = ClientBase(slot1);
    if (!cl)
        return;
    char* msg = cl + kClientMessageOfs;
    const int n = ReadInt(msg, kSzCursize);
    if (n <= 0)
        return;
    char* data = *reinterpret_cast<char**>(msg + kSzData);
    if (!data)
        return;
    if (g.captureSlot != slot1) {
        g.capture.clear();
        g.capture.reserve(CaptureMax());
    }
    g.captureSlot = slot1;
    g.capture.insert(g.capture.end(),
                     reinterpret_cast<std::uint8_t*>(data),
                     reinterpret_cast<std::uint8_t*>(data) + n);
    ClearMessageStaging(msg);
}

// Queue the complete-message prefix of capture; keep an incomplete tail.
// An unparseable buffer is left intact — flushing it would ship a partial
// message (the old svc_print-only scan did that for stufftext/layout/etc.).
void FlushCompletePrefix() {
    if (g.captureSlot <= 0 || g.capture.empty())
        return;
    const int n = static_cast<int>(g.capture.size());
    const int end = RelDef_LastCompleteEnd(g.capture.data(), n);
    if (end <= 0)
        return;
    if (end >= n) {
        FlushCapture();
        return;
    }
    const int slot = g.captureSlot;
    if (QueuePush(slot, g.capture.data(), end))
        ++g.deferred;
    else
        Buddy_DebugPrintf("reldef: drop capture slot %d len %d\n", slot, end);
    g.capture.erase(g.capture.begin(), g.capture.begin() + end);
    // A non-opcode tail is payload with no header. Keeping it would ship
    // after the finished messages ("Illegible", last command svc_layout).
    if (g.capture.empty() || g.capture[0] < 1 || g.capture[0] > 0x28) {
        if (!g.capture.empty())
            Buddy_DebugPrintf("reldef: drop tail slot %d len %d\n", slot,
                              static_cast<int>(g.capture.size()));
        g.capture.clear();
        g.captureSlot = 0;
    }
}

// Append one engine write atomically; flush capture when the next write won't fit.
// Returns false when the write was too large for a blob (caller counts a drop).
bool CaptureAppend(int slot, const void* data, int len) {
    if (!data || len <= 0)
        return true;
    const auto* p = static_cast<const std::uint8_t*>(data);
    const int capMax = CaptureMax();
    if (len > capMax)
        return false;
    if (g.captureSlot != slot) {
        g.captureSlot = slot;
        g.capture.clear();
        g.capture.reserve(capMax);
    } else if (g.capture.capacity() < static_cast<std::size_t>(capMax))
        g.capture.reserve(capMax);
    int room = capMax - static_cast<int>(g.capture.size());
    if (len > room) {
        // Peel complete messages. Never flush the leftover tail: it is the
        // header of the message this write belongs to, and sealing it would
        // put the payload in the next blob (client: Illegible).
        FlushCompletePrefix();
        if (g.captureSlot != slot) {
            g.captureSlot = slot;
            g.capture.clear();
            g.capture.reserve(capMax);
        }
        room = capMax - static_cast<int>(g.capture.size());
    }
    if (len > room)
        return false;
    g.capture.insert(g.capture.end(), p, p + len);
    if (static_cast<int>(g.capture.size()) >= capMax)
        FlushCompletePrefix();
    return true;
}

bool SlotIsSpawned(int slot1) {
    char* cl = ClientBase(slot1);
    return cl && ReadInt(cl, kClientStateOfs) >= kCsSpawned;
}

bool QueuePopWrite(int slot1) {
    if (slot1 < 1 || slot1 > kMaxSlots)
        return false;
    if (!SlotIsSpawned(slot1)) {
        ClearSlotPending(slot1);
        return false;
    }
    SlotQueue& q = g.slot[slot1 - 1];
    if (q.blobs.empty())
        return false;
    char* cl = ClientBase(slot1);
    if (!cl)
        return false;
    char* msg = cl + kClientMessageOfs;
    const int relLen = ReadInt(cl, kClientReliableLenOfs);
    const int cursize = ReadInt(msg, kSzCursize);
    const auto& blob = q.blobs.front();
    const int blobLen = static_cast<int>(blob.size());
    int wait = 0;
    if (!q.enqueued.empty())
        wait = EngineFramenum() - q.enqueued.front();
    if (wait < 0)
        wait = 0;
    if (!RelDef_CanDrip(relLen, cursize, blobLen, g.limits, g.policy.frameFirst,
                        wait, g.policy.maxDripWaitTicks))
        return false;
    if (!detour_SZ_Write::oSZ_Write)
        return false;
    ++g.bypass;
    detour_SZ_Write::oSZ_Write(
        msg, const_cast<void*>(static_cast<const void*>(blob.data())),
        static_cast<int>(blob.size()));
    --g.bypass;
    q.bytes -= static_cast<int>(blob.size());
    q.blobs.pop_front();
    q.enqueued.pop_front();
    ++g.dripped;
    return true;
}

// Pull changing/reconnect out of the queue and put them in front of whatever
// is already staged. Caller clears the queue afterwards.
void RescueLevelChange(int slot1) {
    if (slot1 < 1 || slot1 > kMaxSlots || !detour_SZ_Write::oSZ_Write)
        return;
    char* cl = ClientBase(slot1);
    if (!cl)
        return;
    std::vector<std::uint8_t> keep;
    for (const auto& b : g.slot[slot1 - 1].blobs)
        if (RelDef_HasLevelChange(b.data(), static_cast<int>(b.size())))
            keep.insert(keep.end(), b.begin(), b.end());
    if (g.captureSlot == slot1 &&
        RelDef_HasLevelChange(g.capture.data(),
                              static_cast<int>(g.capture.size())))
        keep.insert(keep.end(), g.capture.begin(), g.capture.end());
    if (keep.empty())
        return;
    char* msg = cl + kClientMessageOfs;
    const int maxs = ReadInt(msg, kSzMaxsize);
    const int n = ReadInt(msg, kSzCursize);
    std::vector<std::uint8_t> cur;
    if (n > 0) {
        char* raw = *reinterpret_cast<char**>(msg + kSzData);
        if (raw)
            cur.assign(reinterpret_cast<std::uint8_t*>(raw),
                       reinterpret_cast<std::uint8_t*>(raw) + n);
    }
    if (maxs > 0 && static_cast<int>(keep.size() + cur.size()) > maxs)
        cur.clear();
    ClearMessageStaging(msg);
    ++g.bypass;
    detour_SZ_Write::oSZ_Write(msg, keep.data(), static_cast<int>(keep.size()));
    if (!cur.empty())
        detour_SZ_Write::oSZ_Write(msg, cur.data(), static_cast<int>(cur.size()));
    --g.bypass;
}

// Older queued blobs must leave before this frame's staging bytes. Dripping
// onto a staging buffer that already holds a layout puts the next blob's
// first byte after that layout's NUL.
void DripSlot(int slot1) {
    char* cl = ClientBase(slot1);
    if (!cl || !detour_SZ_Write::oSZ_Write)
        return;
    SlotQueue& q = g.slot[slot1 - 1];
    if (q.blobs.empty())
        return;
    char* msg = cl + kClientMessageOfs;
    const int n = ReadInt(msg, kSzCursize);
    char* raw = n > 0 ? *reinterpret_cast<char**>(msg + kSzData) : nullptr;
    // The SV_Map flush is about to transmit this buffer, then SpawnServer
    // drops the client below spawned and the queue is discarded. Lifting
    // "changing" out to drip a scoreboard deletes the packet that starts
    // the client's loading menu.
    if (raw && RelDef_HasLevelChange(reinterpret_cast<const std::uint8_t*>(raw), n))
        return;
    std::vector<std::uint8_t> now;
    if (raw)
        now.assign(reinterpret_cast<std::uint8_t*>(raw),
                   reinterpret_cast<std::uint8_t*>(raw) + n);
    ClearMessageStaging(msg);
    while (QueuePopWrite(slot1)) {
    }
    if (now.empty())
        return;
    // A frame-first hold must not let this newer staging bypass the front blob.
    if (!q.blobs.empty()) {
        if (!QueuePush(slot1, now.data(), static_cast<int>(now.size())))
            Buddy_DebugPrintf("reldef: drop staging slot %d len %d\n", slot1,
                              static_cast<int>(now.size()));
        else
            ++g.deferred;
        return;
    }
    const int maxs = ReadInt(msg, kSzMaxsize);
    const int cur = ReadInt(msg, kSzCursize);
    if (maxs > 0 && cur + static_cast<int>(now.size()) > maxs) {
        if (!QueuePush(slot1, now.data(), static_cast<int>(now.size())))
            Buddy_DebugPrintf("reldef: drop staging slot %d len %d\n", slot1,
                              static_cast<int>(now.size()));
        else
            ++g.deferred;
        return;
    }
    ++g.bypass;
    detour_SZ_Write::oSZ_Write(msg, now.data(), static_cast<int>(now.size()));
    --g.bypass;
}

int SlotFromNetchan(void* netchan) {
    return SlotFromPointer(netchan, kClientNetchanOfs);
}

bool ReorderNetchan(int slot, void* netchan, int length, void* data,
                    detour_Netchan_Transmit::tNetchan_Transmit original) {
    if (!original || !detour_SZ_Write::oSZ_Write || slot < 1 ||
        slot > kMaxSlots)
        return false;
    SlotQueue& q = g.slot[slot - 1];
    if (q.blobs.empty())
        return false;
    char* cl = ClientBase(slot);
    if (!cl)
        return false;
    // A reconnect can reuse a slot before the next pre-send cleanup. Never
    // replay old parcels ahead of the fresh serverdata handshake.
    if (ReadInt(cl, kClientStateOfs) < kCsSpawned) {
        ClearSlotPending(slot);
        return false;
    }
    if (ReadInt(cl, kClientReliableLenOfs) != 0)
        return false;
    char* msg = cl + kClientMessageOfs;
    const int n = ReadInt(msg, kSzCursize);
    const int captureLen =
        g.captureSlot == slot ? static_cast<int>(g.capture.size()) : 0;
    if (n <= 0 && captureLen <= 0)
        return false;
    char* raw = n > 0 ? *reinterpret_cast<char**>(msg + kSzData) : nullptr;
    if (n > 0 && !raw)
        return false;
    const int maxs = ReadInt(msg, kSzMaxsize);
    if (maxs > 0 && n + captureLen > maxs)
        return false;

    std::vector<std::uint8_t> current;
    current.reserve(static_cast<std::size_t>(n + captureLen));
    if (captureLen > 0)
        current.insert(current.end(), g.capture.begin(), g.capture.end());
    if (n > 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(raw);
        current.insert(current.end(), bytes, bytes + n);
    }
    if (RelDef_HasLevelChange(current.data(), static_cast<int>(current.size())))
        return false;

    const auto& older = q.blobs.front();
    int wait = 0;
    if (!q.enqueued.empty()) {
        wait = EngineFramenum() - q.enqueued.front();
        if (wait < 0)
            wait = 0;
    }
    if (!RelDef_CanDrip(0, 0, static_cast<int>(older.size()), g.limits,
                        g.policy.frameFirst, wait, g.policy.maxDripWaitTicks)) {
        const bool frameHold =
            g.policy.frameFirst && g.limits.maxDripBytes > 0 &&
            static_cast<int>(older.size()) > g.limits.maxDripBytes &&
            (g.policy.maxDripWaitTicks <= 0 ||
             wait < g.policy.maxDripWaitTicks);
        if (!frameHold)
            return false;
        // Keep newer reliable bytes behind the held blob while allowing the
        // snapshot itself through. Restore them after the transmit.
        ClearMessageStaging(msg);
        if (captureLen > 0) {
            g.capture.clear();
            g.captureSlot = 0;
        }
        original(netchan, length, data);
        ClearMessageStaging(msg);
        ++g.bypass;
        detour_SZ_Write::oSZ_Write(
            msg, current.data(), static_cast<int>(current.size()));
        --g.bypass;
        return true;
    }
    ClearMessageStaging(msg);
    if (captureLen > 0) {
        g.capture.clear();
        g.captureSlot = 0;
    }
    ++g.bypass;
    detour_SZ_Write::oSZ_Write(
        msg, const_cast<std::uint8_t*>(older.data()), static_cast<int>(older.size()));
    --g.bypass;

    original(netchan, length, data);

    ClearMessageStaging(msg);
    ++g.bypass;
    detour_SZ_Write::oSZ_Write(msg, current.data(), static_cast<int>(current.size()));
    --g.bypass;
    q.bytes -= static_cast<int>(older.size());
    q.blobs.pop_front();
    q.enqueued.pop_front();
    ++g.dripped;
    return true;
}

template <typename WriteNow>
void HandleMessageWrite(void* sb, const void* data, int len, WriteNow&& writeNow,
                        bool canArmOpcode = false, bool isBulk = false,
                        bool isStringWrite = false) {
    if (!g.ready || !g.enabled || g.bypass) {
        writeNow();
        return;
    }
    const int slot = SlotFromMessage(sb);
    if (slot < 0) {
        writeNow();
        return;
    }
    // Ghoul reliable's body is SZ_GetSpace'd into this buffer immediately
    // after the opcode. Only a verified message boundary may arm this path;
    // an arbitrary 0x1A in a binary/text payload is not Ghoul's opcode.
    if (canArmOpcode && len == 1 && data &&
        RelDef_WriteThroughOp(*static_cast<const unsigned char*>(data))) {
        char* cl = ClientBase(slot);
        char* msg = cl ? cl + kClientMessageOfs : nullptr;
        const int stagedLen = msg ? ReadInt(msg, kSzCursize) : 0;
        const char* staged =
            stagedLen > 0 && msg ? *reinterpret_cast<char**>(msg + kSzData) : nullptr;
        const bool stagedBoundary =
            RelDef_EndsAtMessageBoundary(
                reinterpret_cast<const std::uint8_t*>(staged), stagedLen);
        const bool captureForSlot =
            g.captureSlot == slot && !g.capture.empty();
        const bool captureBoundary =
            !captureForSlot ||
            RelDef_EndsAtMessageBoundary(g.capture.data(),
                                         static_cast<int>(g.capture.size()));
        if (cl && ReadInt(cl, kClientStateOfs) >= kCsSpawned &&
            stagedBoundary && captureBoundary &&
            !(captureForSlot && stagedLen > 0)) {
            if (captureForSlot)
                FlushCaptureToMessage(slot);
            writeNow();
            return;
        }
    }
    // Level-change stufftext jumps the queue. Older scoreboards and prints
    // are for the map that is about to go away.
    if (data && RelDef_IsLevelChange(static_cast<const std::uint8_t*>(data), len)) {
        writeNow();
        return;
    }
    // Still joining: serverdata, configstrings, and "cmd begin" are two or
    // more writes into this mailbox. They
    // must not join a capture left over from the previous map — the next
    // send deletes that capture once the client is no longer spawned, and
    // the client sits on the loading menu with no begin.
    bool joining = false;
    if (slot >= 1 && slot <= kMaxSlots) {
        char* cl = ClientBase(slot);
        joining = cl && ReadInt(cl, kClientStateOfs) < kCsSpawned;
    }
    if (!joining && slot >= 1 && slot <= kMaxSlots) {
        int& phase = g.dlPhase[slot - 1];
        if (phase > 0) {
            // Lockstep unit in progress (download/SP-data): every byte goes
            // stock-immediate, preserving the engine's atomic write order.
            if (RelDef_DlStep(phase, isBulk) == RELDEF_DL_NOW) {
                writeNow();
                return;
            }
            // else: data never came (short message) — classify normally below.
        } else if (canArmOpcode && len == 1 && data) {
            // Arm only at a message boundary: staging and capture both empty
            // means this byte opens a new message, so 0x13 here is really
            // download (a level byte mid-print never qualifies).
            const unsigned char op = *static_cast<const unsigned char*>(data);
            char* cl = ClientBase(slot);
            char* msg = cl ? cl + kClientMessageOfs : nullptr;
            const bool stagingEmpty = !msg || ReadInt(msg, kSzCursize) <= 0;
            const bool captureEmpty = g.captureSlot != slot || g.capture.empty();
            // Both must be empty. Arming while capture still holds older mail
            // writes the new packet into staging and it goes out first.
            if (RelDef_IsLockstepOp(op) && stagingEmpty && captureEmpty) {
                phase = 3;
                writeNow();
                return;
            }
        }
    }
    if (!joining && g.captureSlot > 0 && g.captureSlot != slot)
        FlushCapture();

    // Frame/effect packets can legally use the reliable mailbox in a few
    // multicast paths, but their bodies are opaque to the server-side cutter.
    // SZ_Write supplies the complete packet; preserve it as one blob only at
    // a real message boundary, never when this is a body write.
    if (!joining && isBulk && data && len > 0 &&
        RelDef_IsOpaquePacket(static_cast<const std::uint8_t*>(data), len)) {
        char* cl = ClientBase(slot);
        char* msg = cl ? cl + kClientMessageOfs : nullptr;
        const int stagedLen = msg ? ReadInt(msg, kSzCursize) : 0;
        const char* staged =
            stagedLen > 0 && msg ? *reinterpret_cast<char**>(msg + kSzData) : nullptr;
        const bool captureForSlot =
            g.captureSlot == slot && !g.capture.empty();
        const bool stagedBoundary = RelDef_EndsAtMessageBoundary(
            reinterpret_cast<const std::uint8_t*>(staged), stagedLen);
        const bool captureBoundary =
            !captureForSlot ||
            RelDef_EndsAtMessageBoundary(g.capture.data(),
                                         static_cast<int>(g.capture.size()));
        if (stagedBoundary && captureBoundary) {
            const RelDefAction act = ClassifySlot(slot, len);
            if (act == RELDEF_WRITE_NOW && !captureForSlot) {
                writeNow();
                return;
            }
            if (act == RELDEF_WRITE_NOW || act == RELDEF_QUEUE) {
                // Existing complete bytes are older than this opaque write.
                // Keep them in front of it without sending either through the
                // message cutter. Separate blobs also avoid exceeding the
                // engine message size when staging is already large.
                int pushed = 0;
                bool queued = true;
                if (captureForSlot)
                    queued = QueuePushRaw(
                        slot, g.capture.data(), static_cast<int>(g.capture.size()));
                if (queued && captureForSlot)
                    ++pushed;
                if (queued && stagedLen > 0)
                    queued = QueuePushRaw(slot, staged, stagedLen);
                if (queued && stagedLen > 0)
                    ++pushed;
                if (queued)
                    queued = QueuePushRaw(slot, data, len);
                if (queued) {
                    ++pushed;
                    g.deferred += pushed;
                    g.capture.clear();
                    g.captureSlot = 0;
                    ClearMessageStaging(msg);
                    return;
                }
            }
            ++g.dropped;
            Buddy_DebugPrintf("reldef: drop opaque slot %d len %d\n", slot, len);
            return;
        }
    }

    // Once this tick's capture holds bytes for a slot, every later write for
    // that slot must join the capture. Letting a later write go NOW while
    // older bytes sit in capture/queue would ship newer bytes first
    // (reorder) and could leave a blob ending mid-svc_*.
    // Joining clients skip this: classify below write-nows and drops the
    // stale capture instead of appending "cmd begin" onto it.
    if (!joining && g.captureSlot == slot && !g.capture.empty()) {
        if (!CaptureAppend(slot, data, len)) {
            ++g.dropped;
            Buddy_DebugPrintf("reldef: drop write slot %d len %d\n", slot, len);
        } else {
            ++g.captured;
        }
        return;
    }

    RelDefAction act = ClassifySlot(slot, len);
    if (act == RELDEF_WRITE_NOW && isStringWrite) {
        // Unicast layout is one SZ_Write and already ends on its NUL. A later
        // string whose opcode is elsewhere must not be appended after it.
        char* cl = ClientBase(slot);
        char* msg = cl ? cl + kClientMessageOfs : nullptr;
        const int n = msg ? ReadInt(msg, kSzCursize) : 0;
        const char* staged = (n > 0 && msg) ? *reinterpret_cast<char**>(msg + kSzData) : nullptr;
        if (!RelDef_StringContinues(reinterpret_cast<const std::uint8_t*>(staged), n)) {
            ++g.dropped;
            Buddy_DebugPrintf("reldef: drop orphan string slot %d len %d\n", slot, len);
            return;
        }
    }
    if (act == RELDEF_WRITE_NOW) {
        if (g.captureSlot == slot)
            FlushCaptureToMessage(slot);
        else if (g.captureSlot > 0)
            FlushCapture();
        writeNow();
        return;
    }
    if (act == RELDEF_QUEUE) {
        char* cl = ClientBase(slot);
        if (cl) {
            char* msg = cl + kClientMessageOfs;
            if (ReadInt(msg, kSzCursize) > 0)
                AbsorbMessageIntoCapture(slot);
        }
        if (!CaptureAppend(slot, data, len)) {
            ++g.dropped;
            Buddy_DebugPrintf("reldef: drop write slot %d len %d\n", slot, len);
        } else {
            ++g.captured;
        }
        return;
    }
    ++g.dropped;
    Buddy_DebugPrintf("reldef: drop write slot %d len %d\n", slot, len);
}

// Classifier-only MSG_WriteLong hook: when equip/countdown continues a
// deferred capture, append 4 B here. No engine fixture — build + cutter
// tests cover the stream shape; this path is integration-only.
bool AppendLongToCapture(void* sb, int c) {
    if (!g.ready || !g.enabled || g.bypass)
        return false;
    const int slot = SlotFromMessage(sb);
    if (slot < 0)
        return false;
    char* cl = ClientBase(slot);
    if (!cl || ReadInt(cl, kClientStateOfs) < kCsSpawned)
        return false;
    if (g.captureSlot != slot || g.capture.empty())
        return false;
    if (RelDef_LastCompleteEnd(g.capture.data(),
                               static_cast<int>(g.capture.size())) >=
        static_cast<int>(g.capture.size()))
        return false;
    char* msg = cl + kClientMessageOfs;
    if (ReadInt(msg, kSzCursize) > 0)
        AbsorbMessageIntoCapture(slot);
    const unsigned char b[4] = {static_cast<unsigned char>(c),
                                static_cast<unsigned char>(c >> 8),
                                static_cast<unsigned char>(c >> 16),
                                static_cast<unsigned char>(c >> 24)};
    if (!CaptureAppend(slot, b, 4)) {
        ++g.dropped;
        Buddy_DebugPrintf("reldef: drop long slot %d\n", slot);
        return true;
    }
    ++g.captured;
    return true;
}

bool MessageHasLevelChange(char* cl) {
    char* msg = cl + kClientMessageOfs;
    const int n = ReadInt(msg, kSzCursize);
    if (n <= 0)
        return false;
    char* raw = *reinterpret_cast<char**>(msg + kSzData);
    return raw && RelDef_HasLevelChange(reinterpret_cast<const std::uint8_t*>(raw), n);
}

void SuppressLevelChangeSnapshots() {
    const int n = g.maxClients;
    for (int slot = 1; slot <= n; ++slot) {
        g.holdSnapshot[slot - 1] = false;
        char* cl = ClientBase(slot);
        if (!cl || !RelDef_SuppressSnapshot(ReadInt(cl, kClientStateOfs),
                                            MessageHasLevelChange(cl)))
            continue;
        *reinterpret_cast<int*>(cl + kClientStateOfs) = kCsConnected;
        g.holdSnapshot[slot - 1] = true;
    }
}

void RestoreLevelChangeSnapshots() {
    const int n = g.maxClients;
    for (int slot = 1; slot <= n; ++slot) {
        if (!g.holdSnapshot[slot - 1])
            continue;
        g.holdSnapshot[slot - 1] = false;
        char* cl = ClientBase(slot);
        if (cl && ReadInt(cl, kClientStateOfs) == kCsConnected)
            *reinterpret_cast<int*>(cl + kClientStateOfs) = kCsSpawned;
    }
}

void PublishTickGauges() {
    int oldest = 0;
    const int now = EngineFramenum();
    for (int slot = 1; slot <= g.maxClients; ++slot) {
        const SlotQueue& q = g.slot[slot - 1];
        if (!q.enqueued.empty()) {
            int w = now - q.enqueued.front();
            if (w < 0)
                w = 0;
            if (w > oldest)
                oldest = w;
        }
    }
    const int captureBytes =
        g.captureSlot > 0 ? static_cast<int>(g.capture.size()) : 0;
    RelDef_PublishGauges(g.queued, g.dripped, g.dropped, captureBytes, oldest);
}

}  // namespace reldef_internal

void reldef_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    RelDef_InitCvars();
    reldef_internal::RefreshCache();
    reldef_internal::g.ready = true;
}

void reldef_SZ_Write(void* sb, void* data, int length,
                     detour_SZ_Write::tSZ_Write original) {
    if (!original)
        return;
    reldef_internal::HandleMessageWrite(sb, data, length,
                                        [&] { original(sb, data, length); },
                                        false, true);
}

void reldef_Netchan_Transmit(
    void* netchan, int length, void* data,
    detour_Netchan_Transmit::tNetchan_Transmit original) {
    if (!original)
        return;
    const int slot = reldef_internal::SlotFromNetchan(netchan);
    if (!reldef_internal::g.ready || !reldef_internal::g.enabled ||
        !reldef_internal::ReorderNetchan(slot, netchan, length, data, original))
        original(netchan, length, data);
}

void reldef_MSG_WriteByte(void* sb, int c,
                          detour_MSG_WriteByte::tMSG_WriteByte original) {
    if (!original)
        return;
    const unsigned char b = static_cast<unsigned char>(c);
    reldef_internal::HandleMessageWrite(sb, &b, 1,
                                        [&] { original(sb, c); }, true);
}

void reldef_MSG_WriteShort(void* sb, int c,
                           detour_MSG_WriteShort::tMSG_WriteShort original) {
    if (!original)
        return;
    unsigned char b[2] = {static_cast<unsigned char>(c),
                          static_cast<unsigned char>(c >> 8)};
    reldef_internal::HandleMessageWrite(sb, b, 2,
                                          [&] { original(sb, c); });
}

void reldef_MSG_WriteLong(void* sb, int c,
                          detour_MSG_WriteLong::tMSG_WriteLong original) {
    if (!original)
        return;
    if (!reldef_internal::AppendLongToCapture(sb, c))
        original(sb, c);
}

void reldef_MSG_WriteString(void* sb, char* s,
                            detour_MSG_WriteString::tMSG_WriteString original) {
    if (!original)
        return;
    const int len = s ? static_cast<int>(std::strlen(s)) + 1 : 1;
    reldef_internal::HandleMessageWrite(
        sb, s ? static_cast<const void*>(s) : "", len,
        [&] { original(sb, s); }, false, false, true);
}
void reldef_CL_SendClientMessagesPre() {
    if (!reldef_internal::g.ready)
        return;
    reldef_internal::RefreshCache();
    if (reldef_internal::g.enabled) {
        // Lockstep units never span ticks (engine writes each atomically in one
        // frame); disarm stale phases so an aborted unit can't bypass later mail.
        for (int i = 0; i < reldef_internal::kMaxSlots; ++i)
            reldef_internal::g.dlPhase[i] = 0;
        const int n = reldef_internal::g.maxClients;
        for (int slot = 1; slot <= n; ++slot) {
            if (!reldef_internal::SlotIsSpawned(slot)) {
                reldef_internal::RescueLevelChange(slot);
                reldef_internal::ClearSlotPending(slot);
            }
        }
        reldef_internal::FlushCapture();
        for (int slot = 1; slot <= n; ++slot) {
            if (!reldef_internal::SlotIsSpawned(slot))
                continue;
            reldef_internal::DripSlot(slot);
        }
        reldef_internal::SuppressLevelChangeSnapshots();
        if (reldef_internal::g.disabling &&
            reldef_internal::PendingEmpty()) {
            reldef_internal::g.enabled = false;
            reldef_internal::g.disabling = false;
        }
    }
    reldef_internal::PublishTickGauges();
}

void reldef_CL_SendClientMessagesPost() {
    reldef_internal::RestoreLevelChangeSnapshots();
}
