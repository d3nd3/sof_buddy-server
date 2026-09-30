# lagometer

Tick **budget breakdown** HUD on the minigames tab (+use+score).

The bar is one 100 ms tick (40 columns). Colours:

| Colour | Segment | Source |
|--------|---------|--------|
| Green | **Game** | `G_RunFrame` wall time (real ticks) |
| Yellow | **Cmd** | Post-tick `Cbuf_Execute` drain (cmdpark) |
| White | **Frame** | Rest of tick `SV_Frame` (net, send, hooks) |
| Black `-` | **Free** | Unused budget |

Shows the **last real tick** (clamp_monitor gate; spawn/settle ticks skipped). Map checksum change resets.

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.

Tests: `tools/tests/lagometer/run.sh`
