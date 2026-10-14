# profiles — persistent player identity

Profiles stores a server-minted guid in the client's `team_red_blue`
userinfo value. The guid survives reconnects and team changes, while the
server registry maps it to a nickname and a privilege.

Privilege values:

| Value | Meaning | Current menu access |
|---:|---|---|
| `0` | server admin | server-side commands and cvars, lagometer, stats page, upload match |
| `1` | map admin | stats page, upload match, gamepark transfer on `.rcon map` |
| `2` | normal user | player pages, stats page |

Only privilege `0` opens the server-side command and cvar pages and the
lagometer. Privilege `0` and `1` see the upload-match row, may issue
`.upload_match` / `.cancel_upload`, and trigger the gamepark transfer. The
stats detail page is open to every signed-in profile. An unsigned slot has
no privilege and is never treated as an admin.

## Identification and repair

The server mints a 24-digit guid and carries it in:

```
team_red_blue = <packed decimal>
```

The packed decimal holds a 23-bit fold of the guid with the team in the
low bit (`AssignTeam` reads `atoi(... ) & 1`, so the parity is the team).
It is exact for every client cvar type (below 2^24, so float-stored
clients keep it bit-exact). The native hook also accepts the legacy long
`<guid><0|1>-blue|-red` text form, 62-digit guids, and truncated decimals
on read — but never pushes them. A bare `0`/`1` means the client lost its
identity (team menu collapse). When restoring, the carrier is synced to
the live server team, so a stale or collapsed value never overrides where
the player actually is.

### Guid lengths: 24-digit canonical, 62-digit deprecated

New identities are always 24 digits (`profile_register` mints them, and
the pushed `team_red_blue` carrier is always the short packed decimal).
62-digit guids are deprecated: harder to remember and type, and nothing
produces them anymore. They keep working internally — registry load/save,
`team_red_blue` parse, and every `profile_*` command still accept
them — but signing in, importing, or loading the registry with one prints
a deprecation notice. To migrate a player: `profile_register <nickname>`
for a fresh 24-digit guid, `profile_signin` it, then `profile_del` the
old 62-digit entry.

Each slot keeps the last known guid in memory. If the client collapses the
carrier, the hook preserves the team assignment and stuffs the packed
carrier back to that client; the next userinfo update binds the slot again.
Team repair uses live team, parsed userinfo team, then the slot's last team.
`_sofbuddy_profiles_stufftext 0` prints the repair instead of sending it.

`profile_query` reports `ok`, `wrong`, or `guest`; wrong slots are repaired
automatically, printed for manual repair, or reported pending when no team
bit is available. A carrier that validly names a *different* profile than
the slot holds (e.g. a menu re-sending a remembered value) never switches
teams: stock AssignTeam is skipped for it and the restore path re-pushes
the slot's own pack, so opening menus cannot flip teams by stale carrier.

A fresh sign-in (auto-bind on connect, `profile_signin`, or `.login`)
broadcasts `live name signed in as [Role]nickname. Welcome!` to the
server — once per sign-in, not on every userinfo sighting, and never while
the player is still connecting (sign-ins detected mid-handshake are
announced at `ClientBegin`, when fully in the world). The line is
yellow; only the role tag takes the privilege color (`[Admin]` magenta,
`[MapAdmin]` cyan, `[Player]` green). Signing out broadcasts the mirror
line (`live signed out as [Role]nick. Goodbye!`), so `profile_signout all`
names every player it signs out. When the sofplus sound slot cvar
`_sp_sv_sound_13` holds `misc/talk.wav`, the welcome also fires
`sp_sv_sound_broadcast 13`.

### The registry: guid, nickname, privilege

