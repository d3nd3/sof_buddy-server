#!/usr/bin/env bash
# Host-side tests for src/features/stufftext pure logic
# (stufftext_audit.cpp validators, stufftext_reconnect_logic.h state machine).
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=gnu++17 -g -fno-strict-aliasing \
    -I../../../src/features/stufftext \
    -o "$out/test_stufftext_audit" test_stufftext_audit.cpp \
    ../../../src/features/stufftext/stufftext_audit.cpp
"$out/test_stufftext_audit"
g++ -std=gnu++17 -g -fno-strict-aliasing -Wall -Wextra \
    -I../../../src/features/stufftext \
    -o "$out/test_stufftext_reconnect" test_stufftext_reconnect.cpp
"$out/test_stufftext_reconnect"
