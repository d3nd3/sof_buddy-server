# Feature: `ctf_spawn`

CTF-only change to **team spawn selection**: pick the team spawn point that is **farthest from the nearest living enemy**, but **avoid** points where a **teammate** is already within ~**56** units (same idea as overlapping a spawn). If **every** team spawn is blocked by a teammate, fall back to the farthest-from-enemy spawn anyway (you can spawn inside a teammate; enemies on the point are still handled by the game’s existing **`KillBox`** / telefrag behaviour).

Non-CTF modes are unchanged: the override forwards to the original **`SelectTeamDeathmatchSpawnPoint`** when `deathmatch` is not **CTF** (`games_t::DM_CTF` = **4**).

## Files in this folder

| File | Role |
|------|------|
| **`ctf_spawn.cpp`** | Implements **`ctfspawn_SelectTeamSpawn`** (override entry point) using `oG_Find`, `oOnSameTeam`, `oInfo_ValueForKey`, `oSelectRandomDeathmatchSpawnPoint`, and waypoint helpers. |
| **`cvar.h`** / **`cvar.cpp`** | Handles feature-specific cvar creation and checks (`_sofbuddy_custom_respawn`). |
| **`hooks/hooks.json`** | **`override`: true** on **`SelectTeamDeathmatchSpawnPoint`** → single choke point; stock logic is replaced only on the CTF path. |
| **`hooks/pointers.json`** | Marks helper symbols as **pointer-only** so the generator does not install detours on them—only fills **`o*`** trampolines after the game DLL loads. |

Symbol names and RVAs live in the repo-root **`detours.yaml`** (shared registry).

## Implementation notes

- **`ctf_spawn.cpp`** calls the game only through **`SOF_EP_*`** macros (and `detour_Com_DPrintf::oCom_DPrintf` for a null check before **`CtfDPrintf`**). During one spawn selection it caches **`g_edicts`** and **`maxclients`** and passes them into **`MinEnemyDist` / `AllyBlocks`** so each candidate spawn does not re-query globals or duplicate work between those two passes.
- **Module image**: All game RVAs are relative to **`oldgamex86.dll`**, not the shim’s `gamex86.dll`.
- **Assumed PE base for RVAs in `detours.yaml`**: **`0x50000000`** (typical for this game DLL in IDA). If your binary uses another image base, recompute **RVA = virtual_address − image_base** and update the YAML.
- **Hard-coded offsets** in `ctf_spawn.cpp` (edict layout, cvar slots, WP manager object, `FOFS(classname)`, etc.) were taken from **retail `gamex86.dll`** disassembly / decompilation aligned with Linux `gamex86.so` symbols. A different patch level or build can desync these; re-validate with IDA on **your** `oldgamex86.dll`.
- **Waypoint write**: After choosing a spawn, the feature mirrors the stock path: **`__thiscall`** WP index functions on the global WP manager object, then stores the result at **`client/ps` offset `0x408`**, matching the vanilla team-spawn tail so prediction / waypoint state stays consistent.
- **Cvar value read**: `deathmatch` mode is read as **`(int)cvar->value`** with **`value` at `+0x18`** from the `cvar_t` pointer loaded from a **global slot** (`kRvaDeathmatchCvar`). Confirm with **`InitDeathmatchSystem`** or similar if you port to another build.

## Enabling

Set **`ctf_spawn: true`** in **`src/features/features.yaml`**. If you set it to **`false`**, the generator stops emitting detours and registrations for this feature’s symbols; you should also **rename `ctf_spawn.cpp`** (e.g. to `ctf_spawn.cpp.off`) so the project does not compile code that expects those generated hooks and `o*` pointers.

## Debug logging (temporary)

`ctf_spawn.cpp` emits many **`Com_DPrintf`** lines: **CtfDPrintf** forwards formatted output through **`SOF_EP_Com_DPrintf`** on every team spawn evaluation: deathmatch mode, team/classname, each candidate spawn (enemy distance, ally-block flag, origin), chosen spot, waypoint apply, and stock fallbacks. In the SOF1 / Quake-style console, **`developer` must be non-zero** (or your build’s equivalent) for those messages to appear. Remove or gate this block when polishing.
