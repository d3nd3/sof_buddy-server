# Native telemetry: hook and data map

> **Status:** design. The `telemetry` feature itself is not implemented yet.
> This document decides where each field/event comes from so the feature does
> not grow two competing data paths.

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
| `map_end` | `G_RunFrame` intermission; `level.intermissiontime`, team scores, flags | `_sp_sv_on_map_end`, `tmp_server.cfg` | **Native.** Compose with the existing `clamp_monitor` override; emit the CTF result from game state, including the deciding flag event. |
| `player_connect` | `ClientConnect` + `ClientBegin`; `ent->client`, userinfo | `_sp_sv_on_client_begin`, `connect\...` log | **Native.** |
| `player_disconnect` | `ClientDisconnect`; snapshot before clearing | `_sp_sv_on_client_disconnect`, `disconnect\...` log | **Native.** Emit final player stats. |
| `team_change` | `ClientUserinfoChanged` / `PutClientInServer`; `gclient->resp.team`, userinfo | `_sp_sv_on_client_userinfo_change`, `_sp_sv_info_client_team` | **Native.** SoFPlus has no dedicated event. |
| `player_match_stats` | Map-end snapshot of `gclient` fields | `sp_sv_info_client <slot>` | **Native reads.** Cross-check with SoFPlus only if a field is hard to read. |
| `player_kill`, `player_death`, `player_suicide`, `player_teamkill` | `player_die` Post; victim/killer, MOD, team | `_sp_sv_on_client_die`, `die\...` log, `sp_sv_info_client_mod` | **Native.** `player_die` is already overridden by `skinteam_quiet`; compose with that override, do not replace it or assume a standalone Post hook runs. |
| `flag_capture`, `flag_return`, `flag_drop` | CTF capture/recover/drop hooks; `ent->ctf_flags`, teams | `_sp_sv_on_ctf_flag_capture`, `flag\...` log, `_sp_sv_info_flag_*` | **Native.** This is also the reliable source for the final cap. |
| `chat`, `chat_team` | `ClientCommand` (`say`, `say_team`) | none | **Native only.** Parse the command before or alongside the existing command dispatch. |
| `admin_command`, `map_vote` | `ClientCommand` / console command path | SoFPlus vote commands | **Native** for the event; SoFPlus only for config. |
| `damage`, `weapon_fire` | Combat functions (optional) | none | **Native optional.** Keep off in the lean profile. |
| `player_sample` | `ClientEndServerFrame` / `G_RunFrame`; `gclient` fields | `sp_sv_info_client` | **Native reads** at a configured cadence. Optional. |
| `server_metrics` | `G_RunFrame`; server/level counters | `sp_sv_info_frames` | **Native.** Optional cadence. |
| GUID / name | Profiles registry + userinfo | `_sp_sv_info_client_guid`, `_sp_sv_info_client_name` | **Native profiles.** Already solved by the profiles feature. |
| Country | IP from `netadr` + GeoIP | `_sp_sv_info_client_country` | **SoFPlus/GeoIP fallback.** Not needed for the lean profile. |
| PPM / anti-cheat state | Optional native counters | `_sp_sv_info_client_ppm_*`, `_sp_sv_info_client_ac` | **SoFPlus fallback.** Derived data, not event-critical. |

## Hook ownership and composition

The telemetry feature must not create a second exclusive override for a
function another feature already owns. Existing owners:

- `player_die` — `skinteam_quiet` override; `minigames/lagometer` Post
  callback. The override currently calls the original directly, so a standalone
  Post callback is not guaranteed to run. Telemetry must compose with the
  override (or the override must dispatch the manager's Post callbacks) before
  relying on this hook.
- `configstring` — `skinteam_quiet` override. If flag configstrings are needed,
  compose through the existing override or read the resulting state; do not
  install another override.
- `ClientCommand` — profiles/minigames/clsv have wrappers on the game export
  slot. Telemetry should observe inside the existing wrapper or use the shared
  callback path; do not swap `ge+0x34` again.
- `ClientBegin`, `ClientUserinfoChanged`, `ClientDisconnect` — profiles and
  lagometer wrappers. Add callbacks or chain through the existing wrapper.
- `G_RunFrame` — `cpu_optimizations/clamp_monitor` override. A standalone Post
  callback will not run while the override is active. Telemetry must be called
  from `clampmon_RunFrame`, or that override must dispatch a telemetry/shared
  Post hook.
- `ClientEndServerFrame` — stufftext/server_demo/minigames callbacks. Add a
  callback with an explicit priority.

If a needed function has no owner, telemetry can own the detour. Otherwise it
is a consumer. The override-dispatch gap is an infrastructure issue worth
fixing once, so Post callbacks work on `player_die` and `G_RunFrame` for every
feature.

## Writer and lifecycle requirements

- One JSON object per line, newline-terminated; never rewrite or truncate the
  active file.
- Fixed-size SPSC ring buffer between the game thread and one writer thread.
  Queue full = drop the event and increment a dropped counter; never block the
  tick.
- Flush and join the writer on `ShutdownGame` / DLL unload.
- `session_uuid` once per DLL/game lifetime.
- `match_uuid` once per `SpawnEntities` / map start.
- Event `uuid` = UUIDv5(session, match, seq, type) or equivalent stable input.
- `seq` monotonic per session; `occurred_at` UTC with milliseconds.
- Rotate or cap the spool (`_sofbuddy_telemetry_max_mb`) and keep the previous
  file for the pusher.

## Suggested cvars

| Cvar | Default | Meaning |
|---|---|---|
| `_sofbuddy_telemetry` | `0` | Master enable. |
| `_sofbuddy_telemetry_dir` | `telemetry/spool` | Spool directory under the user dir. |
| `_sofbuddy_telemetry_max_mb` | `64` | Per-session spool cap. |
| `_sofbuddy_telemetry_queue_kb` | `256` | Bounded in-memory queue. |
| `_sofbuddy_telemetry_sample_sec` | `30` | Player/server sample cadence; `0` = off. |
| `_sofbuddy_telemetry_events` | lean set | Bitmask/individual toggles for chat, combat, flags, samples. |
| `_sofbuddy_telemetry_dropped` | read-only | Events dropped because the queue was full. |

## Delivery phases

1. **Lean lifecycle:** `session_*`, `map_*`, `player_connect/disconnect`,
   `team_change`, `player_match_stats`. No per-kill or per-tick events.
2. **CTF:** native flag capture/recover/drop events and the deciding-cap
   handling; this removes the final-cap ambiguity without relying on
   `_sp_sv_info_num_flags_*`.
3. **Combat:** `player_die` Post → kill/death/suicide/teamkill.
4. **Chat/admin:** `ClientCommand` observer.
5. **Samples/metrics:** configurable cadence.
6. **Optional detail:** damage, weapon fire, per-respawn events.

## Transport

The game host cannot reach the private backend directly. The producer only
writes a local spool. Delivery is either:

- run `sof-db-push` on a host that can reach `/v1/ingest`; or
- add an SSH spool puller on `pi4`, analogous to `sof-db-demosync`, that copies
  completed lines and runs the same pusher.

The spool must remain usable without the backend: a stalled or unreachable
pusher must not affect the game thread.
