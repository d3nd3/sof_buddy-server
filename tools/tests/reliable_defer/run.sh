#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g \
    -I../../../src/features/reliable_defer \
    -o "$out/test_reliable_defer" test_reliable_defer.cpp
"$out/test_reliable_defer"
