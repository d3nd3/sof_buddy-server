# lagometer

Live server performance HUD on the minigames layout tab. Reads the same
`_sofbuddy_*` gauges as `sofbuddy_cpuopt_status` (drain last/max, tick
lateness, high/low clamps, cmdpark backlog, tunables).

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console (`~`) | Toggle lagometer for yourself |
| `sofbuddy_lag` / `.lag` | client console | Aliases registered the same way |
| `lag_show <slot>` | server | Open for a 0-based slot |

Client words are handled in the shim **before** stock `ClientCommand`. If the game
is not registered, stock DM would treat unknown commands as **chat** (`Cmd_Say_f`)
— that is the “sent as text” behaviour, not a client forward issue.

Close with `lag` again, **score** alone, or **+use+score**.

`_sofbuddy_lagometer_enable` (default `1`) gates registration. Requires
`_sofbuddy_minigames_enable 1`.

Text-only layout (no `sb/mg/*` sprites or ghoul downloads).

Tests: `tools/tests/lagometer/run.sh`
