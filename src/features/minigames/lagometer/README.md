# lagometer

Tick **budget breakdown** HUD on the minigames tab (+use+score).

Eligible ticks are those that pass the lowclamp gate (`svs.realtime >= sv.time` at `SV_ReadPackets` pre) and then advance `sv.time`. Each key is its own clock (QPC). They are **not** slices of one stopwatch, so they are not subtracted from each other:

| Colour | Segment | Clock |
|--------|---------|--------|
| Green | **Sim** | `G_RunFrame` body (clamp_monitor), inside `SV_RunGameFrame` |
| Red | **Engine** | `SV_ReadPackets` since the previous tick, excluding **Move**, plus `SV_ReadPackets` post → `SV_RunGameFrame` post minus **Sim**, plus **`CL_SendClientMessages`**. One red **Eng** value on the raw line and in the bar |
| White | **Move (clc_move)** | Sum of `SV_ExecuteClientMessage` @ `0x200636C0` since the previous tick: the full per-client packet dispatch, i.e. checksum + `lastServerFrame` + `InPacket` decrypt + 3x `MSG_ReadDeltaUserCmd` + `COM_BlockSequenceCRCByte` + every `SV_ClientThink` for that packet. `SV_Frame` reads packets on calls that do not run a game tick; those moves are kept for the tick that commits |
| Yellow | **Buffer** | cmdpark post-tick `Cbuf_Execute` drain, sampled **before** the lagometer commit (`SV_Frame` post priority 1, after cmdpark’s priority 9) |
| Black `-` | **Free** | Unused 100 ms budget |

**Wall** on the HUD is the sum of those keys. Highclamp log/broadcast runs after the engine window and is not included.

Connect/readpacket work on player join stays **outside** the timed window (before `SV_ReadPackets` post); those tags use **sticky** (±4 `sv.framenum`) and merge onto the commit tick. Map load: **`mapspawn`**, **`mapchg`**, optional **`settle`** / **`framenum_gap`** tags. After each map change, the first **`_sofbuddy_lagometer_warmup_frames`** eligible ticks are **not** considered for “busiest” (map-init spikes ignored). **Highclamp** appears only when that stored tick actually clamped: `highclamp  dropped N ms`.

Keeps the **10 heaviest** eligible ticks since map load (by **raw wall ms**). Rank **#1** is the heaviest. Each player picks a rank on their own: `.mg_lag max` (heaviest), `.mg_lag min` (lightest of the ten), `.mg_lag next`, `.mg_lag prev`. `.mg_lag live` follows the newest tick instead, and is the only minigame view pushed every tick. Every other rank waits for the normal layout refresh.

| Tag | Source |
|-----|--------|
| `mapspawn` | `SpawnEntities` @ `0x500BDB50` (existing gamex86 shim) |
| `mapchg` | minigames map checksum changed |
| `settle` / `framenum_gap` | first ticks after map change or `sv.framenum` discontinuity |
| `moveN` | `N` `SV_ExecuteClientMessage` dispatches when **Move** ≥ 40 ms |
| `preconnect` / `connect` / `spawn` / `userinfo` / `disconnect` | `game_export_t` +0x20…+0x30, chained after profiles |
| `putclient` | `PutClientInServer` @ `0x500F38A0` |
| `respawn` | `respawn` @ `0x500F3550` |
| `death` | `player_die` @ `0x500F2A30` |

On **spsv**, `ClientBegin_HOOK` replaces `ge+0x28` for SoFPlus stats and does **not** call game `ClientBegin`; use **`putclient`** / **`respawn`**. Multiple tags append with `+` (48-char cap). Add bogus init frames to `kLagSkipSvFramenum` in `lagometer.cpp` if needed.

| Command | Who | Action |
|---------|-----|--------|
| `.mg_lag` | client | Toggle lagometer |
| `.mg_lag live` | client | Newest tick, updated as it happens |
| `.mg_lag max` / `min` / `next` / `prev` | client | Heaviest, lightest of the ten, lighter, heavier |
| `lag_show <slot>` | server | Arm slot (0-based) |

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.

| Cvar | Default | Meaning |
|------|---------|---------|
| `_sofbuddy_lagometer_warmup_frames` | `200` | Count of **eligible ticks** to skip for “busiest” after each `mapchg` (`0` = off). **Not** “hide framenum &lt; N”. Read on map change; **raising** it mid-map (above the value at last `mapchg`) re-arms skip and clears busiest. Countdown ticks normally to zero. |

Tests: `tools/tests/lagometer/run.sh`
