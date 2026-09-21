#!/usr/bin/env python3
"""Announce engine highclamps in-game on vanilla (non-modded) servers.

Tail-follows a server log file (like `tail -F`: rotation/truncation safe,
stdlib polling only). Every logged line that *is* an engine highclamp report
— the `sv highclamp` line the engine prints with `sv_showclamp 1` — bumps a
counter and sends, as an unconnected (0xFFFFFFFF) datagram::

    rcon <password> say sv_highclamp <n>

to <host>:<port>, so players see `sv_highclamp 3` on the third occurrence.

Matching is deliberately exact-line (case-insensitive `sv highclamp` /
`sv_highclamp`, timestamp prefixes tolerated) and never matches our own
`say sv_highclamp N` echo back — otherwise the announcement would re-trigger
itself into an rcon storm.

Usage:
    python3 tools/watch_highclamp.py [--log user-28916/sof.log] [--host 127.0.0.1]
        [--port 28916] --password SECRET

Run it from the folder holding the per-server `user-<port>/` directories; the
log defaults to `user-<port>/sof.log` for the given port.
"""

import argparse
import os
import socket
import sys
import time

ENGINE_LINES = ("sv highclamp", "sv_highclamp")


def is_highclamp_line(line):
    """True only for the engine's own report, never for our say echo."""
    s = line.strip().lower()
    if not s or s[-1].isdigit():
        # Blank lines, and our own `say sv_highclamp N` echo back.
        return False
    return s in ENGINE_LINES or s.endswith(" " + ENGINE_LINES[0]) or s.endswith(ENGINE_LINES[1])


def send_rcon(sock, addr, password, command):
    payload = b"\xff\xff\xff\xff" + ("rcon %s %s" % (password, command)).encode()
    sock.sendto(payload, addr)


def fmt_hms(seconds):
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return "%d:%02d:%02d" % (h, m, s)


def fmt_rate(count, elapsed):
    # Units follow the session length: per-hour extrapolation of a burst is
    # meaningless, so sub-minute sessions read per-second, scaling up after.
    if elapsed <= 0:
        return "-"
    if elapsed < 90:
        return "%.1f/s" % (count / elapsed)
    if elapsed < 5400:
        return "%.2f/min" % (count / (elapsed / 60.0))
    return "%.2f/hour" % (count / (elapsed / 3600.0))


def watch(path, host, port, password, poll):
    addr = (host, port)
    # A zero/negative poll is a busy spin (sleep(0) never blocks) — a full
    # core for zero benefit, since this only announces into game chat.
    if poll < 0.05:
        print("poll %.3fs too low, using 0.05s" % poll, flush=True)
        poll = 0.05
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    count = 0
    t0 = time.monotonic()  # session start: the average is count / wall time
    fh = None
    dev_ino = None

    def open_log():
        # Startup open: skip history, like tail -f. (Rotation uses a plain
        # open below instead: a new inode holds only unseen bytes, so it is
        # read from the start — nothing there can be a duplicate.)
        f = open(path, "r", encoding="utf-8", errors="replace")
        f.seek(0, os.SEEK_END)
        try:
            st = os.stat(path)
            return f, (st.st_dev, st.st_ino)
        except OSError:
            return f, None

    print("watching %s -> rcon %s:%d (poll %.2fs)" % (path, host, port, poll), flush=True)
    try:
        while True:
            if fh is None:
                if os.path.exists(path):
                    fh, dev_ino = open_log()
                else:
                    time.sleep(poll)
                    continue
            line = fh.readline()
            if not line:
                # EOF: the only place rotation/truncation is checked, so a
                # flooded log costs one readline per line and nothing else.
                # No throttle on catch-up: delaying reads delays announcements,
                # which is the one thing this script must not do.
                try:
                    st = os.stat(path)
                    if dev_ino is not None and (st.st_dev, st.st_ino) != dev_ino:
                        fh.close()
                        try:
                            fh = open(path, "r", encoding="utf-8", errors="replace")
                        except OSError:
                            fh = None
                            dev_ino = None
                            continue
                        try:
                            st = os.stat(path)
                            dev_ino = (st.st_dev, st.st_ino)
                        except OSError:
                            dev_ino = None
                    elif st.st_size < fh.tell():
                        fh.seek(0, os.SEEK_SET)  # truncation / copytruncate
                except OSError:
                    pass
                time.sleep(poll)
                continue
            if is_highclamp_line(line):
                count += 1
                elapsed = time.monotonic() - t0
                full = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
                command = "say sv_highclamp %d (avg %s @ %s)" % (
                    count, fmt_rate(count, elapsed), full)
                try:
                    send_rcon(sock, addr, password, command)
                except OSError as e:
                    print("rcon send failed: %s" % e, flush=True)
                    continue
                print("[%s] highclamp #%d: sent %r (%d in %s = %s)" % (
                    full, count, command, count, fmt_hms(elapsed),
                    fmt_rate(count, elapsed)), flush=True)
    except KeyboardInterrupt:
        elapsed = time.monotonic() - t0
        full = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
        print("[%s] total: %d highclamps in %s = %s" % (
            full, count, fmt_hms(elapsed), fmt_rate(count, elapsed)), flush=True)
    finally:
        if fh is not None:
            fh.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", default=None,
                    help="server log file to follow (default: user-<port>/sof.log)")
    ap.add_argument("--host", default="127.0.0.1", help="server address (default: 127.0.0.1)")
    ap.add_argument("--port", type=int, default=28916, help="server port (default: 28916)")
    ap.add_argument("--password", default=None,
                    help="rcon password (falls back to RCON_PASSWORD env, which stays out of ps)")
    ap.add_argument("--poll", type=float, default=0.2, help="poll interval in seconds (default: 0.2)")
    args = ap.parse_args(argv)
    password = args.password or os.environ.get("RCON_PASSWORD")
    if not password:
        ap.error("rcon password required: --password or RCON_PASSWORD env")
    log = args.log or os.path.join("user-%d" % args.port, "sof.log")
    watch(log, args.host, args.port, password, args.poll)
    return 0


if __name__ == "__main__":
    sys.exit(main())
