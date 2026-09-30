#!/usr/bin/env bash
# Host-side tests for src/features/sv_tracktime: the tracker maths in
# sv_tracktime_logic.h, driven with synthetic clc_move arrivals. No server, no
# Wine, no engine headers - the header is pure by construction.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/sv_tracktime_test"
mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra \
    -I"$root/src/features/sv_tracktime" \
    -o "$out/test_sv_tracktime" "$root/tools/tests/sv_tracktime/test_sv_tracktime.cpp"
"$out/test_sv_tracktime"
