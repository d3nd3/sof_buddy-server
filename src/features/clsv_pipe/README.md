# clsv_pipe — `_sp_cl_sv_*` stufftext pipeline

Pushes dynamic content (`.func` / `.sp` chunks) to clients after they
connect, using the `_sp_cl_sv_0 .. _sp_cl_sv_99` client cvars as a
stufftext transport — the same cvars SoFplus servers drive with
`sp_sv_client_cvar_set SLOT NUMBER STRING`.

This shim has no SoFplus server-script VM, so it implements the
observationally identical transport one level down: every send goes out as
stufftext `set _sp_cl_sv_N "text"`, which sets the same client cvar and
fires the same `sp_sc_on_change` handler. The client only needs the small
listener in `assets/addons/pipe.func` (regenerate any lane count with
`tools/clsv_gen_listener.py`).

## Usage

1. Produce a chunk file with the existing parsers (this feature focuses on
   the pipeline, not the condensing):
   ```
   python3 parsers/func_parser/func_parser.py mycontent.func -o mycontent.cfg
   ```
2. Copy it where the server can read it, e.g. `Base/clsv/mycontent.cfg`.
3. On the server console:
   ```
   clsv_load base/clsv/mycontent.cfg
   clsv_send all          # or: clsv_send 3
   clsv_status
   clsv_cancel all
   ```
   New spawns auto-start when `_sofbuddy_clsv_auto` is 1 (default).
4. Clients need the listener (`sofplus/addons/pipe.func`) — see
   Bootstrap below. Once it is in place, everything else, including
   updates to `pipe.func` itself, flows over the pipe.

## Bootstrap: getting pipe.func onto the client

The pipe carries everything *except* its own first install: without the
listener's `sp_sc_on_change` handlers, `_sp_cl_sv_*` sets land silently.
The three-step bootstrap closes that loop:

1. **Server:** place `assets/addons/pipe.func` at
   `<gamedir>/sofplus/addons/pipe.func`, set `allow_download 1` (the
   engine still gates serving on it) and `_sofbuddy_pipe_dl 1`. While
   enabled, every map spawn registers the file via the engine's own
   `SV_GhoulFileIndex` (compact first-free slot, idempotent, visible in
   `clsv_status` as `ghoul=<index>`). Disabling leaves a stale table
   entry behind on purpose: clearing it mid-table would end the client's
   download walk early for everything after it, while a stale entry costs
   only one failed request per connect (the walk continues past
   "File not found").
2. **Client (once):** connect. The precache walk requests every
   `CS_GHOULFILES` entry verbatim, so it asks for
   `download sofplus/addons/pipe.func` by itself — no console typing.
   `dl_unlock` lets exactly that path past spsv's `sofplus/` block;
   the client saves it under `sofplus/addons/` and picks it up on the
   next restart, when `spf_sc_addons_init` auto-loads it and execs
   `pipe_init` (a leading `-`, as in `-pipe.func`, opts out).
3. **Steady state:** `clsv_send` delivers content; `pipe.func` updates
   themselves ship as chunks that `sp_sc_cvar_save` back to disk, so the
   download path is first-install-only.

## Wire format

Loader input is parser `.cfg` output: `set NAME "VALUE"` lines (values
already `%"`-escaped, ≤ 255) plus `sp_sc_func_load_cvar ENTRY` lines.
`sp_sc_cvar_unescape` lines are ignored — the pipeline emits its own.

One job, in order (reliable channel = order guaranteed, DONE arrives last):

| send | cvar | text |
|---|---|---|
| INFO | `_sp_cl_sv_98` | `<gen>` (job tag, resets the client counter) |
| DATA × K | `_sp_cl_sv_L` ring | `sset <name> <escaped-value>` (define, still encoded) |
| CMD × K | `_sp_cl_sv_L` ring | `sp_sc_cvar_unescape <n> <n>` (decode in place) |
| CMD × E | `_sp_cl_sv_L` ring | `sp_sc_func_load_cvar <entry>` (activate) |
| DONE | `_sp_cl_sv_99` | `<gen>` (triggers client echo + ack) |

