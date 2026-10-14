#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g \
    -I../../../src/features/minigames/tictactoe \
    -I../../../src/features/minigames \
    -o "$out/test_tictactoe" test_tictactoe.cpp
"$out/test_tictactoe"
