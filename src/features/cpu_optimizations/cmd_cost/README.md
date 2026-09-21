# cmd_cost

Per-command handler wall time for SoF dedicated servers. Answers: *which
`Cmd_ExecuteString` handler is eating the tick?* and *how close are addon
scripts to the 100 ms intra-tick limit?*

Times the **`function` pointer** inside each `cmd_functions` node — not
tokenization, not the linked-list scan (`hash_lookup` already targets that).
Cheap handlers are batch-measured with **spin** (Wine QPC quantum is ~15.6 ms;
single-call samples read as zero). A static **addons** pass estimates min/avg/max
cost from every `<user>/sofplus/addons/*.func` file.

**On by default** (`_sofbuddy_cmdcost 1`). Handlers are **not** patched until
live timing is on; spin and addons never leave trampolines installed.

## Why

sofplus runs thousands of commands per tick through `Cmd_ExecuteString`. When
`_sofbuddy_cmdpark_cbuf_max` climbs but `_sofbuddy_cmdcost_max` stays flat, the
problem is usually script shape (a `while` unroll), not one expensive builtin.
This module names the handler and feeds `cmdcost::PredictMs()` for future tick
budget work.

## How it works

### Lazy handler wrap (live timing)

IDA (`SoF.exe`):

| symbol | RVA | role |
|---|---|---|
| `Cmd_ExecuteString` | `0x200194F0` | walks `cmd_functions`, `call [esi+8]` @ `0x2001955E` |
| `cmd_functions` | `0x241840` | head of 12-byte nodes: `next+0 name+4 function+8` |
| `Cmd_AddCommand` | `0x20019130` | allocates nodes |

When `_sofbuddy_cmdcost 1`, the first `Cmd_ExecuteString` **patches** each
`function` slot to `TimedCmd`, which QPC-times the original and returns. Set
`_sofbuddy_cmdcost 0` → **unwrap immediately** (no trampoline overhead).

- **Spin / blob / slots** only walk `cmd_functions` into an orig table — they
  call handlers directly, never patch.
- **`sofbuddy_cmdcost_wrap`** re-syncs the orig table; patches only if live
  timing is on (sofplus registers commands late).
- **Pre/Post hooks** on `Cmd_ExecuteString` are a fallback when unwrapped;
  less accurate (includes tokenize), unused once patched.

### Spin (cheap commands)

One call is often below one QPC tick. Spin calls the **original** handler in a
tight loop until ≥32 ms of wall time (or `SpinCap` for flooders), then stores
**per-call** ms. Session-breakers (`map`, `wait`, `exec`, player `.vote`, …) are
skipped; sofplus flooders are hard-capped so bench does not multicast print/sound.

### Addons (static `.func` model)

`sofbuddy_cmdcost_addons` parses every `.func` under `<user>/sofplus/addons/`
(`user` cvar + `sofplus/addons/`). This is **static synthesis** — it never
executes addon scripts. For each function and each hook event
(`_sp_sv_on_map_begin`, `_sp_sc_on_change_*`, …) it reports min/avg/max ms.

**Per-command cost:** live `cmdcost` EMA when ≥4 samples exist; otherwise
bench fallbacks from `tools/tests/cmd_cost/results_sofplus.txt`. When both
exist, EMA is blended toward the fallback. String-heavy builtins
(`sp_sc_cvar_hex`, `sp_sc_cvar_save`, `sp_sc_cvar_append`, …) scale with
estimated literal/cvar length.

**Control flow:**

- `sp_sc_flow_if` — folds when operands are known (`val 1 == val 1`, known
  `cvar`/`set` state). Otherwise min = cheaper branch, max = costlier.
- `sp_sc_flow_while` — bound from `val N`, `#maxclients`, or preceding `set`/`add`
  on the loop cvar. Unknown start → min 0 iters; known start → min = max.
- Inlines `sp_sc_func_exec` callees; skips `sp_sc_timer` callbacks (deferred).
- Event handlers with `~par_slot` bind `eventSlot` (default 0).

**Prime live samples before analyze:**

```text
sofbuddy_cmdcost_addons prime [slot] [maxclients] [maxwhile] [defwhile] [tick_ms]
```

`prime` spins each unique `sp_*`/`echo`/`set` name found in addons (same safe
rules as `sofbuddy_cmdcost_spinall`), then runs the static pass so predictions
use fresh EMA instead of fallbacks.

