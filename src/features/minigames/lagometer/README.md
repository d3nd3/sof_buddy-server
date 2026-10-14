# lagometer

Tick **budget breakdown** HUD on the minigames tab (+use+score).

Eligible ticks are those that pass the lowclamp gate (`svs.realtime >= sv.time` at `SV_ReadPackets` pre) and then advance `sv.time`. Each key is its own clock. They are **not** slices of one stopwatch, so they are not subtracted from each other:

**Clock:** intervals use **thread CPU time** (on Wine/Linux: host `CLOCK_THREAD_CPUTIME_ID`; elsewhere `GetThreadTimes`), not wall `QueryPerformanceCounter`. SoF drains with non-blocking `recvfrom` (no Q2-style `select` in `NET_GetPacket`); wall QPC still counted **host preemption** and other off-CPU gaps inside a timed region as if it were engine work. Log: `thread-cpu (linux cputime) (wine)` when applicable.

| Colour | Segment | Clock |
|--------|---------|--------|
| Green | **Sim** | `G_RunFrame` body (clamp_monitor), inside `SV_RunGameFrame` |
| Red | **Engine** | `SV_ReadPackets` since the previous tick, excluding **Move**, plus `SV_ReadPackets` post → `SV_RunGameFrame` post minus **Sim**, plus **`CL_SendClientMessages`**. One red **Eng** value on the raw line and in the bar |
| White | **Move (clc_move)** | Sum of `SV_ExecuteClientMessage` @ `0x200636C0` since the previous tick: the full per-client packet dispatch, i.e. checksum + `lastServerFrame` + `InPacket` decrypt + 3x `MSG_ReadDeltaUserCmd` + `COM_BlockSequenceCRCByte` + every `SV_ClientThink` for that packet. `SV_Frame` reads packets on calls that do not run a game tick; those moves are kept for the tick that commits |
| Yellow | **Buffer** | cmdpark **post-tick** `Cbuf_Execute` drain only (QPC wall in cmdpark, not thread CPU). Sim/Eng/Move use thread CPU, so Buf can look “chunky” (e.g. ~16 ms) while others stay sub-ms. Sampled after cmdpark `SV_Frame` post (priority 9), before lagometer commit (priority 1). |
| Black `-` | **Free** | Unused 100 ms budget |

**Wall** on the HUD is the sum of those keys. Highclamp log/broadcast runs after the engine window and is not included.

Connect/readpacket work on player join stays **outside** the timed window (before `SV_ReadPackets` post); those tags use **sticky** (±4 `sv.framenum`) and merge onto the commit tick. Map load: **`mapspawn`**, **`mapchg`**, optional **`settle`** / **`framenum_gap`** tags. After each map change, the first **`_sofbuddy_lagometer_warmup_frames`** eligible ticks are **not** considered for “busiest” (map-init spikes ignored). **Live mode still updates** during warmup. **Highclamp** appears only when that stored tick actually clamped: `highclamp  dropped N ms`.

Keeps the **10 heaviest** eligible ticks since map load (by **raw wall ms**). Rank **#1** is the heaviest. The menu page starts at the heaviest tick and exposes `previous page` and `next page` actions. Hold `+use` with `+forward` / `+back` to select an action and tap `+use` to activate it; hold `+use` and press `+moveleft` to return to the previous pushed menu. Live mode follows the newest tick and is the only minigame view pushed every tick. Live never paints a zero-filled placeholder (keeps the last good canvas until a tick with `raw_total > 0`). Move/Eng below the `%.1f` display floor keep the previous values for up to 3 ticks (and same-`sv.framenum` re-reads are idempotent) so those keys do not flash `0.0`; sustained zeros still show. Every other rank waits for the normal layout refresh.

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
| `lag_show <slot>` | server | Arm slot (0-based) |

The lagometer page itself is server-admin only (privilege `0`): other
profiles never see it in the minigames list, and opening it without that
privilege is refused.

While the lagometer page is open, hold `+use` and use its page actions.

Requires `_sofbuddy_minigames_enable 1` and `_sofbuddy_lagometer_enable 1`.

| Cvar | Default | Meaning |
|------|---------|---------|
| `_sofbuddy_lagometer_warmup_frames` | `200` | Count of **eligible ticks** to skip for “busiest” after each `mapchg` (`0` = off). **Not** “hide framenum &lt; N”. Read on map change; **raising** it mid-map (above the value at last `mapchg`) re-arms skip and clears busiest. Countdown ticks normally to zero. |

### Debug mode (wall vs cpu — ping spikes)

Bars stay on **thread CPU**. Debug adds **QPC wall** for the same commit window.

| HUD | Meaning |
|-----|---------|
| **gap** | `wall − cpu` for the whole tick — time the thread was not executing while hooks were open (host sched / Wine). |
| **rpC / rpW / rpG** | CPU vs wall inside **`SV_ReadPackets`**; **rpG** = sched inside that function. |
| **rpN** | How many **`SV_ReadPackets`** visits summed into this commit (often &gt;1 between `sv.time` steps). |
| **pkt** | Reserved (no per-packet hook by default); use move/rpN for load. |
| **move** | **`SV_ExecuteClientMessage`** count (move dispatches). |
| **shellG** | Sched wall in ReadPackets **outside** Move (`(rp−move)` wall − cpu) — client scan, `Netchan_Process`, drain loop. |

When **many players** load the server, **pkt** and **move** rise. If **rpG** / **shellG** rise with them, the main thread is **waiting on the host** inside net processing — inputs and acks are late → **everyone’s ping jumps** even though CPU bars look fine. Server log (throttled): `[lagometer] sched …` when **gap ≥ 20 ms**.

Not `NET_GetPacket` blocking sleep: SoF uses non-blocking `recv`; the gap is **off-CPU** or long wall inside ReadPackets without matching CPU.

Tests: `tools/tests/lagometer/run.sh`
