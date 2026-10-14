#!/usr/bin/env python3
"""Miniature SoF/Q2 server + client loops linked by svc_frame.

Transcribes tools/tests/tick_pacing/test_tick_pacing.cpp:
  WinMain -> Qcommon_frame -> SV_Frame (server)
  CL_ParseFrame on each svc_frame snapshot
  CL_AddEntities at 7 ms client frames (~1000/7 fps)

Usage:
  python3 tools/mini_sv_cl.py              # real-time (default)
  python3 tools/mini_sv_cl.py --fast       # instant sim (for --check-rate)
  python3 tools/mini_sv_cl.py --seconds 20 --drain 7 --drain-period 100 --drain-offset 95
  python3 tools/mini_sv_cl.py --settle elapsed  # buggy credit -> high clamp 5-7
"""

import argparse
import time

TICK_MS = 100
TICK_HZ = 1000.0 / TICK_MS  # server sim: 10 Hz
CL_FRAME_MS = 7.0  # client sim step: 1000/7 fps


class Clock:
    def __init__(self, realtime=True, speed=1.0):
        self.wall = 0.0
        self.realtime = realtime
        self.speed = max(speed, 0.001)

    def advance(self, ms):
        if ms <= 0:
            return
        self.wall += ms
        if self.realtime:
            time.sleep(ms / 1000.0 / self.speed)

    @property
    def engine(self):
        return int(self.wall)


class SvcFrame:
    __slots__ = ("servertime", "wall")

    def __init__(self, servertime, wall):
        self.servertime = servertime
        self.wall = wall


class Server:
    def __init__(self, clk, wall_origin=0.0):
        self.clk = clk
        self.wall_origin = wall_origin
        # Post map-load: sv.framenum>=1, sv.time=100*framenum (unsigned < gate).
        self.framenum = 1
        self.sv_time = TICK_MS
        self.svs_realtime = 0
        self.highclamps = 0
        self.lowclamps = 0
        self.cmd_queue = []
        self.next_drain_at = 0.0
        self.old_engine = 0
        self.snap_walls = []
        self.late_ms = []

    def queue_cmd(self, cost_ms):
        self.cmd_queue.append(cost_ms)

    def drain_cmds(self):
        cost = 0.0
        while self.cmd_queue:
            c = self.cmd_queue.pop(0)
            self.clk.advance(c)
            cost += c
        return cost

    def loop_iter(self, sleep_ms, frame_ms, iter_drain_ms, settle, outbox):
        old = self.old_engine
        self.clk.advance(sleep_ms)
        while self.clk.engine - old < 1:
            self.clk.advance(0.05)
        msec = self.clk.engine - old
        self.old_engine = self.clk.engine

        if iter_drain_ms > 0:
            self.queue_cmd(iter_drain_ms)
        drained = self.drain_cmds()
        # tickpace_SvFramePre: optional straddle credit into msec (sampled pre-drain).
        if settle and drained > 0 and self.svs_realtime + msec < self.sv_time:
            shortage = self.sv_time - self.svs_realtime - msec
            if shortage > 0:
                msec += drained if settle == "elapsed" else shortage

        self.svs_realtime += msec
        if self.svs_realtime < self.sv_time:
            if self.sv_time - self.svs_realtime > TICK_MS:
                self.svs_realtime = self.sv_time - TICK_MS
                self.lowclamps += 1
            return msec

        self.framenum += 1
        self.sv_time = self.framenum * TICK_MS
        ideal = self.wall_origin + TICK_MS * (self.framenum - 1)
        self.late_ms.append(self.clk.wall - ideal)
        self.clk.advance(frame_ms)

        if self.sv_time < self.svs_realtime:
            lost = self.svs_realtime - self.sv_time
            print("sv highclamp: deleted %d ms" % lost)
            self.svs_realtime = self.sv_time
            self.highclamps += 1

        self.snap_walls.append(self.clk.wall)
        outbox.append(SvcFrame(self.sv_time, self.clk.wall))  # svc_frame / snapshot
        return msec

    def rate_hz(self):
        # Measured from svc_frame spacing (authoritative 10 Hz output).
        if len(self.snap_walls) < 2:
            return 0.0
        span = self.snap_walls[-1] - self.snap_walls[0]
        if span <= 0:
            return 0.0
        return (len(self.snap_walls) - 1) * 1000.0 / span

    def rate_drift_ms(self):
        # Harness: abs(wall - 100*framenum) should stay within one tick.
        d = self.clk.wall - self.sv_time
        return d if d >= 0 else -d


