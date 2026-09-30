#pragma once

// sv_tracktime: per-slot client frametime accounting, pure and host-testable.
//
// The server sees one SV_ClientThink call per usercmd_t the client sent in a
// clc_move, and usercmd_t.msec is the client's own claim of how long its last
// frame took (q_shared.h: `byte msec`). Nothing on the server forces that byte
// to be true, so a client that wants a stat readout to say "1000 fps" can send
// msec=1 while still running at 60.
//
// This header holds the part that needs no engine: folding samples into a
// per-slot accumulator, turning a sample window into the two rates that have to
// agree, and formatting one console row. sv_tracktime.cpp supplies the clock,
// the client pointer, the cvar plumbing and the console commands.
//
// The comparison, spelled out once:
//   claim_fps = 1000 * frames / sum(msec)      - rate implied by the bytes
//   real_fps  = 1000 * (frames-1) / span       - rate the server actually saw
//   drift     = (claim_fps - real_fps) / real_fps * 100
// drift near zero means the reported frametime is real: the client's own
// msec total and the wall-clock time between its packets agree, so the average
// frametime is not a fiction. Positive drift means msec is *understated*
// (claims more frames per second than it sends); negative means inflated.
//
// Arrive times are microseconds because a 200 fps client frames every 5 ms and
// GetTickCount's ~15.6 ms tick would smear the very gaps being measured; the
// caller passes a monotonic sub-ms clock (see sv_tracktime.cpp).

#include <cstdint>
#include <cstdio>

namespace tracktime {

constexpr int kMaxSlots = 64;

// Rolling window depth. 256 samples is a bit over 4 s at 60 fps, which is long
// enough for a stable average and short enough that an admin sees the change
// immediately.
constexpr int kRing = 256;
constexpr int kRingMin = 32;
constexpr int kRingMax = kRing;

// A silence at least this long is not part of any rate: tabbing away, a
// reconnect or a stalled link. It is counted separately and excluded from both
// averages instead of dragging them into nonsense.
constexpr long long kStallUs = 2000000;  // 2 s

// Below this many samples in the window there is no verdict, whatever the
// numbers say.
constexpr int kMinSamplesDefault = 30;

enum class Verdict : unsigned char {
    NoData = 0,    // not enough samples yet (or no usable wall-clock span)
    Ok,            // msec matches the rate the client is really sending
    MsecLow,       // claims more frames per second than it sends (faked fast)
    MsecHigh,      // claims fewer frames per second than it sends (faked slow)
};

inline const char* VerdictText(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "ok";
        case Verdict::MsecLow: return "MSEC-LOW";
        case Verdict::MsecHigh: return "MSEC-HIGH";
        case Verdict::NoData: break;
    }
    return "no-data";
}

/** Per-slot accumulator. `infoHash` identifies the connection so a reconnect
 *  (or a different player in the same slot) starts a fresh measurement
 *  instead of averaging two sessions together. */
struct Slot {
    // Cumulative, current connection.
    long long frames = 0;      // ClientThink calls
    long long sumMs = 0;       // sum of usercmd_t.msec
    long long sumGapUs = 0;    // wall time between samples, stalls excluded
    long long stalls = 0;      // silences >= kStallUs
    long long stalledMs = 0;   // wall time spent in those silences
    long long zeroMs = 0;      // samples reporting msec == 0
    std::uint32_t minMs = 0;   // per-sample msec extremes
    std::uint32_t maxMs = 0;
    std::uint32_t worstGapUs = 0;
    long long firstUs = 0;     // clock at the first sample
    long long lastUs = 0;      // clock at the most recent sample
    std::uint32_t infoHash = 0;
    bool seeded = false;

    // Rolling window, oldest-to-newest walkable ring.
    std::uint8_t msec[kRing] = {};
    std::uint32_t gapUs[kRing] = {};  // interval *ending* at this sample
    int head = 0;
    int count = 0;
};

inline void ResetWindow(Slot& s) {
    s.head = 0;
    s.count = 0;
}

/** Drops everything, including the window and the connection identity. */
inline void Reset(Slot& s) {
    s = Slot();
}

/** Restarts the measurement when the slot changed hands (see infoHash). */
inline void ResetFor(Slot& s, std::uint32_t infoHash) {
    Reset(s);
    s.infoHash = infoHash;
}

/** Folds one clc_move usercmd into the slot. `nowUs` is a monotonic clock in
 *  microseconds; `msec` is usercmd_t.msec straight off the wire. */
