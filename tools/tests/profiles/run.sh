#!/usr/bin/env bash
# Host-side tests for src/features/profiles pure logic (profiles_logic.h).
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing -Wall -Wextra \
    -I../../../src/features/profiles \
    -o "$out/test_profiles_logic" test_profiles_logic.cpp
"$out/test_profiles_logic"
