# lagometer

Live server performance HUD on the minigames layout tab. Reads the same
`_sofbuddy_*` gauges as `sofbuddy_cpuopt_status` (drain last/max, tick
lateness, high/low clamps, cmdpark backlog, tunables).

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console (`~`) | Toggle lagometer for yourself |
| `sofbuddy_lag` / `.lag` | client console | Aliases registered the same way |
| `lag_show <slot>` | server | Arm for a 0-based slot (view needs +use+score) |

Client words are handled in the shim **before** stock `ClientCommand`. If the game
is not registered, stock DM would treat unknown commands as **chat** (`Cmd_Say_f`)
— that is the “sent as text” behaviour, not a client forward issue.

**+use+score** opens the minigame tab with the lagometer by default (when enabled
and no other minigame owns that slot). Updates at scoreboard rate
(`ClientEndServerFrame`, not every server tick). Close tab with **score** alone.
Client `lag` still toggles arm/disarm when you want the HUD off without another
game running.

Sofplus/spsv may deliver dot-commands as `say .word …`; the platform unwraps
that before matching registered client words.

`_sofbuddy_lagometer_enable` (default `1`) gates registration. Requires
`_sofbuddy_minigames_enable 1`.

Text-only layout (no `sb/mg/*` sprites or ghoul downloads).

Tests: `tools/tests/lagometer/run.sh`
