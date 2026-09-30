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

Diagnostics go to the server console via `gi.dprintf` (and `User/sof.log` with
`logfile 1`). Routine feature chatter is off by default (opt-in, e.g.
`_sofbuddy_clamp_notify_ms`).

---

## Cvars

### How to set them

| Where | How | Use for |
|---|---|---|
| **Command line** | `+set _sofbuddy_hashmap 1` | Anything marked **load-time** below — these are read once and never again |
| **Server config** | `set _sofbuddy_cmdpark_reserve_ms 5` | Normal tuning; put it in the config your server already runs at startup |
| **Live console / rcon** | `_sofbuddy_tickpace 0` | Anything marked **live** — takes effect on the next frame, no restart |

Two properties in the tables below matter:

- **`ARCHIVE`** — the engine writes the value into your config when it saves.
- **`NOSET`** — the engine refuses `set` from the console. Every output cvar is
  `NOSET`: they are readouts, not settings, and writing to them is meaningless.

> **A cvar only exists if its feature is compiled in.** Features are toggled at
> **build** time in `src/features/features.yaml`, not at runtime. If a cvar
> below does not exist on your server, its feature was built out. The default
> build ships **`cpu_optimizations`**, **`stufftext`**, **`reliable_defer`**, and
> **`print_guard`**. Plan:
> [`src/features/cpu_optimizations/README.md`](src/features/cpu_optimizations/README.md)

---

### Feature switches

**Settings** (you set these):

| Cvar | Default | When read | Flags | What it does |
|---|:---:|:---:|:---:|---|
| `_sofbuddy_cpuopt` | `1` | live | `ARCHIVE` | Master off-switch for every `cpu_optimizations` module. Hashmap/zpool only honour it at `GameDllLoaded` |
| `_sofbuddy_cmdpark_strict` | `1` | live | `ARCHIVE` | Tick first: park inter-tick inserts, move every pre-frame queue aside, run everything post-tick. Highclamp / lowclamp violations log one console line |
| `_sofbuddy_strict_exit` | `0` | live | `ARCHIVE` | Quit on a strict violation instead of logging and continuing |
| `_sofbuddy_tickpace` | `1` | live | `ARCHIVE` | Tick pacing: settle + optional spin onto the 100 ms boundary |
| `_sofbuddy_cmdpark` | `1` | live | `ARCHIVE` | Park every `cmd_text` insert in the reserve window; drip after the tick |
| `_sofbuddy_hashmap` | `1` | **load-time** | `ARCHIVE` | Replace the engine's cvar/command/alias linked lists with hash maps. Read once at game-DLL load; `+set 0` to skip |
| `_sofbuddy_cbuf_insert` | `1` | live | `ARCHIVE` | Shift the command buffer in place instead of round-tripping it through the zone allocator |
| `_sofbuddy_zpool` | `1` | **load-time** | `ARCHIVE` | Recycle zone allocations. Read once at game-DLL load; `+set 0` to skip |
| `_sofbuddy_custom_respawn` | `1` | live | — | CTF spawn selection: pick the team spawn farthest from the nearest living enemy, avoiding teammates |
| `_sofbuddy_stufftext` | `1` | live | `ARCHIVE` | Server console command `stufftext`: send `svc_stufftext` to clients |
| `_sofbuddy_reldef` | `1` | live | `ARCHIVE` | Queue server→client reliable staging so bursty prints/stufftext do not coalesce into one blob. Bypassed during connect (`state < cs_spawned`) |
| `_sofbuddy_example_enabled` | `1` | live | — | Template feature, for developers. Not built by default |

`clamp_monitor` has no on/off switch — it is pure measurement and always
active when built in. `_sofbuddy_cmdcost` defaults on; handlers are only patched
while live timing is enabled (`1`), and spin/bench never leaves wraps installed.
set `1` to time handlers without touching tick pacing.

---

### Tick pacing — `tick_pacing`

Server ticks are supposed to run every 100 ms. The engine samples its clock
*before* running the console command buffer, so a tick whose boundary passes
during heavy sofplus scripting is not noticed until the whole loop has gone
round again. This feature closes that gap.

**Settings** (you set these) — all `ARCHIVE`, all read live, all clamped to the ranges shown:

