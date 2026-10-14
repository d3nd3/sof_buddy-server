# gamepark — claim follows admin map changes (inside profiles)

When a signed-in server admin (privilege `0`) or map admin (privilege `1`)
changes the map with a sofplus `.rcon` command, they become the signed-in
gamepark holder and any previous holder's rcon session is logged out.
Lives inside the profiles feature folder because the trigger is gated on
roster privilege, like the stats upload.

## Why deferred

The `ClientCommand` hook runs **before** sofplus, so at command time it
cannot know whether `.rcon map` will succeed — rcon login state, granted
commands, and map validity are all sofplus-side. Acting optimistically would
transfer the claim (and log people out) on attempts that change nothing.
Instead the hook only records intent; the `SV_Frame` Post check acts solely
when the requested map really loads. Failed attempts expire silently after
60 s, and a map that loads to something else (rotation, vote, another admin)
drops the pending intent.

The `.rcon` command itself is never consumed: rcon auth, grants, the
discord/screenshot side effects (if any), and the map change stay entirely
sofplus-side. There is no menu page — chat intent plus an actual map load is
the whole signal.

## Trigger set

`.rcon` subcommands that load a new map: `map`, `gamemap`
(case-insensitive). `restart` only reloads in place and is excluded, as is a
bare `.rcon map` with no argument (sofplus usage error, no load). Both the
say-box (`say .rcon map q2dm1`) and direct console (`.rcon map q2dm1`) forms
are recognised. Slots without privilege `0`/`1` are ignored completely.

## Transfer lines

Queued through one `Cbuf_AddText` blob (chunked under 900 B), mirroring the
`spf_sv_gamepark.func` claim flow:

```
sp_sv_info_client <slot>
sp_sc_info_time
set _spf_sv_gamepark_claimed #_sp_sc_info_time_min
add _spf_sv_gamepark_claimed #_spf_sv_gamepark_duration
set _spf_sv_gamepark_claimed_ip <issuer ip>
zero _spf_sv_gamepark_claimed_inactive
sp_sc_cvar_sset hostname #_spf_sv_gamepark_hostname #_sp_sv_info_client_name
zero _spf_sv_rcon_username_<i> / _ip_<i> / _commands_<i>  (old-claim slots)
```

Guards at transfer time (all drop with a console log, no lines queued):

- issuer slot still spawned;
- `_spf_sv_gamepark_duration > 0` (gamepark configured);
- `_spf_sv_gamepark_claimed > 0` (claimed — an unclaimed server is never
  hijacked, and a resetting one is left alone);
- issuer engine address readable (bare dotted quad from
  `svs.clients[slot] + 0x5284`, the same bytes `sp_sv_info_client` feeds
  `inet_ntoa` — verified against the spsv decompile; the userinfo `ip` key is
  empty on this build, so it is never used);
- hostname rename skipped when `_spf_sv_gamepark_hostname` is empty.

Logout is targeted: only spawned slots whose `_spf_sv_rcon_ip_<i>` equals the
**old** claim ip — and which are not the issuer — are cleared, so unrelated
rcon sessions are untouched. Nothing is written to `spf_sv_rcon.cfg`: this
ends sessions, it does not edit the admin list.

## Files/timing notes

- No files: all state is sofplus cvars (which survive map loads engine-side)
  plus one in-memory pending (latest intent wins; cleared on DLL reload via
  `GameDllLoaded`).
- The transfer queues at the first `SV_Frame` Post that observes the new
  map, so it lands before the gamepark 10 s periodic check and before
  `on_client_begin`/`disconnect` handlers can act on stale claim state.
- Cost when idle: one pending-flag check per `SV_Frame`. While pending: one
  `mapname` read per frame. Transfer scans spawned slots' rcon cvars once.

## Commands / cvars

Server console / rcon:

| Command | What it does |
|---|---|
| `gamepark_status` | transfers so far, pending intent (slot, target map, age) |

Settings (all `ARCHIVE`, read live):

| Cvar | Default | What it does |
|---|---|---|
| `_sofbuddy_gamepark` | `1` | master enable (`0` = observe nothing, transfer never fires) |

Gauges (read-only `NOSET`):

| Cvar | What it tells you |
|---|---|
| `_sofbuddy_gamepark_transfers` | completed transfers |
| `_sofbuddy_gamepark_errors` | failed / dropped operations |
| `_sofbuddy_gamepark_pending` | `1` while a transfer intent is armed |

Player console: no new words. `.rcon map|gamemap <name>` behaves exactly as
before for everyone; privilege `0`/`1` issuers additionally arm the transfer.

## Wiring (native)

- `gamepark/callbacks/callbacks.json`: `GameDllLoaded` →
  `gamepark_OnGameDllLoaded` (cvars, `gamepark_status`, pending reset).
  Parent feature is `profiles`, so no `features.yaml` change.
- `gamepark/hooks/hooks.json`: `SV_Frame` Post → `gamepark_SvFramePost`
  (complete on requested-map load / drop stale; priority 55, multi-callback
  safe).
- `minigames.cpp`: `MgTakeClientCommand` calls
  `Gamepark_HandleClientCommand()` right after the stats filter, before
  `DispatchMinigameClientCmd` (hence before the spsv trampoline).
- Pure line-building/parsing in `gamepark_logic.h` (host-testable, no engine
  calls). `MaxClients`/spawned/rcon scans reuse the minigames API; identity
  and privilege come from profiles (`Profiles_IsLevel01Admin`, userinfo
  `name` via `Info_ValueForKey`, address via the engine netchan).