class Client:
    def __init__(self, clk, showclamp):
        self.clk = clk
        self.showclamp = showclamp
        self.cl_time = 0.0
        self.servertime = 0
        self.next_frame = 0.0
        self.inited = False
        self.last_snap_wall = -1.0
        self.highclamp_prints = 0
        self.highclamp_max = 0
        self.snap_gaps_ge5 = 0

    def cl_parse_frame(self, pkt):
        """CL_ParseFrame @0x2000306c: clamp cl.time into [st-100, st]."""
        st = pkt.servertime
        if not self.inited:
            self.cl_time = 0.0
            self.inited = True
            self.next_frame = pkt.wall
        else:
            gap = pkt.wall - self.last_snap_wall
            if gap >= 105.0:
                self.snap_gaps_ge5 += 1
        if self.cl_time > st:
            self.cl_time = float(st)
        elif self.cl_time < st - TICK_MS:
            self.cl_time = float(st - TICK_MS)
        self.servertime = st
        self.last_snap_wall = pkt.wall
        self._run_frames(pkt.wall)

    def _run_frames(self, until_wall):
        """CL_AddEntities @0x20004460: 7 ms client frames, showclamp on overshoot."""
        if not self.inited:
            return
        while self.next_frame + CL_FRAME_MS <= until_wall:
            self.next_frame += CL_FRAME_MS
            self.cl_time += CL_FRAME_MS
            if self.cl_time > self.servertime:
                hi = int(self.cl_time - self.servertime)
                if self.showclamp:
                    print("high clamp %d" % hi)
                self.highclamp_prints += 1
                if hi > self.highclamp_max:
                    self.highclamp_max = hi
                self.cl_time = float(self.servertime)

    def poll(self):
        self._run_frames(self.clk.wall)


def run(seconds, sleep_ms, frame_ms, drain_ms, drain_period, drain_offset,
        iter_drain_ms, settle, showclamp, check_rate, realtime, speed):
    clk = Clock(realtime=realtime, speed=speed)
    sv = Server(clk, wall_origin=clk.wall)
    cl = Client(clk, showclamp)
    outbox = []
    end = seconds * 1000.0
    sv.next_drain_at = clk.wall + drain_offset
    t0 = time.monotonic()

    if realtime:
        print("running %.1fs sim at %.1fx real time..." % (seconds, speed), flush=True)

    while clk.wall < end:
        if drain_period > 0 and clk.wall >= sv.next_drain_at:
            sv.queue_cmd(drain_ms)
            sv.next_drain_at += drain_period

        sv.loop_iter(sleep_ms, frame_ms, iter_drain_ms, settle, outbox)

        while outbox:
            cl.cl_parse_frame(outbox.pop(0))
        cl.poll()

    gaps = [sv.snap_walls[i] - sv.snap_walls[i - 1]
            for i in range(1, len(sv.snap_walls))]
    avg_gap = sum(gaps) / len(gaps) if gaps else 0.0
    hz = sv.rate_hz()
    drift = sv.rate_drift_ms()

    elapsed = time.monotonic() - t0
    print("---")
    print("wall %.1fs | sv framenum %d | %d svc_frames | %.2f Hz (want %.1f) | drift %.1fms" % (
        clk.wall / 1000.0, sv.framenum, len(sv.snap_walls), hz, TICK_HZ, drift))
    if realtime:
        print("real time %.1fs (%.2fx)" % (elapsed, elapsed / max(seconds, 0.001)))
    print("snap gap avg %.2fms (min %.1f max %.1f) | sv highclamp %d | lowclamp %d" % (
        avg_gap, min(gaps) if gaps else 0.0, max(gaps) if gaps else 0.0,
        sv.highclamps, sv.lowclamps))
    print("client showclamp prints %d (max %d) | snap gaps >=105ms %d" % (
        cl.highclamp_prints, cl.highclamp_max, cl.snap_gaps_ge5))

    if check_rate:
        gap_ok = not gaps or max(abs(g - TICK_MS) for g in gaps) <= 3.0
        ok = abs(hz - TICK_HZ) <= 0.05 and drift <= TICK_MS and gap_ok
        if not ok:
            print("RATE CHECK FAILED: %.2f Hz drift %.1fms gaps ok=%s" % (
                hz, drift, gap_ok))
            return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--sleep", type=float, default=1.0, help="WinMain Sleep(1) ms")
    ap.add_argument("--frame", type=float, default=2.0, help="G_RunFrame wall ms")
    ap.add_argument("--drain", type=float, default=7.0, help="async cmd cost ms")
    ap.add_argument("--drain-period", type=float, default=100.0)
    ap.add_argument("--drain-offset", type=float, default=95.0)
    ap.add_argument("--iter-drain", type=float, default=0.0)
    ap.add_argument("--settle", choices=("off", "shortage", "elapsed"), default="off",
                    help="straddle credit into msec: shortage=fixed, elapsed=buggy 5-7")
    ap.add_argument("--quiet", action="store_true", help="suppress high clamp prints")
    ap.add_argument("--check-rate", action="store_true",
                    help="exit 1 if server rate drifts from 10 Hz")
    ap.add_argument("--fast", action="store_true",
                    help="instant sim (no real-time sleep)")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="real-time multiplier (2 = 2x fast-forward)")
    args = ap.parse_args()
    settle = None if args.settle == "off" else args.settle
    raise SystemExit(run(args.seconds, args.sleep, args.frame, args.drain,
                         args.drain_period, args.drain_offset, args.iter_drain,
                         settle, not args.quiet, args.check_rate,
                         not args.fast, args.speed))


if __name__ == "__main__":
    main()
