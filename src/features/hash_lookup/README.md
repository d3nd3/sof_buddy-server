# hash_lookup

Turns the engine's cvar, command and alias dictionaries into hash maps.

**Off by default.** Set `_sofbuddy_hashmap 1` before the game DLL loads —
`+set _sofbuddy_hashmap 1` on the command line, or in a config that runs before
the first map — and it is read once, at `GameDllLoaded`.

## Why

SoF inherits Quake 2's dictionaries-as-linked-lists. Every cvar read, every
cvar write and every console command walks a list and `strcmp`s each node:

| function | scans |
|---|---|
| `Cvar_Get`, `Cvar_Set2`, `Cvar_FullSet`, `Cvar_VariableValue`, `Cvar_VariableString` | all cvars |
| `Cmd_ExecuteString` | all commands, then all aliases, then (via `Cvar_Command`) all cvars |

That is fine for a client with a couple of hundred cvars and a human at the
keyboard. It is not fine for a server running sofplus scripting, where the
script engine is the one typing: each script variable is a cvar, each script
line goes through `Cmd_ExecuteString`, and a command that is neither a command
nor an alias falls all the way through to a full cvar scan. The lists grow with
the script set, so the per-lookup cost grows with it, and all of it lands in
the gap between server ticks.

The functions this feature patches are not a guess. Scanning `spsv.dll` for
embedded engine addresses shows sofplus's entire engine interface — identical
in the 2012 and 2019 builds:

```
Cmd_AddCommand 0x20019130   Cmd_RemoveCommand 0x200191E0
Cmd_Argc       0x20018D20   Cmd_Argv          0x20018D30   Cmd_Args 0x20018D50
Cmd_ExecuteString 0x200194F0
Cvar_Get       0x20021AE0   Cvar_Set 0x20022190   Cvar_SetValue 0x20022350
cvar_vars      0x2024B1D8   (walked directly)
```

`Cvar_Set` and `Cvar_SetValue` are thin wrappers over `Cvar_Set2`. So the hot
scans are `Cvar_Get`, `Cvar_Set2` and `Cmd_ExecuteString` — all three patched
here.

## How: patch the search, keep the function

There is no callable `Cvar_FindVar` in SoF.exe — it is **inlined** into each of
its callers. Rather than re-implement those functions (Cvar_Set2 alone is ~1KB
of latch / noset / info-validation / callback logic, and silent drift there
would be worse than the CPU it saves), this feature replaces only the *search*.

Every one of the scans has the same shape: a loop that walks a list and leaves
through one of two labels, with the matched node in a fixed register. The loop
head is overwritten with a `jmp` to a small assembly stub that does the O(1)
lookup and enters **that same function's own label** with **that same
register** set. Everything before and after the search is untouched engine
code, byte for byte.

```
Cvar_Get @ 0x20021AE0
  0x20021B54  mov ebp, cvar_vars   <-- overwritten with jmp hl_stub_cvarget
  0x20021B5E  ...scan loop...          (now unreachable)
  0x20021BE0  found:     expects ebp = cvar_t*
  0x20021BC4  not found:
```

### Patch sites

| function | patch RVA | len | key at | found (reg) | not found |
|---|---|---|---|---|---|
| `Cvar_VariableValue` | `0x216DE` | 6 | `[esp+0x14]` | `0x2174D` (edx) | `0x21742` |
| `Cvar_VariableString` | `0x2176E` | 6 | `[esp+0x14]` | `0x217DC` (edx) | `0x217D2` |
| `Cvar_Get` | `0x21B54` | 6 | `[esp+0x98]` | `0x21BE0` (ebp) | `0x21BC4` |
| `Cvar_Set2` | `0x21D87` | 7 | `[esp+0x98]` | `0x21E11` (ebp) | `0x21DF1` |
| `Cvar_FullSet` | `0x221C7` | 7 | `[esp+0x98]` | `0x22253` (ebp) | `0x22231` |
| `Cvar_Command` | `0x224CF` | 5 | `[esp+0x10]` | `0x2253C` (ebp) | `0x22532` |
| `Cmd_ExecuteString` | `0x1950C` | 6 | `cmd_argv[0]` | `0x19531` cmd / `0x1959E` alias (esi) | `0x1958C` |
| `Cmd_RemoveCommand` | `0x191E8` | 5 | — | resumes at `0x191ED` | — |

