#!/usr/bin/env bash
# Host-side tests for src/features/cpu_optimizations/qpc_timer.
#
# The feature's translation units are #included by the harness and built
# for the host (32-bit, so the engine's curtime offset lines up), against stub
# headers that stand in for <windows.h> and the generated detour types. No
# server, no Wine: it drives the real Sys_Milliseconds override against a
# synthetic engine image and a virtual QPC clock.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -Istub -I../../../include -I../../../src/features/cpu_optimizations/qpc_timer \
    -o "$out/test_qpc_timer" test_qpc_timer.cpp
"$out/test_qpc_timer"
