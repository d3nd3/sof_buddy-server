#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g -fno-strict-aliasing \
    -I../../../include \
    -o "$out/test_cmd_cost_addons" test_cmd_cost_addons.cpp
"$out/test_cmd_cost_addons"
