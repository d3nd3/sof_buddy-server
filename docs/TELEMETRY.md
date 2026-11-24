# Native telemetry: hook and data map

> **Status:** implemented as the experimental `telemetry` feature
> (`src/features/telemetry/`). The lean profile is live: session/map
> lifecycle, connect/disconnect, team change, map-end player stats, and
> optional death, flag-capture and player-sample events. Chat, admin/vote,
> damage and weapon-fire events are still unimplemented.
>
> This document decides where each field/event comes from so the feature does
> not grow two competing data paths. `src/features/telemetry/README.md` is the
> operational reference (cvars, spool layout, thread model).

## Decision

**Native hooks are the primary telemetry source. SoFPlus cvars/commands are a
fallback and a low-cadence snapshot source, not the event bus.**

Use native hooks when the data is:

- an event (connect, disconnect, map start/end, kill, flag capture, chat);
- needed immediately, in order, or with a precise actor/slot;
- a counter that SoFPlus is known to miss or update late (notably the deciding
  CTF cap).

Use SoFPlus cvars/commands when the data is:

- already derived by SoFPlus and expensive for us to recreate (country, PPM,
  anti-cheat GUID, some scoreboard fields);
- needed only as a periodic snapshot (map-end scoreboard, player samples);
- a compatibility path for servers that do not run our DLL or do not enable the
  telemetry feature.

Do **not** poll SoFPlus cvars every tick. `sp_sv_info_client <slot>` sets ~30
cvars, does string formatting and GeoIP work, and is not an event source. Do
**not** use `_sp_sv_info_num_flags_*` as the authoritative CTF result; the
counter can miss the final cap.

## Implemented surface (lean)

| Event | Hook / read | Optional |
|---|---|---|
| `session_start`, `session_end` | `GetGameAPI` / `Telemetry_Shutdown` | no |
| `map_start` | `SpawnEntities` post, deferred to the first non-spectator client | no |
| `map_end` | `SV_Frame` post + `level.intermissiontime`, map change, shutdown | no |
| `player_connect` | `ClientBegin` / `PutClientInServer` post, one per map occupancy | no |
| `player_disconnect` | `ClientDisconnect` pre (snapshot before the slot clears) | no |
| `spectator_enter`, `spectator_leave` | `PutClientInServer` post when `resp.spectator` changes | no |
| `team_change` | `ClientUserinfoChanged` post, active players only | no |
| `player_match_stats` | map-end snapshot beside `map_end` | no |
| `player_death` | `player_die` post | bit `1` |
| `flag_capture` | `ctf_base_touch` override, counts the deciding cap | bit `2` |
| `player_sample` | `SV_Frame` post at `_sofbuddy_telemetry_sample_sec` | bit `4` |

Every event is one JSON object per line in
`<userdir>/<dir>/session-<uuid>.jsonl`. The backend contract is
`sof-db-backend/docs/NATIVE_TELEMETRY.md`; the producer never calls the
backend.

`telemetry_selftest` queues one synthetic session/map/player/flag/death/stats
sequence for schema and writer validation. It is for an empty test server, not
a populated production map.

## Stats, spectator time, and labelling invariants

The database exists to answer two questions: **per-player stats** and
**red/blue winrates per map**. That means transition snapshots, not a
per-tick/per-shot firehose:

- Persist at connect, disconnect, spectator enter/leave, team change, map
  start/end, and map-end or disconnect stats. Samples stay off by default.
- Sum and average in the backend. Per-kill detail remains optional; a flag
  capture stays event-level because it is the only evidence for the deciding
  CTF cap.
- Every native player event carries `guid`, `ip`, `ping`, team, and the current
  cumulative counters. The labelling heuristics need these at the occupancy
  boundary, not a reconstructed log.

The spectator rules are deliberate:

- `playtime_sec` / `active_playtime_sec` count only time spent **not** in
  spectator. Spectating never inflates practice time or per-match averages.
- `player_match_stats` includes the final `spectator` flag and the active
  playtime, so a player who ends the map in spec still contributes the stats
  from the time they actually played.
- SoF resets `gclient_t.resp.score` in `spectator_respawn`, while frags,
  deaths and flags are cumulative. The producer keeps `score_total` by summing
  scoring segments; it must not treat the post-spectate score as the map total.
- A spectate enter/leave does **not** create a new identity or a new label
  occupancy. One connect stays one label instance, updated with active time
  and the cumulative counters; spectate-only instances are excluded by the
  backend.

The backend projection writes native connect/state/stats events into
`match_slot_instances`, so manual labels and the IP/country/ping heuristics work
on native-only matches without waiting for `games.log`.

## What SoFPlus actually hooks

