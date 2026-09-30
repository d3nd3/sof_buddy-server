# lagometer

Tick **budget breakdown** HUD on the minigames tab (+use+score).

Aligned with **`SV_Frame`** after the lowclamp early return (`svs.realtime >= sv.time`):

| Colour | Segment | Meaning |
|--------|---------|---------|
| Green | **SV Tick (frame)** | Wall time for that tick’s **`SV_Frame`** body (past the return), minus ClientThink and post-tick console drain |
| Yellow | **Console (buffer)** | Post-tick **`Cbuf_Execute`** drain (cmdpark) |
| White | **ClientThink** | Sum of **`SV_ClientThink`** wall time that tick |
| Black `-` | **Free** | Unused 100 ms budget |

Keeps the **busiest** paired tick since map load. HUD shows **`sv.framenum`**; add init-script frames to `kLagSkipSvFramenum` in `lagometer.cpp`. Resets on map checksum change.

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.

Tests: `tools/tests/lagometer/run.sh`
