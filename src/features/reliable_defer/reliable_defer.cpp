// reliable_defer: queue server→client reliable staging (netchan.message).
// Hooks SZ_Write + MSG_Write* so every path is covered. Within a frame, all
// client appends for one slot accumulate in a capture; blobs are cut only at
// complete svc_print (0x0B) boundaries so a blob never ends mid-message
// (orphan opcode+level = "Illegible server message" on the client).

#include "cvar.h"
#include "engine.h"
#include "reliable_defer_logic.h"

#include "buddy_import.h"
#include "generated_detours.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

namespace reldef_internal {

constexpr int kMaxSlots = 64;

struct SlotQueue {
    std::vector<std::vector<std::uint8_t>> blobs;
    std::vector<int> enqueued;  // sv_framenum when each blob was queued
    int bytes = 0;
};

struct State {
    bool ready = false;
    RelDefPolicy policy;
    SlotQueue slot[kMaxSlots];
    int captureSlot = 0;
    std::vector<std::uint8_t> capture;
    // Drip's own oSZ_Write bypass depth.
    int bypass = 0;
    // Per-slot lockstep bypass phase (see RelDef_DlStep): binary multi-write
    // messages started at a message boundary skip classification entirely.
    int dlPhase[kMaxSlots] = {};
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

int MaxClients() {
    void* cv = Buddy_GetEngineCvar("maxclients", "8", 0, nullptr);
    const int n = static_cast<int>(Buddy_ReadCvarValue(cv, 8.0f));
    return n > 0 && n <= kMaxSlots ? n : 8;
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

RelDefLimits CurrentLimits() {
    g.policy = RelDef_ReadPolicy();
    return RelDef_ComputeLimits(EngineBuffersize(), g.policy);
}

int CaptureMax() {
    const RelDefLimits lim = CurrentLimits();
    return lim.msgMaxsize > 0 ? lim.msgMaxsize : 1384;
}

char* ClientBase(int slot1) {
    if (slot1 < 1 || slot1 > kMaxSlots)
        return nullptr;
    char* clients = *Rva<char*>(kRvaSvsClients);
    if (!clients)
        return nullptr;
    return clients + static_cast<unsigned>(slot1 - 1) * kClientStride;
}

int SlotFromMessage(void* sb) {
    if (!sb)
        return -1;
    char* clients = *Rva<char*>(kRvaSvsClients);
    if (!clients)
        return -1;
    const auto sbAddr = reinterpret_cast<std::uintptr_t>(sb);
    const auto baseAddr = reinterpret_cast<std::uintptr_t>(clients);
    if (sbAddr < baseAddr + kClientMessageOfs)
        return -1;
    const auto diff = sbAddr - baseAddr;
    const auto rel = diff - kClientMessageOfs;
    if (rel % kClientStride != 0)
        return -1;
    const int slot0 = static_cast<int>(rel / kClientStride);
    if (slot0 < 0 || slot0 >= MaxClients())
        return -1;
    return slot0 + 1;
}

int ReadInt(const char* base, unsigned ofs) {
    return *reinterpret_cast<const int*>(base + ofs);
}

bool Enabled() {
    void* cv = Buddy_GetEngineCvar("_sofbuddy_reldef", "1", 1, nullptr);
    return Buddy_ReadCvarValue(cv, 1.0f) != 0.0f;
}

int EngineFramenum() {
    int* p = Rva<int>(kRvaSvFramenum);
    return p ? *p : 0;
}

bool QueuePush(int slot1, const void* data, int len) {
    if (slot1 < 1 || slot1 > kMaxSlots || len <= 0 || !data)
        return false;
    SlotQueue& q = g.slot[slot1 - 1];
    g.policy = RelDef_ReadPolicy();
    if (g.policy.maxQueue > 0 && static_cast<int>(q.blobs.size()) >= g.policy.maxQueue) {
        ++g.dropped;
        return false;
    }
    if (g.policy.maxQueueBytes > 0 && q.bytes + len > g.policy.maxQueueBytes) {
        if (!q.blobs.empty()) {
            q.bytes -= static_cast<int>(q.blobs.front().size());
            q.blobs.erase(q.blobs.begin());
            q.enqueued.erase(q.enqueued.begin());
            ++g.dropped;
        }
        if (q.bytes + len > g.policy.maxQueueBytes) {
            ++g.dropped;
            return false;
        }
    }
    std::vector<std::uint8_t> blob(static_cast<std::size_t>(len));
    std::memcpy(blob.data(), data, static_cast<std::size_t>(len));
    q.bytes += len;
    q.blobs.push_back(std::move(blob));
    q.enqueued.push_back(EngineFramenum());
    ++g.queued;
    return true;
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

RelDefAction ClassifySlot(int slot1, int incomingLen) {
    char* cl = ClientBase(slot1);
    if (!cl)
        return RELDEF_WRITE_NOW;
    const int state = ReadInt(cl, kClientStateOfs);
    if (state < kCsSpawned) {
        ClearSlotPending(slot1);
        return RELDEF_WRITE_NOW;
    }
    char* msg = cl + kClientMessageOfs;
    const RelDefLimits lim = CurrentLimits();
    SlotQueue& q = g.slot[slot1 - 1];
    return RelDef_Classify(ReadInt(msg, kSzCursize), incomingLen,
                           ReadInt(cl, kClientReliableLenOfs),
                           static_cast<int>(q.blobs.size()), q.bytes, lim, g.policy,
                           state);
}

void FlushCapture() {
    if (g.captureSlot <= 0 || g.capture.empty())
        return;
    const int slot = g.captureSlot;
    const int len = static_cast<int>(g.capture.size());
    if (QueuePush(slot, g.capture.data(), len))
        ++g.deferred;
    else
        Buddy_DebugPrintf("reldef: drop capture slot %d len %d\n", slot, len);
    g.capture.clear();
    g.captureSlot = 0;
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
    if (g.captureSlot != slot1)
        g.capture.clear();
    g.captureSlot = slot1;
    g.capture.insert(g.capture.end(),
                     reinterpret_cast<std::uint8_t*>(data),
                     reinterpret_cast<std::uint8_t*>(data) + n);
    ClearMessageStaging(msg);
}

constexpr std::uint8_t kSvcPrint = 0x0B;  // SV_BroadcastPrintf opcode (IDA 0x200618D0)

// End offset of the last complete svc_print in capture, or 0 when the first
// message is incomplete/unknown. Capture always starts at a message boundary,
// so scanning forward from 0 is sound.
int LastCompletePrintEnd() {
    const int n = static_cast<int>(g.capture.size());
    const std::uint8_t* p = g.capture.data();
    int pos = 0;
    int lastEnd = 0;
    while (pos < n) {
        if (p[pos] != kSvcPrint)
            break;
        if (pos + 2 > n)
            break;  // need opcode + level
        int k = pos + 2;
        while (k < n && p[k] != 0)
            ++k;
        if (k >= n)
            break;  // string runs past the buffer: incomplete
        pos = k + 1;
        lastEnd = pos;
    }
    return lastEnd;
}

// Queue the complete-print prefix of capture, keep the incomplete tail for
// the slot. Falls back to a whole flush when nothing parseable is present.
void FlushCompletePrefix() {
    if (g.captureSlot <= 0 || g.capture.empty())
        return;
    const int end = LastCompletePrintEnd();
    const int n = static_cast<int>(g.capture.size());
    if (end <= 0 || end >= n) {
        FlushCapture();
        return;
    }
    const int slot = g.captureSlot;
    if (QueuePush(slot, g.capture.data(), end))
        ++g.deferred;
    else
        Buddy_DebugPrintf("reldef: drop capture slot %d len %d\n", slot, end);
    g.capture.erase(g.capture.begin(), g.capture.begin() + end);
    // captureSlot stays: the tail belongs to the same slot.
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
    }
    int room = capMax - static_cast<int>(g.capture.size());
    if (len > room) {
        // Split at the last complete svc_print so no blob ends mid-message
        // (a blob ending with orphan opcode+level breaks the client parser).
        FlushCompletePrefix();
        if (g.captureSlot != slot) {
            g.captureSlot = slot;
            g.capture.clear();
        }
        room = capMax - static_cast<int>(g.capture.size());
    }
    if (len > room) {
        FlushCapture();
        if (g.captureSlot != slot) {
            g.captureSlot = slot;
            g.capture.clear();
        }
        room = capMax;
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
    const RelDefLimits lim = CurrentLimits();
    int wait = 0;
    if (!q.enqueued.empty())
        wait = EngineFramenum() - q.enqueued.front();
    if (wait < 0)
        wait = 0;
    if (!RelDef_CanDrip(relLen, cursize, blobLen, lim, g.policy.frameFirst,
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
    q.blobs.erase(q.blobs.begin());
    if (!q.enqueued.empty())
        q.enqueued.erase(q.enqueued.begin());
    ++g.dripped;
    return true;
}

template <typename WriteNow>
void HandleMessageWrite(void* sb, const void* data, int len, WriteNow&& writeNow,
                        bool canArmOpcode = false, bool isBulk = false) {
    if (!g.ready || !Enabled() || g.bypass) {
        writeNow();
        return;
    }
    const int slot = SlotFromMessage(sb);
    if (slot < 0) {
        writeNow();
        return;
    }
    if (slot >= 1 && slot <= kMaxSlots) {
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
            // means this byte opens a new message, so 0x13/0x24/0x25 here is
            // really a lockstep opcode (a level byte mid-print never qualifies).
            const unsigned char op = *static_cast<const unsigned char*>(data);
            char* cl = ClientBase(slot);
            char* msg = cl ? cl + kClientMessageOfs : nullptr;
            const bool stagingEmpty = !msg || ReadInt(msg, kSzCursize) <= 0;
            const bool captureEmpty = g.captureSlot != slot || g.capture.empty();
            if (RelDef_IsLockstepOp(op) && stagingEmpty && captureEmpty) {
                phase = 3;
                writeNow();
                return;
            }
        }
    }
    if (g.captureSlot > 0 && g.captureSlot != slot)
        FlushCapture();

    // Once this tick's capture holds bytes for a slot, every later write for
    // that slot must join the capture. Letting a later write go NOW while
    // older bytes sit in capture/queue would ship newer bytes first
    // (reorder) and could leave a blob ending mid-svc_*.
    if (g.captureSlot == slot && !g.capture.empty()) {
        if (!CaptureAppend(slot, data, len)) {
            ++g.dropped;
            Buddy_DebugPrintf("reldef: drop write slot %d len %d\n", slot, len);
        } else {
            ++g.captured;
        }
        return;
    }

    const RelDefAction act = ClassifySlot(slot, len);
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

}  // namespace reldef_internal

void reldef_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    reldef_internal::g.policy = RelDef_ReadPolicy();
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

void reldef_MSG_WriteString(void* sb, char* s,
                            detour_MSG_WriteString::tMSG_WriteString original) {
    if (!original)
        return;
    const int len = s ? static_cast<int>(std::strlen(s)) + 1 : 1;
    reldef_internal::HandleMessageWrite(
        sb, s ? static_cast<const void*>(s) : "", len,
        [&] { original(sb, s); });
}
void reldef_CL_SendClientMessagesPre() {
    if (!reldef_internal::g.ready || !reldef_internal::Enabled())
        return;
    // Lockstep units never span ticks (engine writes each atomically in one
    // frame); disarm stale phases so an aborted unit can't bypass later mail.
    for (int i = 0; i < reldef_internal::kMaxSlots; ++i)
        reldef_internal::g.dlPhase[i] = 0;
    const int n = reldef_internal::MaxClients();
    for (int slot = 1; slot <= n; ++slot) {
        if (!reldef_internal::SlotIsSpawned(slot))
            reldef_internal::ClearSlotPending(slot);
    }
    reldef_internal::FlushCapture();
    for (int slot = 1; slot <= n; ++slot) {
        if (!reldef_internal::SlotIsSpawned(slot))
            continue;
        // Drain in FIFO order while the lane is open and the next blob fits
        // staging. Blobs hold whole messages (prints, svc_equip, ...), so
        // filling staging reassembles the original byte order exactly.
        while (reldef_internal::QueuePopWrite(slot)) {
        }
    }
}
