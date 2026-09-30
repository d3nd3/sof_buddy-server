# cmdtext_parking

Park **every** `cmd_text` insert that arrives within `reserve_ms` of a server
tick, and move the queued buffer aside instead of running it near the
boundary: console input, `.COMMAND`, `clc_stringcmd`, sofplus timers,
`Cbuf_AddText`, `Cbuf_InsertText`, and `Cbuf_ExecuteText` EXEC_INSERT /
EXEC_APPEND. EXEC_NOW still runs immediately (it is not an insert).
One store (a heap side buffer), one drip: moved and parked bytes all come back
through `DripOne` — one newline-bounded chunk per safe sub-tick, plus one
chunk after every fired tick (that post-tick slot is what keeps a
permanently-behind server draining). Under strict there are no safe
sub-ticks: every pre-frame queue moves aside and everything runs post-tick.

`tick_pacing` handles drain-boundary skip; this module arms the park-backlog
Sleep skip when strict or `reserve_ms` is on.

## Behaviour

- **`_sofbuddy_cmdpark_reserve_ms`** (default 0): strict-style protection
  (park arrivals, move the queued buffer aside) applied when the tick is
  within this many ms. Pure X — `0` = off. Larger ducks earlier at the cost
  of a few ms of script latency.
- Client text from `SV_ReadPackets` is parked until after the tick
  (then dripped). That is `.COMMAND` / stringcmd. A `map` / `gamemap`
  command (SoFPlus `;map @real@ …` included) stays in `cmd_text`, and so
  does the rest of that `SV_ReadPackets`, so the next drain starts the
  level change instead of dripping it one chunk per tick.
- **`_sofbuddy_cmdpark_strict`**: park all inter-tick inserts (not during
  `SV_Frame`), move every pre-frame queue aside, run everything post-tick. No
  between-tick execution at all. Clamp violations log one console line in
  clamp_monitor (history opt-in via `_sofbuddy_clamp_log_file`). Set
  `_sofbuddy_strict_exit 1` to quit on a violation instead.
- After the tick, parked text is dripped **one newline-bounded chunk per
  sub-tick** (≤ 4096 bytes, must fit in 8 KB `cmd_text`).
- While parking is armed and the side store is non-empty, `tick_pacing` may skip
  WinMain's `Sleep(1)` only when a game tick is due within the spin window
  (`_sofbuddy_tickpace_spin_ms`). Strict mode drips parked bytes on the
  post-tick slot only — skipping sleep with a large backlog but no tick due
  would spin the main loop at 100% without draining (connect storms hit this).

**Parked vs moved aside** — two timings, one store:
- *Parked* — one insert (`Cbuf_AddText` / `InsertText` / `ExecuteText`
  insert+append) diverted to a side buffer instead of `cmd_text`. Happens line
  by line, whenever the insert arrives near the tick (or anywhere outside
  `SV_Frame` under strict).
- *Moved aside* (`_sofbuddy_cmdpark_defers`) — the whole queued buffer
  appended to that same side buffer instead of running on top of the boundary.
  Happens at most once per pre-frame drain, inside the same X-ms window (or
  once already due under strict without reserve).
- Both come back through the same drip: one newline-bounded chunk per safe
  sub-tick, plus one chunk after every fired tick. A quiet server sits at 0
  defers while parking every timer, and that's healthy — there was simply
  never a queue worth moving.

## Cvars

**Settings** (you set these):

| cvar | default | what it does |
|---|---|---|
| `_sofbuddy_cmdpark` | 1 | Master (also needs `_sofbuddy_cpuopt 1`) |
| `_sofbuddy_cmdpark_reserve_ms` | 0 | ms cushion; `0` = stock inserts |
| `_sofbuddy_cmdpark_strict` | 1 | Tick over console |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_cmdpark_cbuf_max` | ms | Worst drain |
| `_sofbuddy_cmdpark_defers` | count | Pre-frame queues moved aside |
| `_sofbuddy_cmdpark_cbuf_cursize` | bytes | Current 8 KB occupancy |
| `_sofbuddy_cmdpark_cbuf_fill_max` | bytes | Peak 8 KB occupancy |

Old names (`_sofbuddy_dotcmd_defer`, `_sofbuddy_tickpace_reserve_ms`,
`_sofbuddy_tickpace_strict`) are gone.

```
tools/tests/cmdtext_parking/run.sh
```