Register contract at every stub entry: `esp` is exactly what the engine had
(the surrounding function still reads its arguments through `[esp+N]`), and
`ebx`/`esi`/`edi`/`ebp` hold whatever the engine put there — the cdecl call
preserves them, and each stub sets only the one register its re-entry label
expects. `eax`/`ecx`/`edx` are scratch: the loop being replaced clobbered them
too, and every re-entry label writes them before reading. The one exception is
`Cmd_RemoveCommand`, whose `edx` still holds the name its not-found path
prints, so that stub saves it across the call.

Bytes past the 5-byte `jmp` are padded `0x90`. They are dead either way — the
only entry into those loops was the head that is now a jump.

### Safety

`Install()` verifies the original bytes at all eight sites **before touching
any of them**; one mismatch and nothing is patched at all. So an unrecognised
engine build, or another mod that got to the same bytes first, degrades to
"feature off" rather than to a wild jump. The expected bytes were verified
against two shipped server engines — sofplus 2012 `SoF-spsv.exe` and sofplus
2019 `sof-spsv16.exe` — and are byte-identical in both.

`HashLookup_Shutdown()` runs from `DllMain`'s detach path and puts the original
bytes back. This is **mandatory**, not hygiene: spsv `FreeLibrary`/reloads this
DLL between games, and the jumps point into this image.

## Keeping the index in sync with the list

### The list stays the source of truth

The table does not replace the engine's linked list — it is a **derived index
over it**. Nothing here relinks, reorders, copies or frees anything the engine
owns. The table holds `name -> node`, borrowing the engine's own pointers, and
the list is left exactly as the engine built it.

That is deliberate, because plenty of code still walks the list directly and
must keep working untouched: `Cvar_GetLatchedVars`, `writeconfig`, `cvarlist_f`,
`Cvar_Info`, `SG_WriteLatched` — and sofplus itself, which walks `cvar_vars`
directly (that stray `cvar_vars` reference in `spsv.dll`). None of those are
patched, and none of them need to be.

### The anchor

Each index remembers two pointers. `last_head` is the raw list head as of the
last sync, and is purely the fast-path compare. `anchor` is the first node
actually *indexed*, and is where the walk stops.

They are different on purpose. `anchor` must be a node nothing can free,
unlink or reorder — see "sofplus temporaries" below, where the real head very
often is not such a node. Because nodes are only ever added at the head,
everything from the current head down to `anchor` is exactly the set added
since, and everything from `anchor` onward is already in the table.

```
                       anchor  (head as of the last sync)
                          |
  head ->  N7 -> N6 -> [ N5 -> N4 -> N3 -> ... -> N0 -> NULL ]
           \___________/   \_________________________________/
            added since        already indexed; never revisited
            the last sync
```

So every lookup begins with:

```c
void* h = *head;
if (h == last_head)
    return;              // steady state: one load, one compare, done
// else: walk h .. anchor, indexing each eligible node;
//       last_head = h, anchor = first node indexed on this walk
```

- **Nothing changed** — one load and one compare, then straight to the hash
  lookup. This is the case essentially every time.
- **Nodes were added** — walk only the new prefix. Each node is inserted
  exactly once over the life of the process, so the amortised cost of keeping
  the index current is one insert per cvar/command/alias ever created.

This is why nothing needs to hook the creation paths. A cvar created by
`Cvar_Get`, by `Cvar_Set2` falling through to it, by the engine at startup, or
by sofplus, is discovered identically: the next lookup notices the head moved
and picks it up. The index never has to be told.

The sync runs *before* the table is consulted, which is what makes a miss
trustworthy: "not in the table" means "not in the list", so the stub can safely
take the engine's not-found branch.

### What the anchor compare can and cannot see

This is the whole coherency argument, and it is where reads and writes part
company. Everything below the prepend row is a **real** hazard, not a
hypothetical one — sofplus does all three (see the next section):

| what happens | head moves? | anchor compare notices? | consequence |
|---|---|---|---|
| any lookup / read | no | n/a | **cannot** stale the table |
| node prepended | yes | **yes** — self-healing | new prefix walked in |
| node relinked / list reordered | no | **no** | table would keep serving a stale position |
| node unlinked mid-list | no | **no** | table would serve a node no longer in the list |
| node freed | no | **no** | table would serve a **dangling pointer** |
| `name` freed or repointed | no | **no** | borrowed key dangles; lookups mismatch |

