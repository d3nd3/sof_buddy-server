# commands viewer

Server-command catalog on the minigames layout tab. The companion to the
cvars browser (`.mg_cvars`): one in-game page per 8 commands, same
`+use+score` flow. Listed in the `.mg` menu as `server command list`.

| Command | Who | Action |
|---------|-----|--------|
| `.mg_cmds [page\|next\|prev]` | client console (`~`) | Arm + pick page (no arg toggles) |
| `cmds_show <slot> [page]` | server | Arm for a 0-based slot (view needs +use+score) |
| `cmds_list` | server | Dump the full catalog to the server console |

Arming does not open the tab. **+use+score** to view; values refresh while
the tab is open. Pages are per-slot in-memory state — no cvars. Like every
viewer except the menu itself, arming needs a signed-in profile.

`_sofbuddy_cmds_enable` (default `1`) gates registration. Requires
`_sofbuddy_minigames_enable 1`.

Naming policy: `_sofbuddy_` is the public admin prefix (settings + documented
gauges). Internal plumbing must use `_sb_internal_`, never `_sofbuddy_`. This
viewer keeps paging in memory and creates no internal cvars.

Text-only layout (no sprites or ghoul downloads).

Tests: `tools/tests/commands/run.sh`