The names below are from the 20140531 `spsv.dll` IDA database. They are useful
as a reference for which game/engine moments matter, not as ABI we should call.

| SoFPlus hook | What it observes / maintains |
|---|---|
| `ClientConnect_HOOK` | Connection start, IP, connect log |
| `ClientBegin_HOOK` | Client finished loading; player/spectator counts; connect event |
| `ClientUserinfoChanged_HOOK` | Name/team/skin/userinfo changes; FOV, spectator, flood guard |
| `ClientDisconnect_HOOK` | Disconnect; player/spectator counts; full stats log |
| `ClientCommand_HOOK` | `.commands`, votes, say/say_team pass-through; no generic chat event |
| `SpawnEntities_HOOK` | Map start; map/entity setup; resets per-map counters |
| `ShutdownGame_HOOK` | Game DLL shutdown |
| `G_RunFrame_HOOK` | Tick; intermission/map end; `end` + `stats` logs; map cvars |
| `PLAYERDIE_HOOK` | Deaths, suicides, teamkills, frags, MOD counters; `die` log |
| `flagCaptured_HOOK` | Team flag counters, per-player flag captures; `flag` log |
| `ctfFlagRecovered_hook` | Per-player flag recoveries |
| `gi_configstring_HOOK` | `CS_CTF_BLUE_STAT` / `CS_CTF_RED_STAT` to `_sp_sv_info_flag_*` |
| `ClientThink_HOOK` | Client frame/think accounting |

SoFPlus exposes snapshots through `sp_sv_info_client <slot>` and
`sp_sv_info_frames`, and script events through `_sp_sv_on_*`. It has no event
for chat, team change, damage, or weapon fire.

## Hook and data matrix

| Data / event | Native hook or read | SoFPlus equivalent | Choice |
|---|---|---|---|
| `session_start` / `session_end` | `GetGameAPI` / `ShutdownGame`; DLL load/unload | none (implicit) | **Native.** Generate `session_uuid` here. |
| `map_start` | `SpawnEntities`; map name, cvars, `level` | `_sp_sv_on_map_begin`, `_sp_sv_info_map_current`, `_sp_sv_info_map_count` | **Native.** Generate `match_uuid` here. |
| `map_end` | `SV_Frame` Post + `level.intermissiontime`; team scores, flags | `_sp_sv_on_map_end`, `tmp_server.cfg` | **Native.** Emit the CTF result from game state, including the deciding flag event. |
| `player_connect` | `ClientConnect` + `ClientBegin`; `ent->client`, userinfo | `_sp_sv_on_client_begin`, `connect\...` log | **Native.** |
| `player_disconnect` | `ClientDisconnect`; snapshot before clearing | `_sp_sv_on_client_disconnect`, `disconnect\...` log | **Native.** Emit final player stats. |
| `team_change` | `ClientUserinfoChanged` / `PutClientInServer`; `gclient->resp.team`, userinfo | `_sp_sv_on_client_userinfo_change`, `_sp_sv_info_client_team` | **Native.** SoFPlus has no dedicated event. |
| `player_match_stats` | Map-end snapshot of `gclient` fields | `sp_sv_info_client <slot>` | **Native reads.** Cross-check with SoFPlus only if a field is hard to read. |
| `player_kill`, `player_death`, `player_suicide`, `player_teamkill` | `player_die` Post; victim/killer, MOD, team | `_sp_sv_on_client_die`, `die\...` log, `sp_sv_info_client_mod` | **Native.** `skinteam_quiet` keeps the override and now dispatches the shared Post callbacks. |
| `flag_capture` | `ctf_base_touch` override; `ent->ctf_flags`, teams | `_sp_sv_on_ctf_flag_capture`, `flag\...` log, `_sp_sv_info_flag_*` | **Native.** This is also the reliable source for the final cap. Recover/drop are not emitted yet. |
| `chat`, `chat_team` | `ClientCommand` (`say`, `say_team`) | none | **Native only.** Not implemented in the lean profile. |
| `admin_command`, `map_vote` | `ClientCommand` / console command path | SoFPlus vote commands | **Native** for the event; SoFPlus only for config. |
| `damage`, `weapon_fire` | Combat functions (optional) | none | **Native optional.** Keep off in the lean profile. |
| `player_sample` | `SV_Frame` Post; `gclient` fields | `sp_sv_info_client` | **Native reads** at the `_sofbuddy_telemetry_sample_sec` cadence. Optional. |
| `server_metrics` | `G_RunFrame`; server/level counters | `sp_sv_info_frames` | **Native.** Optional cadence. |
| GUID / name | Profiles registry + userinfo | `_sp_sv_info_client_guid`, `_sp_sv_info_client_name` | **Native profiles.** Already solved by the profiles feature. |
| Country | IP from `netadr` + GeoIP | `_sp_sv_info_client_country` | **SoFPlus/GeoIP fallback.** Not needed for the lean profile. |
| PPM / anti-cheat state | Optional native counters | `_sp_sv_info_client_ppm_*`, `_sp_sv_info_client_ac` | **SoFPlus fallback.** Derived data, not event-critical. |

