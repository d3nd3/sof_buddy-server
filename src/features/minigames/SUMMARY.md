# minigames

Client-screen 2D UI platform: routes `ClientCommand` and score input, owns the
`layout_string` channel, and re-asserts `STAT_LAYOUTS` every server frame.
Ships the tictactoe game, tick-budget lagometer, live cvar viewer, and
server/player command viewers, with per-slot menus and admin-only pages.
