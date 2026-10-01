# commands viewer

Server-command catalog on the minigames layout tab. The companion to the
cvars viewer: one in-game page per 8 commands, same `+use+score` flow as
`lagometer`.

| Command | Who | Action |
|---------|-----|--------|
| `cmds [page\|next\|prev]` | client console (`~`) | Arm + pick page (no arg toggles) |
| `commands [page\|next\|prev]` | client console | Alias, same handler |
| `cmds_show <slot> [page]` | server | Arm for a 0-based slot (view needs +use+score) |
| `cmds_list` | server | Dump the full catalog to the server console |

Arming does not open the tab. **+use+score** to view; **score** alone closes;
`cmds` again disarms. Pages are per-slot in-memory state — no cvars.

`_sofbuddy_cmds_enable` (default `1`) gates registration. Requires
`_sofbuddy_minigames_enable 1`.

Naming policy: `_sofbuddy_` is the public admin prefix (settings + documented
gauges). Internal plumbing must use `_sb_internal_`, never `_sofbuddy_`. This
viewer keeps paging in memory and creates no internal cvars.

Text-only layout (no sprites or ghoul downloads).

Tests: `tools/tests/commands/run.sh`
