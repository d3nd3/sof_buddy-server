#!/usr/bin/env bash
# Remove local CMake output directories (full reconfigure next build).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DIRS=(build)
if [[ -n "${BUILD_DIR:-}" ]]; then
  DIRS=("$BUILD_DIR")
fi

for d in "${DIRS[@]}"; do
  if [[ -e "$d" ]]; then
    rm -rf "$d"
    echo "Removed $d/"
  fi
done

echo "Clean done."
