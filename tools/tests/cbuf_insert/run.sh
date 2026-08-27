#!/usr/bin/env bash
# Host-side tests for src/features/cbuf_insert.
#
# The feature's three translation units are #included by the harness and built
# for the host (32-bit, so the engine's cvar_t offsets line up), against stub
# headers standing in for <windows.h> and the generated detour types. The core
# test is differential: the in-place fast path is run against a transcription
# of the engine's own Cbuf_InsertText and the resulting buffers are compared
# byte for byte.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -Istub -I../../../include -I../../../src/features/cbuf_insert \
    -o "$out/test_cbuf_insert" test_cbuf_insert.cpp
"$out/test_cbuf_insert"
