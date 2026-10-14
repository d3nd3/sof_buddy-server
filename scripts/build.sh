#!/usr/bin/env bash
# Configure (if needed) and cross-compile gamex86.dll with MinGW i686.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${BUILD_TYPE:-Release}"
GENERATOR=""
TOOLCHAIN="$ROOT/cmake/toolchain-mingw32.cmake"
# Explicit dir wins (--dir flag, then BUILD_DIR env). Otherwise each build
# type gets its own directory so switching types never forces a full rebuild.
BUILD_DIR_EXPLICIT="${BUILD_DIR:-}"
BUILD_DIR="$BUILD_DIR_EXPLICIT"

usage() {
  echo "Usage: $0 [--debug] [--make] [--dir DIR]"
  echo "  --debug   CMAKE_BUILD_TYPE=Debug in build/debug (default: Release in build)"
  echo "  --make    use 'Unix Makefiles' instead of Ninja"
  echo "  --dir D   build directory (default: build or build/debug)."
  echo "            Env BUILD_DIR overrides the default before flags."
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) BUILD_TYPE=Debug; shift ;;
    --make)  GENERATOR="Unix Makefiles"; shift ;;
    --dir)
      [[ $# -ge 2 ]] || usage
      BUILD_DIR="$2"
      BUILD_DIR_EXPLICIT="$2"
      shift 2
      ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1"; usage ;;
  esac
done

if [[ -z "$BUILD_DIR_EXPLICIT" ]]; then
  if [[ "$BUILD_TYPE" == "Debug" ]]; then
    BUILD_DIR="build/debug"
  else
    BUILD_DIR="build"
  fi
fi

# generate_hooks.py writes its headers/sources wherever SOF_BUDDY_SERVER_GEN_DIR
# points (default: legacy build/). Point it at this build dir for both the
# configure and the build invocations below; otherwise a fresh directory fails
# with missing generated_*.h. Anyone invoking cmake manually needs this too.
if [[ "$BUILD_DIR" = /* ]]; then
  export SOF_BUDDY_SERVER_GEN_DIR="$BUILD_DIR"
else
  export SOF_BUDDY_SERVER_GEN_DIR="$ROOT/$BUILD_DIR"
fi

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
  # generate_hooks.py needs PyYAML; default to an interpreter that has it.
  # Env PYTHON3 overrides (e.g. a venv python).
  -DPython3_EXECUTABLE="${PYTHON3:-/usr/bin/python3}"
)
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN")
fi
if [[ -n "${CMAKE_CXX_COMPILER_LAUNCHER:-}" ]]; then
  CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER_LAUNCHER="$CMAKE_CXX_COMPILER_LAUNCHER")
  CMAKE_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER:-$CMAKE_CXX_COMPILER_LAUNCHER}")
fi
if [[ -n "${SOFPLUS_CURSOR_RULES:-}" ]]; then
  CMAKE_ARGS+=(-DSOFPLUS_CURSOR_RULES="$SOFPLUS_CURSOR_RULES")
fi
cmake "${CMAKE_ARGS[@]}"

cmake --build "$BUILD_DIR"

echo "Built: $BUILD_DIR/gamex86.dll"
