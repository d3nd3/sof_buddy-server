// Host tests for src/features/stufftext/stufftext_reconnect_logic.h.
// Pure state-machine checks: no engine, no windows, no stubs needed.
#include <cstdint>
#include <cstdio>

#include "stufftext_reconnect_logic.h"

static int g_fails = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                     \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

int main() {
    StuffReconnectConfig cfg;  // 10000 ms wait, 2000 ms welcome delay

    // Begin parks in WAIT_CONNECTING with a clean seen set.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 3, 1000u, false);
        CHECK(st.stage == STUFF_RECONNECT_WAIT_CONNECTING);
        CHECK(st.targetSlot == 3);
        CHECK(st.stageStartMs == 1000u);
        CHECK(st.priorAttract == false);
        CHECK(st.targetLeft == false);
        CHECK(st.seenConnecting[3] == false);
        // Early poll, target not connecting: nothing due.
        CHECK(StuffReconnect_Poll(st, cfg, 1001u, false) == STUFF_RECONNECT_NONE);
        CHECK(st.stage == STUFF_RECONNECT_WAIT_CONNECTING);
        // Just before the timeout: still nothing.
        CHECK(StuffReconnect_Poll(st, cfg, 1000u + 9999u, false) == STUFF_RECONNECT_NONE);
        CHECK(st.stage == STUFF_RECONNECT_WAIT_CONNECTING);
    }

    // Target observed connecting: welcome goes out immediately.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 3, 5000u, false);
        StuffReconnect_MarkSeen(st, 3);
        CHECK(StuffReconnect_Poll(st, cfg, 5100u, true) == STUFF_RECONNECT_SEND_WELCOME);
        CHECK(st.stage == STUFF_RECONNECT_WAIT_WELCOME);
        CHECK(st.stageStartMs == 5100u);
        // Welcome delay not yet over: nothing.
        CHECK(StuffReconnect_Poll(st, cfg, 5100u + 1999u, false) == STUFF_RECONNECT_NONE);
        // Delay over: finish, machine back to idle.
        CHECK(StuffReconnect_Poll(st, cfg, 5100u + 2000u, false) == STUFF_RECONNECT_FINISH);
        CHECK(st.stage == STUFF_RECONNECT_IDLE);
        // Idle polls never fire.
        CHECK(StuffReconnect_Poll(st, cfg, 999999u, true) == STUFF_RECONNECT_NONE);
    }

    // Timeout path: no observation, welcome still goes out, then finish.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 0, 0u, true);
        CHECK(st.priorAttract == true);
        CHECK(StuffReconnect_Poll(st, cfg, 10000u, false) == STUFF_RECONNECT_SEND_WELCOME);
        CHECK(st.stage == STUFF_RECONNECT_WAIT_WELCOME);
        CHECK(StuffReconnect_Poll(st, cfg, 12000u, false) == STUFF_RECONNECT_FINISH);
        CHECK(st.stage == STUFF_RECONNECT_IDLE);
    }

    // GetTickCount wrap: start near UINT32_MAX, action after the wrap.
    {
        StuffReconnectState st;
        const std::uint32_t near_wrap = 0xFFFFFF00u;  // 256 ms before wrap
        StuffReconnect_Begin(st, 1, near_wrap, false);
        CHECK(StuffReconnect_Poll(st, cfg, 0xFFFFFFFFu, false) == STUFF_RECONNECT_NONE);
        // 10000 ms after start == 9744 ms after the wrap.
        CHECK(StuffReconnect_Poll(st, cfg, 9744u, false) == STUFF_RECONNECT_SEND_WELCOME);
        // Welcome delay across the same wrap.
        CHECK(StuffReconnect_Poll(st, cfg, 9744u + 2000u, false) == STUFF_RECONNECT_FINISH);
    }

    // MarkSeen ignores out-of-range slots; Reset clears everything.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 0, 0u, false);
        StuffReconnect_MarkSeen(st, -1);
        StuffReconnect_MarkSeen(st, 64);
        StuffReconnect_MarkSeen(st, 1000);
        for (int i = 0; i < StuffReconnectState::kMaxSlots; ++i)
            CHECK(st.seenConnecting[i] == false);
        st.targetLeft = true;
        StuffReconnect_Reset(st);
        CHECK(st.stage == STUFF_RECONNECT_IDLE);
        CHECK(st.targetSlot == -1);
        CHECK(st.targetLeft == false);
    }

    // Final list: present target first, then other seen+present slots.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 3, 0u, false);
        StuffReconnect_MarkSeen(st, 1);
        StuffReconnect_MarkSeen(st, 3);  // target seen too: listed once
        StuffReconnect_MarkSeen(st, 5);
        StuffReconnect_MarkSeen(st, 7);  // seen but gone: skipped
        bool present[StuffReconnectState::kMaxSlots] = {};
        present[1] = true;
        present[3] = true;
        present[5] = true;
        present[6] = true;  // present but never seen: skipped
        int out[StuffReconnectState::kMaxSlots] = {};
        const int n = StuffReconnect_FinalList(st, present, 8, out,
                                               StuffReconnectState::kMaxSlots);
        CHECK(n == 3);
        CHECK(out[0] == 3);
        CHECK(out[1] == 1);
        CHECK(out[2] == 5);
    }

    // Final list: vanished target is skipped, others stay.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 3, 0u, false);
        st.targetLeft = true;
        StuffReconnect_MarkSeen(st, 1);
        StuffReconnect_MarkSeen(st, 3);
        bool present[StuffReconnectState::kMaxSlots] = {};
        present[1] = true;
        present[3] = false;  // left for good
        int out[StuffReconnectState::kMaxSlots] = {};
        const int n = StuffReconnect_FinalList(st, present, 8, out,
                                               StuffReconnectState::kMaxSlots);
        CHECK(n == 1);
        CHECK(out[0] == 1);
    }

    // Final list honours slotCount, maxOut and nulls.
    {
        StuffReconnectState st;
        StuffReconnect_Begin(st, 70, 0u, false);  // target beyond count: skipped
        StuffReconnect_MarkSeen(st, 0);
        StuffReconnect_MarkSeen(st, 1);
        bool present[StuffReconnectState::kMaxSlots] = {};
        present[0] = present[1] = true;
        int out[2] = {-1, -1};
        const int n = StuffReconnect_FinalList(st, present, 8, out, 2);
        CHECK(n == 2);
        CHECK(out[0] == 0);
        CHECK(out[1] == 1);
        CHECK(StuffReconnect_FinalList(st, present, 8, nullptr, 2) == 0);
        CHECK(StuffReconnect_FinalList(st, nullptr, 8, out, 2) == 0);
        CHECK(StuffReconnect_FinalList(st, present, 0, out, 2) == 0);
        CHECK(StuffReconnect_FinalList(st, present, 8, out, 0) == 0);
    }

    if (g_fails == 0)
        std::printf("stufftext_reconnect logic: all tests passed\n");
    return g_fails == 0 ? 0 : 1;
}
