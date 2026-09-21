# cpu_optimizations

Keep a sofplus dedicated server on its **100 ms tick** without growing the
engine's 8 KB `cmd_text` or splitting a drain with `cmd_wait`.

This folder is one build feature (`cpu_optimizations` in
`src/features/features.yaml`). Each subdirectory is a module with its own
cvars. Runtime switches stay per-module so you can A/B without a rebuild.
`_sofbuddy_cpuopt 0` turns **all** runtime modules off at once (live, except
`qpc_timer`, which stays independent so the clock never jumps; hashmap/zpool
only skip install at `GameDllLoaded`).

`stufftext` stays a separate feature (admin delivery, not tick/CPU policy).

## Constraints

- `cmd_text` stays **8192 bytes**. Park overflow beside the engine
  (`cmdtext_parking`), do not retarget `data`.
- Drains stay **atomic**. Do not use `cmd_wait`. `cmdtext_parking` is the only
  `Cbuf_Execute` override; other modules hook Pre/Post or `Cbuf_AddText`.
- `zpool` is compiled here; **`_sofbuddy_zpool` defaults to 1** (load-time).

## Modules

| Module | Role | Default |
|---|---|---|
| `clamp_monitor` | highclamp / lowclamp gauges | always on when built |
| `cmd_cost` | per-handler timing (lazy wrap) + spin; `sofbuddy_cmdcost_addons` static `.func` model; `sofbuddy_cmdcost_{events,usercmds,timers}` live sofplus introspection | `_sofbuddy_cmdcost 1` |
| `tick_pacing` | skip Sleep(1) when a drain hid the tick boundary; straddle settle | `_sofbuddy_tickpace 1` |
| `qpc_timer` | QPC `Sys_Milliseconds` (independent of cpuopt/tickpace) | `_sofbuddy_qpc 1` |
| `cmdtext_parking` | park all `cmd_text` inserts in the reserve window; drip after the tick | `_sofbuddy_cmdpark 1` |
| `hash_lookup` | hash maps for cvar / command / alias lists | `_sofbuddy_hashmap 1` (load-time) |
| `cbuf_insert` | in-place `Cbuf_InsertText` instead of zone round-trip | `_sofbuddy_cbuf_insert 1` |
| `zpool` | zone allocator recycle | `_sofbuddy_zpool 1` (load-time) |

Module READMEs in each subdirectory are the implementation notes. Gauges
live there and in the root README; this table is **settings only**.

## Settings

**Folder-wide** (you set these):

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_cpuopt` | 1 | live | Master. `0` disables every module below (hashmap/zpool only at `GameDllLoaded`) |
| `_sofbuddy_cmdpark_strict` | 1 | live | Tick over console: park all inter-tick inserts, move every pre-frame queue aside, run everything post-tick. **Highclamp** / **lowclamp** violations log one console line |
| `_sofbuddy_strict_exit` | 0 | live | Quit on a strict violation instead of logging and continuing |

**`tick_pacing`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_tickpace` | 1 | live | Drain-boundary skip, settle, spin skip. `0` = those off. Also off if `_sofbuddy_cpuopt 0`. Park backlog skip is separate (`cmdtext_parking`) |
| `_sofbuddy_tickpace_settle` | 1 | live | With tickpace on: credit `untilAfterMsec` on straddle when `Sys_Milliseconds` has also advanced by that amount. `0` = skip-sleep only |
| `_sofbuddy_tickpace_spin_ms` | 2 | live | With tickpace on: dedicated only. Skip WinMain's `Sleep(1)` if the tick is due within this many ms. Cap 20. `0` = off |

**`qpc_timer`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_qpc` | 1 | live | QPC `Sys_Milliseconds`. `0` = stock `timeGetTime`. Independent of `_sofbuddy_cpuopt` / `_sofbuddy_tickpace` |

**`cmdtext_parking`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_cmdpark` | 1 | live | Park inserts in the reserve window; drip after the tick |
| `_sofbuddy_cmdpark_reserve_ms` | 0 | live | ms cushion before the tick. `0` = stock inserts |

