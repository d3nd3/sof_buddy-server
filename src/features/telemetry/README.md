# telemetry

Native JSONL event stream for `sof-db-backend`.

## What it emits

| Event | When |
|---|---|
| `session_start` / `session_end` | Game DLL load/unload |
| `map_start` | First non-spectator client on a map |
| `map_end` | Intermission, map change, or server shutdown |
| `player_connect` | First spawn of a map occupancy |
| `player_disconnect` | Before `ClientDisconnect` clears the slot |
| `spectator_enter` / `spectator_leave` | `resp.spectator` changes in `PutClientInServer` |
| `team_change` | `ClientUserinfoChanged` changes `resp.team` while active |
| `player_match_stats` | Map end, one per connected slot |
| `player_weapon_stats` | Map end or disconnect, per-weapon MOD frags/deaths |
| `player_weapon_accuracy_stats` | Map end or disconnect, per-weapon shots/hits/damage (optional, bit 32) |
| `player_death` | `player_die` Post (optional, `events` bit 1) |
| `flag_capture` | `ctf_base_touch` succeeds (optional, bit 2) |
| `player_sample` | Configured cadence (optional, bit 4) |

Every line is one event:

```json
{"uuid":"...","type":"map_start","occurred_at":"...","seq":42,
 "session_uuid":"...","match_uuid":"...","data":{"map":"dm/gerctf1",...}}
```

## Files

```text
<userdir>/<dir>/session-<session-uuid>.jsonl
```

`dir` defaults to `telemetry/spool`. `sof-db-push` reads `*.jsonl` by byte
offset, so a partially written final line is safely ignored until it is
complete. Rotation uses `session-<uuid>-1.jsonl`, `-2`, ... when
`_sofbuddy_telemetry_max_mb` is reached.

## Threading and failure behavior

- The game thread builds the complete JSON line and appends it to a bounded
  in-memory queue. It never opens or writes the spool file.
- One writer thread owns the file. It drains the queue, flushes, and rotates.
- Queue full = drop the event and increment the drop counter. The tick is
  never blocked by disk or backend failure.
- `ShutdownGame` / DLL detach flushes the queue and joins the writer.
- No database, DNS, or socket call is made by this feature.

## Cvars

| Cvar | Default | Meaning |
|---|---|---|
| `_sofbuddy_telemetry` | `0` | Master switch. Set to `1` in the server config. |
| `_sofbuddy_telemetry_dir` | `telemetry/spool` | Directory under the engine userdir. Read at DLL load. |
| `_sofbuddy_telemetry_max_mb` | `64` | Per-file cap; `0` = unlimited. |
| `_sofbuddy_telemetry_queue_kb` | `256` | Bounded in-memory queue. |
| `_sofbuddy_telemetry_events` | `2` | Bitmask: `1` combat/death, `2` flags, `4` player samples, `32` accuracy aggregates. |
| `_sofbuddy_telemetry_sample_sec` | `30` | Sample cadence; `0` = off. |

Enable it in `dedicated.cfg`:

```text
set _sofbuddy_telemetry "1"
set _sofbuddy_telemetry_dir "telemetry/spool"
set _sofbuddy_telemetry_events "2"
```

## Hook composition

Telemetry deliberately observes existing ownership instead of replacing it:

- `ClientBegin`, `ClientUserinfoChanged`, `ClientDisconnect` are body detours;
  profiles/lagometer keep their `game_export_t` wrappers.
- `PutClientInServer` is a Post callback shared with lagometer. It starts the
  per-map occupancy on the first spawn and emits spectator transitions.
- `player_die` is a Post callback; `skinteam_quiet`'s override dispatches it.
- `CWorldWeapons_attack` and `t_damage` are Pre callbacks used only by the
  optional accuracy aggregate. They count in RAM and emit nothing per shot.
- `SV_Frame` is a Post callback alongside clamp_monitor/minigames.
- `ctf_base_touch` is a telemetry override because no other feature owns it.
  SoFPlus replaces the spawned base's touch pointer but calls the original
  body, so this still sees every capture, including the deciding cap.

## Detailed collection later

The default profile is intentionally lean. For the current optional families,
what is not implemented yet, and how accuracy is aggregated without storing a
per-shot firehose, see
[`docs/TELEMETRY_DETAIL.md`](../../../docs/TELEMETRY_DETAIL.md).

Current mask summary:

| Bit | Family | Status |
|---:|---|---|
| `1` | per-death `player_death` | implemented |
| `2` | `flag_capture` | implemented |
| `4` | `player_sample` cadence | implemented |
| `8` | raw `damage` | planned, no-op today |
| `16` | raw `weapon_fire` | planned, no-op today |
| `32` | `player_weapon_accuracy_stats` aggregate | implemented |

## Score, spectator and labelling

- `active_playtime_sec` / `playtime_sec` exclude spectator time.
- `score_total` sums score segments because SoF resets `resp.score` in
  `spectator_respawn`; frags, deaths and flags remain cumulative.
- One connect is one label occupancy. Spectator enter/leave updates it rather
  than creating a new identity; a spec-only occupancy has zero active time
  and is excluded from match stats.
- Per-weapon MOD counters follow the SoFPlus `.stats` grouping. Victim deaths
  count for every MOD; attacker frags count only for non-teamkill kills;
  teamkills remain in the separate teamkill total.
- Snapshots include GUID, IP and ping so the backend labelling heuristics can
  attach a player without relying on `games.log`.

## Delivering the spool

The game host only writes local files. On the current private backend, run
`sof-db-push` on a host that can reach `/v1/ingest`, or add an SSH spool puller
on the collector host analogous to `sof-db-demosync`. Do not add database or
network code to this feature.

## Verifying

```text
_sofbuddy_telemetry 1
map dm/gerctf1
```

On an empty test server, `telemetry_selftest` queues one synthetic
session/map/player/flag/death/stats sequence so the writer and backend schema
can be checked without a real client. It does not read the live match, so do
not run it on a populated production server.

Then inspect the spool:

```bash
ls -l /path/to/userdir/telemetry/spool
tail -n 5 /path/to/userdir/telemetry/spool/session-*.jsonl | python3 -m json.tool
```

A map with no players deliberately produces no match row: `map_start` is
emitted on the first player event, and an empty map end is skipped.
