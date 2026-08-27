# cbuf_insert

Makes `Cbuf_InsertText` shift the command buffer in place instead of round-
tripping it through the zone allocator.

**Off by default** (`_sofbuddy_cbuf_insert 0`). Measurement runs in both
states, so flipping it is a controlled A/B on your own server. See
*Why it ships off* below — this tree has form here.

## What the engine does

`Cbuf_InsertText` (`0x200181D0`) is stock Quake 2, with `Cbuf_AddText` inlined
into the middle of it:

```
temp = Z_Malloc(cursize);        // 0x200181DF
memcpy(temp, data, cursize);     // 0x200181FA   rep movsd
SZ_Clear(&cmd_text);             // 0x20018203
  ... strlen(text) twice, SZ_Write(text) ...     the inlined Cbuf_AddText
SZ_Write(&cmd_text, temp, cursize);              // 0x20018265  copy back
Z_Free(temp);                    // 0x2001826B
```

With `C` bytes already queued, that touches `C` **three** times — `Z_Malloc`
(`0x2001F120`) `rep stosd`-zeroes the `C+16` bytes it returns before handing
them over, then the copy out, then the copy back — plus a `malloc`/`free` pair
and two `strlen`s of `text`. `Cbuf_AddText` (`0x20018180`), by comparison,
touches only the new text.

It is also quadratic: K inserts against a buffer holding C bytes cost O(K·C),
because every one of them moves everything already queued. `cmd_text` is an
8KB static array (`Cbuf_Init` @ `0x20018160` is
`SZ_Init(&cmd_text, &cmd_text_buf, 0x2000)`), so C can reach 8192.

## What this does instead

```c
memmove(data + len, data, cursize);
memcpy(data, text, len);
cursize += len;
```

One pass over C instead of three, no allocator, one `strlen` instead of two,
and byte-for-byte the same buffer contents.

The fast path is taken only when the result provably fits: `cursize + len <
maxsize`. That is stricter than either overflow test the original makes (the
inlined `Cbuf_AddText` checks `len >= maxsize` against an *already-cleared*
buffer, and the copy-back leans on `SZ_GetSpace`), so whenever this path runs
the original would have succeeded with no message and the same bytes.
Everything else — every overflow — delegates to the engine, so the
pathological behaviour stays exactly the engine's own instead of being
reimplemented from a guess about what it ought to be.

## Why sofplus makes this worth doing

Verified in `spsv.dll` (SoFPlus, 583 functions, base `0x10000000`), not
inferred:

- **`Cbuf_AddText` does not appear in it at all.** SoFPlus resolves
  `Cbuf_InsertText` at load time into a function pointer at `0x1002F9AC`
  (written once from `DllMainBegin` @ `0x1000B571`) and calls through it from
  **12 sites**. Calling through a pointer to the engine's entry still goes
  through an entry detour, which is why this override reaches them.
- Those sites are the hot ones: `sofplusScriptEventDispatcher` (`0x1000CD60`),
  `sp_sc_flow_if` (×2), `sp_sc_flow_while_`, `sp_sc_func_exec_` (×2),
  `sp_sc_exec_cvar` (×2), `sp_sc_exec_file`, `sv_map`, `sv_gameMap`.

`sp_sc_flow_while_` is the one that matters most. Every iteration of a sofplus
`while` loop builds its continuation and inserts it:

```c
_snprintf(v36, 0x1000, "%s;%s \"%s\" \"%s\" ... \"%s\";", body, cmd0, arg1..arg7);
Cbuf_InsertText(v36);
```

— the loop body **plus a fully re-quoted re-invocation of the `while` command
itself**, into a 4KB scratch buffer, trampolining through the command buffer
once per iteration. Each of those iterations pays the whole
`Z_Malloc`/zero/copy/copy/`Z_Free` cost above.

### Insert semantics are not optional

Nobody should be tempted to "just use `Cbuf_AddText`". Insert puts text at the
*front*, so it runs before what is already queued, and `sp_sc_func_exec_`
depends on that for argument passing: it sets the `~1`, `~2`, … argument cvars
with `Cvar_Set` and *then* inserts the function body. Those cvars are global
mutable state, so the body has to run before anything else that might overwrite
them. Appending instead of inserting would not just reorder execution, it would
hand functions the wrong arguments.

The goal here is to make the insert cheap, not to avoid it.

## Why it ships off

`src/features/zpool/README.md` records this tree's own cautionary tale: an
optimisation targeting these exact `Z_Malloc`/`Z_Free` call sites, obviously
correct on paper, that measured to **nothing** on a live server — and whose
lesson was written down as *"a confirmed hot call site is not a confirmed
bottleneck."*

That result applies directly to part of the cost model above. If the allocator
round trip is free on this server, then what is left is the byte traffic — the
zero-fill and the two copies — and **nobody has yet measured how big `cursize`
actually is when sofplus inserts.** That number decides whether this is worth
anything, and it is exactly what `_sofbuddy_cbuf_insert_max` and
`_sofbuddy_cbuf_insert_bytes` are here to tell you.

So: the override installs unconditionally and measures every call, and the cvar
chooses which implementation runs underneath. Same workload, same counters,
one flag.

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `_sofbuddy_cbuf_insert` | 0 | `1` = in-place shift, `0` = delegate to the engine. Read per call, so it is live-settable for A/B without a restart. |

Outputs (NOSET, published at most 10×/second — publishing five cvars per call
would cost more than the function does):

