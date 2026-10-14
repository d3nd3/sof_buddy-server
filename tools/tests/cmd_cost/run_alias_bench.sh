#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
pkill -f SoF-spsv.exe 2>/dev/null || true
sleep 2
python3 -u tools/tests/cmd_cost/bench_alias.py "$@"
