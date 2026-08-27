#!/usr/bin/env bash
# Host-side tests for src/features/tick_pacing.
#
# The feature's three translation units are #included by the harness and built
# for the host (32-bit, so the engine's cvar_t offsets line up), against stub
# headers that stand in for <windows.h> and the generated detour types. No
# server, no Wine: it drives the real code against a transcription of the
# engine's WinMain loop / Qcommon_frame / SV_Frame and a virtual clock.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -Istub -I../../../include -I../../../src/features/tick_pacing \
    -o "$out/test_tick_pacing" test_tick_pacing.cpp
"$out/test_tick_pacing"
