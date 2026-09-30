# profiles — player identification via `team_red_blue`

Native port of **git-projects/sof-profiles** (`profiles.func`,
`ext_trigger.func`, `userinfo_rcon.py`, export-fire). The `.func` stack is
obsolete when this feature is built in: no `sofplus/addons/*.func`, no
Python services, no `user-<PORT>` symlink, no `rcon_password` wiring, no
`_sp_sv_limit_userinfo_change`, no 200 ms snapshot timers.

Naming note: the legacy `.func` used `prof_admin_add/del`,
`prof_apply`, `prof_register`, and `prof_enforce`. These are players, not
admins, so the native commands are `profile_register` (mint),
`profile_import` (keep an existing guid), `profile_del`, `profile_signin`
(auto re-read with just a slot, explicit push with slot + guid/nickname),
`profile_signoff` (unbind by slot, guid, or nickname), and `profile_query`
(audit + repair). Old `prof_*` names still work as
deprecated aliases.

## The identification system

SoF has no persistent player identity: names change freely and IPs change
constantly. Profiles adds one — a 24-digit **guid** (bearer token) per
roster player — and stores it where it survives reconnects, team switches,
and client restarts: the client's own `team_red_blue` userinfo key. The
server mints guids, remembers which guid belongs on which slot, and
re-pushes it whenever the client loses it. Nothing is installed on the
client; a stock client works.

Five pieces make this up: the guid, the carrier, the registry, the
per-slot guid, and the stufftext loop.

### 1. The guid

A 24-digit decimal string, e.g. `602380633711624767525303`. It is a bearer
token: whoever carries it in their userinfo *is* that roster entry. Minted
server-side with OS entropy (`RtlGenRandom`, `rand()` fallback) plus a
registry-collision retry — `profile_register` prints it, and it never needs
to be typed again (`profile_signin` accepts nicknames). 62-digit legacy
guids from the old system are still accepted on read and in the registry,
so old entries keep working.

### 2. The carrier: `team_red_blue`

Every client already sends a `team_red_blue` userinfo key; the server
repurposes its *value* to carry the guid alongside the team bit:

```
team_red_blue = <guid><0|1>-blue|-red      e.g. 6023806337116247675253030-blue
```

The team bit MUST terminate the leading digit run. Verified in IDA:
`dmctf_c::AssignTeam` (gamex86) reads the key with
`atoi(Info_ValueForKey(userinfo, "team_red_blue"))` and teams `(v & 1) +
1` (1 = blue, 2 = red) — and `atoi`'s parity is set entirely by the last
digit of the leading digit run. So the older prefix text
`blue-<guid>-0 | red-<guid>-1` form `atoi()`s to 0 (no leading digits)
and reads as blue for *every* carrier, force-teaming red players to
blue — prefix text is unworkable, full stop. But a bit-terminated run
reads correctly (overflow wraps, low bit stays the trailing bit), which
leaves room for a human-readable suffix after a dash: the color word is
readability plus a free mismatch check (bit says blue but word says red
→ identity rejected). Three shapes arrive on read:

| userinfo value | meaning | example |
|---|---|---|
| `<guid><0\|1>-blue\|-red` | identity + team (canonical, emitted; color must match the bit) | `6023806337116247675253030-blue` |
| all-digit `<guid><0\|1>` | identity + team (plain form, e.g. legacy 62-digit guids — with suffix they'd exceed the 64-char value limit) | `6023806337116247675253030` |
| bare `0` / `1` | **collapsed** — team only, identity lost | `1` |
| 32-bit decimal of `<guid><bit>` | same identity after SoF `atol` wrap; low bit is the team | `303041696` |

Collapse happens two ways: the client rebuilds the key itself (team menu,
team swap reset it to a bare team bit) — and the game itself writes it
back: the same `AssignTeam` does `Com_Sprintf(buf, "%d", team - 1)` +
`Info_SetValueForKey(userinfo, "team_red_blue", buf)` whenever it assigns.
A collapsed slot's guid is gone from userinfo — recovering it is the
slot guid's job (§4). A 32-bit wrap of this slot's `<guid><bit>` is the
same identity (§5). Anything else (empty, garbage, color/bit mismatch) is
invalid and treated the same way.

### 3. The registry: guid ↔ nickname

Humans can't type 24 digits, so each guid maps to a nickname. The registry
holds both directions:

- `guid → nickname` — who is this identity?
- `nickname_clean → guid` — look up the identity by name, so
  `profile_signin 0 Alice` works with no guid juggling.

`nickname_clean` is derived by stripping color escapes (`%NN`, `^X`),
lowercasing, and keeping `a-z0-9` only (`Slot0Test` → `slot0test`). One
clean key maps to one guid; re-using a nickname steals the key while the
old guid entry survives until explicitly deleted (same as the `.func`
`~guid_by_` overwrite).

### Registry file: one file per server

The whole roster lives in a **single file** — no per-player files, no
subdirectories:

`<user>/sofplus/data/profiles/registry.cfg`

`<user>` is the server's user directory (the same `User/` or
`user-<PORT>/` dir the game writes its own configs to), so each server
instance has its own roster. It is the same path the `.func` used via
`sp_sc_cvar_save` relative to `sofplus/data/`, which is why old files
import as-is.