The mechanism is self-healing for exactly **one** kind of mutation: a prepend.
Everything below that line is invisible to it — the head pointer is unchanged,
so the fast path returns immediately and the table goes on answering with a
node that has moved, left the list, or been freed.

That asymmetry is the reason the two xref questions are different questions:

- **Reads** decide *which functions to patch*. An unpatched read site is not a
  correctness problem, it just stays O(n). This is purely a performance
  question, and it is how the eight patch sites were chosen.
- **Writes** decide *whether the table can go stale* — and specifically writes
  the anchor compare cannot see. Enumerating writes to the list *head* is not
  enough for that, because a relink, an unlink or a free never touches the
  head. Those have to be found some other way, and then either proven not to
  exist or handled explicitly.

A stale answer is also much worse than a slow one. In `Cmd_ExecuteString` a
freed `cmd_function_t` means `call *(node+8)` through reclaimed memory.

### sofplus temporaries: why the engine audit was not enough

The audit below covers **SoF.exe**. That is not the whole program. sofplus
caches `&cvar_vars` (`spsv.dll` stores the immediate `0x2024b1d8` into its own
global) and a pointer to the engine's `Z_Free`, and it uses both to implement
"temporary" cvars — the `~`-prefixed ones — by destroying them itself:

| `spsv.dll` | what it does |
|---|---|
| `0x10005750` | walks `cvar_vars`; for every node with `name[0] == '~'`, `Z_Free`s the **name**, `string`, `latched_string` and the **node**, unlinking it mid-list (`0x100057C3`) |
| `0x10005800` | unlinks every `~` node into a saved scope list, without freeing |
| `0x10005890` | splices a saved scope list back on and rewrites the head |

That is an unlink, a free, and a relink — all three of the rows a head-pointer
comparison cannot see — and the freed `name` is the exact string the table
borrows as its key. Indexing those nodes is a use-after-free. It showed up on a
live server as `~fake is write protected.`: the table returned a `Z_Free`d node,
the block had been recycled, and `flags` at `+0x0C` came back with `CVAR_NOSET`
set. It could as easily have been a crash.

**So `~`-prefixed cvars are never indexed.** They are skipped during sync and
routed to the exact linear walk on lookup, which is cheap because sofplus keeps
temporaries at the front of the list. Everything else — the several hundred
permanent engine and sofplus cvars, plus all commands and aliases — is indexed
as before, and `anchor` is pinned to the first *permanent* node so that freeing
a head temporary can never dangle it or force a full re-index.

Both shipped builds behave this way: the 2014 `spsv.dll` and the 2019
`spsv-16.dll` each cache `&cvar_vars` as an immediate and gate on `cmpb $0x7e`.

**Limitation, stated plainly:** this rests on sofplus's `~` naming contract,
which is a documented user-facing concept rather than an internal detail — but
it is still a contract. A different mod that removes or reorders cvars *not*
prefixed with `~` would reintroduce exactly this class of bug, and nothing here
would detect it.

### The audit that closes the gap

So the three things below were checked by sweeping the whole engine image —
every store to a `next` field and every `Z_Free`, not just the heads' xrefs.
They establish that *the engine* only ever prepends. They say nothing about
other modules loaded into the process, which is the gap the section above
closes.

**1. Who writes the head** (confirms additions really are prepends, which is
what makes the anchor walk correct):

- `cvar_vars`: one store, `Cvar_Get` `0x20021D4D`, and nothing anywhere takes
  its address (no `push offset` / `mov reg,imm32` form of it exists).
- `cmd_alias`: one store, `Cmd_Alias_f` `0x20018C41`.
- `cmd_functions`: `Cmd_AddCommand` `0x200191C1` plus seven copies inlined
  into `Cmd_Init`, all prepends; and one address-of, in `Cmd_RemoveCommand`.

**2. Who writes a node's `next`** (invisible to the anchor compare):

- cvar_t `+0x1C`: exactly **one** store in the entire image — `0x20021D4A`,
  the `var->next = cvar_vars` half of the same prepend. Cvars are never
  unlinked or reordered, so nothing here is invisible.
- cmd/alias `next` (`+0x00`): the prepends above, plus `Cmd_RemoveCommand`'s
  mid-list unlink `*back = cmd->next` at `0x20019240`. That is the one
  destructive operation on any list during operation — and being invisible to
  the anchor compare is precisely why it gets an explicit patch.

