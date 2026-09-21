# tick_pacing

Makes server ticks fire on their own 100ms boundary instead of on whichever
loop iteration happens to notice the boundary has already gone by -- without
moving `svs.realtime` ahead of the engine's own `msec` (that leftover is what
clients showclamp as a micro-stutter).

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

## What a drain is

A **drain** is one complete run of `Cbuf_Execute`. Holding drains and parking
inserts is **`cmdtext_parking`**, not this module. Settle still matters because
the clock is sampled *before* that drain.

## The 8KB `cmd_text` limit (IDA: `SoF.exe`, `spsv.dll`)

The command buffer is a stock Quake 2 `sizebuf_t` over a static array.
`Cbuf_Init` @ `0x20018160` is `SZ_Init(&cmd_text, &cmd_text_buf, 0x2000)` —
**8192 bytes**, hard-coded. There is no grow path: `cmd_text.maxsize` is fixed
at init and every add/insert path checks `cursize + len >= maxsize`.

### Hard limits

| limit | where | what happens |
|---|---|---|
| **8192 B** queued | `Cbuf_AddText`, `Cbuf_InsertText`, `Cbuf_InsertFromDefer`, `Cbuf_ExecuteText` | `Com_Printf("Cbuf_AddText: overflow\n")`; the **new** text is dropped. On insert, whatever was already queued is restored from the temp copy. |
| **~1024 B** per command line | `Cbuf_Execute` @ `0x20018530` | each `;`- or newline-terminated line is copied to a `0x400`-byte stack buffer before `Cmd_ExecuteString`. There is no length check — a line longer than 1023 bytes is a stack smash. |
| **4096 B** per sofplus insert | `spsv.dll` | `sp_sc_flow_while_` (`0x100048F3`) and `sofplusScriptEventDispatcher` (`0x1000CC50`) build text in a `0x1000`-byte local and call `Cbuf_InsertText` / `Cbuf_AddText`. Sofplus caps its own output; it does not handle engine overflow. |

There is also a second static stash, `defer_text_buf` @ `0x2023F840` (~8200 B,
sized to hold a full `cmd_text` copy). The **engine** uses it only on map
change: `SV_Map` calls `Cbuf_CopyToDefer` after `SpawnServer` to park whatever
was queued, and `CL_SendClientMessages` later jumps to `Cbuf_InsertFromDefer` to
prepend it back. That is not a sofplus workaround and not available during normal
play — it survives a map load, not sustained script load.

### How sofplus works around 8KB (IDA: `spsv.dll`)

Sofplus does **not** bypass the limit. Verified in `spsv.dll` (base
`0x10000000`):

- `DllMainBegin` resolves `Cbuf_InsertText` → `0x200181D0` and `Cbuf_AddText`
  → `0x20018180` into function pointers at `0x1002F9AC` / `0x1002F9A8`. There
  are **no** `Cbuf_*` implementations inside the DLL and no overflow recovery.
- Script flow uses **`Cbuf_InsertText` only** (12 call sites) — insert-at-front
  is required for `sp_sc_func_exec_` argument binding, not optional.
- Long-running control flow **trampolines one small insert per drain**. A `while`
  loop does not queue the whole loop body at once; each iteration
  `_snprintf`s at most 4096 bytes (`body;while "arg1" … "arg7";`) and inserts
  that single line, which re-queues itself on the next drain. The buffer is
  meant to stay nearly empty between iterations.
- Event dispatch uses `Cbuf_AddText` (append) when not inserting, with the same
  4096-byte local cap via `strncat`.

Sofplus's strategy is **stay well under 8KB by draining often and inserting
little**, not expand the buffer. The `cbuf_insert` feature documents the
complementary cost: each insert shifts the whole queue (`O(cursize)`), which
hurts when the buffer is large — another reason sofplus keeps it small.

## Two different quantities

Keep these apart; they are clamped by different code and respond to different
fixes.

* **wall lateness** — how late a tick executes against real time. Bounded by
  one loop iteration *plus* the stale-clock deficit. This is what players feel.
* **overshoot** — `svs.realtime - sv.time` at the moment the tick runs. This is
  the exact expression `SV_RunGameFrame` clamps, and highclamp fires when it
  exceeds 100.

The drain-boundary skip below attacks extra Sleep(1) delay. It does not, and cannot,
reduce a drain that is simply longer than a tick.

## What the feature does

### drain-boundary skip + settle (with the feature)

WinMain samples `msec` *before* `Cbuf_Execute`. A tick whose 100ms boundary
passes during that drain is invisible until the next loop. If the wall clock
already crossed the boundary during the drain:

1. Skip the next WinMain `Sleep(1)` (always when tickpace is on). That is the
   sound fix: the next sample's `msec` already contains the drain because
   `oldtime = newtime` runs *after* `Qcommon_frame`.