inline void Note(Slot& s, long long nowUs, unsigned msec) {
    long long gap = 0;
    if (s.seeded) {
        gap = nowUs - s.lastUs;
        if (gap < 0) {
            // A clock that went backwards cannot say anything about a rate:
            // drop the window rather than invent a zero-length interval.
            ResetWindow(s);
            gap = 0;
        } else if (gap >= kStallUs) {
            // Not a rate, a disconnection. Keep the cumulative counters (they
            // already excluded this span) and start the window over.
            ++s.stalls;
            s.stalledMs += gap / 1000;
            ResetWindow(s);
            gap = 0;
        } else {
            s.sumGapUs += gap;
            if (static_cast<long long>(s.worstGapUs) < gap)
                s.worstGapUs = static_cast<std::uint32_t>(gap);
        }
    } else {
        s.seeded = true;
        s.firstUs = nowUs;
        s.minMs = msec;
    }

    s.lastUs = nowUs;
    ++s.frames;
    s.sumMs += static_cast<long long>(msec);
    if (msec > s.maxMs)
        s.maxMs = msec;
    if (msec < s.minMs)
        s.minMs = msec;
    if (msec == 0)
        ++s.zeroMs;

    s.msec[s.head] = static_cast<std::uint8_t>(msec);
    s.gapUs[s.head] = static_cast<std::uint32_t>(gap);
    s.head = (s.head + 1) % kRing;
    if (s.count < kRing)
        ++s.count;
}

/** One measurement: a window or a whole connection, described the same way. */
struct Report {
    long long frames = 0;
    long long sumMs = 0;
    long long spanMs = 0;    // wall time the frames actually covered
    double avgMs = 0.0;      // sumMs / frames: the client's claimed frametime
    double claimFps = 0.0;   // 1000 * frames / sumMs
    double realFps = 0.0;    // 1000 * (frames-1) / spanMs
    double driftPct = 0.0;   // (claimFps - realFps) / realFps * 100
    Verdict verdict = Verdict::NoData;
};

/** The shared arithmetic. `minFrames` below 2 is raised to 2: a single sample
 *  has no interval and therefore no rate. */
inline Report MakeReport(long long frames, long long sumMs, long long spanMs, float tolPct,
                         int minFrames) {
    Report r;
    r.frames = frames;
    r.sumMs = sumMs;
    r.spanMs = spanMs;
    if (minFrames < 2)
        minFrames = 2;
    if (frames < minFrames)
        return r;  // NoData: too little to call anything

    if (frames > 0 && sumMs > 0) {
        r.avgMs = static_cast<double>(sumMs) / static_cast<double>(frames);
        r.claimFps = 1000.0 * static_cast<double>(frames) / static_cast<double>(sumMs);
    }
    if (frames > 1 && spanMs > 0) {
        r.realFps = 1000.0 * static_cast<double>(frames - 1) / static_cast<double>(spanMs);
    } else {
        return r;  // NoData: no usable wall-clock span to compare against
    }

    if (sumMs == 0) {
        // "Zero milliseconds per frame" is the limit of an understated frametime:
        // the client claims an unbounded rate while sending at a measured one.
        r.verdict = Verdict::MsecLow;
        r.driftPct = 1000.0;
        return r;
    }
    if (!(r.claimFps > 0.0))
        return r;

    const double tol = (tolPct > 0.0f) ? static_cast<double>(tolPct) : 0.0;
    r.driftPct = (r.claimFps - r.realFps) / r.realFps * 100.0;
    if (r.driftPct > tol)
        r.verdict = Verdict::MsecLow;
    else if (r.driftPct < -tol)
        r.verdict = Verdict::MsecHigh;
    else
        r.verdict = Verdict::Ok;
    return r;
}

/** The newest `windowFrames` samples (fewer if the slot is younger). The gap
 *  stored on the oldest sample leads *out* of the window, so it is dropped: n
 *  samples describe at most n-1 intervals. That also covers the connection's
 *  very first sample, whose leading interval does not exist - the window then
 *  spans n-2 intervals, one frame out of n. */
inline Report WindowReport(const Slot& s, int windowFrames, float tolPct, int minFrames) {
    int n = s.count < windowFrames ? s.count : windowFrames;
    if (n > kRing)
        n = kRing;
    long long frames = 0, sumMs = 0, spanUs = 0;
    int idx = s.head;  // one past newest
    for (int k = 0; k < n; ++k) {
        idx = (idx == 0) ? (kRing - 1) : (idx - 1);
        sumMs += s.msec[idx];
        ++frames;
        if (k > 0)
            spanUs += static_cast<long long>(s.gapUs[idx]);
    }
    return MakeReport(frames, sumMs, spanUs / 1000, tolPct, minFrames);
}

/** Everything since the connection started (or the last reset), stalls
 *  excluded - see Note(). */
inline Report TotalReport(const Slot& s, float tolPct, int minFrames) {
    if (!s.seeded)
        return Report();
    return MakeReport(s.frames, s.sumMs, s.sumGapUs / 1000, tolPct, minFrames);
}

inline int ClampWindow(int frames) {
    if (frames < kRingMin)
        return kRingMin;
    if (frames > kRingMax)
        return kRingMax;
    return frames;
}

/** One table row. Pure string work so the harness can pin the layout. */
inline int FormatRow(char* out, std::size_t cap, int slot0, const char* name,
                     const Report& w) {
    const char* nm = (name && name[0]) ? name : "-";
    return std::snprintf(out, cap, "  %2d  %-16s %7lld %9lld %7.2f %9.1f %9.1f %+9.1f%%  %s",
                         slot0, nm, static_cast<long long>(w.frames),
                         static_cast<long long>(w.sumMs), w.avgMs, w.claimFps, w.realFps,
                         w.driftPct, VerdictText(w.verdict));
}

}  // namespace tracktime
