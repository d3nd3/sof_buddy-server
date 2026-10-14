#pragma once

// Pure, host-testable state machine for the stufftext reconnect chain.
//
//   stufftext_reconnect <slot>
//     1. verify + set the engine's attractloop flag (read-back checked), then
//        neuter the connect-refusal check for the chain window (see the patch
//        notes in stufftext_reconnect.cpp): the flag reads set, but nobody -
//        target included - is refused while it runs,
//     2. stuff "reconnect" to the target slot,
//     3. once the target is observed server-side as cs_connected
//        (reconnecting) - or after a timeout - stuff the welcome line,
//     4. restore the refusal byte, then the attractloop flag,
//     5. stuff "reconnect" to the target when still present, plus every other
//        slot that entered cs_connected at any tick while the flag was set.
//
// Why the patch: stock SVC_DirectConnect refuses *every* remote connect while
// attractloop is set - including the target's own (only true NA_LOOPBACK
// bypasses; a UDP client, even on 127.0.0.1, is refused). Without neutering,
// the target parks in the getchallenge/connect retry loop, its slot goes
// zombie/free, and no later unicast has anyone to reach. The 1-byte patch
// cannot discriminate target from strangers, so during the window exclusion
// is traded for a working resync: whoever arrives is tracked and re-stuffed
// in the final round.
//
// No engine / windows includes on purpose: tools/tests/stufftext builds this
// header for the host. The engine binding (flag poke + read-back, code patch,
// client-state scan, Buddy_StuffText sends, SV_Frame ticker) lives in
// stufftext_reconnect.cpp and only feeds this machine observations
// (targetConnecting, clock) and executes the actions it returns.
//
// Wrap safety: all times are uint32_t milliseconds in GetTickCount() domain;
// elapsed comparisons use unsigned subtraction so the 49.7-day wrap is safe.

#include <cstddef>
#include <cstdint>

enum StuffReconnectStage : std::uint8_t {
    STUFF_RECONNECT_IDLE = 0,
    // First "reconnect" stuffed, lock armed, waiting for the target to show
    // up as cs_connected (or for the timeout to fire).
    STUFF_RECONNECT_WAIT_CONNECTING = 1,
    // Welcome stuffed, waiting out the reliable-delivery delay before the
    // unlock and the final "reconnect" round.
    STUFF_RECONNECT_WAIT_WELCOME = 2,
};

enum StuffReconnectAction : std::uint8_t {
    STUFF_RECONNECT_NONE = 0,
    // Caller must stuff the welcome line to the target now.
    STUFF_RECONNECT_SEND_WELCOME = 1,
    // Chain is over (state already back to IDLE): caller must restore the
    // refusal byte, then the attractloop flag, then send the final round.
    STUFF_RECONNECT_FINISH = 2,
};

struct StuffReconnectConfig {
    // How long to wait for the target to enter cs_connected before sending
    // the welcome anyway. Covers zombie expiry, sv_reconnect_limit rejection
    // plus one client retransmit cycle (getchallenge every 3 s).
    std::uint32_t waitConnectingMs = 10000u;
    // Quiet period after the welcome so it leaves in the reliable stream
    // before the unlock and the final round are stuffed.
    std::uint32_t welcomeDelayMs = 2000u;
};

struct StuffReconnectState {
    static const int kMaxSlots = 64;
    StuffReconnectStage stage = STUFF_RECONNECT_IDLE;
    int targetSlot = -1;
    std::uint32_t stageStartMs = 0;
    bool priorAttract = false;
    // Latched once the target slot reads anything other than connected or
    // spawned (zombie / free / unreadable). Informational: with the refusal
    // neutered the target normally passes through zombie into cs_connected,
    // so this marks the disconnect half of its cycle in the logs.
    bool targetLeft = false;
    // Slots observed in cs_connected at any tick while the chain was active
    // (including the target). Used for the final round.
    bool seenConnecting[kMaxSlots] = {};
};

