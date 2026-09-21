#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -Istub -I../../../include -I../../../src/features/cpu_optimizations/cmd_cost \
    -o "$out/test_cmd_cost" test_cmd_cost.cpp
"$out/test_cmd_cost"
