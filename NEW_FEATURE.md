# Creating a new feature

Every feature lives in **one self-contained folder**: `src/features/<name>/`.
Features stack: each one hooks stock game functions through the shared detour
system, so multiple features can patch the same or different functions without
touching each other.

There are exactly **four** things a feature can consist of:

| File | Purpose | Required |
|------|---------|----------|
| `<name>.cpp` (+ optional `.h`) | Your code: hook callbacks, cvars, logic. | yes |
| `hooks/hooks.json` | Full detours (Pre/Post or **override**) on functions declared in `detours.yaml`. | optional |
| `hooks/pointers.json` | Call-only helpers (`SOF_EP_Name(...)`): resolved address, **no** patch. | optional |
| `callbacks/callbacks.json` | Lifecycle callbacks (e.g. `GameDllLoaded`). | optional |

Nothing else needs registering anywhere else by hand except the two global
files below.

---

## Step 1 — Find your targets (RVAs)

All game-side addresses are **RVAs into `oldgamex86.dll`** (retail image base
`0x50000000`, so `RVA = IDA address − 0x50000000`). Get them from IDA on the
stock DLL (the `gamex86.i64` database).

Rules:

- RVA values go in `detours.yaml` as strings **below `0x10000000`**
  (e.g. `"0xF2FD0"`). Anything ≥ `0x10000000` is treated as an absolute,
  already-mapped address (only correct for `SoF.exe`, base `0x20000000`).
- Verify each RVA points at the right function (decompile it) — a wrong RVA
  means patching random code.

## Step 2 — Declare symbols in `detours.yaml`

Append one entry per function you want to hook or call:

```yaml
  - name: MyFunction            # unique, used everywhere else
    module: GameDll             # GameDll=oldgamex86.dll | SofExe=SoF.exe | RefDll | PlayerDll
    identifier: "0x123456"      # RVA (see step 1)
    return_type: void*          # int, void, char*, bool/qboolean, short, ...
    calling_convention: __cdecl # __cdecl | __thiscall (pass `thisp` as first param) | __stdcall
    detour_len: 0               # 0 = auto-length (keep it 0 unless told otherwise)
    params:
      - type: void*
        name: ent
```

Notes:

- `printf`-style variadic functions need `variadic: true` and may only be used
  **pointer-only** (step 5) — no full typed detours for them.
- Only symbols **referenced by an enabled feature** get generated; unused
  declarations cost nothing.

## Step 3 — Enable the feature

Create the folder and toggle it on in **`src/features/features.yaml`**:

```yaml
features:
  example: true
  ctf_spawn: true
  my_feature: true      # <- add this
```

The folder layout:

```
src/features/my_feature/
├── my_feature.cpp
├── cvar.h                  (optional, see step 6)
├── cvar.cpp
├── hooks/hooks.json        (optional)
├── hooks/pointers.json     (optional)
└── callbacks/callbacks.json (optional)
```

> Disabling later: set `my_feature: false`. That is the whole step — CMake
> reads the same `features.yaml` (via `generate_hooks.py --list-enabled`) and
> compiles only enabled features' sources, so a disabled feature is not built
> and cannot fail to compile against symbols the generator deliberately did not
> emit. Editing `features.yaml` re-runs CMake configure.
>
> If the feature has a detach entry point called from `src/gamex86.cpp`, guard
> it with `#ifdef SOF_FEATURE_MY_FEATURE` — CMake defines one such macro per
> enabled feature, upper-cased.

## Step 4 — Full detours (`hooks/hooks.json`)

Patch the stock function so every call goes through your code:

```json
[
  {
    "function": "MyFunction",
    "callback": "myfeature_MyHook",
    "phase": "Post"
  }
]
```

- `phase`: `"Pre"` runs before the original, `"Post"` after (original's return
  value passed along where applicable).
- **Override mode** — replace the function entirely; your callback receives
  the original trampoline as an extra trailing argument:

```json
[
  {
    "function": "SelectTeamDeathmatchSpawnPoint",
    "callback": "ctfspawn_SelectTeamSpawn",
    "override": true
  }
]
```

```cpp
// original has type detour_SelectTeamDeathmatchSpawnPoint::tSelectTeamDeathmatchSpawnPoint
void* ctfspawn_SelectTeamSpawn(void* ent,
    detour_SelectTeamDeathmatchSpawnPoint::tSelectTeamDeathmatchSpawnPoint original)
{
    // ... your logic; call original(ent) to reach stock behaviour
}
```

## Step 5 — Call-only helpers (`hooks/pointers.json`)

For functions you want to **call**, not intercept:

```json
["G_Find", "OnSameTeam"]
```

