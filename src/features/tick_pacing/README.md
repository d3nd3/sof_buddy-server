# tick_pacing

Makes server ticks fire on their own 100ms boundary instead of on whichever
loop iteration happens to notice the boundary has already gone by.

## The loop, as the engine actually runs it

From IDA (`SoF.exe` / `SoF-spsv.exe`, shared `.text`, base `0x20000000`):

```
WinMain @0x20066300
    Sleep(1); PeekMessage pump
    do { newtime = Sys_Milliseconds(); msec = newtime - oldtime; } while (msec < 1);
    Qcommon_frame(msec);
    oldtime = newtime;                      <-- note: *after* Qcommon_frame

Qcommon_frame @0x2001F720
    fixedtime / timescale fold msec into ebx     @0x2001F7C0-0x2001F814
    while ((s = Sys_ConsoleInput())) Cbuf_AddText(s);
    Cbuf_Execute();                              @0x2001F885
    SV_Frame(ebx);                               @0x2001F8BB

SV_Frame @0x2005F5B0
    if (!svs.initialized) return;                @0x2005F5BF / 0x2005F5D2
    svs.realtime += msec;                        @0x2005F5E2
    SV_CheckTimeouts(); SV_ReadPackets();        @0x2005F5EA / 0x2005F5EF
    if (svs.realtime < sv.time) {                @0x2005F615  (unsigned)
        if (sv.time - svs.realtime > 100) {      @0x2005F61D  (unsigned, strict)
            "sv lowclamp"; svs.realtime = sv.time - 100;
        }
        return;
    }
    SV_RunGameFrame();                           @0x2005F6D4

SV_RunGameFrame @0x2005F3F0
    sv.framenum++; sv.time = 100 * sv.framenum;
    ge->RunFrame(); ...
    if (sv.time < svs.realtime) { "sv highclamp"; svs.realtime = sv.time; }
```

The clock is sampled *before* `Cbuf_Execute` runs, so however long console
commands take — sofplus scripting lives here — that time is not in
`svs.realtime` when the tick decision is made. A tick whose boundary passes
during a drain is not noticed until the loop has gone all the way round again:
another `Sleep(1)`, another pump, another drain. The tick slips by a whole
extra iteration, not just by the length of the commands.

## Two different quantities

Keep these apart; they are clamped by different code and respond to different
fixes.

* **wall lateness** — how late a tick executes against real time. Bounded by
  one loop iteration *plus* the stale-clock deficit. This is what players feel.
* **overshoot** — `svs.realtime - sv.time` at the moment the tick runs. This is
  the exact expression `SV_RunGameFrame` clamps, and highclamp fires when it
  exceeds 100.

The settle correction below attacks wall lateness. It does not, and cannot,
reduce a drain that is simply longer than a tick.

## What the feature does

### settle (always on with the feature)

Carry `svs.realtime` forward to *now* just before `SV_Frame`'s comparison, so
the tick decision is made against the real current time rather than a sample
taken before `Cbuf_Execute` ran.

The correction is **carried**, not applied-and-undone. `State::carried` records
how much of `svs.realtime` is ours rather than the engine's, and each frame
applies the single delta `want - carried`:

* a frame where the feature is off, or no map is running, has `want == 0` and so
  *unwinds* the correction instead of stranding it;
* the next `msec` — which spans the same elapsed time, because `oldtime = newtime`
  happens after `Qcommon_frame` — is never double-counted;
* if `svs.realtime` is not where `SV_Frame` Post left it, the engine re-anchored
  it (`SpawnServer` resets it to 0 on a map change; the two clamps overwrite it
  wholesale) and the carry is forgotten rather than subtracted from a value we
  did not write.

An earlier version added the correction in Post and subtracted it again. That
is *not* what caused the reported lowclamp spam — see "What lowclamp is not",
below — but it had a real defect: on a lowclamp the Post guard correctly
declined to subtract, and then forgot the correction, stranding up to 100ms in
the engine's clock permanently. The carry scheme cannot strand anything.

Every write is gated on `ServerRunning()`, which is now `sv.state == ss_game`
**and** `svs.initialized != 0` (`0x20396DE0`, immediately before `svs.realtime`
at `0x20396DE4` — the first two fields of `server_static_t`, exactly as in stock
Quake 2). `svs.initialized` is `SV_Frame`'s own precondition at `0x2005F5BF`,
taken before the `+= msec`: on that path the engine never accumulates the clock,
so neither may this feature.

### spin — `_sofbuddy_tickpace_spin_ms` (default 0)

When the tick is due within `spin_ms`, busy-wait to the boundary instead of
going round the loop again. Dedicated servers only.

### reserve — `_sofbuddy_tickpace_reserve_ms` (default 3)

Don't *start* a drain that will not fit before the next boundary. The gate size
is `worst recent drain + reserve`, capped at `kMaxRoomNeededMs` (10ms).

That cap is load-bearing. Without it, a saturated server death-spirals: holding
a drain defers the work arriving during the hold, which lengthens the next
drain, which raises the gate, which holds longer. Measured at 88% duty:
61ms overshoot became 681ms, with 16 seconds of tick lateness. Capping the gate
at 10ms bounds how much arriving work one hold can defer.

The consequence is honest: a drain longer than ~10ms cannot be kept off the
boundary. If your command buffer regularly costs 70ms, no phase of a 100ms tick
has room for it, and no scheduling policy fixes that.

### Drains are atomic