**3. Who frees a node, or a `name`** — the table borrows `name` pointers as
keys, so they have to outlive every lookup. The only `Z_Free` of a cvar's
`name` (`[var+0]`) is in the exit teardown below; everything else in `cvar.c`
frees `string` (`+0x04`) or `latched_string` (`+0x08`), which the index never
touches.

### Removal: the one case that needs telling

`Cmd_RemoveCommand` unlinks mid-list and `Z_Free`s the node, and the anchor
compare cannot see either. Its stub therefore **drops the whole command index**
before the engine frees anything; the next lookup finds `anchor == NULL` and
rebuilds by walking the list once.

A wholesale rebuild rather than a surgical erase is deliberate: it cannot leave
a freed node reachable no matter which node went, or how many, or whether the
name was ambiguous in case. Removals are rare — a rebuild costs one walk of a
few hundred nodes, against the certainty of never calling through a dangling
function pointer.

### Exit teardown

Each list does have a bulk destructor, and each is an `atexit` handler — a C++
static destructor, not something reachable during play:

| handler | registered at | frees |
|---|---|---|
| `0x20021A90` | `0x20021A85` | every cvar: `name`, `string`, `latched_string`, node |
| `0x20019110` | `0x20019105` | every `cmd_functions` node |
| `0x20018B20` | `0x20018B15` | every `cmd_alias` node |

These are why a *call*-xref search reports no callers for them: they are
referenced by `push offset` into `atexit`. None of them nulls its head pointer,
so after they run the heads dangle — but the engine's own unpatched scans would
walk that same freed memory, so the index is no worse there. Ordering is safe
for this DLL: the exe's `atexit` handlers run before `ExitProcess`, so the
stubs are still mapped while they execute, and `DllMain`'s detach unpatches
afterwards.

### Matching the engine's comparisons exactly

- **Cvars** compare with `strcmp` — case-sensitive.
- **Commands and aliases** are matched by `Cmd_ExecuteString` with `stricmp`,
  so both those indexes fold case. Note `Cmd_AddCommand`'s duplicate check is
  case-*sensitive*, which is why that scan is deliberately left unpatched: an
  accelerated case-insensitive check there would start rejecting `Foo` when
  `foo` exists. It only runs on registration, and its `Cvar_VariableString`
  call is accelerated anyway.
- Because a case-differing duplicate *can* therefore exist, the sync walks
  newest-first and never lets an older node overwrite a newer one under the
  same folded key — the engine's scan starts at the head, so the newest wins
  there too.
- Keys containing bytes `>= 0x80` take a linear walk instead of the table:
  our ASCII folding matches msvcrt's `stricmp` in the C locale, but not
  necessarily beyond it, and a wrong *miss* on `Cmd_ExecuteString` would
  silently drop a command.

### The `matrix` quirk

Every inlined copy of the cvar search carries the same oddity: after the name
compare fails, it *also* matches when the requested name is exactly `"matrix"`
and the node's name is exactly `"timescale"`.

```asm
0x20021BA7  mov edi, offset "matrix"     ; vs the requested name, 7 bytes
0x20021BA9  jnz  next
0x20021BAD  mov edi, offset "timescale"  ; vs this node's name, 10 bytes
0x20021BBB  jz   found
```

So `Cvar_FindVar("matrix")` returns whichever of the `matrix` and `timescale`
cvars sits nearer the head. It is a single-name special case, so the hash path
is bypassed for that one name and it takes the exact linear walk.

## Predicted CPU saving

These are **predictions**, not live-server measurements — nothing has been
measured on a running server yet. But the lookup costs underneath them are
measured, not guessed.

### Measured lookup cost

The engine's scan was replicated instruction-faithfully — the same linked walk,
the same `strcmp`, and crucially the same per-mismatch `repe cmpsb` pair for the
`matrix` quirk — and timed against this feature's index over the same list.
Host-native 32-bit build, Intel i5-3350P @ 3.1 GHz:

| cvars in list | engine scan, average hit | engine scan, miss | hash index | speedup (hit) |
|---:|---:|---:|---:|---:|
| 200 | 2.5 µs | ~5 µs | 65 ns | 38× |
| 400 | 4.9 µs | ~10 µs | 65 ns | 75× |
| 700 | 8.8 µs | 17.0 µs | 71 ns | 124× |
| 1200 | 15.2 µs | 29.4 µs | 78 ns | 196× |
| 2000 | 25.0 µs | ~50 µs | 71 ns | 351× |