| Cvar | Meaning |
|---|---|
| `_sofbuddy_cbuf_inserts` | Total `Cbuf_InsertText` calls |
| `_sofbuddy_cbuf_insert_bytes` | Cumulative bytes of already-queued text shifted. **This is the quantity the optimisation removes two thirds of.** |
| `_sofbuddy_cbuf_insert_max` | Largest `cursize` seen at insert time. If this stays small, there is nothing here to win. |
| `_sofbuddy_cbuf_insert_us` | Cumulative microseconds inside `Cbuf_InsertText`. **The A/B number**: same workload at `0` and at `1`. |
| `_sofbuddy_cbuf_insert_slow` | Calls that delegated (overflow, or the cvar off) |

## How to decide whether to keep it on

1. Run with `_sofbuddy_cbuf_insert 0` through a representative busy period —
   ideally the same script-heavy benchmark the zpool README used.
2. Read `_sofbuddy_cbuf_insert_max` and `_sofbuddy_cbuf_insert_bytes`. A small
   `max` means stop here: the buffer is never big enough for the copying to
   matter.
3. Note `_sofbuddy_cbuf_insert_us` and `_sofbuddy_cbuf_inserts`, set
   `_sofbuddy_cbuf_insert 1`, run the same workload, and compare µs per insert.
4. Cross-check `_sofbuddy_highclamps` (clamp_monitor) over both periods.

Report the result in this file either way. A measured negative is worth as much
as a measured positive and costs the next person a week.

## Scope, and what is deliberately not done

- **`Cbuf_ExecuteText` (`0x20018380`) carries a second, inlined copy** of the
  same insert body for its `EXEC_INSERT` branch (`0x2001842A`). This hook does
  not reach it. It is also not on sofplus's path — sofplus calls
  `Cbuf_InsertText` directly — so it is left alone rather than patched for
  symmetry. (That function also `__alloca_probe`s 8KB of stack on every call,
  for its `EXEC_NOW` path.)
- **`Cbuf_Execute` is quadratic too**, in the same buffer: after extracting
  each line it does `memcpy(data, data + i, cursize - i)` to shift the
  remainder down, so draining N commands is O(N · cursize). Fixing that
  properly means giving `cmd_text` a head gap — storing the data at an offset
  and moving a start pointer instead of shifting — which would also make
  inserts O(len) instead of O(cursize) and collapse the quadratic behaviour on
  both sides. It is the right end state and it is deliberately not attempted
  here: `cmd_text.data` is read directly by `Cbuf_Execute`, `Cbuf_AddText`,
  `Cbuf_ExecuteText`, `Cbuf_CopyToDefer`, `Cbuf_InsertFromDefer` and
  `SZ_Write`, and every one of them would have to move together. Do that only
  if step 2 above says the byte traffic is genuinely expensive.
- The engine has exactly **one** internal caller of `Cbuf_InsertText`
  (`newsave` @ `0x20061690`), so the blast radius of this override is
  essentially sofplus alone.

## Addresses

All in `SoF.exe` / `SoF-spsv.exe`, base `0x20000000`, declared in
`detours.yaml` as **absolute** addresses — `SofExe` RVA resolution goes through
`GetModuleHandleA("SoF.exe")`, which is NULL on a `SoF-spsv.exe` server.

- `Cbuf_InsertText`  `0x200181D0` (overridden)
- `Cbuf_AddText`     `0x20018180` (read only; the cheap sibling)
- `Cbuf_ExecuteText` `0x20018380` (read only; second inlined copy of the body)
- `Cbuf_Execute`     `0x20018530` (read only; the quadratic drain above)
- `Cbuf_Init`        `0x20018160` (read only; `SZ_Init(..., 0x2000)`)
- `Z_Malloc`         `0x2001F120` (read only; `malloc(n+16)` then zero-fills)
- `cmd_text.data`    base + `0x23F828`
- `cmd_text.maxsize` base + `0x23F82C`
- `cmd_text.cursize` base + `0x23F830`

`cmd_text` is a stock `sizebuf_t` at `0x2023F820` (`allowoverflow` +0x00,
`overflowed` +0x04, `data` +0x08, `maxsize` +0x0C, `cursize` +0x10,
`readcount` +0x14). `EngineReady()` refuses to bind if `maxsize` is not 8192,
so a build with a different layout degrades to "delegate everything" rather
than to a wild `memmove`.

In `spsv.dll` (base `0x10000000`): `Cbuf_InsertText` function pointer
`0x1002F9AC`, `sp_sc_func_exec_` `0x10008120`, `sp_sc_flow_while_`
`0x100048F3`, `sofplusScriptEventDispatcher` `0x1000CD60`.

## Verification

`tools/tests/cbuf_insert/run.sh` builds the feature's three translation units
on the host (32-bit, so the `cvar_t` offsets line up) against stub headers, a
synthetic engine image and a real 8KB `cmd_text`. No server, no Wine.

The core test is **differential**: the engine's own `Cbuf_InsertText` is
transcribed from the disassembly, and every case runs twice from the same start
state — once through the transcription, once through the fast path — comparing
the resulting buffer bytes and `cursize`. An optimisation of this shape is only
worth anything if it is indistinguishable from what it replaces, so that is
what gets asserted. 48 combinations of queued size (0 … 6000) and inserted
length (1 … 2000) all match byte for byte and all take the fast path.

The rest: insert ordering really is front-insertion; an overflow case delegates
and the engine's own overflow path runs; `_sofbuddy_cbuf_insert 0` delegates
every call *and still measures*; the fast path performs zero zone allocations;
the counters track the bytes actually shifted; empty buffer and empty text
match the engine; detach hands `cvar_t.string` back.
