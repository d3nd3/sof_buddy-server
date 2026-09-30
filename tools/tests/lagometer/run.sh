#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/lagometer_test"
mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra \
    -I"$root/src/features/minigames" \
    -I"$root/src/features/minigames/lagometer" \
    -o "$out/test_lagometer" "$root/tools/tests/lagometer/test_lagometer.cpp"
"$out/test_lagometer"
