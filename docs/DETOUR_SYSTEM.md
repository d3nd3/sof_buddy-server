# Detour system (sof_buddy-server)

This repo mirrors the **data-driven detour workflow** from the **sof_buddy** client project: declare symbols in **`detours.yaml`**, wire behaviour in **`src/features/<name>/hooks/hooks.json`** and **`callbacks/callbacks.json`**, and enable optional features in **`features/features.yaml`** (same family as **`detours.yaml`**: comments, clear `true` / `false` toggles). The Python tool **`tools/generate_hooks.py`** (adapted from sof_buddy) emits generated headers under **`${CMAKE_BINARY_DIR}/generated`** when CMake runs (including **`generated_engine_pointers.h`** / **`generated_engine_pointers.cpp`** for pointer-only call helpers). If **`features.yaml`** is absent, the generator falls back to legacy **`features/FEATURES.txt`** (one name per line).

## Layout

| Path | Role |
|------|------|
| `detours.yaml` | Registry of hookable functions (name, module, RVA or absolute address, signature). |
| `features/features.yaml` | Optional features under `src/features/<name>/`: map **`features:`** with **`name: true|false`**, and/or an **`enabled:`** list of names. **`core`** is not configured here (always on). |
| `src/core/hooks.json` | Optional core function hooks. |
| `src/core/callbacks.json` | Optional core lifecycle callbacks. |
| `src/core/pointers.json` | Core pointer-only symbols (resolved to **`detour_Name::oName`**, no detour). Always processed. E.g. **`Com_DPrintf`** for developer print. |
| `src/features/<feature>/hooks/hooks.json` | Per-feature Pre/Post (or override) hooks on `detours.yaml` names. |
| `src/features/<feature>/hooks/pointers.json` | Optional: extra pointer-only symbols for that feature. |
| `${CMAKE_BINARY_DIR}/generated/generated_engine_pointers.h` | **`SOF_EP_Name(...)`** macros expand to **`detour_Name::oName(__VA_ARGS__)`** (prefix avoids clashing with qualified **`detour_Name::oName`** in source—bare **`oName(...)`** macros used to recurse badly). **`EnginePointers_Bind()`** (in the generated `.cpp`) assigns fallbacks after all `RegisterPointerOnlyFunctions_*` when resolve failed (always non-null at call sites). |
| `src/features/<feature>/callbacks/callbacks.json` | Register for shared hooks (e.g. `GameDllLoaded`). |

Only detours **referenced** by enabled features (hooks, pointers, or **`oName` / `SOF_EP_Name`** in source) are generated. **`core`** hooks/callbacks/pointers are always in scope (not gated by `features.yaml`).

**Variadic** functions (e.g. `Com_DPrintf`) must be **pointer-only** (listed in `pointers.json`); full typed detours are not generated for them.

### How `pointers.json` works (same as sof_buddy)

1. Every name must exist in **`detours.yaml`** (module, RVA or absolute address, return type, calling convention, parameters; use **`variadic: true`** for `printf`-style functions with `...`).
2. **`pointers.json`** is a JSON array of those names (strings, or `{"function":"Name"}` objects).
3. The generator emits **`detour_Name::tName`**, **`oName`**, and **`RegisterPointerOnlyFunctions_<Module>()`** code that resolves the address and assigns **`oName`** — **no** trampoline, no `TypedSharedHookManager`.
4. If the same function is listed in **`hooks.json`** and **`pointers.json`**, **hooks win** (full detour); the pointer entry is ignored.
5. For short call sites, **`generated_engine_pointers.h`** defines **`SOF_EP_Name(...)`** to **`detour_Name::oName(__VA_ARGS__)`**. You may also call **`detour_Name::oName`** directly. Call **`EnginePointers_Bind()`** once after all **`RegisterPointerOnlyFunctions_*()`** so unresolved pointers get a generated fallback stub (never null at call sites).

## Server shim lifecycle

After the engine calls **`GetGameAPI`** the first time, the shim:

1. `ProcessDeferredRegistrations()` — materialize auto-registered detours.
2. `RegisterAllFeatureHooks()` — wire callbacks from JSON.
3. `RegisterPointerOnlyFunctions_*()` — fill pointer-only originals.
4. `ApplyGameDetours()` — install patches for **GameDll** (and other modules as configured).
5. `DispatchHook<void*>("GameDllLoaded", Post, game_export)` — run feature callbacks.

Add new lifecycle hooks by dispatching from the appropriate place in C++ and listing callbacks in JSON (same pattern as sof_buddy `DISPATCH_SHARED_HOOK`).

## Build requirements

- **Python 3** with **PyYAML** (`sudo apt install python3-yaml`).
- Env **`SOF_BUDDY_SERVER_GEN_DIR`** overrides the generator output directory (CMake sets it to `build/generated`).

## Further reading

Full detail (override hooks, variadic limits, module naming): **`docs/DETOUR_SYSTEM.md`** in the **sof_buddy** repository.

The original SOF1 game SDK headers live under **`src/engine/`** in this repo (reference only).
