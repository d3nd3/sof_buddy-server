# sof_buddy-server

A server-side mod for **Soldier of Fortune 1**. It ships as a `gamex86.dll`
shim that loads the stock game DLL beside it and hosts a data-driven detour
system, so behaviour changes and instrumentation can be added without touching
the original binary.

It runs on the **game server** — dedicated or listen-server host — not on
players' clients. Nothing here has to be installed by the people connecting to
you.

Everything an admin interacts with is a **cvar**. That is what the rest of this
document is mostly about.

---

## Install

1. In your SoF `base` folder, rename the original `gamex86.dll` to
   **`oldgamex86.dll`**.
2. Drop this project's built **`gamex86.dll`** into `base` beside it.
3. Start the server as usual.

The shim logs to `sofbuddy-shim.log` next to the executable. If something is
wrong, that file says so on the first line.

---

## Cvars

### How to set them

| Where | How | Use for |
|---|---|---|
| **Command line** | `+set _sofbuddy_hashmap 1` | Anything marked **load-time** below — these are read once and never again |
| **Server config** | `set _sofbuddy_tickpace_reserve_ms 5` | Normal tuning; put it in the config your server already runs at startup |
| **Live console / rcon** | `_sofbuddy_tickpace 0` | Anything marked **live** — takes effect on the next frame, no restart |

Two properties in the tables below matter:

- **`ARCHIVE`** — the engine writes the value into your config when it saves.
- **`NOSET`** — the engine refuses `set` from the console. Every output cvar is
  `NOSET`: they are readouts, not settings, and writing to them is meaningless.

> **A cvar only exists if its feature is compiled in.** Features are toggled at
> **build** time in `src/features/features.yaml`, not at runtime. If a cvar
> below does not exist on your server, its feature was built out. The default
> build ships **`clamp_monitor`**, **`hash_lookup`**, **`tick_pacing`** and
> **`cbuf_insert`**.

---

### Feature switches

The master on/off for each feature.

| Cvar | Default | When read | Flags | What it does |
|---|:---:|:---:|:---:|---|
| `_sofbuddy_tickpace` | `1` | live | `ARCHIVE` | Tick pacing: make server ticks fire on their own 100 ms boundary instead of whenever the loop next notices |
| `_sofbuddy_hashmap` | `0` | **load-time** | `ARCHIVE` | Replace the engine's cvar/command/alias linked lists with hash maps. Needs `+set` — read once at game-DLL load |
| `_sofbuddy_cbuf_insert` | `0` | live | `ARCHIVE` | Shift the command buffer in place instead of round-tripping it through the zone allocator |
| `_sofbuddy_zpool` | `0` | **load-time** | `ARCHIVE` | Recycle zone allocations. **Measured no effect** — kept as a recorded negative result |
| `_sofbuddy_custom_respawn` | `1` | live | — | CTF spawn selection: pick the team spawn farthest from the nearest living enemy, avoiding teammates |
| `_sofbuddy_example_enabled` | `1` | live | — | Template feature, for developers. Not built by default |

`clamp_monitor` has no on/off switch — it is pure measurement and always
active when built in.

---

### Tick pacing — `tick_pacing`

Server ticks are supposed to run every 100 ms. The engine samples its clock
*before* running the console command buffer, so a tick whose boundary passes
during heavy sofplus scripting is not noticed until the whole loop has gone
round again. This feature closes that gap.

**Tunables** — all `ARCHIVE`, all read live, all clamped to the ranges shown:

| Cvar | Default | Range | Meaning |
|---|:---:|:---:|---|
| `_sofbuddy_tickpace_spin_ms` | `0` | 0 – 20 | When a tick is due within this many ms, busy-wait to the boundary rather than sleeping past it. Costs CPU. Dedicated servers only |
| `_sofbuddy_tickpace_reserve_ms` | `3` | 0 – 50 | Headroom a command-buffer drain must have before it is allowed to *start*. `0` = stock scheduling |
| `_sofbuddy_tickpace_defer_max_ms` | `200` | 0 – 1000 | Never hold a drain longer than this, whatever the headroom says |

**Readouts** — all `NOSET`:

