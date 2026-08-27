#!/usr/bin/env bash
# Host-side tests for src/features/clamp_monitor.
#
# The feature's two translation units are #included by the harness and built
# for the host (32-bit, so the engine's cvar_t offsets line up), against stub
# headers that stand in for <windows.h> and the generated detour types. No
# server, no Wine: it drives the real code with a transcribed model of the
# engine's SV_Frame / SV_RunGameFrame / SpawnServer.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -Istub -I../../../include -I../../../src/features/clamp_monitor \
    -o "$out/test_clamp_monitor" test_clamp_monitor.cpp
"$out/test_clamp_monitor"
