# minigames

Platform for 2D minigames on the client screen (tictactoe now, chess
later). Games implement rules + drawing; the platform owns the three
tricky pieces:

1. **Input routing** — detours the stock game DLL's `ClientCommand` and
   delivers client console words (`ttt ...`) to the registered game.
   Everything else passes through to stock untouched.
2. **Layout channel** — two server paths hit the same client `layout_string`
   buffer (cap `0x400`). See [Layout delivery](#layout-delivery-svc_layout-vs-sp_print)
   below for when to use each.
3. **Visibility** — re-asserts `ps.stats[STAT_LAYOUTS]` every server frame
   for visible slots, because stock `G_SetStats` rewrites stats each frame.
4. **Minigame view** — hooks stock `cmd_score_f` @ `0xF6710`: with the DM
   scoreboard open, **+use + score** switches to minigame and **latches** score
   to minigame-only (score toggles minigame/hidden; no vanilla scoreboard until
   **+use + score** again). Death/intermission still force the vanilla scoreboard.

## Writing a new game (chess, ...)

1. Create `src/features/<game>/` with your rules (pure logic, unit-test
   it like `tools/tests/tictactoe/` does).
2. Draw with the canvas API (`minigames_api.h`): `MgCanvasClear`,
   `MgCanvasText/Center/Pic` (stock `xv`/`yv` tokens, 640x480, whole page
   capped at 1024 chars — far under the client's 0x400 `layout_string`).
3. Register: `MgRegisterGame({"word", OnClientCmd})` on `GameDllLoaded`,
   read words with `MgArgv(n)`. One **display owner per client slot** — pass
   your client word as `gameId` to `MgShowLayout` / `MgPushLayout` /
   `MgClearLayout` (taking display preempts any other game on that slot).
   **One running session server-wide** — the first `MgTakeDisplay` for a
   `gameId` starts it; another `gameId` preempts via optional `onSessionEnd`
   (stop timers / per-frame work). Session ends when that game has no slots left.
4. Slots are 1-based. `MgSlotSpawned` / `MgEdictForSlot` validate;
   `MgMaxClients` bounds loops. Admin commands: `MgRegisterConsoleCommand`.
5. Add `<game>: true` to `src/features/features.yaml`.

## Offsets (verified, no magic)

| Pointer | Value | Provenance |
|---------|-------|------------|
| `game_export_t.ClientCommand` | `ge+0x34` | Retail `gamex86.dll` `GetGameAPI` stores it at export `+0x34`; the function reads `ent->client` at `+0x74` and calls `gi.argv` |
| `game_export_t.edicts` / `edict_size` | `ge+0x60` / `+0x64` (= 0x464) | Same binary (`mov [0x5015c934], 0x464`) |
| `edict_t.client` | `ent+0x74` | Same as `stufftext`/`ctf_spawn`; confirmed by the ClientCommand body above |
| `gclient_t.ps` | `client+0` | First field (game source) |
| `player_state_t.stats` | `ps+164`, `short` elems | Offsets compiled from the game source (x86) |
| `STAT_LAYOUTS` | `stats[9]` | Game + engine agree; client draws layout while bit 0 set |
| `gclient.pers.health` | `client+0x2FC` | `G_SetStats` layout gate (dead) |
| `gclient.buttons` | `client+0x47C` | `+use` held check for use+score open |
| `gclient.ps.pmove.pm_type` | `client+0` | `PM_DEAD` (=3) before `player_die` sets `health=-1` |
| `level.intermissiontime` | `gamex86+0x15D1C8` | `G_SetStats` layout gate + intermission scoreboard |
| `gi.argc` / `gi.argv` | import slots 9 / 10 | Source field order (`bprintf` lands on 12, as existing wrappers confirm) |

Every pointer is bounds-checked (`IsBadReadPtr`, edict range, in-use,
client present) before use. Unload: `Minigames_Shutdown()` removes the
detour (wired in `DllMain` detach).

## Settings

| Setting | Default | Meaning |
|---------|---------|---------|
| `_sofbuddy_minigames_enable` | `1` | Platform master switch (Tab cycle, `mg_*`, idle banner). `0` = no routing, no platform ghoul/sprites. |
| `_sofbuddy_minigames_bg` | `0` | Idle/`mg_test` backdrop: `0` = `sb/mg/pn` tile, `1` = `sb/mg/bg` panel. |
| `_sofbuddy_ttt_enable` | `0` | Offer tictactoe on this server (`ttt`, `ttt_*`, `sb/tt/*`). Not “active game”; display is per-slot via `MgTakeDisplay`. |
| `_sofbuddy_lagometer_enable` | `1` | Offer lagometer (`lag` client cmd, `lag_show` admin). Unregistered `lag` falls through to stock chat — see lagometer README. |

## Console API for scripts (`mg_*`)

Everything a SoFPlus script (`tictactoe.func` and friends) needs — no C
required. Players drive games by typing `ttt <cell>` in their game
console (or binding it, e.g. a SoFPlus client alias `.ttt` that forwards
`ttt %1` to the server); the engine forwards unknown client commands to
the server, where the platform routes the registered word to its game.

| Command | Meaning |
|---------|---------|
| `mg_push <slot> <tokens...>` | Send a pre-composed layout token stream to one slot (`xv/yv/string/picn/tc`, spaces allowed — everything after the slot is the canvas, 1024 cap) |
| `mg_show <slot> <0\|1>` | Turn layout visibility on/off for a slot (minigame page while on) |
| `mg_clear <slot>` | Hide the board and wipe the client's stored layout |
| `mg_idle <slot>` | Minigame tab with idle placeholder (post-game timeout target) |
| `mg_ghoul_list [filter]` | Dump non-empty `CS_GHOULFILES` slots (download diagnostics) |
| `mg_ghoul_register <path>` | Register a downloadable file (e.g. `sb/tt/x.m32`) |
| `mg_sp_register <package>` | `gi.SP_Register` — load a StringPackage by reference name (no `.sp` suffix) |
| `mg_print <slot\|all> <level> <text...>` | `svc_print` — broadcast or unicast console chat (`level` 2 = center chat) |
| `mg_center <slot\|all> <text...>` | `svc_centerprint` — bottom-center screen text |
| `mg_caption <slot\|all> <sp_id>` | `svc_captionprint` — yellow subtitle line (needs a registered `.sp` string ID) |
| `mg_cin <slot\|all> <x> <y> <speed> <text...>` | `svc_cinprint` — cinematic/typewriter text |
| `mg_welcome <slot\|all>` | `svc_welcomeprint` — server welcome banner |
| `mg_name <slot> <from_slot> <color> <text...>` | `svc_nameprint` — attributed chat-style line |
| `mg_countdown <slot\|all> <seconds>` | `svc_countdown` — on-screen timer byte |

Rules still live in C (or in script logic calling the game's own
commands, e.g. `ttt_start` / `ttt_move` / `ttt_end`): the `mg_*` layer is
canvas + visibility + push + resource registration + screen text.

## Minigame view (Tab / `score`)

When the buddy minigames platform is enabled, **`cmd_score_f` is detoured**:

| Input | Latched? | Result |
|-------|----------|--------|
| **+use + score** | No (scoreboard open) | Minigame view + latch |
| **+use + score** | Yes | Unlatch; hide layout (vanilla score again) |
| **Score only** | Yes | Toggle minigame ↔ hidden (never vanilla scoreboard) |
| **Score only** | No | Stock DM scoreboard toggle |

Starting a game (`MgShowLayout(..., true)`) opens the minigame view directly.
Death and intermission bypass this and show the stock scoreboard.

CTF scoreboard hotkey reminder (when minigames enabled): after stock
`dmctf_c::clientScoreboardMessage` runs, the hint is **appended** to the
existing layout via **`Buddy_SP_PrintLayout(ent, 0x0700, tokens)`** — layout
append via **`strip/sofbuddy.sp`**. On first use the server creates
`strip/sofbuddy.sp` if missing (package ID **7** — retail-empty slot between
6 and 8 — index **0**, `SP_FLAG_LAYOUT`, `TEXT "%s"`), CRCs the template bytes,
publishes **`strip/sofbuddy-<CRC>.sp`**, and registers **`sofbuddy-<CRC>`**
(SoF++-nix pattern: clients re-download when content changes). Registration:
`FS_LoadFile("strip/sofbuddy-<CRC>")` precheck, then `SP_Register("sofbuddy-<CRC>")`
(`SP_RegisterServer` @ `0x20057F20`), which sets a `CS_STRING_PACKAGES` entry
(the configstring **value** is `sofbuddy-<CRC>`; clients fetch
`strip/sofbuddy-<CRC>.sp`). Requires client `allow_download_stringpackage 1`
(stock stufftext sends this).
See [Layout delivery](#layout-delivery-svc_layout-vs-sp_print).

### Layout sprites (`assets/sb/`)

Grouped short paths to save layout tokens:

```
assets/sb/
  mg/bg.m32   → picn sb/mg/bg   (640×480 opaque black; regen: `python3 tools/gen_mg_bg.py`)
  mg/pn.m32   → picn sb/mg/pn   (semi-transparent tile; bg cvar 0)
  mg/id.m32   → picn sb/mg/id   (idle banner)
  tt/b.m32    → picn sb/tt/b    (tictactoe board)
  tt/x.m32    → picn sb/tt/x    (tictactoe X)
  tt/o.m32    → picn sb/tt/o    (tictactoe O)
```

Duplicate under `tictactoe/assets/sb/`. Deploy: `./scripts/cpminigame_assets.sh` → `export/sb/` + `Base/sb/`.

## Downloads: `CS_GHOULFILES`

Sprites reach clients through the connect-time download walk
(`CL_RequestNextDownload`, verified in disassembly): for each
non-empty configstring it checks the file locally and fetches what is
missing, then the server serves it from its own directories. Two ranges
matter for pictures:

| Range | Client filename | Gate |
|-------|-----------------|------|
| `CS_IMAGES` (903–1158) | `<text>` + `.m32` appended | none |
| `CS_GHOULFILES` (1497–3511) | `<text>` verbatim | none (unlike sounds/maps/stringpackages, no `allow_download_*` check) |

We register in **`CS_GHOULFILES`** (`MgRegisterGhoulFile`, 2048 slots,
mostly empty): full path with extension, e.g. `sb/tt/x.m32`.
Checklist when a sprite doesn't arrive:

1. The file exists **on the server** (`Base/sb/tt/x.m32`) — the
   server can only serve what it has ("File not found." in the client log
   means this step).
2. It is registered **before clients finish precache** — `MgSyncGhoulFiles`
   on map load strips `sb/mg/*` + `sb/tt/*` and re-registers whatever is
   enabled; mid-map enable (`_sofbuddy_*_enable 0→1`) appends via
   `MgRegisterGhoulFile` (no-op if already present). Disable/switch does
   **not** remove entries. Already-connected clients still need to **reconnect**
   (download walk runs at connect, not mid-game).
3. Entries must be **contiguous from slot 1498** (client skips 1497): the
   walker's first empty `CS_GHOULFILES` entry ends the whole ghoul range.
   Map changes re-compact buddy-owned paths; `mg_ghoul_list` shows the live
   table — check for gaps from non-buddy entries.
4. **`reliable_defer` is not the cause** for missing ghoul downloads: join
   traffic (`state < spawned`) passes through untouched, and `svc_download`
   (`0x13`) uses an in-hook lockstep bypass so chunks are never split/delayed.

## String Packages (SP): what they are and what we use

SoF ships a String Package system (see the Jedi Outcast SDK
`qcommon/strip.cpp` for the documented cousin): localised text entries
with flags, formatted with `%hu %hd %d %p %s` (plus `%n`), sent via
`SP_Print_` as `svc_sp_print` (`0x22`), `svc_sp_print_data_1` (`0x24`,
short id + byte len + bytes) or `svc_sp_print_data_2` (`0x25`, short id +
short len + bytes), unicast or broadcast. Minigame **full pages** use
`SP_Print(LAYOUT_RESET)` + **`svc_layout`** (see [Layout delivery](#layout-delivery-svc_layout-vs-sp_print)).
For free-form screen text use `mg_print` / `mg_center`; for SP-flagged
captions use `mg_caption` after `mg_sp_register` (or ship a `.sp` under
`strip/` and register it the SoFree way: one `%%s` entry per message template).

| SP flag | Value | Meaning |
|---------|-------|---------|
| `SP_FLAG_CENTERED` | `0x01` | Centered, vertical bottom (`SCR_CinematicString`) |
| `SP_FLAG_TYPEAMATIC` | `0x02` | Typewriter effect |
| `SP_FLAG_CAPTIONED` | `0x04` | Screen-center captions (needs `cl_subtitles 1` or `ALWAYS_PRINT`) |
| `SP_FLAG_CREDIT` | `0x08` | Fading credits (`x;y;duration;fadein;fadeout;r,g,b;file`) |
| `SP_FLAG_LAYOUT` | `0x10` | Layout string. A leading `*` is the trick: bare `*` **clears** the layout, `*<text>` **replaces** it, anything else **appends** (capped at 0x400) |
| `SP_FLAG_ALWAYS_PRINT` | `0x40` | Forces `CAPTIONED` output |
| (none) | — | Falls through to `Com_Printf` (console) |

## Layout delivery: `svc_layout` vs `SP_Print`

Both paths end up in the client's `layout_string`, which
`SCR_ExecuteLayoutString` draws while `stats[STAT_LAYOUTS]` bit 0 is set.
They differ in **merge semantics** and **what belongs on the wire**.

| Path | Opcode / API | Client effect |
|------|----------------|---------------|
| **`svc_layout`** | `0x2` + `WriteString` | **Replaces** the whole `layout_string` (`Com_sprintf` into the 0x400 buffer). Anything already there (e.g. a stock scoreboard) is wiped. |
| **`SP_Print` + `SP_FLAG_LAYOUT`** | `0x22` / `0x24` / `0x25` | **Merge**: bare `*` clears; `*tokens` replaces; any other text **appends** (space-separated, capped at 0x400). Stock DM/CTF scoreboards are built entirely this way (`dm_generic` / `dm_ctf` layout entries). |

Verified in retail client disassembly: `svc_layout` handler @ `CL_ParseServerMessage`
case `0x2`; SP layout merge @ `Print_SP_Message` when flag `0x10` is set.

### When to use which

| Goal | Use |
|------|-----|
| **Own the full screen** (minigame page, default lagometer when enabled) | `SP_Print(DM_GENERIC_LAYOUT_RESET)` then **`svc_layout`** with the new token stream — what `PushLayoutPayload` / `MgPushLayout` do. You are replacing the canvas on purpose. |
| **Add tokens to an existing layout** (e.g. one line on a stock scoreboard) | **`SP_Print` layout append** only. A trailing `svc_layout` would destroy the scoreboard. Needs a registered `.sp` entry with `SP_FLAG_LAYOUT` and `%s`; minigames auto-create **`strip/sofbuddy.sp`** (`0x0700`) when missing. |
| **Text outside the layout channel** | `mg_center` / `centerprintf`, captions, etc. — separate opcodes, no merge rules. Not used for the CTF scoreboard hint (layout append only). |

### String package IDs (`strip/*.sp`)

Retail ships fixed `ID N` values inside each `.sp`; the high byte of
`SP_Print` ids is that package id (`0x0700` = package **7**, index **0**).
The **configstring slot** for download is assigned by `SV_FindIndex` (first free
slot in `CS_STRING_PACKAGES+1 … +30`, base **1463** / `0x5B7`) — not the same
number as the `.sp` `ID` field, though retail leaves **ID 7** unused in `.sp`
files. The remaining **empty** retail **ID** slots include:

`0–6`, `8–14`, `50–55`, `57–68`, `99–130`, `150–195`, `200`, `203`, `205–208`,
`210–239`, `241–244`, `246–248`, `251–253`, …

**`sofbuddy.sp` uses ID 7** — the unused gap between retail slots 6 and 8 —
so it does not collide with stock packages. Format matches SoFree's `sofree.sp`
shape: **`COUNT 2`**, index **0** = `SP_FLAG_LAYOUT` + `TEXT "%s"` (`0x0700`
append), index **1** = `SP_FLAG_CREDIT` + `TEXT "%s"` (`0x0701`, reserved for
custom credit images later). In a C char array SoFree writes `"%%s"` so the file
gets a single `%`; we `fwrite` the body directly so the literal is already
`"%s"`. The server keeps **`strip/sofbuddy.sp`** as the editable template;
registration uses **`sofbuddy-<CRC32>.sp`** / **`SP_Register("sofbuddy-<CRC32>")`**
so a content change gets a new configstring name and clients fetch the new file.
Registered from `mg_SvFramePost` (after send, not inside scoreboard/multicast)
and retried on minigame client/console commands if that fails. Do not call
`SP_Register` from `clientScoreboardMessage` — map rotate/intermission already
fills `sv.multicast` and can `SZ_GetSpace` overflow.

### Wire size (why append matters)

- **Full replace (`svc_layout`)**: one message carries the **entire** layout string. Fine when the server owns the whole page (~100–1000 chars for a minigame board).
- **Stock scoreboard**: already sent as **many small SP layout messages** during `clientScoreboardMessage` (reset, team rows, clients, spectators). The client holds the merged result; the server does **not** keep a copy.
- **Append one line after scoreboard**: send **only the hint tokens** (~100 bytes) via SP layout append. Re-sending the scoreboard plus hint in a single `svc_layout` would be much larger and requires reconstructing layout the server never stored.

`reliable_defer` applies to both families: each `svc_layout` or SP print should arrive as one whole reliable parcel (no torn layout tokens mid-packet).

`.sp` files may also carry a `NOTES` field, and colour names `$P_WHITE`,
`$P_RED`, `$P_GREEN`, `$P_YELLOW`, `$P_BLUE`, `$P_PURPLE`, `$P_CYAN`,
`$P_BLACK`, `$P_HWHITE`, `$P_HRED`, `$P_HGREEN`, `$P_HYELLOW`, `$P_HBLUE`,
`$P_CAMOBROWN`, `$P_CAMOGREEN`, `$P_SEAGREEN`, `$P_SEABLUE`, `$P_METAL`,
`$P_DBLUE`, `$P_DPURPLE`, `$P_DGREY`, `$P_PINK`, `$P_BLOODRED`, `$P_RUSSET`,
`$P_BROWN`, `$P_TEXT`, `$P_BAIGE`, `$P_LBROWN`, `$P_ORANGE` (plus `\a \b \t
\v` escapes and `\x1B`), which the engine substitutes while loading the
package.

## Layout tokens (verified in the client)

From `SCR_ExecuteLayoutString` @ `0x20014510` (all comparisons confirmed
in disassembly):

| Token | Meaning |
|-------|---------|
| `xl n` / `xr n` | x from left / from right (`xr` adds screen width) |
| `xv n` | x = n + width/2 − 160 (so `xv 160` ≈ screen center) |
| `yt n` / `yb n` | y from top / from bottom |
| `yv n` | y = n + height/2 − 120 |
| `picn name` | `Draw_Pic(x, y, name)` — note: `picn`, not `pic` |
| `string "…"` | Color-aware text at x, y |
| `altstring "…"` | Alternate-charset text (chars drawn with high bit set) |
| `tc n` / `ac n` | Text / alt colour state from a 16-entry table (`MgCanvasTc`; community palette: 0 black, 1 green, 2 dark green, 3 darker green, 4 yellow, 5 red, 6 white) |

(Scoreboard family `client_sb` / `ctf_*_sb` / `spect_sb` / `team_sb` and
`num`/`hnum`/`stat_string` exist in the same function by Q2 convention;
unverified here — stick to the table above.)

## Sprites: `picn` + image registration

`picn` draws by name. Retail pictures (`pics/*`, `env/*` that ship with the game)
need no setup if the client already has them. Buddy bundles short **`sb/*`**
sprites (see `minigames_api.h`). Custom files must exist on the client; server-side, `MgRegisterImage(name)` (`gi.imageindex`, slot 3)
precaches the name into a configstring so it participates in precache /
download like the stock `pics/scope/arrows128_*` registrations. The
`CS_GHOULFILE` configstring range has thousands of free slots and accepts
any extension, which is the recommended home for custom sprite files.
