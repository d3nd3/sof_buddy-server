# lagometer

One-screen **tick headroom** HUD on the minigames tab (+use+score).

**Spare ms** = `100 − (worst cmd drain + worst tick SV_Frame)` on this map. Higher
is healthier (more time left in the 100 ms tick). Peaks reset when the map checksum
changes.

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer |
| `sofbuddy_lag` / `.lag` | client console | Aliases |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.
Cmd drain uses `_sofbuddy_cmdpark_cbuf_last` (cpu optimizations). SV_Frame time is
wall time for frames where `sv.time` advances.

Tests: `tools/tests/lagometer/run.sh`
