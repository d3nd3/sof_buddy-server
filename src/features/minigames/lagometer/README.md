# lagometer

One-screen **tick headroom** HUD on the minigames tab (+use+score).

**Spare ms** = `100 − worst tick SV_Frame wall time` on this map (cmd drain is
inside that window). Higher
is healthier (more time left in the 100 ms tick). Peaks reset when the map checksum
changes.

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer |
| `sofbuddy_lag` / `.lag` | client console | Aliases |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.
SV_Frame wall time is sampled on frames where `sv.time` advances; the first two
ticks after a map change are ignored (spawn settle).

Tests: `tools/tests/lagometer/run.sh`
