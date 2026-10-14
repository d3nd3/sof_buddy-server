# stats — admin-vouched per-player match statistics (inside profiles)

Records per-player end-of-match frags, keyed by profile guid. Lives inside
the profiles feature folder because it cannot work without it: identity,
sign-in state, and privilege all come from the roster.

## Trust model

Clean data is the hard part. Instead of trusting every scoreboard, we trust
admins: a signed-in **server admin** (privilege `0`) or **map admin**
(privilege `1`) may issue `.upload_match` / `.cancel_upload`. The request
itself is the good-faith signal ("this was a real match, record it").

- Non-admin (or signed-out) `.upload_match` is consumed privately before
  sofplus ever sees it — same pattern as `.login` credential handling, so the
  attempt never reaches chat.
- Admin `.upload_match` records a native pending **first**, then falls through
  to the original sofplus dot-function, which still does its scheduling +
  discord screenshot exactly once. Our `ClientCommand` hook is the outer spsv
  detour, so registration order is ours-first by construction; no manual
  `sp_sc_func_exec` re-invocation is needed and the `.func` scheduling logic
  (`_spf_upload_pending`, intermission-immediate branch, one-upload-per-map)
  is preserved verbatim.
- Duplicate mid-map `.upload_match` while pending is consumed with the same
  "only one upload per map" message the `.func` uses (no double message).
- `.cancel_upload` clears the native pending and falls through so sofplus
  clears its own pending + message. No duplicate messages by design.
- Menu single-tap (`upload match` row, privilege `0`/`1` only): taps are not
  client commands, so fallthrough cannot apply — the trigger arms the native
  pending and queues the sofplus schedule itself (`set _spf_upload_pending 1`
  + `set _spf_upload_slot`, then a broadcast), mirroring the `.func` mid-map
  branch verbatim. The map-end hook then fires screenshot + discord exactly
  as if chat had scheduled it. Duplicate/refusal rules match the chat path.
  Navigation is closed at intermission, so the row is schedule-only; at map
  end the console remains the path. Cancel stays chat-only (`.cancel_upload`).

## What is recorded

At flush time (intermission, see below) one snapshot per signed-in slot:

- guid (registry key), registry nickname, live userinfo name, team
  (blue/red/spec from `gclient+0x324`), frags (`ps.stats[STAT_FRAGS]`,
  i.e. `resp.score` as the scoreboard shows it).
- Guests (no roster identity) are skipped — stats require a profile.
- Header: sequence, UTC, map, uploader guid + nickname.

Frags-only is deliberate phase 1: it is the official scoreboard number, it
needs no per-kill hook, and it keeps the hot path at zero cost. K/D splits
would need a `player_die` tap — currently overridden by `skinteam_quiet`
(single-override rule), so that is a follow-up (notify from the existing
override), not this change.

## When it flushes

- `.upload_match` mid-map sets a native pending (uploader slot/guid/nick +
  map). No file IO yet.
- `SV_Frame` Post checks once per frame: if the map changed under a pending,
  the pending is dropped (forced map change, no record); if
  `level.intermissiontime != 0`, the pending flushes now — snapshot +
  totals rewrite + log append, all synchronous, a few KB.
- `.upload_match` issued during intermission flushes synchronously in the
  command handler (mirrors the `.func` immediate branch).

## Storage: flat files, not SQLite

`<user>/sofplus/data/stats/` (per-server, same convention as
`profiles/registry.cfg`):

- `totals.cfg` — aggregated per-guid, rewritten on flush:
  `<guid> <matches> <frags> <nickname> last_map=<map> last_seen=<utc>`
  Sorted by guid, capped at 4096 entries (same policy as the registry:
  past the cap, existing guids still update). Legacy `<guid> <nick>` lines
  load as 0/0.
- `matches.log` — append-only, one `match <seq> <utc> <map> <guid> <nick>`
  header plus N `player <guid> <frags> <team> <nick> [| <live>]` rows per
  recorded match. Sequence continues across restarts (headers counted on
  load).

Why not SQLite:

| Concern | Flat files | SQLite |
|---|---|---|
| Binary size / deps | zero (CRT `fopen` only) | ~0.5–1 MB amalgamation + locking code in a 32-bit Wine DLL |
| Tick risk | append/rewrite of a few KB on flush only (≤1/map, <1 ms) | `fsync`/journal stalls on the game thread; needs background thread + queue to be safe |
| Concurrency | single game thread, no locks | same thread, but library locking + WAL recovery for no benefit |
| Queries | per-player totals + append-only history (no joins) | SQL buys nothing at this scale |
| Ops | copy two text files to clone/move; hand-editable | binary file, needs tooling |
| Failure mode | torn write = one lost match at worst (small files, close+rename discipline) | torn journal = whole DB suspect |

