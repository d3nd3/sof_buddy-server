// Host-side tests for src/features/sv_tracktime/sv_tracktime_logic.h.
//
// Drives the real tracker with synthetic clc_move arrivals: a client that
// reports its frametime honestly, one that fakes it fast, one that fakes it
// slow, plus the reconnect/stall bookkeeping and the row format.

#include "sv_tracktime_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace tracktime;

static int fails = 0;

#define CHECK(cond, msg)                    \
    do {                                    \
        if (!(cond)) {                      \
            std::printf("FAIL: %s\n", msg); \
            ++fails;                        \
        }                                   \
    } while (0)

static bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

/** Feeds `count` samples spaced `frameUs` apart, each reporting `msec`. */
static void Feed(Slot& s, long long& nowUs, int count, long long frameUs, unsigned msec) {
    for (int i = 0; i < count; ++i) {
        tracktime::Note(s, nowUs, msec);
        nowUs += frameUs;
    }
}

static void test_honest_client() {
    Slot s;
    Reset(s);
    long long now = 1000000;
    // More samples than the window so its oldest one is not the connection's
    // first (that one has no leading interval to measure).
    Feed(s, now, 300, 16667, 17);  // ~60 fps, honestly reported as 17 ms

    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(w.frames == kRing, "honest: window is one ring");
    CHECK(w.sumMs == kRing * 17, "honest: msec sum");
    CHECK(Near(w.avgMs, 17.0, 0.001), "honest: avg frametime is the reported one");
    CHECK(Near(w.realFps, 60.0, 0.1), "honest: real fps ~60");
    CHECK(Near(w.claimFps, 1000.0 / 17.0, 0.1), "honest: claimed fps ~58.8");
    CHECK(w.driftPct < 0.0 && w.driftPct > -20.0, "honest: small negative drift");
    CHECK(w.verdict == Verdict::Ok, "honest: ok");
}

static void test_faked_fast() {
    Slot s;
    Reset(s);
    long long now = 2000000;
    Feed(s, now, 300, 16667, 1);  // 60 fps on the wire, claims 1 ms frames

    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(Near(w.realFps, 60.0, 0.1), "fast: real fps is what the server saw");
    CHECK(w.claimFps > 900.0, "fast: claimed fps is nonsense");
    CHECK(w.driftPct > 1000.0, "fast: huge positive drift");
    CHECK(w.verdict == Verdict::MsecLow, "fast: MSEC-LOW");
}

static void test_faked_slow() {
    Slot s;
    Reset(s);
    long long now = 3000000;
    Feed(s, now, 300, 16667, 250);  // claims 4 fps while sending at 60

    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(Near(w.claimFps, 4.0, 0.1), "slow: claimed fps 4");
    CHECK(Near(w.realFps, 60.0, 0.1), "slow: real fps 60");
    CHECK(w.driftPct < -80.0, "slow: large negative drift");
    CHECK(w.verdict == Verdict::MsecHigh, "slow: MSEC-HIGH");
}

static void test_zero_msec() {
    Slot s;
    Reset(s);
    long long now = 4000000;
    Feed(s, now, 60, 16667, 0);  // "0 ms per frame" - the limit of an understated one

    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(w.sumMs == 0, "zero: no msec reported");
    CHECK(w.verdict == Verdict::MsecLow, "zero: MSEC-LOW");
    CHECK(w.claimFps == 0.0, "zero: claim fps undefined");
}

static void test_tolerance_edges() {
    Slot s;
    Reset(s);
    long long now = 5000000;
    Feed(s, now, 300, 16667, 14);  // claims ~71 fps while sending at 60 (+19%)

    const Report tight = WindowReport(s, kRing, 10.0f, kMinSamplesDefault);
    const Report loose = WindowReport(s, kRing, 25.0f, kMinSamplesDefault);
    CHECK(tight.verdict == Verdict::MsecLow, "tolerance: flagged at 10%");
    CHECK(loose.verdict == Verdict::Ok, "tolerance: accepted at 25%");
    CHECK(Near(tight.driftPct, loose.driftPct, 0.001), "tolerance: same drift either way");
    CHECK(Near(tight.driftPct, 19.0, 0.5), "tolerance: drift is what we expect");
}

static void test_min_samples() {
    Slot s;
    Reset(s);
    long long now = 6000000;
    Feed(s, now, 12, 16667, 1);  // 12 faking samples: not enough to accuse anyone

    CHECK(WindowReport(s, kRing, 20.0f, kMinSamplesDefault).verdict == Verdict::NoData,
          "min samples: no verdict below the floor");
    CHECK(WindowReport(s, kRing, 20.0f, 5).verdict == Verdict::MsecLow,
          "min samples: verdict once the floor is met");
}

static void test_stall_resets_window_only() {
    Slot s;
    Reset(s);
    long long now = 7000000;
    Feed(s, now, 100, 16667, 17);
    CHECK(s.count == 100, "stall: window full before the stall");

    now += 60000000;  // a minute of silence (alt-tab, reconnect, dead link)
    tracktime::Note(s, now, 17);
    CHECK(s.stalls == 1, "stall: counted");
    CHECK(s.count == 1, "stall: window restarted");
    CHECK(s.frames == 101, "stall: cumulative frames kept");
    CHECK(s.sumGapUs == 99 * 16667LL, "stall: cumulative span excludes the silence");

    Feed(s, now, 60, 16667, 17);
    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(w.verdict == Verdict::Ok, "stall: fresh window still reads honest");
    const Report t = TotalReport(s, 20.0f, kMinSamplesDefault);
    CHECK(t.frames == 161, "stall: total spans the whole connection");
    CHECK(Near(t.avgMs, 17.0, 0.001), "stall: total average unaffected");
}

