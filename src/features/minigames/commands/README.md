# commands viewer

Server-side command catalog on the minigames layout tab. Opening
`server-side commands` first shows the category selector; choosing a category
shows one in-game page per 8 commands.

| Command | Who | Action |
|---------|-----|--------|
| `cmds_show <slot> [page]` | server | Arm for a 0-based slot (view needs +use+score) |
| `cmds_list` | server | Dump the full catalog to the server console |

The viewer is menu-only. **+use+score** opens the category selector; hold
Tap and release `+use` to choose; hold `+use` with `+forward` / `+back` to
select. Inside a category, `previous page` and `next page` are the available
actions. Hold `+use` and press `+moveleft` to return to the pushed menu. Opening the viewer
adds one navigation entry; category selection and paging do not.
Pages are per-slot in-memory state — no cvars. Like every viewer except the
menu itself, opening needs a signed-in profile and level-0 admin access.

`_sofbuddy_cmds_enable` (default `1`) gates registration. Requires
`_sofbuddy_minigames_enable 1`.

Naming policy: `_sofbuddy_` is the public admin prefix (settings + documented
gauges). Internal plumbing must use `_sb_internal_`, never `_sofbuddy_`. This
viewer keeps paging in memory and creates no internal cvars.

Text-only layout (no sprites or ghoul downloads).

The neighbouring `player commands` page is populated from the live sofplus
top-level `.COMMANDS` list (the same source as `sofbuddy_cmdcost_usercmds`).
It is also menu-only and uses the same `previous page` / `next page` actions.

Tests: `tools/tests/commands/run.sh`
