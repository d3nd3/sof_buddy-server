#!/usr/bin/env bash
# On your machine (multiplexed SSH):
#   ssh -M -S ~/.ssh/chinserverA.ctl -N -f chinserverA
# From repo tools/ (or scp timer_probe_*.c here and build on host):
#   bash chinserver_timer_probe.sh
# Or from dev box: build win32 exe + static linux bin, scp, wine ./timer_probe_win32.exe
set -euo pipefail
cd "$(dirname "$0")"
echo "=== host ==="
uname -a
echo "CLK_TCK=$(getconf CLK_TCK)"
command -v wine >/dev/null && wine --version | head -1

echo ""
echo "=== native linux (CLOCK_MONOTONIC + THREAD_CPUTIME_ID) ==="
cc -O2 -o /tmp/timer_probe_linux timer_probe_linux.c
/tmp/timer_probe_linux

echo ""
echo "=== wine PE (QPC, timeGetTime, GetThreadTimes, buddy syscall) ==="
MINGW=x86_64-w64-mingw32-gcc
if ! command -v "$MINGW" >/dev/null; then
  MINGW=i686-w64-mingw32-gcc
fi
if ! command -v "$MINGW" >/dev/null; then
  echo "no mingw cross-compiler; install mingw-w64 or build timer_probe_win.exe elsewhere"
  exit 1
fi
"$MINGW" -O2 -o /tmp/timer_probe_win.exe timer_probe_win.c
wine /tmp/timer_probe_win.exe
