#!/usr/bin/env bash
# Host-side tests for the .sbd container. The format header is pure: no engine
# headers, no Wine, no SoF process.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/server_demo_test"
mkdir -p "$out"
g++ -std=c++17 -Wall -Wextra \
    -I"$root/src/features/server_demo" \
    -o "$out/test_server_demo" "$root/tools/tests/server_demo/test_server_demo.cpp"
"$out/test_server_demo"