The client parses **nothing**: each data arrival is `sp_sc_exec_cvar`'d
verbatim and counted; DONE echoes the count and sends `pipeack <got>`.
`gen` increments per job so INFO/DONE always *change* (no-change = no
`on_change` fire). The ack is best-effort (unknown client commands forward
to the server, stock Q2 `Cmd_ForwardToServer` behaviour, snooped here via
the `ClientCommand` export hook with call-through); a linger timeout
(`_sofbuddy_clsv_ack_wait_ms`, default 10 s) finishes the job regardless.
Why `sset` + full escaping: the DATA text is the VALUE of an outer
`set _sp_cl_sv_L "..."`, so it must hold no raw `"` (nesting would truncate
the token at `COM_Parse`, whose limit is 255 + NUL per token). `sset` takes
the value as ONE token, so spaces go (`%20`) or the outer `set` sees
argc > 3 and rejects the line. `;` goes (`%3B`) or it would split the inner
command at exec time. `#`/`$` go (`%23`/`%24`) or the client's token-level
substitution would rewrite content at define time instead of at func
runtime. Tab goes (`%09`) or it would split tokens. The decoder
(`sp_sc_cvar_unescape`, generic `%hh 00..ff`) round-trips all of these with
the single unescape CMD the server appends per chunk. Inputs with raw `"`,
CR or LF are rejected at load with a line number (re-run the parsers).

## spcl.dll stufftext whitelist (verified in IDA)

The SoFplus client filters every incoming `svc_stufftext` in
`StuffText_hook` (spcl.dll @ `0x10003480`) before it reaches Cbuf. Anything
else prints `SP Blocked server command: '...'` and is dropped. The rules
that matter for this pipeline:

1. `strncmp(line, "set _sp_cl_sv_", 14) == 0`, then `isdigit(line[14])`,
   then `line[15] == ' '` **or** (`isdigit(line[15])` and
   `line[16] == ' '`). So the cvar number is 0–99, always followed by a
   space. A non-numeric name (e.g. `_sp_cl_sv_info`) fails `isdigit` and
   is blocked — that is why control lives on numeric **98/99**, not on
   string-suffixed cvars.
2. No `;` anywhere in the whole line (`strchr` over the full stufftext).
3. Exactly one `\n`, and it must be the last byte (followed by NUL).

The value itself is never inspected. Compliance per send form:

| send | example wire | gate |
|---|---|---|
| INFO/DONE | `set _sp_cl_sv_98 "7"\n` | numeric + space, no `;`, single trailing `\n` ✓ |
| DATA | `set _sp_cl_sv_3 "sset b_04f6_0 ...%3B..."` | payload escapes (`%3B` is three chars, no raw `0x3B`) ✓ |
| CMD | `set _sp_cl_sv_2 "sp_sc_cvar_unescape f_f5e5_0 f_f5e5_0"\n` | fixed literals, no `;`/`\n` ✓ |

`BuildPlan` rejects any send that would fail the gate (cvar shape,
raw `;`/CR/LF in text, wire cap), and `ClsvWirePassesSpclGate`
(`clsv_logic.h`) models the client check byte-for-byte — the host tests
run every planned wire through it plus negative cases.

## Bandwidth: one cvar vs 90

Constraints (dedicated MP, `maxclients > 1`):

- Reliable mailbox (`netchan.message`) = **1384 B**; only **one** reliable
  parcel in flight per client (`reliable_length`), ACKed per round trip.
- One DATA send ≈ `set _sp_cl_sv_NN "` + `sset <name> ` + ~200 B payload +
  `"` ≈ **230 B** on the wire (`svc_stufftext` + string + NUL).
- 1384 / 230 ≈ **6 sends per packet**. That is the natural full packet.
- `COM_Parse` caps every token at 255 B: chunk payloads stay ≤ 200 B
  (`_sofbuddy_clsv_chunk`, hard cap 240, wire line cap 250).

What the lane count does and does not do:

- Lanes do **not** change throughput. Bytes per tick are set by
  `_sofbuddy_clsv_per_tick` × ~230 B. Lanes only rotate the cvar names.
  Because handlers run synchronously per `set`, reusing one cvar is already
  safe — the experiment axis that matters is **burst vs drip**
  (`per_tick`/`interval`), with lanes as the supporting choice
  (`lanes >= per_tick` keeps each tick's batch on distinct cvars, so the
  client's per-arrival order stays unambiguous and recent chunks stay
  inspectable instead of overwritten).
- **1 lane, 2/tick, every tick (defaults):** ~460 B/tick ≈ 4.6 KB/s at
  10 ticks/s. A 50-chunk (~10 KB) pack lands in ~2 s. Every parcel drips
  under `reliable_defer`'s `maxDripBytes` (692 B), so snapshots are never
  held back, chat never stalls, and a dropped packet retransmits at most
  ~460 B. Minimal client VM churn per tick (2 `on_change` execs).
