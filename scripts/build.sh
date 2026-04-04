#!/usr/bin/env bash
# Configure (if needed) and cross-compile gamex86.dll with MinGW i686.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
GENERATOR=""
TOOLCHAIN="$ROOT/cmake/toolchain-mingw32.cmake"

usage() {
  echo "Usage: $0 [--debug] [--make] [--dir DIR]"
  echo "  --debug   CMAKE_BUILD_TYPE=Debug (default: Release)"
  echo "  --make    use 'Unix Makefiles' instead of Ninja"
  echo "  --dir D   build directory (default: build). Env BUILD_DIR overrides default before flags."
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) BUILD_TYPE=Debug; shift ;;
    --make)  GENERATOR="Unix Makefiles"; shift ;;
    --dir)
      [[ $# -ge 2 ]] || usage
      BUILD_DIR="$2"
      shift 2
      ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1"; usage ;;
  esac
done

if [[ -z "$GENERATOR" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    echo "ninja not found; using Unix Makefiles (install ninja-build for faster builds)"
    GENERATOR="Unix Makefiles"
  fi
fi

# Toolchain is only read on the first configure; repeating -DCMAKE_TOOLCHAIN_FILE on
# an existing build dir is ignored and CMake warns it was "not used".
CMAKE_ARGS=(
  -S "$ROOT" -B "$BUILD_DIR" -G "$GENERATOR"
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
)
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN")
fi
cmake "${CMAKE_ARGS[@]}"

cmake --build "$BUILD_DIR"

echo "Built: $BUILD_DIR/gamex86.dll"
