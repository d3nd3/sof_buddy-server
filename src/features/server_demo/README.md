# server_demo — state demos without the network

This is the first half of a replay system: **record what the server knew**, in a
form that outlives the engine. It is deliberately not a `.dm2` recorder and it
does not hook `Netchan_Transmit`. A `.sbd` file contains the finished
`player_state_t` for every client on every recorded tick, plus the player edict's
`entity_state_t`, roster and level metadata.

That makes it useful immediately for:

- a web renderer (no SoF protocol implementation required),
- offline analysis (positions, view angles, stats over time),
- a future replay server that wants a canonical, versioned source format,
- debugging anti-cheat / movement / spectator behaviour.

It is **not** directly playable by a stock SoF client yet. That needs either a
network-packet replay layer or a state→packet encoder; see
[Where this goes](#where-this-goes).

---

## What is recorded

Per client, per recorded tick, after the stock `ClientEndServerFrame` has run
`G_SetStats`:

| Field | Source | Why |
|---|---|---|
| `player_state_t` (200 B) | `gclient_t.ps`, `client+0` | camera, pmove origin/velocity, view angles, view offset, weapon, HUD stats |
| `entity_state_t` (116 B) | `edict_t.s`, `ent+0` | body origin/angles, model/skin/frame, effects/events |
| slot, frame, level time | game globals | grouping and playback clock |
| name, userinfo, spectator flag | `gclient_t.pers`, `client+0xCC` | roster without needing configstrings |
| active/spectator state | `gclient_t.resp.spectator`, `client+800` | current mode; falls back to `pers.spectator` (`client+0x2DC`) when unavailable |

The two pointer fields inside `player_state_t` (`gun`, `bod`) are process-local;
the recorder zeroes them on disk. Everything else is copied verbatim. The
fixed-layout mirrors and their offsets are checked by `static_assert` in
[`server_demo_format.h`](server_demo_format.h):

```
pmove_state_t ....... 28 bytes
player_state_t ...... 200 bytes
  stats ............. ps+164 (16 shorts)
  gun pointer ....... ps+108 (zeroed)
  bod pointer ....... ps+124 (zeroed)
entity_state_t ...... 116 bytes
```

Capture point is a **Post** hook on `ClientEndServerFrame` (retail RVA
`0xF9A00`), which `G_RunFrame` calls once per client after `G_SetStats` and the
rest of the stock end-of-frame logic. That is the same state the engine is
about to build the client frame from.

**Compatibility:** the RVAs and the 200/116-byte layouts are pinned to the
retail Windows `oldgamex86.dll` (edict stride `0x464`). A different game build
may move them; that needs new offsets and, if the struct changes, a format
version bump rather than silently reinterpreting old files.

---

## File format (`.sbd`, version 1)

All integers are little-endian. There are no raw compiler structs on disk
except through the checked mirrors above. A reader must reject an unknown
version instead of guessing.

```
magic[8]            "SOFDEMO\0"
u16 version         1
u16 header_size     total header bytes, including strings
u32 flags           bit 0: entity states present
u32 ps_size         200
u32 es_size         116
u32 max_clients
u32 tick_ms         100
u32 created_unix
u32 reserved[3]
str16 map_name
str16 level_name
str16 gamedir
str16 server_name
```

Then a sequence of chunks:

```
u8  type
u8  flags
u16 payload_size
payload[payload_size]
```

| Type | Name | Payload |
|---:|---|---|
| 1 | `CLIENT_FRAME` | `u32 frame, u32 time_ms, u8 slot, u8 flags, u16 reserved, PlayerState, [EntityState]` |
| 2 | `ROSTER` | `u32 frame, u32 time_ms, u8 slot, u8 flags, str16 name, str16 userinfo` |
| 3 | `EVENT` | `u32 frame, u32 time_ms, u8 kind, u8 reserved, str16 text` |
| 4 | `END` | `u32 first_frame, u32 last_frame, u32 client_frames, u32 max_clients, u32 duration_ms, u8 reason, u8 reserved, u16 reserved` |
| 5 | `CONFIGSTRING` | `u16 index, str16 value` (one chunk per non-empty configstring, written at recording start) |

`str16` is a `u16` byte length followed by UTF-8/ASCII bytes (no terminator).
At recording start the writer snapshots every non-empty `sv.configstrings`
entry (`CS_MODELS`, `CS_PLAYERSKINS`, sky, images, …). That is what lets a
renderer resolve `entity_state_t.modelindex` to a model path and a player slot
to its skin. Entries are individual chunks so a map with many precached assets
cannot exceed the 64 KiB chunk cap.
Client-frame payload flags: bit 0 = entity state follows, bit 1 = spectator.
The `CLIENT_FRAME` chunk header also sets bit 0 when entity state is present,
so a scanner can skip the body without parsing `PlayerState`. Roster entries
are emitted when a slot's userinfo/name/spectator state changes, including the
first frame after recording starts. `EVENT` is used by `svdemo_mark`;
chat/join/leave kinds are reserved.

A balanced `END` chunk is written on stop, map change, size cap, disable,
shutdown, idle stop and write error, so a truncated file is detectable.
Automatic recordings that never see damage or a frag are discarded instead of
being published, so an idle spectator/limbo/test client cannot create an empty
replay.

### Size

At 8 clients and 10 Hz, one client-frame is 328 bytes (ps + entity), so a
recording is roughly 26 KB/s ≈ 1.6 MB/min. `_sofbuddy_svdemo_entities 0` cuts
that to about 17 KB/s. The default cap is 64 MiB (~40 minutes at 8 clients).

---

## Commands

| Command | Action |
|---|---|
| `svdemo_start [name]` | Start a recording. Without a name: `<map>_YYYYMMDD_HHMMSS.sbd`. |
| `svdemo_stop` | Write `END`, flush and close. |
| `svdemo_status` | File, map, frame range, client-frames, bytes, interval, entity mode. |
| `svdemo_mark <text...>` | Insert an `EVENT` at the current frame (useful for highlights). |
| `svdemo_flush` | Flush the 64 KiB write buffer now. |
| `svdemo_roster` | Force every slot's roster entry to be re-emitted next tick. |

Commands are server-console/rcon commands, registered at `GameDllLoaded`.
`svdemo_start` refuses while another recording is active.

## Cvars

All `ARCHIVE`, read live:

| Cvar | Default | Meaning |
|---|---:|---|
| `_sofbuddy_svdemo` | `1` | Master switch. `0` stops an active recording and refuses new ones. |
| `_sofbuddy_svdemo_auto` | `0` | Start automatically when a spawned, non-spectator player shows actual play (movement, damage or a frag). |
| `_sofbuddy_svdemo_entities` | `1` | Include `entity_state_t` (body/model/effects). |
| `_sofbuddy_svdemo_interval` | `1` | Record every Nth server frame (1 = every 100 ms tick). |
| `_sofbuddy_svdemo_max_mb` | `64` | Per-file cap in MiB; `0` = unlimited. |
| `_sofbuddy_svdemo_dir` | `demos` | Directory under the engine userdir. |

Automatic mode does not start on the first client frame. It waits for a client
that the engine has spawned and the game does not consider a spectator, and
then requires either sustained horizontal movement, a health change or a frag
gain. Single teleports/intermission moves do not count. If no damage or frag
ever occurs, the automatic file is removed at stop. A recording also stops
after 15 seconds with no live non-spectator player left.

Files land in `<FS_Userdir()>/<_sofbuddy_svdemo_dir>/<name>.sbd`. On a typical
Windows server that is `User/demos/…`; the engine creates the directory.

---

## What Q2's `attractloop` is — and is not

`attractloop` is the engine's **local demo/cinematic mode**:

- `SV_Map(attractloop, level, ...)` sets `sv.attractloop`; a level ending in
  `.dm2`, `.cin` or `.pcx` goes to `ss_demo`, `ss_cinematic` or `ss_pic`.
- A demo server reads the pre-recorded message stream and forwards it to the
  local client (`SV_BeginDemoserver` / `SV_SendClientMessages`).
- In stock Q2, remote clients are **refused** while `sv.attractloop` is set:
  `SV_DirectConnect` says `Remote connect in attract loop. Ignored.`
- Server demos (`serverrecord`) are primarily world/entity footage for demo
  merging; they are not a multi-viewer replay service.

So attractloop is useful as a **playback primitive**, not as a replay system by
itself. The interesting move — already proven by SoF++ nix — is to allow remote
clients into an attractloop/demo server and feed them recorded per-client
network frames. That is a different recorder than this one.

| Approach | Records | Stock client playback | Web renderer | Join-in-progress |
|---|---|---|---|---|
| Stock `record` | one client's server messages | local client | no (protocol) | no |
| Stock `serverrecord` | world entity messages | local `demomap`/attractloop | no | no |
| Packet demo (SoF++ nix SSDM) | per-slot `Netchan_Transmit` bytes | yes, with engine hooks | no (unless decoded) | yes, with checkpoints |
| **State demo (this)** | finished `player_state_t` + entity state | no, not yet | **yes** | n/a (seek by frame) |

---

## Where this goes

The format is the stable part. Three consumers can be built on it:

1. **Web renderer.** Parse the header and `CLIENT_FRAME` chunks; render the
   selected slot's camera from `ps.pmove.origin + ps.viewoffset`, `viewangles`,
   `fov`; draw other clients from their `entity_state_t` and roster model/skin.
   A dependency-free viewer already ships in the separate
   [`sofbuddy-tools`](https://github.com/d3nd3/sofbuddy-tools/tree/master/public/replay)
   project: top-down trajectories, a flat first-person canvas mode, and a
   WebGL2 3D mode with real `.m32` world textures, per-face lightmaps,
   skyboxes and depth-tested players loaded from a game `.pak`. Player/world
   meshes and brush submodels are the next renderer layer.
2. **Replay server, state-driven.** A separate process tails `.sbd` (or a ring
   of recent files) and serves a web/observer UI. This gives “recent games on a
   loop” without touching the live server.
3. **Replay server, in-game.** To let stock SoF clients connect to a replay
   stream, add a packet-capture side channel later: hook `Netchan_Transmit`,
   save per-slot reliable/unreliable payloads next to the state chunks, and
   emit checkpoints (configstrings + baselines) so a viewer can join mid-game.
   The state chunks remain the canonical, human-readable source for the web
   renderer and for validation.

The joinable-server path should copy the lessons in
`/home/dinda/git-projects/repos/d3nd3/public/sof/sof-plus-plus-nix/docs/ServerSideDemos.md`:
allow remote connects to the demo state, feed one authoritative session clock,
serve per-viewer POV, and checkpoint often enough that a late joiner does not
need the whole match.

---

## Verify in game

```bash
./scripts/build.sh --profile experimental
./scripts/cptosof
# start the server, load a map, join, then in the server console:
svdemo_start test
# play for a few seconds
svdemo_status
svdemo_mark test-highlight
svdemo_stop
```

The console should show `recording: <userdir>/demos/test.sbd`, then a summary
with the client-frame count. A successful stop writes an `END` chunk; the
dumper below shows `end: stopped ...`.

## Production recording and database sync

`server_demo` is in the **experimental** tier. A stable/unstable game DLL does
not contain it until the feature is promoted. For a production server, build
the experimental profile and enable automatic per-map recording in the server
config:

```text
set _sofbuddy_svdemo "1"
set _sofbuddy_svdemo_auto "1"
set _sofbuddy_svdemo_entities "1"
set _sofbuddy_svdemo_interval "1"
set _sofbuddy_svdemo_max_mb "64"
set _sofbuddy_svdemo_dir "demos"
```

Files land in `<FS_Userdir()>/<dir>/`. With `+set user ./user-server`, that is
`<server-dir>/user-server/demos/`.

The current production pipeline is pull-based because the backend is private.
`sof-db-demosync` runs on the collector host (`pi4`), reads the remote
`demos/` directory over SSH, validates each `.sbd`, and uploads it to
`sof-db-backend`. It records per-file size/mtime state and retries failed
uploads. The backend then exposes the recording through `/demos`.

See `sof-db-backend`'s [`docs/DEMOS.md`](https://github.com/d3nd3/sof-db-backend/blob/main/docs/DEMOS.md)
for the sync config, visibility controls, match linking, and retention.

## Inspect a recording

`tools/svdemo_dump.py` is the reference parser (Python, no dependencies). It
prints the header, roster, events, end summary and a few camera/body samples:

```bash
python3 tools/svdemo_dump.py User/demos/dm_arena_20261122_180000.sbd
python3 tools/svdemo_dump.py match.sbd --slot 2 --samples 20
```

## Viewer

The browser viewer is maintained in
[`sofbuddy-tools`](https://github.com/d3nd3/sofbuddy-tools/tree/master/public/replay).
It serves at `/replay/` from the Vite app:

```bash
git clone git@github.com:d3nd3/sofbuddy-tools.git
cd sofbuddy-tools
npm install
npm run dev
# open http://localhost:5173/replay/
```

Drag a `.sbd` recording, the matching `.bsp`, and a game `pak0.pak` in. It has
top-down, flat first-person and WebGL2 3D modes, with real `.m32` textures,
per-face lightmaps, `.m32` skyboxes and depth-tested player figures. The pak is
read lazily, so the 700 MB stock archive is never copied into memory.
Configstrings from the recording resolve player skins and model paths in the
HUD; the synthetic sample (`?sample=1`) exercises the format without a server.
See the [replay viewer README](https://github.com/d3nd3/sofbuddy-tools/blob/master/public/replay/README.md)
for controls and the `.pak` stacking rules.

## Tests

```bash
tools/tests/server_demo/run.sh
```

Host-side only: header roundtrip, frame/roster/event/end payloads, pointer
zeroing, structure offsets, bad magic/version and truncation rejection.