inline bool StuffReconnect_Elapsed(std::uint32_t nowMs, std::uint32_t startMs,
                                   std::uint32_t spanMs) {
    return static_cast<std::uint32_t>(nowMs - startMs) >= spanMs;
}

inline void StuffReconnect_Begin(StuffReconnectState& st, int targetSlot,
                                 std::uint32_t nowMs, bool priorAttract) {
    st.stage = STUFF_RECONNECT_WAIT_CONNECTING;
    st.targetSlot = targetSlot;
    st.stageStartMs = nowMs;
    st.priorAttract = priorAttract;
    st.targetLeft = false;
    for (int i = 0; i < StuffReconnectState::kMaxSlots; ++i)
        st.seenConnecting[i] = false;
}

inline void StuffReconnect_Reset(StuffReconnectState& st) {
    st.stage = STUFF_RECONNECT_IDLE;
    st.targetSlot = -1;
    st.stageStartMs = 0;
    st.priorAttract = false;
    st.targetLeft = false;
    for (int i = 0; i < StuffReconnectState::kMaxSlots; ++i)
        st.seenConnecting[i] = false;
}

inline void StuffReconnect_MarkSeen(StuffReconnectState& st, int slot) {
    if (slot >= 0 && slot < StuffReconnectState::kMaxSlots)
        st.seenConnecting[slot] = true;
}

// Advance the machine one observation. `targetConnecting` is true when the
// target slot currently reads cs_connected server-side. At most one action
// is returned per call; the caller executes it (send welcome / finish).
inline StuffReconnectAction StuffReconnect_Poll(StuffReconnectState& st,
                                                const StuffReconnectConfig& cfg,
                                                std::uint32_t nowMs,
                                                bool targetConnecting) {
    switch (st.stage) {
        case STUFF_RECONNECT_WAIT_CONNECTING:
            if (targetConnecting ||
                StuffReconnect_Elapsed(nowMs, st.stageStartMs, cfg.waitConnectingMs)) {
                st.stage = STUFF_RECONNECT_WAIT_WELCOME;
                st.stageStartMs = nowMs;
                return STUFF_RECONNECT_SEND_WELCOME;
            }
            return STUFF_RECONNECT_NONE;
        case STUFF_RECONNECT_WAIT_WELCOME:
            if (StuffReconnect_Elapsed(nowMs, st.stageStartMs, cfg.welcomeDelayMs)) {
                st.stage = STUFF_RECONNECT_IDLE;
                return STUFF_RECONNECT_FINISH;
            }
            return STUFF_RECONNECT_NONE;
        case STUFF_RECONNECT_IDLE:
        default:
            return STUFF_RECONNECT_NONE;
    }
}

// Build the final "reconnect" recipient list: the target first when still
// present (regardless of whether it was seen connecting), then every *other*
// seen slot that is still present, ascending. `present[i]` is the
// engine-side occupancy for slot i (cs_connected or cs_spawned) at finish
// time; vanished slots are skipped so a player who left mid-chain is not
// stuffed into a recycled slot.
// `slotCount` is clamped to kMaxSlots. Returns entries written (<= maxOut).
inline int StuffReconnect_FinalList(const StuffReconnectState& st,
                                    const bool* present, int slotCount,
                                    int* outSlots, int maxOut) {
    if (!present || !outSlots || maxOut <= 0 || slotCount <= 0)
        return 0;
    if (slotCount > StuffReconnectState::kMaxSlots)
        slotCount = StuffReconnectState::kMaxSlots;
    int n = 0;
    if (st.targetSlot >= 0 && st.targetSlot < slotCount && present[st.targetSlot] &&
        n < maxOut)
        outSlots[n++] = st.targetSlot;
    for (int i = 0; i < slotCount && n < maxOut; ++i) {
        if (i == st.targetSlot)
            continue;
        if (st.seenConnecting[i] && present[i])
            outSlots[n++] = i;
    }
    return n;
}