## Quick start

```text
# 1. Baseline catalog (safe sofplus cmds + echo/set)
sofbuddy_cmdcost_spinall

# 2. Inspect per-name stats (n, max, ema, avg)
sofbuddy_cmdcost_dump

# 3. Addon script budget vs 100 ms tick
sofbuddy_cmdcost_addons

# 4. Turn off live per-command timing (unwraps handlers)
_sofbuddy_cmdcost 0
```

With a client connected, slot-sensitive server commands:

```text
sofbuddy_cmdcost_slots 0 8        # read-only-ish live samples
sofbuddy_cmdcost_slots 0 8 rough  # also team/spec/mute (mutates state)
```

## Console commands

| command | what it does |
|---|---|
| `sofbuddy_cmdcost_dump` | Print per-name stats: `n max ema avg` (µs column = `ema×1000`) |
| `sofbuddy_cmdcost_reset` | Clear all samples and gauges |
| `sofbuddy_cmdcost_wrap` | Re-walk `cmd_functions`; patch if `_sofbuddy_cmdcost 1` |
| `sofbuddy_cmdcost_spin <name> [n] [slot]` | Benchmark one handler. `n=0` or omitted = adaptive until ≥32 ms |
| `sofbuddy_cmdcost_spinall` | Spin every safe catalog `sp_*` cmd plus `echo`/`set` |
| `sofbuddy_cmdcost_blob <len> [piece]` | Prepare test string cvars for string-op spins — see [Test blob](#test-blob) |
| `sofbuddy_cmdcost_slots [slot] [reps] [rough]` | Queue server slot cmds; needs a connected client (`slot` 0..31) |
| `sofbuddy_cmdcost_addons [prime] [slot] [maxclients] [maxwhile] [defwhile] [tick_ms]` | Static cost model of `<user>/sofplus/addons/*.func` |
| `sofbuddy_cmdcost_events` (`events`) | Live `_sp_sv_on_*` server events + `_sp_sc_on_change_*` hooks |
| `sofbuddy_cmdcost_usercmds` (`usercmds`) | Live `.COMMAND` funcs (dot-prefixed client commands) from sofplus func table |
| `sofbuddy_cmdcost_timers` (`timers`) | Active `sofplus_timer` queue (due ms, callback, self-rescheduling) |

Examples:

```text
sofbuddy_cmdcost_spin sp_sc_cvar_math_add
sofbuddy_cmdcost_spin sp_sv_info_client 0 0
sofbuddy_cmdcost_addons prime 0 16 100000 64 100
sofbuddy_cmdcost_addons 0 16 100000 64 100
sofbuddy_cmdcost_events
events
sofbuddy_cmdcost_usercmds
usercmds
sofbuddy_cmdcost_timers
timers
```

### Events (`sofbuddy_cmdcost_events`)

Reads live handler lists from engine `cvar_vars` for every `_sp_sv_on_*` server
event and `_sp_sc_on_change_*` hook. Handler lines show `name()` with magenta
`(filename.func B)` or `(filename.func U)` when found (`B` = base addons, `U` =
user addons; user wins on duplicate func names).

### User commands (`sofbuddy_cmdcost_usercmds`)

Walks the live sofplus func list (`sp_sc_func_list` shape: linked list from
`dword_1002F918`, skip nodes with non-zero parent). Lists registered functions
whose name starts with `.` (dot commands like `.stats`, `.kdr`). Output uses
coloured `Com_Printf` (RCON / dedicated console — no `developer 1`).

Each line shows `name()`, optional user-facing param list (skips the leading
`~par_slot` and first `*` wildcard), and the defining `.func` basename in magenta
with `B` (base) or `U` (user) when found under live `base`/`user` addon dirs.
When the `.func` source has a `//` comment immediately above the `function .name`
line (nearest non-empty `//` when blank `//` lines sit in between), that text is
printed on the next line in green.

### Timers (`sofbuddy_cmdcost_timers`)

Reads the `sofplus_timer` queue from `spsv.dll`: due ms, callback text.
Self-rescheduling timers (callbacks that call `sp_sc_timer` again) show
`every N ms` (scans `sp_sc_func_exec` callees too). Handler file tags use the
same addon index as usercmds (`B` / `U` for base vs user).

## Test blob

`sofbuddy_cmdcost_spin` passes a fixed one-liner per command. That is fine for
`sp_sc_cvar_math_add`, but useless for string builtins — `sp_sc_cvar_len` on an
empty cvar always costs the same, and you cannot see how cost scales with payload
size. **`sofbuddy_cmdcost_blob`** fills engine cvars once, then subsequent
`sofbuddy_cmdcost_spin` calls on string ops read that payload.

### Syntax

```text
sofbuddy_cmdcost_blob <len> [piece]
```

| arg | range | meaning |
|---|---|---|
| `len` | `0`..`256` | Length of the test string. Values above 256 are clamped — sofplus/engine cvar buffers are short and overflowing them crashes the server |
| `piece` | `1`..`64` (default `1`) | Only affects **`sp_sc_cvar_append`** and **`sp_sc_cvar_append_newline`**: how many `'B'` characters each spin iteration appends onto the destination |

### Cvars created

| cvar | role |
|---|---|
| `_cmdcost_blob` | Source string. Filled with `len` copies of `'A'` (32-char `sp_sc_cvar_append` chunks) |
| `_cmdcost_dst` | Destination/work buffer. Cleared on each blob build; most string spins write here |
| `_cmdcost_hex` | Hex encoding of `_cmdcost_blob`. Built only when `0 < len ≤ 128` (for `sp_sc_cvar_unhex` spins) |

Blob build does not patch handlers — it calls `set` / `sp_sc_cvar_append` /
`sp_sc_cvar_hex` through the orig table (`SyncOrigs` only).

### Commands that read the blob

After `sofbuddy_cmdcost_blob <len>`, these spins use the prepared cvars:

| command | spin line shape |
|---|---|
| `sp_sc_cvar_len` | len of `_cmdcost_blob` |
| `sp_sc_cvar_copy` | copy blob → dst |
| `sp_sc_cvar_escape` / `unescape` | transform blob → dst |
| `sp_sc_cvar_hex` | blob → dst |
| `sp_sc_cvar_unhex` | `_cmdcost_hex` → dst |
| `sp_sc_cvar_nbsp` / `no_color` | blob → dst |
| `sp_sc_cvar_replace` | replace `A`→`X` in blob → dst |
| `sp_sc_cvar_split` | split blob on `_` |
| `sp_sc_cvar_substr` | full blob (`0`..`len`) → dst |
| `sp_sc_cvar_append` | append `piece`×`'B'` onto dst (see below) |
| `sp_sc_cvar_append_newline` | append newline onto dst |
| `sp_sc_cvar_save` | write `_cmdcost_blob` to `cmdcost_save.cfg` (disk I/O) |

`sp_sc_cvar_big_text` uses dst only (no blob). `spinall` does **not** call blob
— run blob + per-command spins yourself for string scaling.

### Append spins (special case)

`append` mutates its destination. To measure steady-state append cost without
the dst growing unbounded across thousands of iterations, each spin iteration:

1. **`PrepDstFromBlob`** — `sp_sc_cvar_copy _cmdcost_dst _cmdcost_blob` (reset
   dst to the source payload)
2. Run `sp_sc_cvar_append _cmdcost_dst <piece×'B'>`

Set `piece` with the second argument:

```text
sofbuddy_cmdcost_blob 128 1    # append 1 char per call
sofbuddy_cmdcost_blob 128 64   # append 64 chars per call
```

### Typical workflow

Scale one command across lengths (matches `tools/tests/cmd_cost/bench_strlen.py`):

```text
sofbuddy_cmdcost_reset
sofbuddy_cmdcost_blob 0
sofbuddy_cmdcost_spin sp_sc_cvar_len 0 0
sofbuddy_cmdcost_blob 64
sofbuddy_cmdcost_spin sp_sc_cvar_len 0 0
sofbuddy_cmdcost_blob 128
sofbuddy_cmdcost_spin sp_sc_cvar_copy 0 0
sofbuddy_cmdcost_dump
```

### Interpreting results

- Most string ops are **flat ~0.3–0.8 µs** at all lengths — the engine spends
  most time in `Cvar_Get` / `Cvar_Set2` list walks, not `strlen` of the payload
  (`results_strlen.txt`, `results_reasoning.txt`).
- **`sp_sc_cvar_hex`** grows with `len` (two nibbles per byte); skip `len=256`
  — output is `2×len` and blows the cvar cap.
- **`sp_sc_cvar_save`** is real disk I/O (hundreds–thousands of µs); anti-correlates
  with blob length.
- **`sp_sc_cvar_replace`** bench args are not a real replace map — rows reflect
  parser printf noise, not LUT cost.

Captured matrix: `tools/tests/cmd_cost/results_strlen.txt`.

## Cvars

**Settings** (you set these):

| cvar | default | what it does |
|---|---|---|
| `_sofbuddy_cmdcost` | `1` | `ARCHIVE`. `1` = live handler timing (patch on first cmd). `0` = off, unwrap now |

Requires `_sofbuddy_cpuopt 1` (folder master).

**Gauges** (read-only; DLL writes them):

| cvar | unit | meaning |
|---|---|---|
| `_sofbuddy_cmdcost_max` | ms | Worst single sample this boot |
| `_sofbuddy_cmdcost_name` | name | Catalog name of that sample |
| `_sofbuddy_cmdcost_ema` | ms | Highest per-name EMA across all names |
| `_sofbuddy_cmdcost_ema_name` | name | Which name owns that EMA |
| `_sofbuddy_cmdcost_n` | count | Sample count for the name last published |

EMA needs ≥4 samples before `PredictMs()` returns non-zero.

## Reading `sofbuddy_cmdcost_dump`

```
[cmdcost] sp_sc_cvar_math_add 128 0.000320 0.000315 0.000318 0.315
          ^name                  ^n  ^max    ^ema    ^avg   ^ema_us
```

- Compare **ema** across commands on the same boot.
- **`[qpc-upper]`** in spin log = flooder capped below one Wine QPC tick; rank
  order is meaningful, not the absolute µs (see `results_reasoning.txt`).
- **Real tick costs** to watch: `sp_sc_cvar_save` (disk), `sp_sc_file_find`,
  `sp_sc_func_list`, large `while` unrolls — not capped print/sound unless you
  let them multicast during bench.

## `cmdcost::PredictMs()`

```cpp
float cmdcost::PredictMs(const char* text);  // first token; 0 if <4 samples
```

Built from live EMA. Used by `sofbuddy_cmdcost_addons` when sampled; planned for
`tick_pacing` reserve (not wired yet). Command catalog is generated at build
from `sofplus-cursor-rules` → `generated_cmd_catalog.h` (~108 names).

## Tests and live benches

**Unit tests** (no Wine):

```bash
tools/tests/cmd_cost/run.sh
tools/tests/cmd_cost_addons/run.sh
```

**Live server** (RCON, needs running `SoF-spsv.exe`):

| script | purpose |
|---|---|
| `tools/tests/cmd_cost/bench_live.py` | spinall + dump; A/B cpu opts |
| `tools/tests/cmd_cost/bench_strlen.py` | string-op scaling vs blob length |
| `tools/tests/cmd_cost/bench_alias.py` | engine alias vs inline func body |
| `tools/tests/cmd_cost/remeasure_zeros.py` | re-spin commands that read 0 |

Captured results: `tools/tests/cmd_cost/results_*.txt`. IDA notes:
`results_reasoning.txt`.

Fixtures: `tools/tests/cmd_cost/fixtures/` (`while_cpu.func` = lag-counter shape).

## Layout

```
cmd_cost/
  cmd_cost.cpp / .h     live timing, spin, console cmds
  cmd_cost_addons.cpp   sofbuddy_cmdcost_addons
  cmd_cost_events.cpp   sofbuddy_cmdcost_events
  cmd_cost_sofplus.cpp  sofbuddy_cmdcost_usercmds / _timers
  func_analyze.cpp / .h .func parser + static cost model
  cvar.cpp / .h         gauges + _sofbuddy_cmdcost
  hooks/                Cmd_ExecuteString Pre/Post
  callbacks/            GameDllLoaded
  bench/                .func files for alias/math live benches
```

Hooks: `Cmd_ExecuteString` Pre/Post (`cmdcost_CmdPre` / `cmdcost_CmdPost`).
Shutdown: `CmdCost_Shutdown()` unwraps and restores gauge `cvar_t.string`
pointers.