Two constants fall out, and they are what the rest of this section uses:

- **~25 ns per node scanned** (≈ 77 cycles), flat across list sizes. That is far
  more than a `strcmp` of a short name should cost, and the reason is the
  `matrix` quirk: `repe cmpsb` runs on *every non-matching node*, it is
  microcoded, and unlike `rep movsb` it has no fast-string path.
- **~65–90 ns per hash lookup**, independent of list size.

Break-even is around **three nodes**. Any list longer than a handful wins.

### What the list lengths actually are on a sofplus server

- **Commands**: 225 static `Cmd_AddCommand` sites in `SoF.exe` plus 8 copies
  inlined into `Cmd_Init`, plus 66 `sp_*` commands in `spsv.dll` — call it
  **~300**.
- **Cvars**: 318 static `Cvar_Get` sites in the engine, 427 `set` lines across
  the sofplus configs, plus whatever the running scripts create. **700–1000+**
  is an ordinary steady state, and heavier script sets go well past it.

### The formula

```
saving per lookup  ≈  25 ns × (nodes the engine would have scanned)  −  70 ns
    hit at depth d :  25d − 70 ns
    miss           :  25n − 70 ns
```

The pathological case is `Cmd_ExecuteString` on a line that is neither a
command nor an alias: it scans **all commands, then all aliases, then every
cvar** via `Cvar_Command`. At 300 commands and 1000 cvars that is ~1300 nodes —
**~32 µs for a single console line**, gone to ~70 ns.

### Measured on a live server

Run with `bench/sofbuddy_bench.func` on a sofplus server (Wine, retail engine,
empty server, ~300 commands / ~700+ cvars). Throughput, higher is better:

| phase | OFF | ON | per-op OFF → ON | speedup |
|---|---:|---:|---|---:|
| A permanent script vars | 88,889/s | 266,667/s | 11.25 → 3.75 µs | **3.0×** |
| B deep engine cvars | 53,333/s | 320,000/s | 18.75 → 3.13 µs | **6.0×** |
| C non-command lines | 88,889/s | 400,000/s | 11.25 → 2.50 µs | **4.5×** |
| D `~` temporaries | 266,667/s | 320,000/s | 3.75 → 3.13 µs | **1.2×** |

### Reconciling this with the microbenchmark

The isolated lookup is 38–350× faster; a whole console line is 3–6× faster.
Both are true, and the gap is the point: **a console line is not all lookup.**

Phase D pins the floor. Its ops still pay tokenisation, the sofplus
interpreter, and `Cvar_Set2`'s `Z_Free`/`CopyString` of the value string —
about **3.1 µs/op that no amount of hashing can remove**. Phase A is therefore
roughly 3.0 µs of floor plus 8.25 µs of lookup, and deleting the lookup gives
3×, not 125×.

So the microbenchmark measures the mechanism and the table above measures the
product. Quote the table.

### Saving per tick, from measured numbers

At ~7.5 µs saved per script line (the phase A mix) and a 100 ms tick:

| script lines per tick | saved | share of the tick |
|---:|---:|---:|
| 100 | 0.75 ms | 0.75 % |
| 500 | 3.8 ms | 3.8 % |
| 1000 | 7.5 ms | 7.5 % |
| 3000 | 22.5 ms | 22.5 % |

An earlier revision of this section predicted 2.9 / 14.3 / 28.6 / 86 ms for
those same line counts — about **4× too optimistic**, because it costed a
script line as if it were entirely lookup. The numbers above replace it.

### Where it does *not* help

- **sofplus `~` temporaries are not indexed at all** (see above) and keep the
  linear walk. They sit at the front of the list, so that walk is short — but
  the cvar-side saving on them is zero by design (measured: phase D still gains
  1.2×, and that comes entirely from command dispatch, not cvar access).
- **A script whose hot variables are all `~` temporaries gets only the command
  half of the win.** The `lag_begin` stress script is exactly this shape —
  `~fake` and `~i` are both temporaries, so what speeds up there is the per-line
  command lookup, not the variable accesses. It is a poor showcase for this
  feature even though it is a good CPU-load generator.
