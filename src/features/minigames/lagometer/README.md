# lagometer

Tick **budget breakdown** HUD on the minigames tab (+use+score).

Samples start **after `SV_ReadPackets`** on ticks that pass the lowclamp gate (`svs.realtime >= sv.time`), through **`SV_Frame` post** when `sv.time` advances:

| Colour | Segment | Meaning |
|--------|---------|---------|
| Green | **Sim** | **`G_RunFrame`** wall time (via clamp_monitor hook) |
| Red | **Engine** | Rest of the tick body (sends, net, etc.) minus sim, ClientThink, buffer |
| White | **ClientThink** | Sum of **`SV_ClientThink`** that tick |
| Yellow | **Buffer** | Post-tick **`Cbuf_Execute`** drain (cmdpark) |
| Black `-` | **Free** | Unused 100 ms budget |

Connect/readpacket work on player join stays **outside** the window (it runs before the timer starts). First **3** continuous ticks after a map change are skipped (settle), same spirit as clamp_monitor.

Keeps the **busiest** eligible tick since map load. HUD shows **`sv.framenum`**; add bad init frames to `kLagSkipSvFramenum` in `lagometer.cpp`.

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.

Tests: `tools/tests/lagometer/run.sh`
