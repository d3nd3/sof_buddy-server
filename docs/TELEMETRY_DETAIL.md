# Detailed telemetry collection

This is the practical guide for turning on more detail later. The lean profile
is the default because it covers per-player stats and map winrates without
storing a per-shot firehose. Everything below is explicit about what exists
today versus what still needs code.

## Current collection levels

Lifecycle, map outcome, player/occupancy stats and per-weapon frag/death
counters are always collected when `_sofbuddy_telemetry` is `1`. The
`_sofbuddy_telemetry_events` bitmask only controls the optional families:

| Bit | Event family | Status | Default |
|---:|---|---|---:|
| `1` | `player_death` per death, with victim, killer, MOD, teamkill/suicide | implemented | off |
| `2` | `flag_capture` per capture, including the deciding CTF cap | implemented | on |
| `4` | `player_sample` snapshot at `_sofbuddy_telemetry_sample_sec` | implemented | off |
| `8` | `damage` per damage application | **not implemented** | — |
| `16` | `weapon_fire` per shot | **not implemented** | — |
| `32` | aggregate accuracy/hit stats | **not implemented** | — |

The numbers `8`, `16` and `32` are a proposed allocation, not current
behavior. Do not set them expecting data until the producer code exists.

### Profiles

**Lean / default (`events = 2`)**

```text
set _sofbuddy_telemetry "1"
set _sofbuddy_telemetry_events "2"
set _sofbuddy_telemetry_sample_sec "0"
```

Collects lifecycle, map start/end, connect/disconnect, spectator transitions,
team changes, map-end player stats, per-weapon frags/deaths, and flag captures.
This is the intended production profile.

**Combat detail (`events = 3`)**

```text
set _sofbuddy_telemetry_events "3"
```

Adds one `player_death` event per death, including MOD, killer, teamkill and
suicide. It does **not** add shots, hits, damage or accuracy. Use it when you
want a kill timeline or later per-kill analysis.

**Samples (`events = 7` plus a cadence)**

```text
set _sofbuddy_telemetry_events "7"
set _sofbuddy_telemetry_sample_sec "30"
```

Adds periodic `player_sample` snapshots (frags, deaths, score, flags, ping,
active playtime). `30` seconds is the recommended cadence; `5` seconds is for a
temporary live dashboard, not long-term storage.

Cvars are read live, so changing the mask does not require a restart. The
feature itself must be compiled into the DLL (experimental profile today).

## What is already always on

These are emitted independently of the optional mask:

| Event | Why it exists |
|---|---|
| `session_start`, `session_end` | group one server process lifetime |
| `map_start`, `map_end` | map winrate, caps, duration, end reason |
| `player_connect`, `player_disconnect` | slot occupancy, GUID/IP/ping, active play |
| `spectator_enter`, `spectator_leave` | exclude spectator time from playtime |
| `team_change` | correct team attribution |
| `player_match_stats` | map-end frags, deaths, flags, score, active playtime |
| `player_weapon_stats` | per-weapon MOD frags/deaths, same grouping as SoFPlus `.stats` |

`player_weapon_stats` is event-driven at the source but emitted only once per
occupancy at map end or disconnect. It is not a per-death record.

## What is not collected today

- per-shot `weapon_fire` events;
- per-hit `damage` events;
- shots-fired, hits, damage dealt, accuracy, headshot/hit-location data;
- per-tick positions, velocity or view angles (the `.sbd` demo recorder covers
  that separately);
- chat, admin commands and votes;
- weapon pickups, ammo, reloads or movement events.

The existing `player_death` event gives you **who killed whom with which MOD**,
and `player_weapon_stats` gives **frags/deaths per weapon**. Neither can produce
accuracy: accuracy needs both shots fired and hits.

## Adding damage / weapon-fire detail later

The recommended design is still transition/summary-first:

1. Hook the weapon fire path and the damage path. Add the hook entries to
   `detours.yaml` and the feature's `hooks/hooks.json`, following the existing
   telemetry hooks. Keep all I/O off the game thread.
2. Map each shot/damage to the same weapon key table used by
   `player_weapon_stats` (`knife`, `pistol`, `shotgun`, ...). Avoid storing raw
   engine weapon indexes without a documented mapping.
3. Decide whether to emit raw events, aggregate counters, or both:
   - raw `weapon_fire` / `damage` is useful for a test map, hit timelines and
     debugging;
   - an aggregate `player_weapon_accuracy_stats` event at map end is better for
     long-term storage and public stats.