Verdict: SQLite's complexity (threading, fsync tuning, backup tooling)
is not worth it for <4096 players × two counters + an append-only log.
If per-weapon / per-kill event streams are ever needed, revisit — that is
the scale where an index helps.

## Runtime cost

- Idle: one `bool` + one `float` (`intermissiontime`) read per `SV_Frame`
  Post. Nanoseconds; no allocations, no syscalls.
- Snapshot (flush only): ≤64 slots × a few validated pointer reads + string
  copies. Microseconds.
- Flush IO (≤1/map): rewrite `totals.cfg` (entries × ~80 B; 100 players ≈
  8 KB) + append ~1–2 KB to `matches.log`. Synchronous, still two orders of
  magnitude under the 100 ms tick. No per-tick, per-frame, or per-kill work.

## Menu visibility

- `stats` detail page (`menuOnly`, every signed-in tier): lifetime totals,
  pending state, and a privilege-aware hint. Same row vocabulary as the
  landing (`145 frags | 3 matches`), widened with top-5 scope.
- `upload match` action row (`menuOnly`, `Level01Only`, no page): server and
  map admins see it (cyan marker, like the `[MapAdmin]` tag); regular
  profiles never see it (`menu.cpp` + `MgGameLevel01Only` gating). One +use
  tap fires the upload — selecting it runs the trigger and navigation rolls
  back to the list, so there is no submenu in between.

## Commands / cvars

Server console / rcon:

| Command | What it does |
|---|---|
| `stats_status` | matches / players / pending (+ pending map/uploader) |
| `stats_list [n]` | top-n lifetime rows by frags (default 10, max 32) |

Player console (server or map admin, signed-in):

| Word | What it does |
|---|---|
| `.upload_match` | vouch + schedule (or immediate at intermission); sofplus still screenshots |
| `.cancel_upload` | clear pending (both native + sofplus) |

Minigames list (`+use+score` tab, server or map admin, signed-in):

| Row | What it does |
|---|---|
| `stats` | detail page for every tier (same vocabulary as the landing rows) |
| `upload match` | one +use tap schedules the upload (native pending + sofplus cvars + broadcast); schedule-only, no page |

Settings (all `ARCHIVE`, read live):

| Cvar | Default | What it does |
|---|---|---|
| `_sofbuddy_stats` | `1` | master enable (`0` = stock sofplus behaviour, no filtering/recording) |

Gauges (read-only `NOSET`):

| Cvar | What it tells you |
|---|---|
| `_sofbuddy_stats_matches` | recorded uploads (includes pre-restart headers) |
| `_sofbuddy_stats_players` | guids in totals |
| `_sofbuddy_stats_errors` | failed / rejected operations |
| `_sofbuddy_stats_pending` | `1` while an upload is scheduled for this map |

## Wiring (native)

- `stats/callbacks/callbacks.json`: `GameDllLoaded` → `stats_OnGameDllLoaded`
  (cvars, load totals + seq base, `stats_*` server commands, `mg_stats` +
  `mg_upload` registration). Parent feature is `profiles`, so no
  `features.yaml` change.
- `stats/hooks/hooks.json`: `SV_Frame` Post → `stats_SvFramePost` (pending
  flush / stale drop). Multi-callback safe (only overrides are exclusive;
  this is Pre/Post).
- `minigames.cpp`: `MgTakeClientCommand` calls
  `Stats_HandleClientCommand()` before `DispatchMinigameClientCmd` (hence
  before the spsv trampoline = before sofplus), plus
  `stats_EnsureRegistered()` / `stats_MaintainForSlot()` alongside the other
  pages; `MgGameLevel01Only` includes `mg_upload` (`MgGameLevel0Only` keeps
  `mg_cmds` / `mg_cvars` / `mg_lag`, which are server-admin only).
- `menu.cpp`: `Level01Only` includes `mg_upload`; `Level0Only` includes
  `mg_lag`. `mg_stats` is tier-2 (everyone signed-in).
- `profiles/cvar.h`: new `Profiles_SlotGuid` / `Profiles_SlotNickname`
  accessors (same-DLL seam, no duplicated slot state).
- Pure logic in `stats_logic.h` (host-testable, no engine calls).
