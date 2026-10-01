#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/cvarview_test"
mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra \
    -I"$root/src/features/minigames" \
    -I"$root/src/features/minigames/cvarview" \
    -o "$out/test_cvarview" "$root/tools/tests/cvarview/test_cvarview.cpp"
"$out/test_cvarview"