A real file looks like this — header comment, then one entry per line,
sorted by guid, rewritten whole on every save:

```
# sof_buddy profiles registry (guid nickname)
111111111111111111111111 Alice Smith
222222222222222222222222 Bob
602380633711624767525303 Slot0Test
```

Storage rules:

- **Format:** `<guid> <nickname>`. The nickname is everything after the
  first blank, so spaces are fine; surrounding quotes are stripped if
  present.
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

Legacy `.func` lines import cleanly and are re-saved in native form on the
next save:

```
set "~reg_602380633711624767525303" "Slot0Test"   # accepted on load
set "~guid_by_slot0test" "602380633711624767525303"  # rebuilt, skipped
```

### 4. The slot guid

Each connected slot stores one `guid` (memory only, cleared on disconnect).
It is written from the first valid `team_red_blue` guid on that slot, and
overwritten when a new roster identity binds. A userinfo whose
`team_red_blue` is only `0` or `1` does not assign it.

| Field | Written when | Purpose |
|---|---|---|
| `guid` | first valid guid; overwritten when a roster identity binds | string used to build the `team_red_blue` re-push |

When `team_red_blue` is only `0` or `1`, that read has no guid in it. The
slot's `guid` still holds the roster guid from the earlier read, so the
server stufftexts the full value back. `restored` records the last pushed
value so that client is fixed once, not every tick.

### 5. The stufftext loop

`dmctf_c::AssignTeam` is hooked (`orig_AssignTeam`): when a slot has a
guid and `team_red_blue` is a bare `0` or `1`, the hook returns without
calling stock `AssignTeam`. The skip blocks the premature team-change
kill on a collapsed userinfo. The stufftext restore below pushes the
full guid value, and the echo
runs `AssignTeam` once with a valid shape.

Any other value still calls stock. SoF `_atol` wraps with no overflow
check. Stock registers `team_red_blue` with flags 3, which keeps the
text. `Cvar_Set2` replaces the string with `%d` only when `CVAR_INT`
(`0x20`) is set: `atol` stops at the first non-digit, so `-blue`, `-red`,
and any other character are dropped, then the integer is stored as
float32 and truncated. Above 2^24 that can collapse both team bits onto
one decimal (`303041696` for both `...9760` and `...9761`, `1676235264`
for dende). A restarted server has an empty slot guid, so that decimal is
matched against the roster: the one guid that stores it is signed in.
Both bits often store the same decimal, so the team comes from the live
team and the push is the packed integer below 2^24:
23 bits folded from the guid, and the team in the low bit. Float32 keeps
that value, `AssignTeam`'s `atoi & 1` is the team, and the server splits
the same integer back into the roster guid and the bit. `-blue` / `-red`
cannot be stored on that client.

The server cannot write client userinfo directly — it *asks* the client to
do it, via the engine's `svc_stufftext` channel (`Buddy_StuffText`,
delivered to that client only). One restore round trip:

1. Hook sees userinfo without a usable guid (§2, collapsed/invalid) while
   the slot's guid names a roster guid.