| cvar | default | range | what it does |
|---|:---:|:---:|---|
| `_sofbuddy_tickpace_spin_ms` | `0` | 0 – 20 | When a tick is due within this many ms, busy-wait to the boundary rather than sleeping past it. Costs CPU. Dedicated servers only |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_tickpace_late_avg` | ms | Rolling mean tick lateness |
| `_sofbuddy_tickpace_saved` | count | Ticks whose boundary passed mid-drain and were caught this loop |

Details: [`src/features/cpu_optimizations/tick_pacing/README.md`](src/features/cpu_optimizations/tick_pacing/README.md)

---

### Command-buffer parking — `cmdtext_parking`

Parks every `cmd_text` insert (console, `.COMMAND`, `clc_stringcmd`, timers,
`Cbuf_AddText` / `InsertText` / `ExecuteText` insert+append) when the tick is
inside the reserve window, and moves the queued buffer aside instead of
running it on the boundary. Drips one fitting chunk per later sub-tick, plus
one chunk after every fired tick.

**Settings** (you set these):

| cvar | default | range | what it does |
|---|:---:|:---:|---|
| `_sofbuddy_cmdpark_reserve_ms` | `0` | 0 – 50 | Move queue aside + park inserts when the tick is within this many ms. `0` = stock |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_cmdpark_cbuf_max` | ms | Worst drain |
| `_sofbuddy_cmdpark_defers` | count | Pre-frame queues moved aside |
| `_sofbuddy_cmdpark_cbuf_cursize` | bytes | Current 8 KB occupancy |
| `_sofbuddy_cmdpark_cbuf_fill_max` | bytes | Peak 8 KB occupancy |

Details: [`src/features/cpu_optimizations/cmdtext_parking/README.md`](src/features/cpu_optimizations/cmdtext_parking/README.md)

---

### Console command cost — `cmd_cost`

Times each `Cmd_ExecuteString` against the sofplus command catalog (from
`sofplus-cursor-rules`). `cmdcost::PredictMs()` is the EMA after four samples
— unused by tick pacing yet.

**Settings** (you set these): `_sofbuddy_cmdcost` (default `1`; set `0` to disable live timing and unwrap).

**Gauges** (read-only; the DLL writes them): `_sofbuddy_cmdcost_max` / `_name`,
`_sofbuddy_cmdcost_ema` / `_ema_name`, `_sofbuddy_cmdcost_n`.

Details: [`src/features/cpu_optimizations/cmd_cost/README.md`](src/features/cpu_optimizations/cmd_cost/README.md)

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

**Settings** (you set these) — all `ARCHIVE`, read live:

| cvar | default | what it does |
|---|:---:|---|
| `_sofbuddy_clamp_notify_ms` | `0` | Log a line when a single clamp deletes at least this many ms. `0` = off (opt-in). At most one line per second |
| `_sofbuddy_clamp_log_file` | `0` | Append every recorded clamp to `sof_buddy-highclamps.txt` (`[timestamp] hostport=<port> <reason>`). `0` = off (opt-in). Independent of strict |
| `_sofbuddy_clamp_window` | `2` | Rolling window in seconds for the average below. Rounded to whole ticks, capped at 16 s |
| `_sofbuddy_clamp_broadcast_ms` | `0` | Tell **all connected players** the server is lagging when the rolling average reaches this. `0` = off. Opt-in, because it is player-visible |
| `_sofbuddy_clamp_broadcast_interval` | `30` | Minimum seconds between those broadcasts |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_highclamps` | count | Highclamp events since boot |
| `_sofbuddy_clamp_avg` | ms | Rolling average of per-tick lost ms |
| `_sofbuddy_clamp_last` | ms | Lost on the most recent tick (`0` if clean) |
| `_sofbuddy_clamp_lost_ms` | ms | Cumulative ms deleted since boot |
| `_sofbuddy_lowclamps` | count | Lowclamp events since boot |
| `_sofbuddy_lowclamp_checks` | count | Frames the lowclamp test ran — denominator for a zero |
| `_sofbuddy_lowclamp_gained_ms` | ms | Cumulative ms the engine invented |
| `_sofbuddy_lowclamp_worst` | ms | Biggest single forward jump |

**Reading a zero.** `_sofbuddy_lowclamps 0` is only meaningful alongside
`_sofbuddy_lowclamp_checks`. That counter counts `SV_Frame`s, not ticks, so on a
live dedicated server it should climb by **hundreds per second**. If it is
stuck at `0`, the measurement is not running and the zero means nothing.

Details: [`src/features/cpu_optimizations/clamp_monitor/README.md`](src/features/cpu_optimizations/clamp_monitor/README.md)

---

### Command buffer inserts — `cbuf_insert`

sofplus scripting calls `Cbuf_InsertText` constantly, and the stock
implementation copies the entire queued buffer out to the heap and back on
every call. This shifts it in place instead.

**Ships on** (`_sofbuddy_cbuf_insert 1`). Measurement still runs in both
states for A/B.

**Settings** (you set these): `_sofbuddy_cbuf_insert` (default `1`).

**Gauges** (read-only; the DLL writes them; at most 10×/second):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_cbuf_insert_us` | µs | Time inside `Cbuf_InsertText`. The A/B number |
| `_sofbuddy_cbuf_insert_bytes` | bytes | Queued text shifted |
| `_sofbuddy_cbuf_insert_max` | bytes | Largest buffer at insert. Small ⇒ skip |
| `_sofbuddy_cbuf_inserts` | count | Total calls |
| `_sofbuddy_cbuf_insert_slow` | count | Fell back to the engine |