The registry stores `guid → nickname`, a clean nickname lookup key, one
privilege value (`0`, `1`, or `2`), and two per-profile settings:
`hide_tips` and `swap_score`.
New profiles default to privilege `2` with tips visible and the vanilla
scoreboard on `score`. Names are cleaned by
stripping SoF color escapes (`%NN`, `^X`), lowercasing, and keeping `a-z0-9`;
the displayed nickname keeps its visible text. Reusing a clean name replaces
its lookup key.

### Registry file: one file per server

The whole roster lives in a **single file** — no per-player files, no
subdirectories:

`<user>/sofplus/data/profiles/registry.cfg`

`<user>` is the server's user directory (the same `User/` or
`user-<PORT>/` dir the game writes its own configs to), so each server
instance has its own roster. It is the same path the `.func` used via
`sp_sc_cvar_save` relative to `sofplus/data/`, which is why old files
import as-is.

A real file is one entry per line, sorted by guid and rewritten on save:

```
# sof_buddy profiles registry (guid privilege nickname [hide_tips] [swap_score])
111111111111111111111111 2 Alice Smith hide_tips=0 swap_score=0
222222222222222222222222 1 Bob hide_tips=1 swap_score=0
602380633711624767525303 0 Slot0Test hide_tips=0 swap_score=1
```

Storage rules:

- **Format:** `<guid> <privilege> <nickname> hide_tips=<0|1> swap_score=<0|1>`.
  Nicknames may contain spaces; older entries without one or both suffixes
  load with that setting off (`0`). The two suffixes parse in any order.
  `<guid>` is 24 digits for new entries; legacy 62-digit entries are
  preserved as-is (deprecated, see above).
- **Migration:** old `<guid> <nickname>` and `<guid> - <nickname>` entries
  load as privilege `2`; explicit `0` and `1` values are preserved.
- **Written** synchronously on every `profile_register`, `profile_import`,
  `profile_del`, and `profile_save` — after a successful command the file
  always matches memory.
- **Read** when the game DLL loads and on `profile_load`. Hand-edit freely,
  then run `profile_load` (or restart) to pick it up; unknown or malformed
  lines are ignored on load, so a stray edit can't corrupt the in-memory
  roster.
- **Missing file = empty roster** on a fresh start. It is created on the
  first mutation (`FS_CreatePath` makes the `sofplus/data/profiles/`
  directory if needed). A failed `profile_load` (no file) leaves the
  in-memory roster untouched.
- **Capped at 4096 entries** — past that, register/import report
  "registry full" (updating an existing guid still works).
- **To clone a roster** to another server, copy the one file. To move a
  single player, `profile_import` their guid + nickname there instead.

Legacy `.func` lines import as privilege `2` and are re-saved in native form:

```
set "~reg_602380633711624767525303" "Slot0Test"   # accepted on load
set "~guid_by_slot0test" "602380633711624767525303"  # rebuilt, skipped
```

### Slot state and audit

Each connected slot keeps its last roster guid in memory. A collapsed
`team_red_blue` value is repaired using the live team, parsed team, or last
known team. The server sends the repair with stufftext and binds the slot
when the client echoes the restored carrier. With
`_sofbuddy_profiles_stufftext 0`, it prints the exact repair instead.

`profile_query` classifies connected slots as:

| State | Meaning |
|---|---|
| `ok` | client carries a roster-known guid |
| `wrong` | known guid was lost and needs repair |
| `guest` | no roster identity is available |

Wrong slots are reported as `pushed`, `manual`, or `pending`. Disconnect
clears the slot binding; map changes re-bind from the client carrier.

## Commands (server console / rcon)

Use the commands below from the server console or rcon. Any argument taking
a guid also takes the short login number (or a nickname where shown); all
output shows the short number. The full guid stays the internal identity
(registry key, fold source, import) — the number is a convenience layer,
and a number shared by several profiles needs the full guid.

