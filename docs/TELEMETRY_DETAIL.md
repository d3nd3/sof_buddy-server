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
| `8` | raw `damage` per damage application | **not implemented** | — |
| `16` | raw `weapon_fire` per shot | **not implemented** | — |
| `32` | aggregate `player_weapon_accuracy_stats` | implemented | off |

Bits `8` and `16` are reserved for raw per-damage and per-shot events. They are
**not implemented**; setting them does nothing today. Bit `32` is implemented
as one aggregate event per occupancy, not as a firehose.

### Profiles

**Lean / default (`events = 2`)**

```text
set _sofbuddy_telemetry "1"
set _sofbuddy_telemetry_events "2"
set _sofbuddy_telemetry_sample_sec "0"
```

Collects lifecycle, map start/end, connect/disconnect, spectator transitions,
team changes, map-end player stats, per-weapon frags/deaths, and flag captures.
This is the intended base profile.

**Accuracy (`events = 34`, or `35` with per-death events)**

```text
set _sofbuddy_telemetry_events "34"
```

Adds one `player_weapon_accuracy_stats` event per occupancy at map end or
disconnect, with per-weapon shots, hits, damage events and damage. It does
**not** store every shot or every damage application. Add `1` (`35`) to also
emit per-death events.

**Combat detail (`events = 3`)**

```text
set _sofbuddy_telemetry_events "3"
```

Adds one `player_death` event per death, including MOD, killer, teamkill and
suicide. It does **not** add raw shots, hits or damage. Use it when you want a
kill timeline or later per-kill analysis.

**Samples (`events = 7` plus a cadence, or `39` with accuracy)**

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

- raw per-shot `weapon_fire` events;
- raw per-hit `damage` events;
- per-victim damage attribution or hit timelines;
- per-tick positions, velocity or view angles (the `.sbd` demo recorder covers
  that separately);
- chat, admin commands and votes;
- weapon pickups, ammo, reloads or movement events.

Accuracy itself **is** collected as an aggregate when bit `32` is set. The
existing `player_death` event gives you **who killed whom with which MOD**,
`player_weapon_stats` gives **frags/deaths per weapon**, and
`player_weapon_accuracy_stats` gives **shots/hits/damage per weapon**. Raw
events are only needed for hit timelines or per-victim analysis.

## Accuracy aggregate (implemented)

The producer installs three native hooks when the feature is enabled:

| Hook | RVA | Role |
|---|---:|---|
| `CWorldWeapons::attack` | `0x659C0` | one call = one shot |
| `t_damage` | `0x8BB10` | valid enemy-player damage = hit/damage |
| human body damage | `0x2B8E0` | exact gore zone of each human hit |

`CWorldWeapons::attack` receives an `attacks_e` value (`ATK_SHOTGUN`,
`ATK_ROCKET`, ...), which is mapped to the same weapon keys as
`player_weapon_stats`. `t_damage` receives a MOD value, which is mapped with
`WeaponIndexForMod`.

Rules:

- one `attack` call increments `shots` (one trigger pull for a shotgun, one
  bullet for a machine gun, one flame tick for the flamethrower);
- the first valid damage event after a shot claims one `hits` entry, so shotgun
  pellets and rocket splash cannot push accuracy above 100%;
- all valid damage events increment `damage_events` and add to `damage`, so
  splash weapons can have more damage events than hits;
- self-damage, team damage, world/monster damage and spectator damage are
  excluded;
- the aggregate is emitted once per occupancy at map end or disconnect.

Accuracy is `hits / shots`. This is trigger-pull accuracy for shotguns, not
pellet accuracy; document that when comparing weapons.

### Exact hit zones and headshot kills

The retail human body damage routine (`0x2B8E0`) resolves the hit against the
posed gore bolts and stores the selected front-zone index in the game DLL
global at RVA `0x130650`. The 26 human zone names are at RVA `0x132F78`. The
producer reads both after the body damage call:

- every valid enemy hit increments the exact zone for that weapon, e.g.
  `GZ_HEAD_FRONT`, `GZ_CHEST_FRONT`, `GZ_GROIN`;
- `headshot_hits` is the sum of `GZ_HEAD_FRONT` + `GZ_HEAD_BACK`;
- a kill is counted as a `headshot_kill` only when the victim's most recent
  damage event was a head zone in the same server frame as `player_die`;
- head/neck/torso/groin/limb buckets are derived from the exact zone names.