`Cbuf_Execute` always runs to completion once started, and `cmd_wait`
(`0x2023F838`) is never written. The engine honours `cmd_wait` as "stop
draining, keep the rest", which looks exactly like the lever this feature wants
— and using it corrupts sofplus's function arguments.

`sp_sc_func_exec_` binds arguments into **global** `~1`/`~2` cvars and *then*
inserts the body. That is only safe because the body is at the front of the
buffer and runs before anything can insert ahead of it. Splitting a drain can
leave a body queued across a tick, where sofplus's own `G_RunFrame` hook inserts
event/timer text at the front and rebinds the args — producing
`Warning: Function '...' expects 2 argument(s)` from `sp_sc_func_exec_`'s own
`v31 != cmd_argC()` check. A script's own `wait` is safe because the script was
written around it; an imposed one breaks an atomicity guarantee callers never
agreed to.

## Cvars

| cvar | default | meaning |
|---|---|---|
| `_sofbuddy_tickpace` | 1 | master switch |
| `_sofbuddy_tickpace_spin_ms` | 0 | busy-wait to the boundary within this many ms |
| `_sofbuddy_tickpace_reserve_ms` | 3 | headroom a drain must have to start; 0 = stock scheduling |
| `_sofbuddy_tickpace_defer_max_ms` | 200 | never hold a drain longer than this |
| `_sofbuddy_tickpace_late_avg` | — | rolling mean tick lateness (ms) |
| `_sofbuddy_tickpace_late_max` | — | worst tick lateness since boot |
| `_sofbuddy_tickpace_cbuf_max` | — | worst single drain (ms) — tells you which regime you are in |
| `_sofbuddy_tickpace_saved` | — | ticks whose boundary passed mid-drain and were caught by the settle correction this loop, instead of firing a whole iteration late |
| `_sofbuddy_tickpace_defers` | — | drains held by the reserve |

`_sofbuddy_tickpace_cbuf_max` is the one to read first. Under ~10ms the reserve
helps; well above it the win has to come from the scripts.

## What lowclamp is not

Lowclamp (`sv.time - svs.realtime > 100`) means the server clock is more than a
whole tick *behind* game time and the engine jumps it forward, inventing the
difference. The stock engine cannot reach it from the tick path: a tick only
fires once `svs.realtime` has caught `sv.time`, leaving a deficit of at most
exactly 100, and the test is strict.

The settle correction was suspected of causing it, on the theory that firing a
tick early and then restoring `svs.realtime` leaves a deficit of `100 +
correction`. **That theory is wrong**, and the reason is structural:
`oldtime = newtime` runs *after* `Qcommon_frame`, so the next iteration's `msec`
always spans the drain the correction accounted for. The compensation is exact
by construction.

This is verified, not argued. `SWEEP=1 tools/tests/tick_pacing/run.sh` runs
6048 configurations — command cost, arrival period, sleep cost, game-frame cost,
commands per tick, reserve, spin — against both the current scheme and the
add-and-undo scheme it replaced, comparing each to a stock run over the same
wall-clock span. Neither produces a single lowclamp the stock engine did not
also produce.

So if a live server is spamming lowclamp, the cause is elsewhere, and
`clamp_monitor`'s `_sofbuddy_lowclamps` / `_sofbuddy_lowclamp_gained_ms` /
`_sofbuddy_lowclamp_worst` are the instruments for finding it.

## Rejected: QPC for `Sys_Milliseconds`

Overriding `Sys_Milliseconds` (`0x20055930`) to return a
`QueryPerformanceCounter`-derived time was drafted and removed. The case for it
was that Wine's `timeGetTime` might be coarse (~15.6ms), which would quantize
`msec` and explain a lot of tick jitter on its own. Measured on this machine's
Wine:

```
QPC frequency: 10000000 Hz
timeGetTime  (no timeBeginPeriod): min step 1 ms, 20 steps in 19.76 ms
timeGetTime  (timeBeginPeriod(1)): min step 1 ms, 20 steps in 19.97 ms
Sleep(1): avg 1.087 ms, worst 1.263 ms (QPC); worst 2 ms as timeGetTime saw it
```

`timeGetTime` is already 1ms-granular, so the swap buys under a millisecond of
resolution. Against that it replaces the timebase for the *entire* engine —
netchan timeouts, ping calculation, download rates, console — risks a
discontinuity at the switchover, and the draft's `int` arithmetic broke at 24.8
days where the engine's `& 0xFFFF0000` base handles the full 49.7-day
`timeGetTime` wrap.

The draft's addresses were also wrong: `sys_ms_base` is `0x20390C30` and
`curtime` is `0x20390D38` (from the write xrefs at `0x20055951` and
`0x20055961`), not the `0x20390D44` / `0x20390D48` it assumed — which is its own
evidence the code had never run.

This feature keeps QPC where it actually earns its resolution: the internal
`Clock` used for elapsed-since-frame-entry, drain costs, and the spin. The
engine's own coarser clock is left alone.

Do not add a `timeBeginPeriod` call here either: `Sys_Init @0x200656F0` already
calls it (`push 1 / call ds:timeBeginPeriod`), unconditionally, from
`Qcommon_Init @0x2001F616`.

## Tests

```
tools/tests/tick_pacing/run.sh            # the suite
SWEEP=1 tools/tests/tick_pacing/run.sh    # the 6048-config lowclamp sweep
```

The harness builds the feature's real translation units for the host (32-bit,
so `cvar_t` offsets line up) against stub headers, and drives them with a
transcription of `WinMain` / `Qcommon_frame` / `SV_Frame` / `SV_RunGameFrame`
and a virtual clock. No server, no Wine.
