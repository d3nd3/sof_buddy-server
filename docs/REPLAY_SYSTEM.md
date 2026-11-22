# SoF1 replay system — design notes

Status: working notes for the first recorder (`server_demo`) and its browser
viewer. The file format, configstring capture, 2D viewer, first-person camera
and a flat-shaded SoF BSP v46 world renderer are implemented and tested.
Textures/models, in-game packet replay and the hosted replay server are not
implemented yet.

## Goal

Build a replay system for Soldier of Fortune 1 with three eventual consumers:

1. **Web renderer** — watch a match in the browser, pick a POV, scrub.
2. **Hosted replay server** — a SoF server that loops recent games; players
   connect, chat live, and ask it to load/pause/seek a replay.
3. **In-game client playback** — stock SoF clients watch the replay feed, with
   per-viewer POV selection.

The first requirement is a capture format that does not depend on the client
protocol and does not require the original players to be online. That is what
`server_demo` now records.

---

## Capture is not playback

Two different recordings are useful, and they answer different questions:

| | **State demo** (this) | **Packet demo** (future side channel) |
|---|---|---|
| Captures | `player_state_t` + `entity_state_t` per client/tick | exact `Netchan_Transmit` bytes per client/frame |
| Playback target | web renderer, analysis, replay UI | stock SoF client |
| Depends on protocol | no | yes, tightly |
| Join in progress | seek to any frame | needs checkpoints (configstrings + baselines) |
| File size | ~26 KB/s at 8 clients | roughly similar, plus reliable traffic |
| Failure mode | client changes need format version | client/server build changes need protocol version |

The state stream is the canonical source. A packet side channel can be added
later to the same container (new chunk types) when in-game playback is ready,
without invalidating existing `.sbd` files.

---

## What `attractloop` actually does

`attractloop` is Q2's local demo/cinematic mode, not a replay service:

- `SV_Map(attractloop, level, ...)` sets `sv.attractloop`. A `.dm2` level goes
  to `ss_demo`, `.cin` to `ss_cinematic`, `.pcx` to `ss_pic`.
- The server reads the pre-recorded message stream and forwards it to the
  local client. There is one session clock; the client sees a normal server.
- Stock Q2 refuses remote connections while `sv.attractloop` is set
  (`SV_DirectConnect`: “Remote connect in attract loop. Ignored.”).
- `serverrecord` writes world/entity footage, primarily for demo merging. It
  does not store player state (`serverrecord` demo frames explicitly omit
  `svc_playerinfo`).

So attractloop is the right **playback primitive** for an in-game replay
server, but we must add three things stock Q2 does not have:

1. allow remote clients into the demo state;
2. feed each client a recorded per-slot packet stream instead of the local
   demo file;
3. checkpoint the stream so a viewer can join in progress.

SoF++ nix has a proof of concept for exactly this in
`sof-plus-plus-nix/src/features/demos.cpp` and
`docs/ServerSideDemos.md`; the engine patches are in `src/cvars.cpp`
(`serverside_demos_modified`). It is a useful blueprint, with the caveat that
it is a Linux executable-side mod, not a Windows game-DLL shim.

### Windows attractloop gate (retail SoF.exe)

The retail check is in `SV_ConnectionlessPacket` (`0x2005eea0`), in the
`connect` branch:

```
0x2005e7fe  mov  al, byte ptr [attractloop]   ; attractloop @ 0x203a1f24
0x2005e806  test al, al
0x2005e808  jz   0x2005e85d                   ; normal server: continue
0x2005e81a  call NET_IsLocalAddress
0x2005e822  test eax, eax
0x2005e824  jnz  0x2005e85d                   ; local client: continue
0x2005e826  ...  "Remote connect in attract loop. Ignored."
```

To admit remote viewers into a demo/attractloop state, patch the short branch
at `0x2005e824` from `75 37` (`jnz`) to `EB 37` (`jmp`). That only gets a
viewer through the handshake; the server still has to feed it frames (the
`Netchan_Transmit` side channel above) and `SV_New_f`/`SV_BeginDemoserver`
still assumes the local demo file. Treat this as a research note, not a
production switch: an unpatch on DLL unload must restore the original bytes.

---

## Hosted replay server — options

### A. State-driven observer server (lowest risk)

A separate process (or a web backend) tails `.sbd` files and serves an observer
UI. The live game server only writes files; the replay host never runs game
code. “Loop recent games” is a directory watcher plus a ring buffer.

- Pros: no engine hooks, no protocol work, safe to run next to the match
  server, easy web renderer.
- Cons: not a SoF server; stock clients cannot connect; no in-game chat.

### B. Packet replay inside a SoF server (stock clients)

Hook `Netchan_Transmit` during recording and save per-slot reliable and
unreliable payloads. During playback, override the transmit path for connected
viewers and feed them the recorded stream. Patch the attractloop remote-connect
check, or run a normal `ss_game` server whose game DLL ignores simulation and
serves frames.

