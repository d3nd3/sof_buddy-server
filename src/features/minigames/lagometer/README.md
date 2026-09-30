# lagometer

Tick **budget breakdown** HUD on the minigames tab (+use+score).

The bar is one 100 ms tick (40 columns). Colours:

| Colour | Segment | Meaning |
|--------|---------|---------|
| Green | **Play** | Running the match (players, physics, AI) |
| Yellow | **Console** | Server commands / scripts draining after the tick |
| White | **Overhead** | Networking, sends, and other engine work that tick |
| Black `-` | **Free** | Time left in the 100 ms budget |

Keeps the **busiest real tick** since map load (clamp_monitor gate; spawn/settle skipped). Resets on map checksum change.

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.

Tests: `tools/tests/lagometer/run.sh`
