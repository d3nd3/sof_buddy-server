// Demonstrates the settle bug: crediting elapsed (post-drain) vs untilAfterMsec.
// g++ -std=gnu++17 -O0 -o gap_trace gap_trace.cpp && ./gap_trace

#include <cstdint>
#include <cstdio>

int main() {
    // svs=93, sampled msec=5, sv.time=100 → need +2 to tick.
    // Drain took 7ms wall; boundary crossed 2ms into drain.
    const int svs = 93, msec = 5, sv = 100;
    const int untilAfterMsec = sv - svs - msec;  // 2
    const int elapsed = 7;

    const int buggyMsec = msec + elapsed;       // old: credit full drain
    const int fixedMsec = msec + untilAfterMsec;

    std::printf("straddle: svs=%d msec=%d drain_elapsed=%d untilAfterMsec=%d\n",
                svs, msec, elapsed, untilAfterMsec);
    std::printf("  buggy credit (elapsed):   msec=%d svs->%d overshoot=%d\n",
                buggyMsec, svs + buggyMsec, svs + buggyMsec - sv);
    std::printf("  fixed credit (shortage):  msec=%d svs->%d overshoot=%d\n",
                fixedMsec, svs + fixedMsec, svs + fixedMsec - sv);
    std::printf("  client showclamp ~= overshoot at tick => buggy prints %d\n",
                svs + buggyMsec - sv);
    return 0;
}