The zone is resolved by the engine's posed model, not by a raw height guess, so
crouching, leaning and model differences are handled by the gore code. Damage
paths that take a special gore branch (for example some fire/explosion/melee
paths) may not refresh the front-zone global; those hits fall back to the last
resolved zone. Treat exact zones as authoritative for bullet/projectile hits
and approximate for special damage types.

### Existing aggregate payload

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
      {"weapon": "shotgun", "label": "Shotgun", "shots": 40, "hits": 22,
       "damage_events": 22, "damage": 810, "headshot_hits": 6,
       "headshot_kills": 2,
       "zones": [{"zone": "GZ_HEAD_FRONT", "hits": 4},
                 {"zone": "GZ_HEAD_BACK", "hits": 2},
                 {"zone": "GZ_CHEST_FRONT", "hits": 11},
                 {"zone": "GZ_GROIN", "hits": 1}]},
      {"weapon": "rocketlauncher", "label": "Rocket launcher", "shots": 7,
       "hits": 6, "damage_events": 11, "damage": 540,
       "headshot_hits": 1, "headshot_kills": 0,
       "zones": [{"zone": "GZ_HEAD_FRONT", "hits": 1},
                 {"zone": "GZ_CHEST_BACK", "hits": 5}]}
    ],
    "source": "native_telemetry"
  }
}
```

## Adding raw damage / weapon-fire detail later

The aggregate path above already answers accuracy. Add raw events only for hit
timelines and debugging:

1. Reuse the existing `attack` and `t_damage` hooks; do not add a second
   parallel fire path.
2. Decide whether raw events are emitted per shot/damage or sampled. Keep all
   I/O off the game thread and reuse the bounded queue.
3. Map engine attack IDs/MODs to documented weapon keys; never store a raw
   engine index without a mapping.
4. Add bits `8` and `16` to `_sofbuddy_telemetry_events` and document them
   here before shipping.
5. Add a backend projection only when the event shape is stable. Until then
   raw `events` preserves the payload.

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

### Why the aggregate is the default

The aggregate is already wired end to end: producer, migration, ingest
projection, match/career views, public player page and admin player profile.
There is no need to enable raw events just to answer accuracy. Raw events are
only worth adding when you need the individual hit timeline.

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

## Backend behavior for raw events

The accuracy aggregate has a normalized projection. Raw `weapon_fire` and
`damage` events do not; unknown event types are stored verbatim in
`events.payload`. You can query a test session immediately:

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
2. Enable the desired mask. For accuracy use `events = 34` (`2 + 32`); for raw
   events use a small map and watch the spool size.
3. Join with a real client, fire a known number of shots and take damage.
4. Inspect the spool and look for `player_weapon_accuracy_stats` at map end or
   disconnect:

   ```bash
   tail -n 50 <userdir>/telemetry/spool/session-*.jsonl | python3 -m json.tool
   ```

5. Push the spool to a test backend or query `events` directly.
6. Remove test rows and files when done.

`telemetry_selftest` validates the writer/schema but does **not** exercise the
weapon or damage hooks. A real client is required for that.

## Implementation checklist

Accuracy aggregate — done:

- [x] Hook `CWorldWeapons::attack` and `t_damage` in `detours.yaml` and
  `src/features/telemetry/hooks/hooks.json`.
- [x] Map engine attack IDs/MODs to the stable weapon keys.
- [x] Add bit `32` and document it.
- [x] Keep the game thread limited to counters and the bounded queue.
- [x] Emit one summary event at map end/disconnect.
- [x] Add backend migration `010_weapon_accuracy.sql`, ingest projection,
  match/career views, public/admin columns.
- [x] Hook the human body damage routine and map the exact `GZ_*` zone table.
- [x] Count per-weapon headshot hits and killing-blow headshot kills.
- [x] Add backend migration `011_weapon_hitzones.sql`, zone views and UI.
- [x] Validate the event through `telemetry_selftest` and the backend tests.

Raw events — still open:

- [ ] Add bits `8`/`16` for raw `damage` / `weapon_fire`.
- [ ] Confirm the raw hook volume on a populated map before enabling broadly.
- [ ] Add a backend migration and projection when the raw shape is stable.
- [ ] Test on an empty server, then one populated map.
- [ ] Use a real client: `telemetry_selftest` does not fire weapons or damage.

Historical maps cannot be backfilled for data that was never collected. In
particular, accuracy is unavailable for matches played before the accuracy DLL
and bit `32` were enabled.