4. Add new opt-in bits to `_sofbuddy_telemetry_events` and document them here
   before shipping.
5. Add backend projection only when the event shape is stable. Until then the
   raw `events` table preserves the payload.

### Proposed `weapon_fire` event

One event per shot, or per shotgun pellet burst if the engine exposes it as one:

```json
{
  "type": "weapon_fire",
  "match_uuid": "...",
  "slot": 3,
  "player_guid": "...",
  "data": {
    "weapon": "shotgun",
    "weapon_id": 9,
    "shots": 1,
    "ammo_after": 7,
    "source": "native_telemetry"
  }
}
```

### Proposed `damage` event

```json
{
  "type": "damage",
  "match_uuid": "...",
  "slot": 3,
  "player_guid": "...",
  "data": {
    "attacker_slot": 3,
    "victim_slot": 5,
    "attacker_guid": "...",
    "victim_guid": "...",
    "weapon": "shotgun",
    "weapon_id": 9,
    "mod": 9,
    "damage": 27,
    "health_before": 100,
    "health_after": 73,
    "teamkill": false,
    "self_damage": false,
    "source": "native_telemetry"
  }
}
```

### Proposed aggregate event (recommended for long-term)

```json
{
  "type": "player_weapon_accuracy_stats",
  "match_uuid": "...",
  "slot": 3,
  "player_guid": "...",
  "data": {
    "name": "PlayerOne",
    "team": "blue",
    "weapons": [
      {"weapon": "shotgun", "shots": 40, "hits": 22, "damage": 810},
      {"weapon": "rocketlauncher", "shots": 7, "hits": 6, "damage": 540}
    ],
    "active_playtime_sec": 900,
    "source": "native_telemetry"
  }
}
```

Accuracy is `hits / shots`. Shotguns need a documented convention: count each
pellet as a shot and each pellet hit as a hit, or count a trigger pull as one
shot and a hit if at least one pellet connects. Do not mix the two. Splash
damage can produce multiple damage events per shot; decide whether `hits`
means damage events or distinct victims. Document the choice before comparing
weapons.

## Volume and retention

Per-shot and per-damage events are much larger than the lean profile:

- a busy 16-player server can generate tens to hundreds of damage events per
  second, plus shots;
- a 64 MiB spool cap can turn over quickly during a long map;
- `sof-db-spoolsync` pulls every 15 seconds by default, so bursts arrive in
  batches.

For general curiosity, prefer the aggregate event or enable raw events on a
test server for one map. If raw events are enabled in production, monitor spool
rotation, DB growth and ingest latency first.

## Backend behavior until a projection exists

Unknown event types are stored verbatim in `events.payload`. You can query a
test session immediately:

```sql
SELECT occurred_at, event_type, slot, payload
FROM events
WHERE match_uuid = '<match-uuid>'
  AND event_type IN ('weapon_fire', 'damage')
ORDER BY occurred_at, id;
```

Adding a normalized table/view later is a backend migration plus an ingest
branch; the historical raw events can be replayed, so do not delete them.

## Safe test recipe

1. Use an empty test server and the experimental DLL.
2. Enable the desired mask and, for raw events, a small map.
3. Join with a real client, fire a known number of shots and take damage.
4. Inspect the spool:

   ```bash
   tail -n 50 <userdir>/telemetry/spool/session-*.jsonl | python3 -m json.tool
   ```

5. Push the spool to a test backend or query `events` directly.
6. Remove test rows and files when done.

`telemetry_selftest` validates the writer/schema but does **not** exercise the
weapon or damage hooks. A real client is required for that.

## Implementation checklist

- [ ] Add the hook(s) to `detours.yaml` and `src/features/telemetry/hooks/`.
- [ ] Confirm the hook runs on the retail `oldgamex86.dll` used by the server.
- [ ] Map engine weapon IDs/MODs to the stable weapon keys.
- [ ] Add the event mask bits and document them in this file and the README.
- [ ] Keep the game thread limited to serialization and the bounded queue.
- [ ] Emit a summary event at map end/disconnect where possible.
- [ ] Add a backend migration and projection when the shape is stable.
- [ ] Add public/admin UI only after the raw/aggregate data is validated.
- [ ] Test on an empty server, then one populated map, before enabling broadly.

Historical maps cannot be backfilled for data that was never collected.