2. `_sofbuddy_tickpace_settle 1` credits `untilAfterMsec` into this frame's
   `msec` so `svs.realtime + msec == sv.time`. Unwind that credit from the next
   sample or the clock runs fast. Credit only when `Sys_Milliseconds` has also
   advanced by `untilAfterMsec` during the drain (integer engine clock, not
   the private float QPC used for skip-sleep). If the tick highclamps
   (`svs.realtime = sv.time`), any settle debt that was still over the new
   `sv.time` is dropped in Post — the engine already removed it, so the next
   frame's unwind must not subtract it again.

`_sofbuddy_tickpace_saved` counts straddling drains that actually fired a
tick. `_sofbuddy_tickpace_late_avg` is tick lateness once the tick fired.

### parked backlog (`cmdtext_parking`, not `_sofbuddy_tickpace`)

While `cmdtext_parking` is armed (strict or `reserve_ms` > 0) and its side
store still holds previous ticks' payload, WinMain's `Sleep(1)` is skipped so
the loop drips it instead of sleeping through it. Empty park sleeps again.
Independent of `_sofbuddy_tickpace`.

Keep drains off the boundary with **`cmdtext_parking`**. A drain longer than a
tick cannot be scheduled around.

### spin — `_sofbuddy_tickpace_spin_ms` (default 2, needs tickpace master)

When the tick is due within `spin_ms`, WinMain's dedicated-server `Sleep(1)`
(`push 1 / call ds:Sleep` at `0x2006639D`, IAT slot `0x2011114C`) is skipped
so a coarse host Sleep quantum cannot oversleep the boundary — and the loop
then iterates as usual, reading packets every iteration. Dedicated servers
only. This replaced the old `SpinToBoundary` busy-wait, which held the main
thread inside `SvFramePre` (after the drain, before `SV_ReadPackets`) and
clumped client packets for the whole spin.

Command-buffer hold / parking is **`cmdtext_parking`**
(`_sofbuddy_cmdpark_reserve_ms`). This module does not touch `Cbuf_Execute`.

## Cvars

**Settings** (you set these):

| cvar | default | what it does |
|---|---|---|
| `_sofbuddy_tickpace` | 1 | Master for drain-boundary skip, settle, and spin skip. `0` = those off. Also off if `_sofbuddy_cpuopt 0`. Park backlog skip is separate (`cmdtext_parking`). |
| `_sofbuddy_tickpace_settle` | 1 | With tickpace on: on straddling drains, credit `untilAfterMsec` into `msec` so the tick fires this frame. Unwind from the next sample. `0` = skip-sleep only. |
| `_sofbuddy_tickpace_spin_ms` | 2 | With tickpace on: skip WinMain's `Sleep(1)` when the tick is due within this many ms. Cap 20. `0` = off. |
| `_sofbuddy_cmdpark_strict` | 1 | Folder-wide. Fatal here: highclamp / lowclamp (not map-load settle) |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_tickpace_late_avg` | ms | Typical tick lateness (rolling mean). Players feel this. |
| `_sofbuddy_tickpace_saved` | count | Times the tick boundary passed during a drain and the next Sleep(1) was skipped so the following sample could notice it. |

## What lowclamp is not

Lowclamp (`sv.time - svs.realtime > 100`) means the server clock is more than a
whole tick *behind* game time and the engine jumps it forward, inventing the
difference. The stock engine cannot reach lowclamp from the tick path: a tick only fires
once `svs.realtime` has caught `sv.time`, leaving a deficit of at most 100,
and the test is strict. Settle does not create a `100 + credit` deficit:
`oldtime = newtime` runs after `Qcommon_frame`, so the next `msec` spans the
drain the credit accounted for.

If a live server is spamming lowclamp, use `clamp_monitor`
(`_sofbuddy_lowclamps` / `_sofbuddy_lowclamp_gained_ms` /
`_sofbuddy_lowclamp_worst`).

## Engine timebase

`Sys_Milliseconds` is owned by **`qpc_timer`** (`_sofbuddy_qpc`, default 1),
not this module: QPC milliseconds instead of the ~15.6 ms-stepped
`timeGetTime`, deliberately outside `_sofbuddy_cpuopt` / `_sofbuddy_tickpace`
so flipping those never jumps the shared clock. What this module keeps is its
own QPC `Clock`, used only to measure sub-tick Cbuf drain time.

## Tests

```
tools/tests/tick_pacing/run.sh            # the suite
SWEEP=1 tools/tests/tick_pacing/run.sh    # lowclamp sweep vs stock
```

The harness builds the feature's real translation units for the host (32-bit,
so `cvar_t` offsets line up) against stub headers, and drives them with a
transcription of `WinMain` / `Qcommon_frame` / `SV_Frame` / `SV_RunGameFrame`
and a virtual clock. No server, no Wine.