2. Server resolves the team bit (live server team first — §6 — then the
   parsed userinfo team, then the slot's last team) and builds
   `<guid><bit>-blue|-red` (plain all-digit for legacy 62-digit guids — §2).
   A client already known to integerize the cvar is stuffed the packed
   decimal (`23` guid bits, team in the low bit) instead.
3. Server stuffs `team_red_blue <full>` into the client's console.
4. The client executes it, updates its own userinfo, and sends the new
   userinfo to the server.
5. `ClientUserinfoChanged` fires with the repaired value → registry bind
   (§7, `ok`).

All of this happens synchronously inside the userinfo hook — no files, no
timers, no rcon round trip (the `.func` needed `dumpuser` + a 200 ms
snapshot delay for the same observation). With
`_sofbuddy_profiles_stufftext 0` (manual mode) step 3 is replaced by a
printed `run: stufftext <slot> team_red_blue <full>` line for the admin to
run by hand.

### 6. Team sources

A re-push needs a team bit, but a collapsed userinfo has no reliable one —
so the team comes from the server side, not the userinfo string:

| Source | Guid? | Use |
|---|---|---|
| `team_red_blue` (hook `userinfo*`) | yes | audit, registry bind |
| `gclient+0x324` (`1`=blue→`0`, `2`=red→`1`) | no | live team bit when re-pushing the slot guid |
| slot's last team | no | final fallback |

Order for a push: live team → parsed userinfo team → slot's last team. No
usable team anywhere means `pending` — reported, retried on the next
observation.

### 7. Audit states (`profile_query`)

`profile_query` walks every in-use edict and classifies each slot.
"Valid" below means the userinfo parses as a real guid: 24 digits
(legacy 62 accepted) in `<guid><bit>-blue|-red` or plain all-digit form
(§2). Anything else — empty, bare team bit, garbage — is not valid.

| State | Meaning |
|---|---|
| `ok` | userinfo carries a roster-known guid → bound (`registered = 1`). A `CVAR_INT` client (truncated decimal or the packed integer) is marked `CVAR_INT` on the line |
| `wrong` | userinfo lost the guid, but the slot's `guid` names a roster guid → needs a re-push |
| `guest` | nobody to fix — left alone (two cases, see below) |

A **guest** is either:

- a well-formed but **unregistered** guid — the shape parses fine, the
  roster just doesn't know it. Typically a guid from another server (or
  typed by hand). If the player belongs here, `profile_import` their
  guid + nickname and the next userinfo read binds automatically; until
  then the server never touches it.
- **no usable guid and no slot guid** — bare `0`/`1`, empty, or garbage, on a
  slot that never held a guid. A `CVAR_INT` decimal that matches one roster
  guid is signed in. A decimal that matches none, or two, stays a guest.

`wrong` is the total diagnosed; `pushed` + `manual` + `pending` partition
it — every wrong slot ends in exactly one of those outcomes. Per-slot
line plus the summary (same shape as the `.func`):

```
query: 2 ok, 1 wrong, 1 pushed, 0 manual, 0 guests, 0 pending
```

- `wrong` — slots diagnosed wrong (slot guid names a roster guid the userinfo lost)
- `pushed` — of those, the server sent the fix itself via stufftext
- `manual` — of those, the server deliberately sent nothing. Manual mode
  (`_sofbuddy_profiles_stufftext 0`) disables automatic stufftext:
  `profile_query` still diagnoses each wrong slot and still computes the
  exact fix, but instead of sending it, it prints
  `run: stufftext <slot> team_red_blue <full>` for the admin to review
  and run by hand. Use it as a dry-run, or when you don't want the
  server writing to client consoles unattended.
- `pending` — of those, still unfixed for one of two reasons: either no
  team bit was available anywhere (live team, userinfo team, and slot
  team all unknown), so no push could even be built — or the push was
  attempted and failed (client went away mid-query, send failed).
  Nothing is lost: the slot guid is intact, and the fix is retried
  automatically on the next userinfo change, or on the next
  `profile_query`.

### Lifecycles

- **Enroll:** `profile_register Alice` (mint, auto-save) →
  `profile_signin 0 Alice` (push by nickname) → client's userinfo updates
  → hook binds → `ok`. The guid is printed once and never typed again.
