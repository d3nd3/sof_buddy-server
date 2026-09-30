# sv_tracktime — is each client's reported frametime real?

Every `usercmd_t` a client sends arrives as `SV_ClientThink(cl, cmd)` from
`clc_move`, and `cmd->msec` is the client's **own claim** of how long its last
frame took (`q_shared.h`: `byte msec`). The server takes that byte at face
value — client-side prediction, movement and every stat readout downstream are
built on it — and nothing checks it against the clock. A client that wants its
frametime to read 1 ms can send `msec = 1` while still running at 60 fps.

This feature measures the disagreement and reports it. It is **measurement
only**: nothing is kicked, clamped, rewritten or broadcast.

## What it measures

Per slot, two numbers that have to agree:

| | formula | meaning |
|---|---|---|
| `claim_fps` | `1000 * frames / sum(msec)` | rate implied by the client's own bytes |
| `real_fps` | `1000 * (frames-1) / span` | ClientThink rate the server actually received |
| `drift` | `(claim_fps - real_fps) / real_fps * 100` | disagreement |

`frames` is the number of `clc_move` usercmds the server processed for that
slot; `span` is wall-clock time between them. When `drift` is near zero the two
agree, so the client's average frametime is not invented.

Verdicts:

| verdict | meaning |
|---|---|
| `ok` | reported msec matches the rate the client is really sending |
| `MSEC-LOW` | claims **more** frames/s than it sends — faked fast (`msec=1` at 60 fps) |
| `MSEC-HIGH` | claims **fewer** frames/s than it sends — faked slow (`msec=250` at 60 fps) |
| `no-data` | fewer than `_sofbuddy_tracktime_min` samples in the window |

`msec = 0` on every frame is the limit of an understated frametime and is
reported as `MSEC-LOW` with a sentinel drift of `+1000%` (there is no finite
claimed rate to compare).

## Window vs total

Each slot keeps a rolling window of the last 256 samples (≈4 s at 60 fps) plus
counters for the whole connection:

- **window** — what the client is doing *now*. This is the verdict, the table
  and the gauges.
- **total** — everything since the connection started. A cheat switched on
  halfway through a map shows up in the window immediately and only slowly in
  the total; a client that was honest all along never flags.

Silences of 2 s or more (alt-tab, reconnect, dead link) are counted as *stalls*
and excluded from both averages — the cumulative span skips the time, and the
window restarts after it. A stall is not a rate.

## Commands

| Command | Action |
|---|---|
| `sv_tracktime` | Table for **every** slot, 0-based, spawned or not |
| `sv_tracktime <slot>` | Window + total for one slot, plus msec extremes, stalls, idle time |
| `sv_tracktime_reset [<slot>\|all]` | Clear the data (all slots when the argument is omitted) |

Output prints straight to the server console (`Com_Printf`) — no
`developer 1` needed, unlike the `gi.dprintf` diagnostics most features log.

```
[tracktime] window 256 samples, min 30, tolerance 20%
[tracktime]  sl  name              frames   sum_ms  avg_ms claim_fps  real_fps     drift  verdict
[tracktime]   0  Grim               256      4352  17.00     58.8      59.9     -1.8%  ok
[tracktime]   1  fastguy            256       256   1.00    1000.0      59.9  +1569.4%  MSEC-LOW
[tracktime]   2  -                    0         0   0.00      0.0       0.0     +0.0%  no-data
[tracktime] 1/3 slots with data, 512 frames in window, 4096 since boot
[tracktime] suspect 1, worst drift 1569.4%
```

## Cvars

**Settings** (you set these) — all `ARCHIVE`, read live:

| cvar | default | what it does |
|---|:---:|---|
| `_sofbuddy_tracktime` | `1` | Master switch. `0` stops measuring; the last figures stay readable |
| `_sofbuddy_tracktime_tolerance` | `20` | Allowed \|drift\| in percent before a slot is called out (`0` = any deviation) |
| `_sofbuddy_tracktime_window` | `256` | Rolling window in samples (32–256) |
| `_sofbuddy_tracktime_min` | `30` | Samples required in the window before any verdict |

**Gauges** (read-only; the DLL writes them, at most 2×/second):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_tracktime_frames` | count | usercmds tracked since boot (spawned slots only) |
| `_sofbuddy_tracktime_suspect` | slots | Slots currently flagged `MSEC-LOW`/`MSEC-HIGH` |
| `_sofbuddy_tracktime_worst_drift` | % | Worst \|drift\| among the flagged slots |

## What this cannot see

- **Only what reached the server.** If the link drops `clc_move` packets,
  `real_fps` falls and an honest client drifts *high* (`MSEC-HIGH`). A flagged
  slot is a reason to look, not a conviction.
- **Spawned slots only.** Samples are taken while `client_t.state ==
  cs_spawned`, so connect/download time is not counted.
- **Cinematics and pauses.** The engine skips `ge->ClientThink` during a
  cinematic freeze; that client simply produces fewer samples.
- **Per-frame clamping on the client.** A client that reports a plausible
  `msec` but renders at a different rate than it sends (for example capped
  sends) is not what this measures — it compares claimed time against *sent
  commands*, not against rendered frames.

## Timing notes

Arrival times come from `QueryPerformanceCounter`, not `GetTickCount`: the
latter advances in ~15.6 ms steps, the same order as the frame gap being
measured. Gaps are kept in microseconds.

Per sample the feature does a slot-pointer lookup, two `VirtualQuery`
validations (client state, userinfo) and a 48-byte userinfo hash — that hash is
what notices a reconnect or a different player in the same slot and starts a
fresh measurement instead of averaging two sessions together (a mid-game name
change restarts it too).

Tests: `tools/tests/sv_tracktime/run.sh` (host-side, drives the tracker maths
in `sv_tracktime_logic.h` with synthetic arrivals).
