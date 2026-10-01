# cvars

Minigame tab listing every live sof_buddy cvar (`_sofbuddy_*` settings and gauges plus `_sb_internal_*` plumbing), walked from the engine `cvar_vars` list. Each row shows the live value, the creation default, and a short description. Pages follow the name after the prefix (`clamp`, `reldef`, …). `lowclamp` / `highclamps` share the clamp page; `pipe` shares clsv. A full group continues on the next page.

Each player has their own view. `.mg_cvars` toggles a list of categories, paged when they do not fit. `.mg_cvars 2` is that list's page. `.mg_cvars clamp` opens the category. `.mg_cvars clamp 2` is page 2, and only matters when the category needs more than one page. The layout prints the command. An unknown name prints the category list. Values refresh while the minigame tab is open.

Requires `_sofbuddy_minigames_enable 1`. Open with +use+score.