- **90 lanes can only pay off with a huge `per_tick`**, and that is where
  it loses on MP: 90 × 230 B ≈ 20 KB bursts into ~15 back-to-back reliable
  parcels. `reliable_defer` queues them (32-parcel / 256 KiB caps, then
  **drops**), every drip over 692 B holds the snapshot (`dumped
  unreliable` — visible hitching for everyone), and one packet loss
  retransmits the whole backlog while chat/layouts stall behind it. The
  90 `on_change` registrations also bulk up the listener for zero gain:
  the packet still only fits ~6 sends.
- The one case where many lanes help is `maxclients == 1`
  (16368 B mailbox ≈ 70 sends/packet) — i.e. solo/listen servers, not
  dedicated MP.

Recommended: `lanes` 4–6, `per_tick` 2–6 (6 = one full packet/tick,
≈ 14 KB/s, a 10 KB pack in under a second), `interval` 1. Raise `per_tick`
past 6 only to measure the degradation described above — that is the
experiment this feature exists to run (`clsv_status` + server log show
sends, jobs, acks and errors per setting).

Packet drop, precisely: the reliable channel itself never loses or
reorders — loss only appears as **stall** (retransmit of the unacked
backlog) or as `reliable_defer` **drops** under flood (oversize parcel or
queue caps). Small paced parcels make both negligible; DONE-last ordering
plus the client's `got` count and the server's ack log make any residual
gap visible (`pipeack` mismatch ⇒ resend with `clsv_send`).

## Cvars

Settings (archive, read live; `lanes`/`per_tick`/`interval`/`chunk` apply
at the next `clsv_send`/auto-start):

| cvar | default | meaning |
|---|---|---|
| `_sofbuddy_clsv` | 1 | master switch (frozen jobs resume when re-enabled) |
| `_sofbuddy_clsv_lanes` | 4 | data lanes `_sp_cl_sv_0..N-1` (1..90) |
| `_sofbuddy_clsv_per_tick` | 2 | sends per server frame per client (1..12, see above) |
| `_sofbuddy_clsv_interval` | 1 | frames between batches (≥ 1) |
| `_sofbuddy_clsv_chunk` | 200 | max %-escaped payload bytes (32..240) |
| `_sofbuddy_clsv_auto` | 1 | auto-start on spawn |
| `_sofbuddy_clsv_ack_wait_ms` | 10000 | linger after DONE awaiting `pipeack` |
| `_sofbuddy_pipe_dl` | 0 | download unlock for `sofplus/addons/pipe.func` (see Bootstrap) |

Gauges (NOSET outputs): `_sofbuddy_clsv_sent` (sends stuffed),
`_sofbuddy_clsv_jobs` (finished jobs), `_sofbuddy_clsv_errors`,
`_sofbuddy_pipe_dl_allowed` (unlock bypasses served).

## How it is wired

No game binary patching. `GameDllLoaded` registers the four console
commands (repoint-safe across map rotates, same pattern as `stufftext`)
and swaps the `game_export_t` `ClientCommand` slot (ge+0x34, same pattern
as `minigames`) for ack snooping with call-through; it also starts the
`dl_unlock` chain on the engine `download` slot when `_sofbuddy_pipe_dl`
is set (verified spsv bypass, fail-closed, per-frame self-heal) and keeps
`sofplus/addons/pipe.func` registered in `CS_GHOULFILES` via the engine's
own `SV_GhoulFileIndex` (pointer-only call, no detour) so connecting
clients request it on their own. An `SV_Frame` Post
hook (already in `detours.yaml`, no new detours) drives spawn-edge
detection (via `svs.clients` states, same layout as `stufftext_reconnect`),
paced batches and linger/disconnect handling. At most one job per slot;
a slot that leaves the game mid-job is cancelled so a recycled slot never
inherits a stale stream. Detach (`Clsv_Shutdown`) restores the export
slot, drops jobs silently and hands the engine back its cvar strings.

## Tests

`tools/tests/clsv_pipe/run.sh` — host tests for `clsv_logic.h` (escaping,
loader accept/reject, plan order + lane rotation + wire caps, spcl gate
model, pacing batches, sanitize clamps, wrap-safe linger clock) and
`dl_unlock.h` (exact-path allowlist, fail-closed negatives).
