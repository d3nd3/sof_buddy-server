# sof_buddy-server

A **Soldier of Fortune (SOF1)** mod that runs on the **game server** (dedicated or listen server host), not on players’ game clients.

This repository targets server-side behaviour and integration—anything that ships to or runs alongside the SOF1 server process—not client UI, assets, or local player binaries.

## Build / install (shim DLL)

This tree builds a **`gamex86.dll` shim** that loads the stock game from `base\oldgamex86.dll` and forwards `GetGameAPI`, while hosting an optional **data-driven detour pipeline** (same workflow as the **sof_buddy** client project): `detours.yaml`, `features/features.yaml` (enable optional features), and per-feature `hooks.json` / `callbacks.json` under `src/features/`. See **`docs/DETOUR_SYSTEM.md`**.

### Linux → Windows (MinGW-w64, 32-bit)

SOF1’s `gamex86.dll` is **32-bit x86**. **CMake** plus **Ninja** is the default: fast incremental builds and a single generator across CLI and `CMakePresets.json`.

The cross-compiler is **MinGW-w64** targeting **i686** (`i686-w64-mingw32-g++`).

Install the toolchain (Debian/Ubuntu example):

```bash
sudo apt install cmake ninja-build g++-mingw-w64-i686 python3-yaml
sudo update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix
sudo update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-posix
```

### Build modes and defaults

**Debug** and **Release** map to CMake’s `CMAKE_BUILD_TYPE`. **Release** is optimized and typical for the DLL you run on a server; **Debug** keeps symbols and minimal optimization so breakpoints and stack traces are usable.

Everything uses a **single build directory** (`build/` by default, or `BUILD_DIR`). With Ninja there is only one active build type in that tree at a time—switching mode **reconfigures** the same folder.

| | **Debug** | **Release** |
|---|-----------|-------------|
| **CMake preset** | `mingw32-cross` | `mingw32-cross-release` |
| **`scripts/build.sh`** | `./scripts/build.sh --debug` | `./scripts/build.sh` (**default**) |
| **Manual `cmake`** | `-DCMAKE_BUILD_TYPE=Debug` | `-DCMAKE_BUILD_TYPE=Release` |

**Defaults:** `./scripts/build.sh` with no flags → **`Release`** into **`build/`**. CMake presets do not auto-select a mode; you choose **Debug** (`mingw32-cross`) or **Release** (`mingw32-cross-release`) in CMake Tools or on the command line.

**CMake presets** (same `build/` for both; switching preset changes `CMAKE_BUILD_TYPE`):

```bash
cmake --preset mingw32-cross-release
cmake --build --preset mingw32-cross-release
```

Debug preset (equivalent type for local investigation):

```bash
cmake --preset mingw32-cross
cmake --build --preset mingw32-cross
```

Manual configure (same generator as presets; **Release** example):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake
cmake --build build
```

Debug: use the same command with `-DCMAKE_BUILD_TYPE=Debug` instead of `Release`.

**Shell helpers** (from repo root):

```bash
./scripts/build.sh              # Release → build/ (default mode + default dir)
./scripts/build.sh --make       # force Unix Makefiles
./scripts/build.sh --debug      # Debug → build/
./scripts/clean.sh              # rm -rf build/
./scripts/rebuild.sh            # clean then build (passes same flags as build.sh)
```

Environment: `BUILD_DIR=mybuild ./scripts/build.sh` sets the build directory (**default:** `build`). Optional: `BUILD_TYPE=Debug|Release ./scripts/build.sh` overrides the script’s default **Release** without using `--debug`.

The DLL is `build/gamex86.dll` (or `$BUILD_DIR/gamex86.dll`). Copy it into the game’s `base` folder next to `oldgamex86.dll`.

#### Troubleshooting (CMake Tools / CLI)

- **`Parse error` in `cmake/toolchain-mingw32.cmake`**: the file must be **UTF-8**, not UTF-16. If an editor re-saved it as Unicode, re-checkout the file or convert encoding. The repo’s `.gitattributes` keeps `*.cmake` as LF text to reduce this.
- **Ninja not found / `ENOENT`**: install **`ninja-build`** and ensure `ninja` is on `PATH` (Debian/Ubuntu: `/usr/bin/ninja` from that package).

### Windows (native, optional)

On Windows you can build the same `CMakeLists.txt` with [MinGW-w64 i686](https://www.mingw-w64.org/) or another CMake generator; no Visual Studio project files are kept in this repo.

### Game install

1. In your SOF1 `base` folder, rename the original `gamex86.dll` to `oldgamex86.dll`.
2. Place the built `gamex86.dll` in `base` beside `oldgamex86.dll`.

The **`src/engine/`** tree (`gamecpp/`, `qcommon/`, `player/`, `ghoul/`) is **optional reference** SDK source from the original game; the shim does not compile it.

## Adding a feature

Behaviour changes ship in the shim by **detouring** code inside **`oldgamex86.dll`** (the stock game DLL renamed beside the shim). The shim’s own `gamex86.dll` image does not contain that logic, so **`GameDll` symbols in `detours.yaml` resolve against `oldgamex86.dll`** (see `src/runtime/detours.cpp`).

### 1. Enable the feature

In **`features/features.yaml`**, add `your_feature: true` under `features:` (or list it under `enabled:`). Only enabled feature directories under `src/features/` are scanned for JSON and sources. If you disable a feature, remove its `.cpp` from **`CMakeLists.txt`** as well, or the build can fail once the generator drops that feature’s detours and declarations.

### 2. Register symbols

Declare every function you hook or call via a pointer in **`detours.yaml`**: `name`, `module` (`GameDll` for game code), `identifier` (RVA as `0x...` below `0x10000000`, or an absolute address), calling convention, return type, and `params`. Run CMake so **`tools/generate_hooks.py`** emits `build/generated/generated_detours.h` / `.cpp` and registrations.

### 3. Layout under `src/features/<name>/`

| Path | Purpose |
|------|---------|
| **`hooks/hooks.json`** | Full detours: `function` (must match `detours.yaml`), `callback`, optional `phase` (`Pre` / `Post`), optional **`override`: true** (your callback receives the original trampoline and replaces the stock function). |
| **`hooks/pointers.json`** | List of `detours.yaml` names resolved to **`detour_Name::oName`** (call-site shorthand: **`SOF_EP_Name(...)`** in `generated_engine_pointers.h`)—**no** patch at those addresses. Use this for helpers (`G_Find`, `OnSameTeam`, etc.) so you do not install redundant hooks. |
| **`callbacks/callbacks.json`** | Lifecycle hooks such as **`GameDllLoaded`** (see `src/features/example/`). |
| **`<name>.cpp`** | Implement callbacks; include **`generated_detours.h`** and **`generated_registrations.h`**. |

Details, override semantics, and variadic limits: **`docs/DETOUR_SYSTEM.md`**.

### 4. Wire the build

Add your **`src/features/<name>/*.cpp`** to the **`add_library(gamex86 …)`** list in **`CMakeLists.txt`**.

### 5. Rebuild

Reconfigure or build so the custom command re-runs the generator (e.g. `cmake --build build`). Install **`build/gamex86.dll`** into `base` as usual.

A full example that uses an **override** plus **pointer-only** helpers is documented in **`src/features/ctf_spawn/README.md`**.

## Setup

_(Add further install and run instructions as the project grows.)_
