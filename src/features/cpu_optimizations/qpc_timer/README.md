# qpc_timer

Feeds the engine QPC milliseconds instead of `timeGetTime` (`Sys_Milliseconds`
@ `0x20055930`). Split out of `tick_pacing` with no behavior change; only the
cvar moved with it (`_sofbuddy_tickpace_qpc` is now `_sofbuddy_qpc` — the old
name is gone, so a stale `tickpace_qpc 0` in a config no longer does anything).

## Why

On some hosts `timeGetTime` steps ~15.6 ms, so WinMain's
`while (newtime - oldtime < 1)` busy-waits a whole quantum and `msec` jumps.
Everything downstream shares this clock — tick timing, ping, netchan,
downloads, console — so it lives in its own module, deliberately outside
`_sofbuddy_cpuopt` / `_sofbuddy_tickpace`: flipping those must never jump the
shared clock.

Stock is:

```
if (!initialized) sys_ms_base = timeGetTime() & 0xFFFF0000;
curtime = timeGetTime() - sys_ms_base;   // DWORD subtract, write @0x20390D38
```

`sys_ms_base` is `0x20390C30` (write @`0x20055951`). `initialized` is
`0x20390D40`. `curtime` is `0x20390D38` (write @`0x20055961`). Those are the
only xrefs; an earlier draft used `0x20390D44` / `0x20390D48` and never ran.

## How

* First hooked call runs the **original**, then locks QPC to that value. Ping,
  netchan, downloads, and console keep the same origin — no step at switchover.
* After that, `origin + (qpc_delta * 1000 / freq)` in **64-bit**, then truncated
  to `uint32_t`. That is the same 49.7-day wrap as `timeGetTime`, not signed
  `int` overflow at 24.8 days.
* `curtime` is written when the millisecond changes (WinMain spin-waits this),
  as stock does.
* QPC going backwards (core migrate) holds the clock instead of following it
  down, and resumes once QPC catches back up past the hold point — same idea
  as sof_buddy `media_timers` `qpc_timers()`. Letting it through would step
  `msec` backwards for the size of the jump.
* No `timeBeginPeriod` here: `Sys_Init @0x200656F0` already `push 1` /
  `timeBeginPeriod` from `Qcommon_Init @0x2001F616`.

sof_buddy starts this clock at 0 and casts elapsed ms to `int` (breaks at
24.8 days). We snapshot stock on the first hooked call (this DLL loads after
`Netchan_Init`) and keep the DWORD wrap.

## Cvars

| cvar | default | what it does |
|---|---|---|
| `_sofbuddy_qpc` | 1 | Master switch. `0` = stock `timeGetTime` clock passthrough. |

## Tests

```
tools/tests/qpc_timer/run.sh
```
