#!/usr/bin/env bash
# Run watch_highclamp.py detached in screen, so it survives the ssh session.
#
#   tools/watch_highclamp.sh [--port 28916] [--host 127.0.0.1]
#                            [--log user-28916/sof.log] [--password SECRET] {start|stop|status}
#
# Password via --password or, better, the RCON_PASSWORD env var (stays out of
# ps output). Reattach any time with:  screen -r <session>
#
# Self-contained: only needs watch_highclamp.py next to it. Copy both files
# anywhere (e.g. ~/server) and run from the folder holding user-<port>/.
set -euo pipefail

PORT=28916
HOST=127.0.0.1
LOG=""
PASSWORD="${RCON_PASSWORD:-}"
ACTION="start"

while [ $# -gt 0 ]; do
    case "$1" in
        --port)     PORT="$2"; shift 2;;
        --host)     HOST="$2"; shift 2;;
        --log)      LOG="$2"; shift 2;;
        --password) PASSWORD="$2"; shift 2;;
        start|stop|status) ACTION="$1"; shift;;
        -h|--help) sed -n '2,8p' "$0"; exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

[ -n "$LOG" ] || LOG="user-$PORT/sof.log"
# Relative log paths resolve against the invoking shell, not the repo root
# we cd into below - absolutize now so both keep their meaning.
INVOKED="$PWD"
case "$LOG" in
    /*) ;;
    *) LOG="$INVOKED/$LOG";;
esac
SESSION="sof-watch-$PORT"
SELFDIR="$(cd "$(dirname "$0")" && pwd)"
PY="$SELFDIR/watch_highclamp.py"
# The python script travels with this wrapper - no repo needed, just keep the
# two files together.
if [ ! -f "$PY" ]; then
    echo "error: $PY not found." >&2
    echo "keep watch_highclamp.sh and watch_highclamp.py in the same folder." >&2
    exit 1
fi

running() { screen -list 2>/dev/null | grep -q "[.]$SESSION[[:space:]:(]"; }

case "$ACTION" in
    start)
        command -v screen >/dev/null || { echo "screen not installed" >&2; exit 1; }
        [ -n "$PASSWORD" ] || { echo "set RCON_PASSWORD or pass --password" >&2; exit 2; }
        if running; then echo "already running: $SESSION (screen -r $SESSION)"; exit 0; fi
        [ -f "$LOG" ] || echo "note: $LOG not present yet - watcher will wait for it"
        RCON_PASSWORD="$PASSWORD" screen -dmS "$SESSION" \
            python3 "$PY" --log "$LOG" --host "$HOST" --port "$PORT"
        sleep 1
        if running; then
            echo "started $SESSION (reattach: screen -r $SESSION)"
        else
            echo "failed: session $SESSION died immediately." >&2
            echo "debug in foreground: RCON_PASSWORD=... python3 \"$PY\" --log \"$LOG\" --host \"$HOST\" --port \"$PORT\"" >&2
            exit 1
        fi
        ;;
    stop)
        running || { echo "not running: $SESSION"; exit 0; }
        screen -S "$SESSION" -X stuff $'\003'  # SIGINT: prints the totals line
        sleep 1
        screen -S "$SESSION" -X quit 2>/dev/null || true
        echo "stopped $SESSION"
        ;;
    status)
        if running; then echo "running: $SESSION"; else echo "not running: $SESSION"; fi
        ;;
esac