Call them via the generated shorthand:

```cpp
#include "generated_engine_pointers.h"

void* spot = SOF_EP_G_Find(from, fieldofs, match, cmp);
// equivalent: detour_G_Find::oG_Find(from, fieldofs, match, cmp);
```

Resolved once at boot against the module from `detours.yaml`; if resolution
fails a safe fallback stub is bound (returns `nullptr`/`0`), and feature code
should still null-check `detour_Name::oName` before first use (see
`ctf_spawn.cpp`).

## Step 6 — Write the code

Skeleton `my_feature.cpp`:

```cpp
#include "generated_detours.h"         // detour_* namespaces, o* originals
#include "generated_registrations.h"   // RegisterAllFeatureHooks decl
#include "log.h"

#include <windows.h>

// Override-style hook (matches step 4):
void* myfeature_MyHook(void* ent,
    detour_MyFunction::tMyFunction original)
{
    if (!original)
        return nullptr;
    // ... pre logic ...
    void* result = original(ent);
    // ... post logic ...
    return result;
}
```

### Cvars

Expose runtime switches through the engine cvar API (bound automatically from
the engine's `game_import_t` — slot map: [`docs/game_import_t.md`](docs/game_import_t.md)).
Copy the small `cvar.h` / `cvar.cpp` pair from
an existing feature and adjust the names:

```cpp
// cvar.cpp
#include "cvar.h"
#include "buddy_import.h"

namespace { void* g_cvEnabled = nullptr; }

void MyFeature_InitCvars() {
    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_my_feature", "1", 0, nullptr);
}

bool MyFeature_Enabled() {
    if (!g_cvEnabled) MyFeature_InitCvars();
    if (!g_cvEnabled) return true;                 // fail open
    return Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
}
```

Gate expensive/behaviour-changing paths on it (see `CtfSpawn_CustomRespawnEnabled()`).

### Safety conventions (keep them)

- Validate every game pointer before dereferencing: copy the tiny
  `IsValidUserPointer` / `IsSafeMemoryBlock` / `IsExecutableCodeAddress`
  helpers from `src/features/ctf_spawn/ctf_spawn.cpp`.
- Guard against re-entrancy in hooks whose original can call them again
  (`static thread_local bool` pattern, as in `ctfspawn_SelectTeamSpawn`).
- Never call engine functions before `GetGameAPI` bootstrap — lifecycle
  callbacks (`GameDllLoaded`) and detours only fire after it.

## Step 7 — Lifecycle callbacks (optional)

`callbacks/callbacks.json`:

```json
[
  { "hook": "GameDllLoaded", "callback": "myfeature_OnGameDllLoaded",
    "priority": 0, "phase": "Post" }
]
```

```cpp
void myfeature_OnGameDllLoaded(void* game_export) {
    PrintOut(PRINT_LOG, "[my_feature] up\n");
}
```

Available today: `GameDllLoaded` (fires once, after the stock DLL init and
detour application). Adding more dispatch points = one
`SharedHookManager::Instance().DispatchHook<...>(...)` call in the shim plus
JSON entries.

## Step 8 — Build, deploy, verify

```bash
./scripts/build.sh --debug   # Debug (symbols) — use without flag for Release
./scripts/cptosof            # install build/gamex86.dll into the Wine SOF Base/
```

Run the dedicated server and confirm, in order:

1. `User/sof.log` reaches `====== Soldier of Fortune Initialized ======`.
2. stderr shows `Registered detour: <YourFunction> ...` and
   `Applied N game.dll detours successfully`.
3. Your `PrintOut` lines appear; exercise the feature in-game.

If the server dies instantly after `LoadLibrary (./base/gamex86.dll)` with a
stack overflow, something re-entered `GetGameAPI` — see
`src/gamex86.cpp::ExportsOnlyGetGameAPI` (the guard that rejects non-stock
game DLLs) and make sure `Base/oldgamex86.dll` is the retail file.

---

## Cheat sheet

| I want to… | Do this |
|---|---|
| Hook a stock function | `detours.yaml` entry + `hooks/hooks.json` + callback in `.cpp` |
| Replace a stock function | same, with `"override": true`; take `original` as last arg |
| Just *call* a stock function | `detours.yaml` entry + `hooks/pointers.json`; use `SOF_EP_Name(...)` |
| Add a runtime switch | copy `cvar.{h,cpp}` pattern; gate on `Buddy_ReadCvarValue` |
| Run code once at startup | `callbacks/callbacks.json` with `GameDllLoaded` |
| Toggle a feature | `src/features/features.yaml` (`true`/`false`) |
| Add addresses | never hardcode — always `detours.yaml` + generator |