| Cvar | Meaning |
|---|---|
| `_sofbuddy_tickpace_cbuf_max` | **Read this one first.** Worst single command-buffer drain, in ms |
| `_sofbuddy_tickpace_late_avg` | Rolling mean tick lateness, in ms |
| `_sofbuddy_tickpace_late_max` | Worst tick lateness since boot, in ms |
| `_sofbuddy_tickpace_saved` | Ticks rescued from slipping into the following loop iteration |
| `_sofbuddy_tickpace_defers` | Drains held back by the reserve |

**Interpreting `_sofbuddy_tickpace_cbuf_max`:** under ~10 ms, the reserve is
doing real work and the tunables are worth tuning. Well above it, no scheduling
policy can keep a drain that long off a 100 ms boundary — the win has to come
from the scripts themselves.

Details, including why command-buffer drains are never split: [`src/features/tick_pacing/README.md`](src/features/tick_pacing/README.md)

---

### Clamp monitoring — `clamp_monitor`

Measures the two ways the engine's clock gets forcibly corrected. They are
opposite failures and mean different things.

- **highclamp** — the server fell behind and the engine *deletes* the time it
  owed. This is a load gauge. Non-zero means starvation.
- **lowclamp** — the server clock ended up more than a whole tick *behind* game
  time and the engine jumps it forward, *inventing* the difference. On a running
  map this should be zero. Non-zero means something is moving the clock outside
  the normal tick path.

**Tunables** — all `ARCHIVE`, read live:

| Cvar | Default | Meaning |
|---|:---:|---|
| `_sofbuddy_clamp_notify_ms` | `5` | Log a line to `sofbuddy-shim.log` when a single clamp deletes at least this many ms. At most one line per second |
| `_sofbuddy_clamp_window` | `2` | Rolling window in seconds for the average below. Rounded to whole ticks, capped at 16 s |
| `_sofbuddy_clamp_broadcast_ms` | `0` | Tell **all connected players** the server is lagging when the rolling average reaches this. `0` = off. Opt-in, because it is player-visible |
| `_sofbuddy_clamp_broadcast_interval` | `30` | Minimum seconds between those broadcasts |

**Readouts** — all `NOSET`:

| Cvar | Meaning |
|---|---|
| `_sofbuddy_highclamps` | Count of highclamp events since boot |
| `_sofbuddy_clamp_avg` | Rolling average of per-tick lost ms, over the window above |
| `_sofbuddy_clamp_last` | Ms lost on the most recent tick (`0` if it was clean) |
| `_sofbuddy_clamp_lost_ms` | Cumulative ms deleted since boot |
| `_sofbuddy_lowclamps` | Count of lowclamp events since boot |
| `_sofbuddy_lowclamp_checks` | Frames the lowclamp test actually ran on — the **denominator** that makes a zero above mean *measured* zero rather than *never measured* |
| `_sofbuddy_lowclamp_gained_ms` | Cumulative ms the engine invented |
| `_sofbuddy_lowclamp_worst` | Biggest single forward jump, in ms |

**Reading a zero.** `_sofbuddy_lowclamps 0` is only meaningful alongside
`_sofbuddy_lowclamp_checks`. That counter counts `SV_Frame`s, not ticks, so on a
live dedicated server it should climb by **hundreds per second**. If it is
stuck at `0`, the measurement is not running and the zero means nothing.

Details: [`src/features/clamp_monitor/README.md`](src/features/clamp_monitor/README.md)

---

### Command buffer inserts — `cbuf_insert`

sofplus scripting calls `Cbuf_InsertText` constantly, and the stock
implementation copies the entire queued buffer out to the heap and back on
every call. This shifts it in place instead.

**Ships off.** Measurement runs in *both* states, so flipping
`_sofbuddy_cbuf_insert` between `0` and `1` across two comparable busy periods
is a controlled A/B on your own server rather than a leap of faith.

**Readouts** — all `NOSET`, published at most 10×/second:

