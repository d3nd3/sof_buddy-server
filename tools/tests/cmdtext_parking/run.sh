#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -Istub -I../../../include -I../../../src/features/cpu_optimizations/cmdtext_parking \
    -o "$out/test_cmdpark" test_cmdpark.cpp
"$out/test_cmdpark"
