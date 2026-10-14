# skinteam_quiet

`skin` and `teamname` are separate userinfo cvars. Setting `skin` does not
change `teamname`.

Retail `ClientUserinfoChanged` (`0x500F4730`) snapshots both, then calls
`PB_InitBody`. On a skin change that returns false, and `stricmp` says the
snapshotted `teamname` is not the loaded model team, stock writes
`CS_PLAYERSKINS` (`0x587`, `name\team\skin\bit`) with a `*` on the team
field. The client prints `TEXT_INVALID_TEAM` (`general.sp` `0x405`) only
when that field starts with `*`:

```
Server doesn't recognise teamname TokMan2 for skin tokman2 so you will use teamname TokMan2 instead.
```

Noteam `.gpm` models use the skin filename as the team (`tokman2` →
`TokMan2`). Case alone does not star. The star is a `teamname` still left
over from the previous skin. A `team_red_blue`-only update does not star.

## Fix

Before stock runs, `teamname` in that userinfo string is set to `skin`
whenever the two differ. Stock's `stricmp` then matches, so it does not
add the `*`.

`configstring` (SoF.exe, or SoF-spsv.exe at the same RVA `0x5CA40`) also
drops a leading `*` on the team field of `CS_PLAYERSKINS`. A `*` on the
skin field is left alone.

If `PB_InitBody` still returns false and the loaded team is the skin, this
feature writes that playerskin with no `*` and returns true, so stock's
starred write does not run.

## Team respawn

The in-function teamplay suicide (`player_die(self, self, self, 100000)`
while inside `ClientUserinfoChanged`) is blocked, and the health,
`deadflag`, and flags stock already wrote are restored. `kill` is outside
that scope, so it still kills.

`AssignTeam` sets edict `+0x380` when `team_red_blue` actually changes.
`ClientThink` (`0x500F5425`) consumes it and respawns. This feature does
not clear `+0x380`. Profiles skips `AssignTeam` on a collapsed bare
`0` / `1`. A client that integerizes `team_red_blue` is switched to a
packed integer whose low bit is the team, and that runs `AssignTeam`,
so `resp.team` and `+0x380` are set. Skipping a wrap
leaves the player on no team: not on the scoreboard, and able to damage
both sides.

Always on when compiled in (`features.yaml`). No cvar.
