# clamp_monitor

Measures server-tick starvation. The engine's `SV_RunGameFrame` advances
`sv_time = framenum*100`, calls the game DLL's `RunFrame`, then immediately
clamps: `if (sv_time < svs_realtime) { svs_realtime = sv_time; }` — the
difference is **deleted permanently** (the server could not keep up).

## Detection: hook `G_RunFrame`, not polling

Detection hooks `G_RunFrame` (`detours.yaml`, `hooks/hooks.json`,
`override: true`) and reads `sv_time`/`svs_realtime` the instant the real game
frame returns. Our hook body runs *before* control gets back to
`SV_RunGameFrame`, so nothing has touched either global since the frame ended
and these are exactly the pre-clamp values the engine's own check is about to
use — an exact, per-event replica of its decision: no polling gaps, no
inference.

The engine's test is an **unsigned** compare (`cmp edx, eax` / `jnb`, IDA
`0x2005f488`), and this feature matches it.

**Why not just poll `svs_realtime` from a background thread?** Earlier
versions of this feature did exactly that, and it doesn't work: the clamp
resets `svs_realtime` to the *new* `sv_time`, which is always higher than any
value an external poll saw before, because `sv_time` itself keeps climbing
100ms every tick. So from outside, `svs_realtime` looks like it only ever
grows — a poll can only ever catch the rare clamp big enough to still read as
a net decrease across a whole sample interval; it's blind to the routine case
of a clamp firing on nearly every tick. Only observing the value at the exact
instant the engine itself checks it (i.e. hooking) is reliable.

There is **no background thread at all** any more. The hook pushes one exact
sample per real tick (0 on non-clamp ticks) into the rolling-average ring,
recomputes the average, and publishes every output cvar — all synchronously on
the engine's own thread, in the same call. An earlier revision kept a thread
around purely to flush cvars once a second; that bought nothing, raced this
file's own state, and had `WaitForSingleObject` on the DllMain detach path.

## Which `G_RunFrame` calls count as a tick

`G_RunFrame` is **not** only called from `SV_RunGameFrame`, and treating every
call as a server tick is what made this feature misreport:

- `SpawnServer` calls it **twice at `ss_loading`** ("run two frames to allow
  everything to settle"), with `sv.framenum` still 0.
- `SV_RunGameFrame`'s cinematic/freeze loop can call it repeatedly for one
  `sv.framenum`.
- Worst of all, the **first real tick of every map** reports a huge bogus
  clamp. `SpawnServer` wipes `sv` (so `sv.framenum` is 0) but then sets
  `sv.time = 1000`, and zeroes `svs.realtime`. The first `SV_RunGameFrame`
  drops `sv.time` back to 100 while `svs.realtime` has meanwhile absorbed the
  entire map-load wall time — so the engine deletes ~900 ms *plus the whole
  load duration* in a single clamp.

Left ungated, that one event added a multi-second entry to
`_sofbuddy_clamp_lost_ms` and `_sofbuddy_highclamps` on every single map
change, logged a bogus `highclamp: 3900 ms deleted`, and — because it alone
dominates a 2-second rolling window — **broadcast a false "server lag" notice
to every player after every map change.**

So a call is only sampled when:

1. `sv.state == ss_game` (2) — excludes the `ss_loading` settle frames, and
2. `sv.framenum` advanced by exactly one since the last call we saw — excludes
   repeated/cinematic frames and detects a map restart, and
3. at least `kSettleTicks` (2) qualifying ticks have passed since the last
   discontinuity — which is what drops the map-load clamp itself.

A discontinuity also **clears the rolling window**: samples from the previous
map must not be averaged into the new one. The cumulative counters
(`_sofbuddy_highclamps`, `_sofbuddy_clamp_lost_ms`) are since-boot and
deliberately survive map changes.

## Interaction with SoFPlus

SoFPlus also hooks `G_RunFrame` — confirmed by reading the `sof-plus-plus-nix`
source (a SoFPlus reimplementation): `orig_G_RunFrame = createDetour(game_exports->RunFrame,
my_G_RunFrame, 5)` (`src/core/game.cpp`), where `createDetour` (`src/util.cpp`)
`memcpy`s whatever bytes are *currently live* at the target address into a
thunk before patching a JMP over them — i.e. it preserves whatever is already
installed there (stock code, or another mod's redirect) as "the original" to
call through, rather than assuming a fixed byte signature. It does **not**
touch the `game_exports->RunFrame` field itself, only the code bytes at the
address that field already points to.

Our detour (`third_party/DetourXS`, same `RegisterDetour` path every hooked
`GameDll` function in this codebase uses) does the identical thing at the
identical address (`G_RunFrame`'s RVA in `oldgamex86.dll` - the same address
`game_exports->RunFrame` holds, since the engine calls through that field).
That's why the two compose correctly regardless of load order: whichever
patches second wraps around the first's JMP as "the original" to call
through. (An earlier revision of this file claimed the IDA instance nicknamed
`spsv.dll` was "an unrelated binary - base `0x10000000`, no `GetGameAPI`
export, no SoF strings, ~500 functions - not SoFPlus". **That was wrong.**
Opened properly it is 583 functions at base `0x10000000` and is unmistakably
SoFPlus: `sp_sc_alias`, `sp_sc_timer`, `sp_sc_cvar_*`, `sp_sc_flow_if`,
`sp_sc_func_exec`, `sofplusScriptEventDispatcher`. The base address and
function count that were offered as evidence *against* it are simply what
SoFPlus looks like. It also carries `G_RunFrame_HOOK` @ `0x10011F70`, which
confirms this section's conclusion directly in the shipped binary rather than
by inference from a reimplementation's source - the `sof-plus-plus-nix`
reading above happened to be right, but it was never the strongest evidence
available.)

Do **not** intercept `G_RunFrame` by overwriting `game_export_t::RunFrame`
directly (a pointer-table swap) instead of going through `RegisterDetour` —
that style doesn't compose with another mod doing the same thing; whichever
one patches last silently wins and the other's hook is lost.

## Why cvar outputs are hand-poked, not `gi.cvar_setvalue`

Engine cvar API calls (`gi.cvar_setvalue`) are documented to crash on some
Wine builds when made from inside frame-path hooks (see `buddy_import.h`), and
every output here is published from the `G_RunFrame` hook. So `cvar.cpp`
writes the `cvar_t` fields directly.

The layout is **not** stock Quake 2's — verified in IDA against
`Cvar_Set2 @ 0x20021d70`:

| Offset | Field |
|---|---|
| `+0x00` | `name` |
| `+0x04` | `string` |
| `+0x08` | `latched_string` |
| `+0x0C` | `flags` (1 = ARCHIVE, 8 = NOSET) |
| `+0x14` | `modified` |
| `+0x18` | `value` |
| `+0x1C` | `next` |

Three things this costs us, all handled in `cvar.cpp`:

- **`string` must be written too.** The engine's console `"name" is "value"`
  print reads `cvar_t.string`, not `.value`; poking only the float leaves the
  console showing `0` forever.
- **`string` is double-buffered.** Each output owns two buffers; a publish
  formats into the spare one and then swaps the pointer with a single aligned
  32-bit store, so a reader can never observe a half-written string.
- **`string` is handed back on detach.** `ClampMonitor_Shutdown()` (called from
  `DllMain`'s `DLL_PROCESS_DETACH`) restores the engine-allocated pointer that
  was displaced at bind time. This is mandatory, not tidiness: spsv
  `FreeLibrary`/reloads this DLL between game restarts, and the engine keeps
  the `cvar_t` for the life of the process — a `cvar_t.string` left pointing
  into an unmapped image is a crash waiting for the next `cvarlist`. Worse,
  `Cvar_Set2` does `Z_Free(var->string)`, so a forced set would hand our static
  buffer to the engine's allocator.

Counters are formatted as exact integers by a small hand-rolled 64-bit
routine, deliberately not `snprintf("%lld")` (this DLL builds against msvcrt,
whose `printf` does not reliably understand the `ll` length modifier). The
previous `"%g"` printed anything past ~1e6 as `1.23457e+06`. Note `cvar_t`
only has a **float** `value`, so the numeric half of a counter still loses
precision past 2^24 — the string is the exact one.

`PrintOut` (this feature's own logger) is safe to call from the hook — it never
calls back into the engine (see `log.cpp`). `gi.bprintf` is likewise called
directly from the hook (for the player broadcast below); stock game code calls
it constantly from this exact frame-path context (e.g. `p_client.cpp`'s
disconnect message, verified against the `sofree` reference source), so it
doesn't carry the same caution as `gi.cvar_setvalue`.

## Root cause of an earlier crash: game_import_t is transient, snapshot it

The broadcast feature crashed (page fault, garbage function pointer) the
first time it actually fired, hours after server boot. Root cause: `game_import_t
*import` handed to `GetGameAPI` is only valid for the duration of that one
call - it's a transient struct on the engine's side, not something that stays
alive for the server's lifetime. Every stock game DLL knows this and
immediately snapshots it: the SDK source (`reference/engine/gamecpp/g_main.cpp`)
does `gi = *import;` into its own persistent global `game_import_t gi;`
(a value, not a pointer), and the compiled retail `GetGameAPI` does the
equivalent `qmemcpy(&GameImport_t, a1, 0x18Cu)` (verified in IDA on
`gamex86.dll`). `src/buddy_import.cpp` used to just cache the raw `import`
pointer (`g_gi = import;`). Cvar creation at bootstrap (`ClampMonitor_InitCvars`,
called from inside that same `GetGameAPI` call) kept working because that
stack region hadn't been reused yet; a broadcast fired thousands of ticks
later read long-since-reused stack memory as if it were still the import
table, producing a garbage `gi.bprintf` pointer and crashing on the call.
Fixed by copying the whole `0x18C`-byte struct into a persistent buffer in
`Buddy_BindGameImport`, matching the stock game DLL's own pattern exactly.
`Buddy_BroadcastPrintf` also now validates the resolved function pointer is
executable memory before calling through it (`VirtualQuery`), so a similar
bug in the future fails safely (logged) instead of crashing the server.

## Player-visible lag broadcast

When `_sofbuddy_clamp_broadcast_ms` is nonzero (default `0`, i.e. off) and the
rolling average reaches that threshold, the hook broadcasts a lag notice to
all clients via `gi.bprintf`, rate-limited to at most once per
`_sofbuddy_clamp_broadcast_interval` seconds. The average is recomputed from
the sample ring on every tick, so it reacts within one rolling window. The
check runs only on ticks that actually clamped, so a decaying tail after the
lag has passed can't trigger a fresh notice.

This replaces the sofplus-script approach below for servers that would rather
have it built in; the sofplus route still works and needs no native cvar.

**Print level: `PRINT_CHAT` (3), not `PRINT_HIGH`.** The engine's actual
`SV_BroadcastPrintf` (IDA `0x200618d0` in `SoF.exe`, matches `sofree`'s
hardcoded `orig_bprintf` address exactly) filters per connected client: for
each one it does `if (printlevel < cl->messagelevel) skip this client` before
writing `svc_print`. `PRINT_CHAT` is the highest level this engine defines, so
it's the only level guaranteed not to be filtered by a client's own
message-level preference, whatever that's set to. A broadcast sent at
`PRINT_HIGH` can be silently dropped for clients with a stricter filter — this
is the leading suspect for "threshold was reached but no client saw anything."
The shim also logs `[clamp_monitor] broadcasting lag notice: ...` (`PrintOut`,
server-side only) right before the `gi.bprintf` call, so the server
console/log tells you whether the threshold ever fired at all, independent of
whether any specific client displayed the message — check that line first when
debugging "no message showed up."

## Cvars

Output cvars (NOSET - the engine won't let a user `set` these from the
console; they're only ever written by `ClampMonitor_SetOutputs`, once per
server tick, from the hook's own sample history):

| Cvar | Default | Meaning |
|---|---|---|
| `_sofbuddy_highclamps` | 0 | Exact count of "sv highclamp" events since boot (hook-driven, see Detection above) |
| `_sofbuddy_clamp_avg` | 0 | Rolling average, over `_sofbuddy_clamp_window` seconds, of exact per-tick lost ms (0 on non-clamp ticks) |
| `_sofbuddy_clamp_last` | 0 | Exact lost ms on the most recent tick (0 if that tick had no clamp) |
| `_sofbuddy_clamp_lost_ms` | 0 | Cumulative total ms deleted by clamps since boot |
| `_sofbuddy_lowclamps` | 0 | Exact count of "sv lowclamp" events since boot (see Lowclamp below) |
| `_sofbuddy_lowclamp_checks` | 0 | `SV_Frame`s on which the lowclamp test was actually evaluated — the denominator that makes a zero above mean *measured* zero |
| `_sofbuddy_lowclamp_gained_ms` | 0 | Cumulative ms the engine invented jumping `svs.realtime` forward |
| `_sofbuddy_lowclamp_worst` | 0 | Biggest single forward jump, in ms |

Tunable cvars (ARCHIVE, settable from the console):

| Cvar | Default | Meaning |
|---|---|---|
| `_sofbuddy_clamp_notify_ms` | 5 | Log threshold (ms) for a single clamp event to print `[clamp_monitor] highclamp: ... ms deleted` to the shim log (server-side only, at most one line per second) |
| `_sofbuddy_clamp_window` | 2 | Rolling window, in seconds, for `_sofbuddy_clamp_avg` and the broadcast check. Rounded to whole 100ms ticks and capped at 16s (the ring buffer holds 160 samples) |
| `_sofbuddy_clamp_broadcast_ms` | 0 | Rolling-average threshold (ms) to broadcast a lag notice to all clients; `0` = disabled (opt-in, since this is player-visible) |
| `_sofbuddy_clamp_broadcast_interval` | 30 | Minimum seconds between broadcast notices |

## Lowclamp: a second hook, on `SV_ReadPackets`

Highclamp and lowclamp are opposite failures and cannot share an anchor.

* **highclamp** deletes time the server owed and could not deliver — it fell
  behind. It happens inside `SV_RunGameFrame`, so the `G_RunFrame` hook sees it.
* **lowclamp** is the reverse: the engine finds the server clock more than a
  whole tick *behind* game time and jumps it forward, inventing the difference.
  It happens on `SV_Frame`'s early-return branch at `0x2005F617`, which returns
  **without ever calling `SV_RunGameFrame`** — so the `G_RunFrame` hook can
  never see a lowclamp, no matter how many there are.

The anchor for it is a Post hook on `SV_ReadPackets @0x2005F100`. That function
is called from `SV_Frame @0x2005F5EF` and from nowhere else (verified: one
caller), after the `svs.realtime += msec` at `0x2005F5E2` and before the
lowclamp test at `0x2005F60A`. Returning from it puts the hook at exactly the
instant the engine is about to evaluate `sv.time - svs.realtime > 100`, on the
same two globals, with nothing able to perturb them in between — the same
standard of exactness the `G_RunFrame` hook meets for highclamp.

Two details the test suite pins:

* **The boundary is strict.** `cmp edx, 64h / jbe` (`0x2005F61D`) means a
  deficit of exactly 100 returns without clamping, and 101 clamps. A tick that
  fires precisely on its boundary leaves exactly 100 *every single time*, so an
  off-by-one here would report a clamp on every well-behaved tick.
* **Map changes are skipped.** `SpawnServer` leaves `sv.time` at 1000 and
  `svs.realtime` at 0, so the first `SV_Frame` of every map is ~900ms behind and
  lowclamps exactly once. That is map loading, not starvation, and it is skipped
  by the same settle window the highclamp side already uses.

### Reading a zero

`_sofbuddy_lowclamps 0` on its own is unfalsifiable — it reads the same whether
the test ran a million times and never fired or the hook never fired at all. So
`_sofbuddy_lowclamp_checks` counts the frames actually tested, and both are
republished once per tick from the `G_RunFrame` path, not only when a clamp
happens. A counter written only when something goes wrong cannot be
distinguished from a counter that is never written.

`_sofbuddy_lowclamp_checks` counts `SV_Frame`s, not ticks, so on a live
dedicated server it should climb by **hundreds per second**, not ten. If it is
climbing at roughly the tick rate, the `SV_ReadPackets` hook is not attached and
the sample is coming from somewhere else; if it is not climbing at all, the
feature is not running.

### Reading the number

The stock engine cannot reach a lowclamp from the tick path at all: a tick only
fires once `svs.realtime` has caught `sv.time`, which leaves a deficit of at
most exactly 100, and the test is strict. So on a running map, away from map
changes, `_sofbuddy_lowclamps` counting up means something moved `sv.time`
forward or `svs.realtime` backward outside that path. It is a "something is
wrong" counter, not a load gauge — unlike `_sofbuddy_highclamps`, which is
exactly a load gauge.

## Addresses

SoF-spsv.exe / SoF.exe share engine `.text`; base `0x20000000`. All three
globals verified by cross-reference in IDA (`SpawnServer` writes all of them).

- `sv.state`     = base + `0x3A1F20` (`ss_dead` 0, `ss_loading` 1, `ss_game` 2)
- `sv.attractloop` = base + `0x3A1F24` (not read here; listed so the three
  neighbours aren't confused with each other)
- `sv.time`      = base + `0x3A1F28`
- `svs.initialized` = base + `0x396DE0` (`SV_Frame`'s own precondition at
  `0x2005F5BF`; adjacent to `svs.realtime`, the first two fields of
  `server_static_t` exactly as in stock Quake 2)
- `svs.realtime` = base + `0x396DE4`
- Anchors: `SV_Frame @ 0x2005F5B0` (accumulate + lowclamp),
  `SV_ReadPackets @ 0x2005F100` (hooked Post, for the lowclamp sample),
  `SV_RunGameFrame @ 0x2005F3F0` (advance time, RunFrame, highclamp),
  `SpawnServer @ 0x2005D790` (wipes `sv`, `sv.time = 1000`, two settle frames)
- `G_RunFrame` (hooked) = `oldgamex86.dll` base `0x50000000` + `0xAE340`,
  `float __cdecl G_RunFrame(int serverframe)` (verified in IDA; matches
  `reference/engine/gamecpp/game.h`'s `game_export_t::RunFrame`, and the
  engine calls it through `gamedllexport + 0x5C`)

⚠ Do not confuse `sv.time`/`svs.realtime` with the look-alike pair at
`0x1E8FC4/0x1E7E68` used by an unrelated dedicated-mode limiter near
`0x20004460` — that pair stays zero on dedicated servers and produced this
feature's first false constants.

## Player alerts via sofplus (alternative to the native broadcast above)

```sofplus
// example: warn players when recent average lag exceeds 10ms
if (_sofbuddy_clamp_avg >= 10) (
    sp_bprintf 2 "_sofbuddy" "server lag detected: " _sofbuddy_clamp_avg "ms avg"
)
```

Run from a sofplus timer - printing from that context is safe. Use this
instead of `_sofbuddy_clamp_broadcast_ms` if you want custom wording/logic;
use the native cvar if you just want it built in with no extra script.

## Verification harness

`tools/tests/clamp_monitor/run.sh` builds and runs the feature's two
translation units on the host (32-bit, so the `cvar_t` offsets line up)
against stub `<windows.h>`/detour headers and a transcription of the engine's
`SV_Frame` / `SV_RunGameFrame` / `SpawnServer`. No server and no Wine needed.
It covers the map-change spike, `ss_loading` frames, sustained overload,
window decay and sizing, exact counter formatting, detach-time string restore,
broadcast threshold and rate limit, the notify threshold, and the freeze case
below.

On a real server:

Freeze test (deterministic clamp): `kill -STOP $(pgrep -n SoF-spsv.exe)` for
~1s, then `kill -CONT`. Expect exactly one
`[clamp_monitor] highclamp: ~900 ms deleted` line and `_sofbuddy_highclamps`
+1.

Continuous-overload test: put the server under sustained CPU starvation.
Expect `_sofbuddy_highclamps` to climb roughly once per tick for as long as
the overload lasts (not stay frozen after the first event) - this is the
case the old polling approach silently failed to detect.

Map-change test: run `map` a few times on an idle server. `_sofbuddy_highclamps`
and `_sofbuddy_clamp_lost_ms` must stay put, and no lag notice may be
broadcast. Before the gating above, each map change added one clamp of several
seconds and (with `_sofbuddy_clamp_broadcast_ms` set) told every player the
server was lagging.
