# zpool

Recycles the engine's zone allocations instead of round-tripping the CRT heap
for every one.

**Off by default.** Set `_sofbuddy_zpool 1` before the game DLL loads —
`+set _sofbuddy_zpool 1` on the command line, or in a config that runs before
the first map. Read once, at `GameDllLoaded`.

## Status: measured no effect — not enabled by default

**This feature neither helps nor hurts measurably.** It is kept in the tree as
a recorded negative result, with `_sofbuddy_zpool` defaulting to `0`.

Measured on a live sofplus server with `_sofbuddy_hashmap 0` throughout, so
this isolates the pool. The timings are `clamp_monitor`'s `highclamp` lines
from `sofbuddy-shim.log` — each bench phase blocks the engine thread, the
engine clamps on the next tick, and clamp_monitor reports the deleted
milliseconds. That is **millisecond** resolution, unlike the benchmark's
whole-second wall clock:

| phase | `_sofbuddy_zpool 0` | `_sofbuddy_zpool 1` | delta |
|---|---:|---:|---:|
| A script vars | 10,604 ms | 10,981 ms | +3.6 % |
| B deep cvars | 14,819 ms | 14,650 ms | −1.1 % |
| C non-command | 9,344 ms | 9,431 ms | +0.9 % |
| D `~` control | 3,234 ms | 3,160 ms | −2.3 % |

All four are inside ±4 %, with no consistent sign. The pool does nothing
measurable in either direction.

Activation is confirmed, not assumed: `sofbuddy-shim.log` carries
`[zpool] enabled: recycling zone blocks up to 256 bytes` for the ON run and
`[zpool] off (_sofbuddy_zpool 0)` for the OFF run, in separate bootstrap
generations.

### Two corrections to earlier claims in this file

1. An earlier revision reported a **25 % regression on phase D**. That was
   wrong. It came from the benchmark's whole-second clock reading 4 s versus
   3 s — a single second of quantisation. At millisecond resolution the same
   phase is 2.3 % *faster* with the pool on. Never trust a ratio built from
   small integers of seconds.
2. The feature was justified by an *assumption* that Wine's
   `RtlAllocateHeap`/`RtlFreeHeap` round trip cost 400–600 ns per pair and so
   dominated the ~3.1 µs/op floor. That number was never measured, and the
   result says otherwise: recycling those allocations away changes nothing, so
   they were not a meaningful share of the cost to begin with.

The allocation *call sites* are real and were verified in the disassembly
(below). What was never verified is that they were **expensive**. That is the
lesson worth keeping: a confirmed hot call site is not a confirmed bottleneck,
and the cheapest way to find out is to measure the primitive directly rather
than reason about what it ought to cost.

### Should it be enabled?

No. It buys nothing measurable while replacing the engine's allocator entry
points, which is real risk for no return. The detour itself is free (installing
it and delegating measured identical to not building it at all), so leaving the
feature compiled in with the cvar off is harmless if you want to keep
experimenting.

## Why it was built

This targets the floor that `hash_lookup` left behind. With hashing on, the
live benchmark measured **~3.1 µs/op** that no amount of lookup work could
remove (`hash_lookup/README.md`, phase D). Most of it is allocation:

- **`Cmd_TokenizeString` (`0x20018FC0`) allocates per argument.** It `Z_Free`s
  every previous `cmd_argv` entry (`0x20018FD8`) and `Z_Malloc`s a fresh one
  per token (`0x200190BD`, stored at `0x200190CD`).
- **`Cvar_Set2` frees and re-`CopyString`s the value on every write.**

So a line like `add _sofbuddy_bench_var 1` costs roughly **four malloc/free
pairs**. And `Z_Malloc` (`0x2001F120`) is not cheap: CRT `malloc`, then
`rep stosd` over the whole block, then a linked-list insert. Under Wine that
`malloc` is `RtlAllocateHeap` behind a lock.

`Z_Malloc`/`Z_Free` are the engine's only allocation chokepoint — 36 and 56
call sites respectively — so intercepting the pair reaches all of it.

## How

Both functions are replaced outright (`override` hooks on `Z_Malloc`
`0x2001F120` and `Z_Free` `0x2001EBC0`). The replacements reproduce the
engine's header, chain linkage and accounting exactly, because `z_stats_f` and
`Z_Touch` walk the same chain:

```
+0x00  prev    void*     previous node, or &z_chain for the head
+0x04  next    void*     next node
+0x08  magic   uint16    0x1D1D, checked by Z_Free
+0x0A  tag     uint16    always 0
+0x0C  size    uint32    full allocation, request + 0x10
+0x10  payload
```

The chain sentinel is at `0x20249E54` and **its `next` field is the head
pointer** at `0x20249E58` — which is why the engine's `prev->next = next`
updates the head without special-casing it. Counters live at `0x20249634`
(count) and `0x20249E70` (bytes).

Freed blocks go onto size-bucketed free lists (16-byte granularity, full
allocations up to 256 bytes, 64 blocks per bucket ≈ 256 KB ceiling). A later
request of the same bucket pops one instead of calling the CRT. Blocks are
still zeroed on reuse — callers rely on `Z_Malloc` returning zeroed memory —
but the heap round trip disappears.

### It is a cache, not an arena

Every block is still allocated **individually, through the engine's CRT**
(`malloc` `0x200FABFE`, `free` `0x200F9D32` — not this DLL's own statically
linked CRT, which would be a different heap).

That is the whole reason the design is safe to unload. spsv `FreeLibrary`s and
reloads this DLL between games; blocks allocated by the pool are still held by
the engine afterwards, and the restored `Z_Free` will hand them to the engine's
`free()`. Because each one is an ordinary CRT pointer, that is correct. An
arena or a suballocator would make that case unfixable.

`ZPool_Shutdown()` runs from `DllMain` detach and returns everything still
sitting in the free lists — those blocks are off the zone chain, so nothing
else would ever free them.

### Double frees still fail loudly

`Z_Free` poisons the header magic before caching a block. A second free finds
`magic != 0x1D1D`, falls through to the engine's original, and raises
`Z_Free: bad magic` exactly as it always did — rather than silently corrupting
a free list.

Anything the pool does not recognise (a null pointer, a block with the wrong
magic, an allocation from before the detour installed) is forwarded to the
original untouched.

## Testing

`zpool.cpp` holds no platform dependency — the `GetModuleHandleA` glue lives in
`zpool_win.cpp` — so the logic is exercised host-side, built 32-bit so pointer
size and the 16-byte header match, against a fake engine image with a
transcribed replica of the engine's own `Z_Malloc`/`Z_Free`:

- header, linkage and accounting identical to the engine's
- recycling actually avoids the CRT, and recycled blocks come back zeroed
- 20,000 mixed alloc/free operations spanning pooled and oversized requests,
  verifying after each phase that the chain walks cleanly in both directions,
  that its length equals the live count, and that both counters balance
- a double free routed to the engine's magic check
- **detach**: the engine's own unpatched `Z_Free` frees 200 blocks the pool
  allocated, with counters landing at zero
- no CRT leak and no CRT double free across the whole run

What is **not** covered without a live server: contention with anything else
that patches these functions, and the actual speedup. Measure that by toggling
`_sofbuddy_zpool` with `hash_lookup/bench/sofbuddy_bench.func` — phase D is the
allocation-heavy floor this feature attacks, so D moving is the signal.