Details: [`src/features/cpu_optimizations/cbuf_insert/README.md`](src/features/cpu_optimizations/cbuf_insert/README.md)

---

### Server-side stufftext — `stufftext`

Sends `svc_stufftext` to clients from the server console (or via rcon):

```
stufftext <slot|all|name> <command...>
```

`slot` is the 0-based client slot, `all` hits every connected client, and
anything else matches the first client whose userinfo `name` contains it
(case-insensitive). `command...` is stuffed into the client's console buffer
(a trailing `\n` is appended when missing) via the stock
`WriteByte(svc_stufftext)` + `WriteString` + `unicast` sequence.

**Settings** (you set these): `_sofbuddy_stufftext` (default `1`).

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_stufftext_sent` | count | Clients successfully stuffed |
| `_sofbuddy_stufftext_errors` | count | Failed / rejected invocations |

`stufftext_reconnect <slot>` (strictly numeric, plus `stufftext_reconnect
cancel`) runs a chained reconnect for one slot: attractloop set (read-back
verified) + connect-refusal neutered → stuff `reconnect` → stuff `echo
Welcome to SoF Buddy - enjoy your stay!` once the slot is seen reconnecting
→ refusal and flag restored → stuff `reconnect` to the target when still
present plus any other slot that entered connecting under the lock.

Details: [`src/features/stufftext/README.md`](src/features/stufftext/README.md)

---

### Dictionary hashing — `hash_lookup`

SoF inherits Quake 2's cvars, commands and aliases as **linked lists**. Every
cvar read, every cvar write and every console command walks a list and
`strcmp`s each node. That is fine for a client with a human at the keyboard; it
is not fine for a server running sofplus scripting, which does thousands of
these per tick.

`_sofbuddy_hashmap 1` (the default) turns all three into hash maps. It is
**load-time only** — `+set _sofbuddy_hashmap 0` to skip.

Details: [`src/features/cpu_optimizations/hash_lookup/README.md`](src/features/cpu_optimizations/hash_lookup/README.md)

---

### Reliable staging — `reliable_defer`

On a dedicated server (`maxclients > 1`) the engine's reliable staging buffer is
**1400 bytes** (`msgMaxsize` 1384). Stock coalesces every `bprintf`, stufftext,
and configstring that lands in `client->netchan.message` in the same tick — or
while an earlier reliable is still unacked — into **one** byte stream per UDP
packet. Bursts can overflow staging or crowd out the entity frame.

`reliable_defer` hooks `SZ_Write` and `MSG_WriteByte` / `Short` / `Long` /
`String`, classifies each append, and queues deferred blobs in buddy memory.
One blob drips into `message` per client per tick when the reliable lane is
open. Connect/join traffic (`client->state < cs_spawned`) always passes
through stock. Read-only gauges: `_sofbuddy_reldef_queued`, `_dripped`,
`_dropped`, `_capture_bytes`, `_oldest_wait`.

**Settings** (you set these) — all `ARCHIVE`, read live:

| cvar | default | what it does |
|---|:---:|---|
| `_sofbuddy_reldef` | `1` | Master enable. `0` = stock coalescing |
| `_sofbuddy_reldef_reserve` | `256` | Staging headroom floor (bumped to `msgMaxsize/8` when larger) |
| `_sofbuddy_reldef_frame_reserve` | `0` | Wire headroom for frame+datagram; `0` = `buffersize/2` |
| `_sofbuddy_reldef_one_per_tick` | `0` | Capture every write (fragile; leave off unless tuning) |
| `_sofbuddy_reldef_max_queue` | `32` | Max queued blobs per client |
| `_sofbuddy_reldef_max_queue_bytes` | `262144` | Max queued bytes per client |

Details: [`src/features/reliable_defer/README.md`](src/features/reliable_defer/README.md)

---

### Print bounds — `print_guard`

Bounds engine paths that `vsprintf` into fixed stack buffers (`SV_BroadcastPrintf`,
`PF_cprintf`, macro expand, `Cbuf_Execute` drains). Without it, long SoFPlus
output (e.g. `sp_sv_print_broadcast #~out` after thousands of `sp_sc_cvar_append`)
can smash the stack.

