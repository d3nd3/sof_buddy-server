#!/usr/bin/env bash
# Serve the viewer locally (only needed for the browser's normal URL bar;
# index.html also works when opened directly from disk).
set -euo pipefail
cd "$(dirname "$0")"
port="${1:-8000}"
echo "SoF state demo viewer: http://127.0.0.1:${port}/"
echo "Sample shortcut:       http://127.0.0.1:${port}/?sample=1"
exec python3 -m http.server "$port"
