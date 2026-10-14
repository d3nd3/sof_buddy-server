#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -m32 -std=gnu++17 -g \
    -I../../../src/features/print_guard \
    -o "$out/test_print_guard" test_print_guard.cpp
g++ -m32 -std=gnu++17 -g \
    -I../../../src/features/print_guard \
    -o "$out/test_cbuf_guard" test_cbuf_guard.cpp
g++ -m32 -std=gnu++17 -g \
    -I../../../include -I../../../src/features/print_guard \
    -o "$out/test_macro_guard" test_macro_guard.cpp \
    ../../../src/features/print_guard/macro_guard.cpp \
    ../../../src/features/print_guard/cvar.cpp
g++ -m32 -std=gnu++17 -g \
    -o "$out/test_parse_guard" test_parse_guard.cpp
"$out/test_print_guard"
"$out/test_cbuf_guard"
"$out/test_macro_guard"
"$out/test_parse_guard"