| Cvar | Meaning |
|---|---|
| `_sofbuddy_cbuf_insert_us` | Cumulative microseconds spent inside `Cbuf_InsertText`. **This is the A/B number** — same workload at `0` and at `1` |
| `_sofbuddy_cbuf_insert_bytes` | Cumulative bytes of already-queued text shifted. The quantity the optimisation removes most of |
| `_sofbuddy_cbuf_insert_max` | Largest buffer seen at insert time. If this stays small, there is nothing here to win |
| `_sofbuddy_cbuf_inserts` | Total calls |
| `_sofbuddy_cbuf_insert_slow` | Calls that fell back to the engine (buffer overflow, or the cvar off) |

Details: [`src/features/cbuf_insert/README.md`](src/features/cbuf_insert/README.md)

---

### Dictionary hashing — `hash_lookup`

SoF inherits Quake 2's cvars, commands and aliases as **linked lists**. Every
cvar read, every cvar write and every console command walks a list and
`strcmp`s each node. That is fine for a client with a human at the keyboard; it
is not fine for a server running sofplus scripting, which does thousands of
these per tick.

`_sofbuddy_hashmap 1` turns all three into hash maps. It is **load-time only** —
put `+set _sofbuddy_hashmap 1` on the command line, or set it in a config that
runs before the first map.

Details: [`src/features/hash_lookup/README.md`](src/features/hash_lookup/README.md)

---

## Recipes

**Is my server actually keeping up?**

```
_sofbuddy_highclamps          should stay 0
_sofbuddy_clamp_avg           should stay 0.00
_sofbuddy_tickpace_cbuf_max   how bad the worst drain got
```

**Is the instrumentation even running?**

```
_sofbuddy_lowclamp_checks     must climb by hundreds per second
_sofbuddy_tickpace_saved      should be climbing on a busy server
```

**Is `cbuf_insert` worth enabling here?**

Run a representative busy period at `_sofbuddy_cbuf_insert 0`, note
`_sofbuddy_cbuf_insert_us` and `_sofbuddy_cbuf_inserts`; repeat at `1`; compare
microseconds per call. If `_sofbuddy_cbuf_insert_max` never gets large, skip it.

**Warn players when the server is struggling** (opt-in, player-visible):

```
set _sofbuddy_clamp_broadcast_ms 25
set _sofbuddy_clamp_broadcast_interval 60
```

---

## Building

SoF's `gamex86.dll` is **32-bit x86**. The default toolchain is MinGW-w64
targeting i686, driven by CMake + Ninja.

```bash
sudo apt install cmake ninja-build g++-mingw-w64-i686 python3-yaml
sudo update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix
sudo update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-posix
```

```bash
./scripts/build.sh              # Release -> build/gamex86.dll  (default)
./scripts/build.sh --debug      # Debug
./scripts/rebuild.sh            # clean, then build
./scripts/clean.sh              # rm -rf build/
```

Both modes share one build directory (`build/`, or `$BUILD_DIR`); switching
mode reconfigures it. CMake presets `mingw32-cross` (Debug) and
`mingw32-cross-release` (Release) do the same thing.

**Enabling and disabling features** is a one-line edit in
`src/features/features.yaml` — set the feature `true` or `false` and rebuild.
CMake reads that file to decide which feature directories to compile, and the
generator emits detours only for enabled features, so nothing else needs
touching.

### Tests

Host-side test suites, no server and no Wine required — they build each
feature's real translation units for the host against stub headers and drive
them with a transcription of the engine's own loop:

```bash
tools/tests/tick_pacing/run.sh
tools/tests/clamp_monitor/run.sh
tools/tests/cbuf_insert/run.sh

SWEEP=1 tools/tests/tick_pacing/run.sh   # 6048-config clamp sweep
```

---

## Developing

- **[`NEW_FEATURE.md`](NEW_FEATURE.md)** — step-by-step guide to adding one.
- **[`docs/DETOUR_SYSTEM.md`](docs/DETOUR_SYSTEM.md)** — how `detours.yaml`,
  `hooks.json`, `pointers.json` and `callbacks.json` fit together, including
  override semantics.
- **`reference/engine/`** — optional SDK reference source from the original
  game. Not compiled.

Behaviour changes are made by **detouring** code inside `oldgamex86.dll`, so
`GameDll` symbols in `detours.yaml` resolve against that image, not against the
shim.