- **Returning player:** connects with the guid still in their config →
  `ClientBegin`/`ClientUserinfoChanged` reads it → slot guid set → `ok`
  with no admin action.
- **Collapse:** team menu resets userinfo to bare `1` → hook sees the
  slot guid → immediate stufftext re-push → client echoes the repaired value
  → `ok` again. If the client is mid-transition with no team yet, the
  slot reports `pending` until the next observation.
- **Re-register:** registry wiped but clients still carry guids →
  `profile_import <guid> <nick>` per entry → next userinfo read binds
  automatically, no signin needed.
- **Disconnect** clears the slot. Map changes keep state; the next
  Begin/userinfo read re-binds.

## Commands (server console / rcon)

Canonical `profile_*` names, registered on boot via `Cmd_AddCommand`;
multi-arg works over rcon, so `profiles_aliases.cfg` entry wrappers are
not needed. Legacy `prof_*` aliases (`prof_admin_add`, `prof_admin_del`,
`prof_apply`, `prof_enforce`, `prof_register`, `prof_get_slot_by_id/nick`,
`prof_admin_save/load`) point at the same handlers
(`prof_admin_add` → `profile_import`, `prof_apply`/`prof_register` →
`profile_signin`, `prof_enforce` → `profile_query`).

| Command | Args | What it does |
|---|---|---|
| `profile_register` | `<nickname>` | mint a fresh guid and add the player; auto-saves (prints the guid) — the normal case |
| `profile_import` | `<guid24> <nickname>` | guid AND nickname, both required — re-register a guid the player already has (see below); auto-saves |
| `profile_del` | `<guid24> / <nickname>` | guid OR nickname, either one — the other may be omitted |
| `profile_signin` | `<slot>` | auto: re-read the guid the client's userinfo carries, bind if roster-known and not already on another slot |
| `profile_signin` | `<slot> <guid/nickname>` | explicit: push a roster entry to the slot (nickname accepted, no guid copy-paste). Refuses when that guid is already on another connected slot |
| `profile_signoff` | `<slot> / <guid> / <nickname>` | unbind: clear the slot guid and collapse client `team_red_blue` to bare team bit |
| `profile_query` | — | audit every connected player, repair `wrong` slots (§7) |
| `profile_get_slot_by_id` / `by_nick` | guid / nickname | prints slot, sets `_profile_found_slot` (and legacy `_prof_found_slot`) |
| `profile_save` / `load` | — | save/load registry file |

When to use `profile_import`: only when the player's client already
carries a guid — e.g. the registry file was lost but clients still have
their `team_red_blue`, or you're copying an entry over from another
server. Re-registering the same guid means the next userinfo read binds
automatically with no `profile_signin` needed. In every other case use
`profile_register` and let the server mint.

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

`profile_get_slot_by_id/nick` maintain `_profile_found_slot` and the
legacy `_prof_found_slot` for script-compat (set via `Cvar_SetValue`).

## Wiring (native)

- Hooks `ClientBegin` + `ClientUserinfoChanged` (+ `ClientConnect` /
  `ClientDisconnect` bookkeeping) via `game_export_t` slots
  `ge+0x24/0x28/0x2c/0x30` — same swap technique as minigames/clsv for
  `ClientCommand`, but on slots no other feature touches.
- Reads `team_red_blue` with `Info_ValueForKey` straight from the hook's
  `userinfo*` (falls back to `gclient+0xCC` on `ClientBegin`).
- `profile_query` walks every in-use edict, audits, and re-pushes.
- No game binary patching: `profiles_OnGameDllLoaded` loads the registry,
  registers `profile_*` (+ deprecated `prof_*` aliases) via the repoint-safe
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
   double-pushes). Update binds from `prof_admin_add` → `profile_import`,
   `prof_apply` → `profile_signin <slot> <guid/nickname>`,
   `prof_enforce` → `profile_query`, etc.; old names still work but
   log under the new canonical names.
4. Stop `export-fire` / `userinfo-rcon` for this host (unless another
   feature still needs them), unset `RCON_PASSWORD`, drop the
   `user-<PORT>` symlink if nothing else uses it. `_sp_sv_limit_userinfo_change`
   can go back to stock.