- `Cmd_AddCommand`'s duplicate scan is deliberately left unpatched.
- Anything not lookup-bound — the game frame itself, ghoul, physics, networking
  — is untouched.

### Caveats

Measured host-native on an i5-3350P, not under Wine on the target box; the
*ratio* travels better than the absolute nanoseconds. The engine replica uses
the same instruction sequence but is compiled code rather than the literal
engine bytes. And the per-tick table assumes a script-line mix that has not been
measured — substitute your own line count and the formula still holds.

To confirm any of this on a real server, `clamp_monitor`'s
`_sofbuddy_clamp_avg` and `_sofbuddy_highclamps` before and after toggling
`_sofbuddy_hashmap` is the direct read.

## Testing

`index.cpp` is portable and is exercised host-side, built 32-bit so pointer
size and node offsets match, against a synthetic engine image built with the
same prepend discipline — bulk insert, incremental sync across table growth,
the `matrix` quirk with and without a real `matrix` cvar, case-insensitive
command/alias matching, command-shadows-alias ordering, case-differing
duplicates, non-ASCII fallback, and stale-entry-after-removal.

The stubs' machine code and every indirect jump target were checked in the
linked object. What is **not** covered without a live server is the patched
engine actually running: the register contract at each re-entry label is read
from disassembly, not observed.

### On-server benchmark

`bench/sofbuddy_bench.func` measures the real thing. Copy it to
`user/sofplus/`, then:

```
sp_sc_func_load_file sofplus/sofbuddy_bench.func
sp_sc_func_exec sofbuddy_bench_init

_sofbuddy_hashmap 0
map <yourmap>                        // the setting is read at game-DLL load
sp_sc_func_exec sofbuddy_bench_run   // records the OFF baseline

_sofbuddy_hashmap 1
map <yourmap>                        // same map, same conditions
sp_sc_func_exec sofbuddy_bench_run   // records the ON result
sp_sc_func_exec sofbuddy_bench_report
```

Two sofplus mechanics dictate the shape of this script, both read out of
`spsv.dll`:

- **`sp_sc_flow_*` share one budget of 100000 iterations per server frame** —
  counter `@0x1002F9F0`, tested against `0x186A0` `@0x100048D2`, and reset at
  exactly one site, `@0x1000C37A`, inside sofplus's per-frame hook. Exceeding
  it prints `Error: Infinite loop prevention` and kills the loop. The budget
  refills per *frame*, not per function — so four phases in one call share one
  budget, and the first one to run consumes all of it. Every phase here
  therefore runs in its own frame, chained by 1 ms timers.
- **That same per-frame hook then calls `@0x10005750`, freeing every `~`
  temporary.** Temporaries do not survive a frame boundary, so all state that
  crosses a timer is held in permanent cvars.

Timing comes from `clamp_monitor`'s `_sofbuddy_clamp_lost_ms`, not from
`_sp_sc_info_time_sec`. A blocking script stalls the engine thread, the engine
clamps on the next tick, and `clamp_monitor` reports exactly how many
milliseconds were deleted — millisecond resolution instead of whole seconds,
which is the only way the fast case is measurable at all.

| phase | workload | expected |
|---|---|---|
| A | permanent (non-`~`) script variables | large gain |
| B | deep engine cvars (`maxclients`, `timelimit`, …) — created at startup, so bottom of the list | largest gain |
| C | lines that are neither command nor alias → commands + aliases + all cvars | largest gain |
| D | `~` temporaries — isolates the *cvar* half only | small gain (~1.2×) |

Phase D existing is the point. `~` temporaries are deliberately not indexed, so
a stress script built on them — the usual shape — measures almost none of the
cvar half of this feature. It is **not** a pure control, though: `add ~t 1` is
still a console command, so its `Cmd_ExecuteString` scan *is* indexed. That is
why D measures ~1.2× rather than 1.0×, and it doubles as the floor measurement
used above. If A/B/C do not move either, the patches did not install — check
the server log for a `[hash_lookup]` line.

A frame that stays under the 100 ms tick budget never clamps and so cannot be
timed: if a result reads 0 ms, raise `_sofbuddy_bench_rounds`. Expect ±100 ms
of tick quantisation on the fast side.

Run it on an empty server: each phase blocks the engine thread for its whole
workload, so ticks stall and any connected client will lag or time out.