**`cmd_cost`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_cmdcost` | 1 | live | Handler timing when `1` (wrap on demand). `0` = no patch. Console: `sofbuddy_cmdcost_{dump,reset,wrap,spin,spinall,slots,addons,events,usercmds,timers}` (aliases: `events`, `usercmds`, `timers`) |

**`clamp_monitor`** (always measuring when built)

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_clamp_notify_ms` | 0 | live | Shim log when a clamp deletes at least this many ms. `0` = off. At most one line per second |
| `_sofbuddy_clamp_window` | 2 | live | Rolling window (s) for the average / broadcast. Whole ticks, cap 16 s |
| `_sofbuddy_clamp_broadcast_ms` | 0 | live | Broadcast lag to all clients when the rolling average reaches this. `0` = off |
| `_sofbuddy_clamp_broadcast_interval` | 30 | live | Minimum seconds between those broadcasts |

**`cbuf_insert`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_cbuf_insert` | 1 | live | In-place `Cbuf_InsertText`. `0` = engine path (still measured) |

**`hash_lookup`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_hashmap` | 1 | load-time | Hash maps for cvar / command / alias lists. `+set 0` before first map to skip |

**`zpool`**

| cvar | default | when | what it does |
|---|---|---|---|
| `_sofbuddy_zpool` | 1 | load-time | Recycle zone allocations. `+set 0` before first map to skip |

## Plan

Work in this order. Do not skip measurement.

1. **Prove the clock.** `_sofbuddy_lowclamp_checks` must climb (hundreds per
   second on dedicated). `_sofbuddy_highclamps` / `_sofbuddy_clamp_avg` are
   the starvation signal. `_sofbuddy_cmdpark_cbuf_max` is the worst drain;
   `_sofbuddy_cmdcost_max` / `_name` is the worst single command.
2. **Protect the tick.** Leave `_sofbuddy_tickpace 1`. Raise `reserve_ms`
   from 0 only while `cbuf_max` is small (under ~10 ms).
   A 30 ms drain cannot be scheduled off a 100 ms boundary.
3. **Park inserts near the tick.** `_sofbuddy_cmdpark 1`. Client `.COMMAND` /
   stringcmd wait until after the tick. Strict parks every inter-tick insert.
4. **Cut dictionary cost.** `_sofbuddy_hashmap 1` by default (load-time).
   sofplus does thousands of list walks per tick; this is the usual win after
   pacing. `+set 0` to skip.
5. **A/B insert.** `_sofbuddy_cbuf_insert 1` by default. Compare
   `_sofbuddy_cbuf_insert_us` against a `0` run if you doubt it.
6. **zpool** defaults on (`_sofbuddy_zpool 1`, load-time). Prior bench was
   flat; `+set 0` if you want the old allocator.
7. **Next: predicted drain.** Feed `cmdcost::PredictMs()` into
   `tick_pacing`'s reserve so a known-heavy queue holds *before* it blows the
   boundary — still one atomic drain, still 8 KB `cmd_text`. Occupancy bytes
   are not CPU.

## Reading the board

Console: `sofbuddy_cpuopt_status` prints every gauge and master switch below
in one place, live. Tunables that are set but doing nothing say why
(`idle: dedicated-only`, `window off`, `ignored: strict never gives up`).

**Gauges** (read-only; the DLL writes them):

```
_sofbuddy_highclamps            stay 0
_sofbuddy_clamp_avg             stay 0.00
_sofbuddy_lowclamp_checks       must climb
_sofbuddy_cmdpark_cbuf_max      worst drain (ms)
_sofbuddy_tickpace_saved        drain hid the boundary; next Sleep skipped
_sofbuddy_cmdpark_defers        pre-frame queues moved aside
_sofbuddy_cmdcost_max / _name   slowest single command this boot
```

A rising `cbuf_max` with a flat `cmdcost_max` usually means a long unroll
(`while` / aliases), not one expensive builtin. Fix the script; do not enlarge
the buffer.