- Pros: stock client, real in-game experience, per-viewer POV possible.
- Cons: protocol-coupled; needs checkpoints, configstrings, baselines, ghoul
  file chunks, reliable/unreliable separation, and careful handling of client
  `clc_move` input.

### C. Hybrid (recommended long term)

Record both:

- state chunks for the web renderer, analytics and validation;
- packet chunks for in-game playback.

The replay server can serve the packet stream to SoF clients and the state
stream to the web UI from the same file. A viewer who joins late gets the
nearest checkpoint; a web viewer can start at any client-frame.

---

## Recording pipeline

```
live server tick (10 Hz)
  └─ ClientEndServerFrame Post (per client)
       ├─ player_state_t  (client+0, 200 B)
       ├─ entity_state_t  (edict+0, 116 B)
       └─ roster on userinfo/name change
            ↓
       .sbd buffer (64 KiB)
            ↓
       <userdir>/demos/<map>_<timestamp>.sbd
```

Current controls:

- manual: `svdemo_start [name]`, `svdemo_stop`, `svdemo_status`,
  `svdemo_mark`, `svdemo_flush`, `svdemo_roster`;
- automatic per map: `_sofbuddy_svdemo_auto 1`;
- size cap: `_sofbuddy_svdemo_max_mb`;
- sampling: `_sofbuddy_svdemo_interval`;
- body/entity capture: `_sofbuddy_svdemo_entities`.

The first consumer is implemented in `tools/svdemo_viewer/`: a dependency-free
browser parser plus a top-down viewer, a first-person camera
(`ps.eye` + `viewangles` + `fov`), and a SoF BSP v46 world renderer. The map
pass is flat-shaded (texture-name colour heuristics + a lightmap sample); real
textures and player/world meshes are still missing.

Next capture additions, in order:

1. **Configstring snapshot** (models/player skins/sky) at file start. The
   roster already carries each player's userinfo model/skin, but world entity
   model indexes still need `CS_MODELS`.
2. **Chat/command events** (`EVENT` kinds 1–3 are reserved) so the web UI can
   show chat and the replay server can keep live chat separate.
3. **Packet side channel** (`PACKET_FRAME` chunks) when in-game playback
   starts.
4. **Ring buffer / retention** — keep the last N matches, evict by size/age,
   write an index file for the replay server.

---

## Hosted replay server behaviour

Target UX:

- The replay host runs a normal SoF server with a replay game DLL/profile.
- A session has one authoritative clock, a playlist of recent demos, and a
  loop flag.
- Each viewer has local state: POV slot, pause/seek? (session vs viewer
  controls need a policy), chat identity.
- Chat is live and independent of the replay stream; commands are parsed by
  the replay server, not simulated by the demo.
- Admin commands: load next/previous, goto frame/time, pause/resume, rate,
  loop, reload playlist.
- Player commands: `demo_pov`, `demo_pov_next`, `demo_pov_prev`,
  `demo_time`, `demo_who`, `demo_list` (subject to permissions).
- Join in progress: send `svc_serverdata`, configstrings, baselines from the
  nearest checkpoint, then start at checkpoint frame. Viewers drift only by
  their own packet scheduling, not by simulation.
- If the packet stream is unavailable but state chunks are, a state→packet
  encoder can synthesize frames; this is more work but keeps one source of
  truth.

Non-goals for the replay server: accepting movement input, running game
physics, scoring, or persisting player stats from the replayed match.

---

## Suggested phases

1. **Now:** state recorder + format tests (done).
2. **Now:** `.sbd` reader/dumper tooling and a dependency-free browser viewer
   with a top-down mode (trails, POV, timeline, HUD, marks), a first-person
   camera, and a flat-shaded SoF BSP v46 world renderer — done. Next: real
   textures, player/world meshes, brush submodels, better occlusion.
3. **Now:** configstring snapshots are in the file; the viewer resolves player
   skins/model paths. Next: chat/join/leave events; file index/retention.
4. Build the packet side channel and a checkpoint format; prototype in-game
   playback on a private server.
5. Patch/avoid the attractloop remote-connect restriction and add per-viewer
   POV + join-in-progress.
6. Add the replay-server command surface, live chat and playlist loop.
7. Harden: permissions, malformed-file handling, backpressure, map/asset
   mismatch, version negotiation.

---

## Open questions

- **Map assets for the web renderer:** BSP v46 geometry now loads, but textures
  (M8/M32), texture coordinates, per-pixel lightmaps and brush submodels still
  need loaders. Decide whether to ship assets to the browser or pre-bake them.
- **Checkpoint granularity:** every 1–2 s is probably enough for join-in-
  progress, but reliable entity deltas may need more.
- **POV policy:** can viewers choose a POV independently while the session clock
  is shared? SoF++ nix does this by selecting a different slot's recorded packet
  for each viewer; it is the right model.
- **Chat identity:** reuse profiles/guid, or let replay viewers be anonymous?
- **Storage:** ring buffer in memory vs disk. Disk is simpler and survives
  crashes; a tail-follow reader can serve it live.
- **Protocol/build coupling:** packet demos must record the SoF build/protocol
  version in the header and refuse mismatches rather than corrupt clients.
