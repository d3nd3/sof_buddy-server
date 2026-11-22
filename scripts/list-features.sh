#!/usr/bin/env bash
# Print each build profile and the features/descriptions it contains.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON_BIN="${PYTHON3:-/usr/bin/python3}"

if ! "$PYTHON_BIN" -c 'import yaml' >/dev/null 2>&1; then
  echo "error: $PYTHON_BIN does not have PyYAML (set PYTHON3 to a suitable interpreter)" >&2
  exit 1
fi

exec "$PYTHON_BIN" "$ROOT/tools/list_features.py" "$@"