## Hook ownership and composition

The telemetry feature must not create a second exclusive override for a
function another feature already owns. Current composition:

- `ClientBegin`, `ClientUserinfoChanged`, `ClientDisconnect` — telemetry
  detours the **stock function body**, not the `game_export_t` slot. Profiles
  and lagometer keep their export wrappers, and their wrappers still reach the
  body, so both observe every transition.
- `PutClientInServer` — lagometer already uses a Post callback; telemetry adds
  a second Post callback with priority 10. It initializes the per-map occupancy
  on the first spawn and emits spectator transitions when `resp.spectator`
  changes.
- `player_die` — `skinteam_quiet` owns the override. Its override now
  dispatches the shared manager's Post callbacks after the real death, so the
  telemetry (and lagometer) Post callback runs. Do not call the original
  directly from an override without dispatching Post.
- `ctf_base_touch` — telemetry owns the override. SoFPlus replaces the spawned
  base's touch pointer with its own wrapper and then calls the original body,
  so this override still sees every capture, including the deciding cap.
- `SV_Frame` — telemetry is a Post callback alongside clamp_monitor and
  minigames. It deliberately does not compose with the `G_RunFrame` override:
  intermission and sample cadence are observable from `SV_Frame` after the
  game frame has run.
- `configstring` — still owned by `skinteam_quiet`; telemetry does not need it
  in the lean profile.
- `ClientCommand` — still wrapped by profiles/minigames/clsv. Telemetry does
  not observe it yet (chat/admin/vote events are unimplemented).
- `ClientEndServerFrame` — owned by stufftext/server_demo/minigames. Telemetry
  does not use it.

If a needed function has no owner, telemetry can own the detour. Otherwise it
is a consumer. Any future override must dispatch the shared Post callbacks, or
the Post subscriber silently stops running.

## Writer and lifecycle behavior

- One JSON object per line, newline-terminated; the active file is only
  appended and flushed.
- Bounded `std::deque` of complete lines between the game thread and one
  writer thread. Queue full = drop the event and increment a dropped counter;
  the tick is never blocked. Errors and drops are reported once on shutdown.
- Flush and join the writer on `ShutdownGame` / DLL unload.
- `session_uuid` once per DLL/game lifetime.
- `match_uuid` once per `SpawnEntities`; only attached to events emitted while
  the match is active, so a late disconnect cannot join the next map.
- Event `uuid` = fresh UUIDv4 per event; duplicate delivery is handled by the
  pusher's batch offset/idempotency, not by event identity.
- `seq` monotonic per session; `occurred_at` UTC with milliseconds.
- Rotate the spool when `_sofbuddy_telemetry_max_mb` is reached; previous
  files stay for the pusher. `sof-db-push` reads by byte offset, so a partial
  final line is ignored until complete.

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `_sofbuddy_telemetry` | `0` | Master enable. Live. |
| `_sofbuddy_telemetry_dir` | `telemetry/spool` | Spool directory under the user dir. Read at DLL load. |
| `_sofbuddy_telemetry_max_mb` | `64` | Per-file cap; `0` = unlimited. Read at DLL load. |
| `_sofbuddy_telemetry_queue_kb` | `256` | Bounded in-memory queue (16–4096). Read at DLL load. |
| `_sofbuddy_telemetry_sample_sec` | `30` | Player sample cadence; `0` = off. Live. |
| `_sofbuddy_telemetry_events` | `2` | Bitmask: `1` deaths, `2` flag captures, `4` samples. Live. |

## Delivery phases

1. **Lean lifecycle:** implemented in the experimental feature.
2. **CTF:** implemented for captures, including the deciding cap. Recover/drop
   events are not emitted yet.
3. **Combat:** implemented for `player_die` (death/suicide/teamkill).
4. **Chat/admin:** not implemented.
5. **Samples/metrics:** player samples implemented; server metrics are not.
6. **Optional detail:** damage, weapon fire and per-respawn events are not
   implemented.

## Transport

The game host cannot reach the private backend directly. The producer only
writes a local spool. Delivery is either:

- run `sof-db-push` on a host that can reach `/v1/ingest`; or
- add an SSH spool puller on `pi4`, analogous to `sof-db-demosync`, that copies
  completed lines and runs the same pusher.

The spool must remain usable without the backend: a stalled or unreachable
pusher must not affect the game thread.
