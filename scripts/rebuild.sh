#!/usr/bin/env bash
# clean.sh then build.sh with the same arguments.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/clean.sh"
exec "$ROOT/scripts/build.sh" "$@"