**On when built** — no master switch. Scripter-facing limits:

| cvar | default | SoFPlus command |
|---|:---:|---|
| `_sofbuddy_printguard_broadcast_max` | `999` | `sp_sv_print_broadcast #~cvar` |
| `_sofbuddy_printguard_client_max` | `1000` | `sp_sv_print_client <slot> #~cvar` |

Details: [`src/features/print_guard/README.md`](src/features/print_guard/README.md)

---

### Client frametime tracking — `sv_tracktime`

`usercmd_t.msec` is the client's own claim of how long its frame took, and the
server takes it at face value. `sv_tracktime` compares that claim against the
`clc_move` rate the server actually received, per slot, and reports whether the
average frametime looks real. Measurement only — it never kicks, clamps or
rewrites anything.

```
sv_tracktime                 table for every slot
sv_tracktime <slot>          window + total for one slot
sv_tracktime_reset [slot|all]
```

Output goes straight to the server console — no `developer 1` needed.

```
[tracktime]  sl  name              frames   sum_ms  avg_ms claim_fps  real_fps     drift  verdict
[tracktime]   0  Grim               256      4352  17.00     58.8      59.9     -1.8%  ok
[tracktime]   1  fastguy            256       256   1.00    1000.0      59.9  +1569.4%  MSEC-LOW
```

`MSEC-LOW` = claims more frames/s than it sends, `MSEC-HIGH` = claims fewer,
`no-data` = not enough samples yet.

**Settings** (you set these) — all `ARCHIVE`, read live:

| cvar | default | what it does |
|---|:---:|---|
| `_sofbuddy_tracktime` | `1` | Master switch. `0` stops measuring; last figures stay readable |
| `_sofbuddy_tracktime_tolerance` | `20` | Allowed \|drift\| in percent before a slot is called out (`0` = any deviation) |
| `_sofbuddy_tracktime_window` | `256` | Rolling window in samples (32–256) |
| `_sofbuddy_tracktime_min` | `30` | Samples required in the window before any verdict |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_tracktime_frames` | count | usercmds tracked since boot (spawned slots only) |
| `_sofbuddy_tracktime_suspect` | slots | Slots currently called out |
| `_sofbuddy_tracktime_worst_drift` | % | Worst \|drift\| among them |

Details: [`src/features/sv_tracktime/README.md`](src/features/sv_tracktime/README.md)

---

## Recipes

**Is my server actually keeping up?**

```
_sofbuddy_highclamps          should stay 0
_sofbuddy_clamp_avg           should stay 0.00
_sofbuddy_cmdpark_cbuf_max    how bad the worst drain got
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

**Log clamp events to the server console** (opt-in):

```
set _sofbuddy_clamp_notify_ms 5
```

**Clients fail to join with `Illegible server message` after enabling buddy**

First try a full server restart after deploying a new `gamex86.dll`. If it
persists, disable deferral live and reconnect:

```
set _sofbuddy_reldef 0
```

That restores stock reliable coalescing while you tune queue limits or report a
bug. Connect/join should already bypass deferral; this is the blunt fallback.

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
tools/tests/cmdtext_parking/run.sh
tools/tests/clamp_monitor/run.sh
tools/tests/cbuf_insert/run.sh
tools/tests/cmd_cost/run.sh
tools/tests/reliable_defer/run.sh
tools/tests/print_guard/run.sh
tools/tests/tictactoe/run.sh
tools/tests/sv_tracktime/run.sh

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
