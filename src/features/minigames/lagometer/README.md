# lagometer

Live server performance HUD on the minigames layout tab. Reads the same
`_sofbuddy_*` gauges as `sofbuddy_cpuopt_status` (drain last/max, tick
lateness, high/low clamps, cmdpark backlog, tunables).

| Command | Who | Action |
|---------|-----|--------|
| `lag` | client console | Toggle lagometer for yourself |
| `lag_show <slot>` | server | Open for a 0-based slot |

Close with `lag` again, **score** alone (minigame tab), or **+use+score**.

`_sofbuddy_lagometer_enable` (default `1`) gates registration. Requires
`_sofbuddy_minigames_enable 1`.

Tests: `tools/tests/lagometer/run.sh`
