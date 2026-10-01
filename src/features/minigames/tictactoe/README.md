# tictactoe

Server-side noughts-and-crosses for two players, built on the
[minigames](../README.md) platform: this feature is only rules
(board state, turns, win/draw) plus drawing. Input routing, the layout
channel and visibility all live in the parent — see its README for the
engine details (same channel the scoreboard uses, same idea as the SoFPlus
`layout.func`/`tetris.func` scripts, but native, so no scripts are needed
and `reliable_defer` carries each board as one whole blob).

## How to play

1. Admin (server console or rcon):
   `ttt_start <slotX 0-based> <slotO 0-based>` — X moves first.
2. Each player types `ttt <1-9>` in their game console on their turn.
   The board shows neon sprite cells (`picn pics/ttt/ttt_x` / `pics/ttt/ttt_o`,
   64px on a 72px pitch, see `assets/`) for taken cells and the cell
   number for empty ones, with a coloured title and status line.
3. First three-in-a-row wins; full board with no winner is a draw.
   Results are announced to the whole server.
4. `ttt_end` clears the boards. `ttt_move <slot> <cell>` lets the admin
   play a move for a slot (testing / shy players).

A game aborts automatically if either player leaves. Moves out of turn,
taken cells, and moves from spectators of the match are rejected with a
message to that player.

## Script version (`tictactoe.func`)

The same game as a SoFPlus script — pick it *or* the C game:

| | C game | Script game |
|---|---|---|
| Client command | `.mg_ttt <1-9>` | `.ttt <1-9>` (script dot-command) |
| Start (admin) | `ttt_start <slotX> <slotO>` | `.ttt_new <slotX> <slotO>` |
| End (admin) | `ttt_end` | `.ttt_end` |
| Rules live in | `tictactoe_logic.h` | script cvars |
| Board via | C canvas | `mg_push` / `mg_show` / `mg_clear` |

Install: copy `tictactoe.func` next to your other `.func` files and load
it (`sp_sc_func_load_file minigames/tictactoe.func` style). Requires
**SoFPlus** (script flow, cvars, dot-commands, `sp_sc_timer`) **and** the
**buddy mod** (`mg_*` for board, sprites, chat). Run
`./scripts/cpminigame_assets.sh` for `Base/sb/` sprites. Parity with the C
game: 5s result screen then idle placeholder, disconnect abort, aligned layout.
Both can coexist — they only share the `mg_*` plumbing, never state.

## How it works (engine)

- **Input:** the stock game DLL's `ClientCommand` (export slot `0x34`,
  read off the live `game_export_t`; verified against retail
  `gamex86.dll` `GetGameAPI`, which stores it at export `+0x34`) is
  detoured with DetourXS. `ttt ...` is swallowed; everything else passes
  through untouched.
- **Board delivery:** `WriteByte(svc_layout=0x2)` + `WriteString(board)` +
  `unicast(ent, reliable)` per player, only when the board changed.
  `reliable_defer` carries each board as one whole blob, so a board can
  never arrive torn.
- **Visibility:** the client draws `layout_string` only while
  `ps.stats[STAT_LAYOUTS=9]` bit 0 is set (`Scr_UpdateScreen`). The stock
  game rewrites stats every frame, so an `SV_Frame` Post hook re-asserts
  the bit for both players every tick. Offsets (`edict->client` +0x74,
  `ps.stats` at client+164, short elems) come from the game source and
  match the shipped structs (`kEdictClient` is already trusted the same
  way by `stufftext`/`ctf_spawn`); every pointer is bounds-checked before
  use.
- **Scoreboard suppression:** stock scoreboard (`clientScoreboardMessage`,
  every 32 frames when `showscores`) appends `client_sb` lines via
  `SP_Print`. Tictactoe **replaces** the whole layout every server frame
  with `svc_layout` in `ttt_SvFramePost` (SV_Frame Post, after
  `ClientEndServerFrame`), so the client sees **only the board** — not
  scoreboard and board together. (SoFFree does the opposite: it re-pushes
  custom art *before* the scoreboard so both show; we don't want that.)
  A **`pics/ttt/ttt_board`** panel (224×224, neon 3×3 grid) sits behind
  the cells so the empty board is visible before any move.
- **Board size:** ~250 chars in 640x480 `xv`/`yv`/`string`/`cstring`
  tokens — far under the client's 0x400 `layout_string` cap.
- **Unload safety:** `Minigames_Shutdown()` removes the ClientCommand
  detour (called from `DllMain` detach like the other features).

## Settings

| Setting | Default | Meaning |
|---------|---------|---------|
| `_sofbuddy_minigames_enable` | `1` | Platform master switch (layout visibility, `mg_show`). |
| `_sofbuddy_ttt_enable` | `0` | Tictactoe master switch. `0` = no client `ttt` word, no admin `ttt_*` commands, no `sb/tt/*` ghoul download. Set `1` to enable. |

## Sprites (`assets/`)

Canonical copies also live under the combined tree
`../assets/pics/ttt/` (same files — run `./scripts/cpminigame_assets.sh` once
for everything).

| File | Role |
|------|------|
| `ttt_board.m32` | Empty 3×3 grid panel (224×224, cyan lines on dark panel) — visible from game start |
| `ttt_x.m32` | Red neon X (64×64) |
| `ttt_o.m32` | Green neon O (64×64) |

Board art was generated with Cursor **GenerateImage** (Flux-style prompt), then
converted with `png_to_m32_resizer/m32lib.py` (`Utility/MyPythonTools`, header
version 4, name `pics/ttt/ttt_board`, black-key luminance &lt; 24 → alpha 0).
X/O prompts (for regeneration):

- X: "Glowing red neon letter X symbol, bold, centered, isolated on a
  pure solid black background, simple flat game HUD icon, high
  contrast, no other text, no shadows, no frame"
- O: same with "green neon letter O ring symbol, bold circle"
- Board: "dark charcoal 3×3 neon cyan grid panel, rounded border, black keyed corners, no X/O/text"

Drawn with `picn pics/ttt/ttt_board` at `(208,102)` — aligns the 224px sprite
with the 208px cell grid (64px cells, 8px gaps). Ghoul files are registered
on map load via `MgSyncGhoulFiles` (platform hook).
registers `sb/tt/*` only when `_sofbuddy_ttt_enable 1` (once per map), so
connecting clients pick them up during the connect-time download walk — no
`allow_download_*` gate for this range.
Use `./scripts/cpminigame_assets.sh` from the repo root (or copy
`assets/sb/tt/*.m32` to server `Base/sb/tt/` manually).

## Tests

`tools/tests/tictactoe/run.sh` — pure rules/draw/win + layout rendering
(no engine needed).