| Command | Args | What it does |
|---|---|---|
| `profile_register` | `<nickname>` | mint a guid and save a normal user |
| `profile_import` | `<guid24> <nickname>` | import an existing guid as a normal user |
| `profile_set_privilege` | `<guid/number/nickname> <0\|1\|2>` | set server/map/user privilege |
| `profile_del` | `<guid/number/nickname>` | delete a roster entry |
| `profile_signin` | `<slot> [<guid/number/nickname>]` | bind or push a roster entry |
| `.login` | `<login number or guid>` (player console) | sign yourself in (needs the minigames client-command hook) |
| `.logout` | — (player console) | sign yourself out |
| `profile_signout` | `<slot\|guid\|number\|nickname>` \| `all` | unbind a slot, or everyone |
| `profile_query` | — | audit and repair bindings |
| `profile_save` / `profile_load` | — | persist or reload the registry |

Use `profile_import` when the player's client already carries a guid,
such as after restoring a lost registry. Otherwise use `profile_register`.
Importing a 62-digit guid works but prints a deprecation notice — prefer a
fresh 24-digit guid from `profile_register`.

`profile_signin` tells the signed-in player their login number in their own
console and asks them to write it down. The number is the decimal guid fold
without the team bit (up to 8 digits instead of 24). With it they can sign
themselves in later with `.login <number>` from their own console (no admin
needed) — the word is consumed privately and never reaches chat, since the
number is the credential. `.login` also accepts the full guid; a number
shared by several profiles needs the full guid to disambiguate.

## Cvars

**Settings** (you set these, all `ARCHIVE`, read live):

| cvar | default | what it does |
|---|---|---|
| `_sofbuddy_profiles` | `1` | master enable (`0` = stock, hooks pass through) |
| `_sofbuddy_profiles_stufftext` | `1` | `1` = stuff the fix; `0` = print `run: stufftext …` instead (manual) |

**Gauges** (read-only `NOSET` outputs):

| cvar | what it tells you |
|---|---|
| `_sofbuddy_profiles_registered` | roster entries |
| `_sofbuddy_profiles_slots` | slots currently bound to a roster player |
| `_sofbuddy_profiles_errors` | failed / rejected operations |

## Wiring (native)

- Hooks `ClientBegin` + `ClientUserinfoChanged` (+ `ClientConnect` /
  `ClientDisconnect` bookkeeping) via `game_export_t` slots
  `ge+0x24/0x28/0x2c/0x30` — same swap technique as minigames/clsv for
  `ClientCommand`, but on slots no other feature touches.
- Reads `team_red_blue` with `Info_ValueForKey` straight from the hook's
  `userinfo*` (falls back to `gclient+0xCC` on `ClientBegin`).
- `profile_query` walks every in-use edict, audits, and re-pushes.
- No game binary patching: `profiles_OnGameDllLoaded` loads the registry,
  registers `profile_*` via the repoint-safe
  `Cmd_AddCommand` path (existing nodes repointed, or `Cmd_AddCommand` on
  first boot), and swaps the four `Client*` export slots. Detach
  (`Profiles_Shutdown`) restores the slots and drops per-slot state. Pure
  parsing/registry/minting lives in `profiles_logic.h` (host-tested, no
  engine calls).

## Migration from `.func`

1. Enable the feature (`src/features/features.yaml`: `profiles: true`),
   rebuild, deploy `gamex86.dll` as usual.
2. Copy the old registry if you have one:
   `<user>/sofplus/data/profiles/registry.cfg` is read as-is (legacy
   `~reg_` lines import cleanly), then re-saved in native form on the
   next `profile_register/del/save`.
3. Remove `profiles.func` / `ext_trigger.func` from `sofplus/addons/`
   (or leave them unloaded — native commands shadow the same
   functionality via the engine command table; running both at once
   double-pushes). Use the canonical commands above.
4. Stop `export-fire` / `userinfo-rcon` for this host (unless another
   feature still needs them), unset `RCON_PASSWORD`, drop the
   `user-<PORT>` symlink if nothing else uses it. `_sp_sv_limit_userinfo_change`
   can go back to stock.
