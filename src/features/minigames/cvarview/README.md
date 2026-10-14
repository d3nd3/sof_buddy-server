# cvars

Minigame tab listing every live sof_buddy cvar (`_sofbuddy_*` settings and gauges plus `_sb_internal_*` plumbing), walked from the engine `cvar_vars` list. Each row shows the live value, the creation default, and a short description. Pages follow the name after the prefix (`clamp`, `reldef`, …). `lowclamp` / `highclamps` share the clamp page; `pipe` shares clsv. A full group continues on the next page.

Each player has their own view. The `.mg` menu opens a selectable category
list; tap and release `+use` to open, hold `+use` with `+forward` / `+back` to
select, and hold `+use` while pressing `+moveleft` to return to the previous
pushed menu. Category and cvar pages expose `previous page` and `next page`
menu items; select them with `+forward` / `+back` and tap `+use`. Values refresh while
the minigame tab is open.

Requires `_sofbuddy_minigames_enable 1`. Open with +use+score.
