# skinteam_quiet

Suppresses the spurious "doesn't recognise teamname" message by syncing
`teamname` to `skin` before stock `ClientUserinfoChanged`, dropping a leading
`*` from `CS_PLAYERSKINS`, and blocking the in-function teamplay suicide while
restoring health, deadflag, and team state.
