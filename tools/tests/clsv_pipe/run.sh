#!/usr/bin/env bash
# Host-side tests for src/features/clsv_pipe pure logic (clsv_logic.h).
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=gnu++17 -g -fno-strict-aliasing -Wall -Wextra \
    -I../../../src/features/clsv_pipe \
    -o "$out/test_clsv_logic" test_clsv_logic.cpp
"$out/test_clsv_logic"
g++ -std=gnu++17 -g -fno-strict-aliasing -Wall -Wextra \
    -I../../../src/features/clsv_pipe \
    -o "$out/test_dl_unlock" test_dl_unlock.cpp
"$out/test_dl_unlock"
