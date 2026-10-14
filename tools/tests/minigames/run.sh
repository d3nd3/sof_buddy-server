#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/minigames_test"
mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra -I"$root/src/features/minigames" \
    -o "$out/test_clientcmd_logic" "$root/tools/tests/minigames/test_clientcmd_logic.cpp"
"$out/test_clientcmd_logic"