static void test_reconnect_resets_everything() {
    Slot s;
    ResetFor(s, 111);
    long long now = 8000000;
    Feed(s, now, 120, 16667, 17);
    CHECK(s.frames == 120 && s.sumMs == 120 * 17, "reconnect: first connection measured");

    ResetFor(s, 222);  // same slot, different player
    CHECK(s.frames == 0 && s.sumMs == 0 && s.count == 0 && !s.seeded,
          "reconnect: counters cleared");
    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(w.verdict == Verdict::NoData, "reconnect: no verdict until new samples");
}

static void test_window_is_bounded_and_fresh() {
    Slot s;
    Reset(s);
    long long now = 9000000;
    const int honest = kRing + 200;
    Feed(s, now, honest, 16667, 17);  // older, honest samples must fall out
    CHECK(s.count == kRing, "window: ring is bounded");
    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    CHECK(w.frames == kRing, "window: reports at most one ring");

    // Now fake the last kRing samples; the window must follow, not average away.
    long long t2 = now + static_cast<long long>(honest) * 16667LL;
    Feed(s, t2, kRing, 16667, 1);
    CHECK(WindowReport(s, kRing, 20.0f, kMinSamplesDefault).verdict == Verdict::MsecLow,
          "window: a fresh lie is caught");

    const Report t = TotalReport(s, 20.0f, kMinSamplesDefault);
    CHECK(t.frames == honest + kRing, "window: total spans the whole connection");
    CHECK(Near(t.avgMs, static_cast<double>(honest * 17 + kRing) / (honest + kRing), 0.001),
          "window: total average dilutes the lie with the honest samples");
    CHECK(t.verdict == Verdict::MsecLow, "window: the long-run total is still off");
}

static void test_small_window() {
    Slot s;
    Reset(s);
    long long now = 10000000;
    Feed(s, now, 240, 16667, 1);  // faking from the start

    CHECK(WindowReport(s, 32, 20.0f, kMinSamplesDefault).verdict == Verdict::MsecLow,
          "small window: 32 samples still catch it");
    CHECK(WindowReport(s, 32, 20.0f, kMinSamplesDefault).frames == 32,
          "small window: frame count follows the setting");
    CHECK(ClampWindow(1) == kRingMin, "clamp: floor");
    CHECK(ClampWindow(9999) == kRingMax, "clamp: ceiling");
}

static void test_extremes() {
    Slot s;
    Reset(s);
    long long now = 11000000;
    Feed(s, now, 5, 16667, 200);
    CHECK(s.minMs == 200 && s.maxMs == 200, "extremes: min/max tracked");
    Feed(s, now, 5, 16667, 1);
    CHECK(s.minMs == 1 && s.maxMs == 200, "extremes: widened on both sides");
    CHECK(s.worstGapUs == 16667, "extremes: worst gap");
    CHECK(s.sumGapUs == 9 * 16667, "extremes: 10 samples, 9 intervals");

    long long backwards = now - 5000000;  // non-monotonic clock must not poison
    const int before = s.count;
    tracktime::Note(s, backwards, 17);
    CHECK(s.sumGapUs == 9 * 16667, "extremes: backwards clock adds no span");
    CHECK(s.stalls == 0, "extremes: backwards clock is not a stall");
    CHECK(s.count == 1, "extremes: backwards clock drops the window");
    CHECK(s.count <= before, "extremes: window never grew on a backwards clock");

    const Report empty = TotalReport(Slot(), 20.0f, kMinSamplesDefault);
    CHECK(empty.verdict == Verdict::NoData && empty.frames == 0, "extremes: fresh slot");
}

static void test_first_interval_is_unknown() {
    Slot s;
    Reset(s);
    long long now = 13000000;
    Feed(s, now, 40, 16667, 17);  // fresh connection: sample 0 has no leading gap

    const Report w = WindowReport(s, kRing, 20.0f, 10);
    CHECK(w.frames == 40, "edge: every sample counted");
    CHECK(w.spanMs == 38 * 16667 / 1000, "edge: the first interval is unknown, not invented");
    CHECK(w.verdict == Verdict::Ok, "edge: still honest");
}

static void test_row_format() {
    Slot s;
    Reset(s);
    long long now = 12000000;
    Feed(s, now, 60, 16667, 17);

    char row[160];
    const Report w = WindowReport(s, kRing, 20.0f, kMinSamplesDefault);
    const int n = FormatRow(row, sizeof(row), 3, "player", w);
    CHECK(n > 0 && static_cast<std::size_t>(n) < sizeof(row), "row: fits the buffer");
    CHECK(std::strstr(row, "player") != nullptr, "row: name");
    CHECK(std::strstr(row, "ok") != nullptr, "row: verdict");

    FormatRow(row, sizeof(row), 0, "", w);
    CHECK(std::strstr(row, "-") != nullptr, "row: unnamed slot placeholder");
}

static void test_verdict_texts() {
    CHECK(std::strcmp(VerdictText(Verdict::Ok), "ok") == 0, "text: ok");
    CHECK(std::strcmp(VerdictText(Verdict::MsecLow), "MSEC-LOW") == 0, "text: low");
    CHECK(std::strcmp(VerdictText(Verdict::MsecHigh), "MSEC-HIGH") == 0, "text: high");
    CHECK(std::strcmp(VerdictText(Verdict::NoData), "no-data") == 0, "text: no data");
}

int main() {
    test_honest_client();
    test_faked_fast();
    test_faked_slow();
    test_zero_msec();
    test_tolerance_edges();
    test_min_samples();
    test_stall_resets_window_only();
    test_reconnect_resets_everything();
    test_window_is_bounded_and_fresh();
    test_small_window();
    test_extremes();
    test_first_interval_is_unknown();
    test_row_format();
    test_verdict_texts();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
