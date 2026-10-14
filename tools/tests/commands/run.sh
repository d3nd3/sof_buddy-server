#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/commands_test"
mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra \
    -I"$root/src/features/minigames" \
    -I"$root/src/features/minigames/commands" \
    -o "$out/test_commands" "$root/tools/tests/commands/test_commands.cpp"
"$out/test_commands"
