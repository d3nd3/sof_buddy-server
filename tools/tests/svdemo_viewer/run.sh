#!/usr/bin/env bash
# Cross-check the browser .sbd parser against the C++ writer, and syntax-check
# the viewer scripts. No server, no Wine, no browser required.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${TMPDIR:-/tmp}/svdemo_viewer_test"
mkdir -p "$out"

g++ -std=c++17 -Wall -Wextra \
    -I"$root/src/features/server_demo" \
    -o "$out/make_fixture" \
    "$root/tools/tests/svdemo_viewer/make_fixture.cpp"

"$out/make_fixture" "$out/fixture.sbd"

node --check "$root/tools/svdemo_viewer/svdemo.js"
node --check "$root/tools/svdemo_viewer/bsp.js"
node --check "$root/tools/svdemo_viewer/viewer.js"
node "$root/tools/tests/svdemo_viewer/parser.test.mjs" "$out/fixture.sbd"
node "$root/tools/tests/svdemo_viewer/bsp.test.mjs"
