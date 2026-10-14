#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=gnu++17 -g -Wall -Wextra \
    -I../../../src/features/skinteam_quiet \
    -o "$out/test_skinteam_quiet_logic" test_skinteam_quiet_logic.cpp
"$out/test_skinteam_quiet_logic"
