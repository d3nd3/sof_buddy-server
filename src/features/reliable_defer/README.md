# reliable_defer

**The one-sentence version:** the server talks to each player through two
channels — a *reliable mailbox* (chat, commands, setup data: everything that
must arrive) and a *snapshot* (positions, sounds, effects: the per-tick game
state). This feature watches the reliable mailbox and, when too much mail
piles up at once, holds some of it back and delivers it over the next few
ticks instead of stuffing it all into one packet.

**Why that matters:** every tick the server already sends each player one UDP
packet. That packet can only carry so much. When a script fires five long
prints in one frame — or a big backlog is still waiting for the player's
acknowledgement — cramming everything into the mailbox at once either bursts
the mailbox (player gets kicked with an overflow) or squeezes the game
snapshot out of the packet (visible lag/teleporting). Holding mail back a
tick or two is always safer than either of those.

## The two channels (an analogy)

Think of it like a post office that sends one van per village per day:

- The **reliable mailbox** (`client+0x52B4`, the engine's `netchan.message`)
  is registered mail. Every letter is tracked, and the van will keep
  re-delivering a letter until the village signs for it (the ACK). Only one
  registered parcel can be "in flight" per player at a time
  (`reliable_length`).
- The **snapshot** (entity frame + `datagram`) is the daily newspaper. If it
  doesn't fit in the van, yesterday's paper is simply thrown away
  (`Netchan_Transmit: dumped unreliable`) — nobody re-sends it.

This feature only ever touches the **reliable mailbox**. The snapshot is
built and sent exactly as stock builds it. One thing to know: if the
reliable half of a packet gets big, stock skips the snapshot for that tick
entirely (`Netchan_Transmit: dumped unreliable`) — the client misses one
frame number, playerstate update and entity delta (about a tenth of a
second of frozen game at 10 ticks/second), then resyncs on the next tick.
A single skipped snapshot is harmless; a *sustained* fat reliable backlog
means sustained hitching. That trade-off is stock behaviour, not something
this feature invented — but see `_sofbuddy_reldef_frame_first` below, which
lets the snapshot jump the queue.

## Every reliable packet type, and what the defer does with it

Packet numbers and layouts below are read from the client's mail sorter
(`CL_ParseServerMessage` @ `0x2000ee30` / sof-bin `0x80ca8a8`). Opcodes `1`–`0x28`
are the legal set. Anything else, including `0x00` and `0x03`, is
`Illegible server message (Last command was %s)`. Opcodes `0x14`–`0x16` are
legal numbers but the client still errors (`Out of place frame data`). The
`%s` is the previous command in that same message, not the previous packet.

A rule of thumb: **small writes usually sail straight through** (written now,
sent this tick). **When the lane is busy**, **a queue has formed**, or the
**mailbox is nearly full**, the write is held. Held mail is cut only on a
boundary the client would also accept. Several whole messages from one tick
often share one parcel; a parcel is not one message.

### Text you read (chat, notices, announcements)

| # | Packet | What it is, in plain words | How the defer treats it |
|---|--------|----------------------------|-------------------------|
| `0x0B` | `svc_print` | An ordinary text line: kills, notices, script output, chat. Wire: opcode, level byte, text, NUL. Three hooked writes (`MSG_WriteByte` twice, then `MSG_WriteString`). | Kept whole. Several prints may share one parcel. The cut is never between the opcode and the text. |
| `0x0C` | `svc_nameprint` | A chat line with player names. Wire: opcode, client byte, team byte, text, NUL. A team byte of `0` is not the end of the message. | Kept whole, same as a print. |
| `0x11` | `svc_centerprint` | Big text in the middle of the screen. Wire: opcode + text + NUL. | Kept whole. |
| `0x12` | `svc_captionprint` | A captioned center-screen line. Wire: opcode + short string id. | Tiny; sails through, or waits behind the queue. |
| `0x26` | `svc_welcomeprint` | "Print the welcome buffer" — no text on the wire. | One byte. |
| `0x02` | `svc_layout` | A screen-layout string (scores, menus). Wire: opcode + text + NUL. `MSG_ReadString` stops at NUL or `0xFF`. Bytes `0x80`–`0xFE` (altstring) stay inside the text. | Game code writes this into `sv.multicast`, then `PF_Unicast` copies the finished message onto the mailbox with **one** `SZ_Write`. The defer sees that one write, not the earlier `WriteByte`/`WriteString`. |
| `0x20` | `svc_cinprint` | A cinematic subtitle. Wire: two shorts, a byte, then text + NUL. | Kept whole. Rare. |

### Single-player campaign messages (`0x22`–`0x27`)

| # | Packet | What it is, in plain words | How the defer treats it |
|---|--------|----------------------------|-------------------------|
| `0x22` | `svc_sp_print` | "Show campaign message number N." Wire: opcode + short id. No payload. | `SP_Print` builds the packet off to the side, copies it onto `sv.multicast` with `SZ_GetSpace`, then `PF_Unicast` delivers it as one `SZ_Write`. Sails through, or queues as that one write. |
| `0x24` | `svc_sp_print_data_1` | Same, with format arguments, payload ≤ 255 bytes. Wire: opcode, short id, **byte** count, then that many bytes. The bytes are binary (they may contain `0x0B` and NUL). | Same delivery as `0x22`. The cutter skips `count` bytes. It does not look for a NUL. |
| `0x25` | `svc_sp_print_data_2` | Same, payload > 255 bytes (up to the 1024-byte format buffer). Wire: opcode, short id, **short** count, then that many bytes. | Same as `0x24`. |
| `0x27` | `svc_sp_print_obit` | Obituary. Same wire shape as `0x24` (short id, byte count, bytes). | Same delivery and same cutter rule as `0x24`. |

### Commands the client must run

| # | Packet | What it is, in plain words | How the defer treats it |
|---|--------|----------------------------|-------------------------|
| `0x0D` | `svc_stufftext` | A console command the server makes *your* game run (e.g. `cmd baselines …`, `precache …`, stuff from admins/mods). Wire: opcode + text + NUL. | Kept whole. May share a parcel with other whole messages. Order with older mail is preserved (see delivery day). `changing` and `reconnect` are the exception: `SV_Map` writes each as one `SZ_Write` and flushes before the map load, so those two go straight into the mailbox even when a fat parcel is waiting, and that flush does not lift them back out. `changing` is the client's cue to run `menu loading`. |
| `0x1F` | `svc_countdown` | A countdown timer. The client reads a **long**, not a byte. Stock writes `WriteByte` + `WriteLong` into multicast (unreliable). | Reliable mailbox: `MSG_WriteLong` @ `0x2001CBF0` joins an in-flight capture (classifier-only); empty capture → stock. |
| `0x21` | `svc_playernamecols` | Scoreboard name colours. Wire: a count, then either a single index or a start/end run. Not a C string. | Not parsed. A capture that *starts* with it is sealed whole. A capture that *contains* it after a parsed message stops at the colour block, and the block rides along only because its first byte looks like an opcode. |

### World & player setup (joining and spawning)

| # | Packet | What it is, in plain words | How the defer treats it |
|---|--------|----------------------------|-------------------------|
| `0x0E` | `svc_serverdata` | "Here is the server you joined" (protocol, map, settings). Sent once per connect. | **Never deferred.** While a player is still joining (`state < spawned`), *everything* passes through untouched, because join traffic is huge, strictly ordered, and uses its own pacing. |
| `0x0F` | `svc_configstring` | One world setting: model names, sounds, light styles… (an ID + a text). Hundreds are sent while joining. | While joining: passed through (see above). Mid-game changes for spawned players: one parcel per string, like a print. |
| `0x23` | `svc_removeconfigstring` | "Forget world setting N." | Tiny; sails through, or queues whole. |
| `0x10` | `svc_spawnbaseline` | The starting state of one entity (so the client can predict it). | **Partly visible:** the defer sees the label byte, but the entity body is written straight into the mailbox by a writer the hooks cannot see — so the body always goes through immediately. In practice baselines flow during the join (bypassed anyway). |
| `0x13` | `svc_download` | A chunk of a downloading file (a short header + raw bytes). | **Bypasses the defer entirely** (see below) — downloads are lockstep request→chunk→request and the payload is binary. |
| `0x1A` | `svc_ghoulreliable` | Ghoul data that must arrive. Wire: opcode, a short byte-count, then that many bytes. `SV_SendClientDatagram` writes the opcode with `MSG_WriteByte`, then the count and the bytes with `SZ_GetSpace` into the same mailbox. | **Written through.** The opcode is not captured. Holding it would send a header whose body stayed behind, and the client dies with `Ghoul :StringTable underflowed`. When the whole message later sits in a parcel, the cutter skips the counted bytes, including a `0x0B` or NUL inside them. |
| `0x06` | `svc_equip` | Equipment. Wire, when the sub-byte is `1`: three count bytes, each followed by that many (string + long) pairs. Other sub-bytes are just opcode + that one byte. | Not parsed at seal time; deferred strings stay in capture and each `WriteLong` appends 4 B via the Long classifier when capture is non-empty. |
| `0x1C` | `svc_ric` | Remote inventory commands: a count, then that many records. Types 0–4 carry one sized argument (1–4 bytes); type 5 and unknown types carry none. | Parsed. The cut is the count, so a `0x0B` or NUL inside an argument stays in the record. |
| `0x1D` | `svc_restart_predn` | Restart prediction (one byte). | Tiny; sails through. |
| `0x1E` | `svc_rebuild_pred_inv` | Rebuild prediction inventory. | Small; sails through, or queues whole. |
| `0x19` | `svc_damagetexture` | "Mark this texture damaged" (an ID + a byte). | Tiny; sails through. |
| `0x28` | `svc_force_con_notify` | Force a console notification. | Tiny; sails through. |

### Connection housekeeping

| # | Packet | What it is, in plain words | How the defer treats it |
|---|--------|----------------------------|-------------------------|
| `0x07` | `svc_nop` | Nothing — a keep-alive beat. | Passes through; there is nothing to hold. |
| `0x08` | `svc_disconnect` | "You are disconnected." | Passes through immediately. |
| `0x09` | `svc_reconnect` | "Please reconnect." | Passes through immediately. |

## Packets the defer never touches

These travel with the **per-frame snapshot** (the newspaper, not registered
mail), or only exist while joining. The hooks watch one mailbox
(`client->netchan.message`) and ignore every other buffer, so these pass by
exactly as stock sends them:

| # | Packet | What it is |
|---|--------|------------|
| `0x01` | `svc_temp_entity` | Momentary effects: muzzle flashes, explosions, beams. |
| `0x04` | `svc_sound_info` | Sound bookkeeping for the frame. |
| `0x05` | `svc_effect` | A visual effect event. |
| `0x0A` | `svc_sound` | A positioned sound. |
| `0x14`–`0x16` | `svc_playerinfo`, `svc_packetentities`, `svc_deltapacketentities` | The client errors (`Out of place frame data`) if any of these show up in this lane. |
| `0x17` | `svc_frame` | The entity frame itself — the snapshot. |
| `0x18` | `svc_culledEvent` | A visibility-culled event. |
| `0x1B` | `svc_ghoulunreliable` | Animation data that may be lost (re-sent next frame anyway). |

Also untouched: **everything sent while a player is still joining**
(`state < spawned` — server info, configstrings, baselines, files). Join
traffic is large and delicately ordered, so the feature steps aside
completely until the player has spawned (any parcel it was holding for that
player is thrown away first, so nothing stale can leak into the fresh
session).

One honest footnote: the hooks decide by *mailbox*, not by *packet type*.
If a mod routes one of the snapshot packets through the reliable mailbox,
it gets the same fair treatment as everything else (small → now, big or
busy → whole parcels, in order). `svc_spawnbaseline` writes most of its
bytes through a back door (`SZ_GetSpace` / delta writers) the hooks cannot
intercept, and it only happens during join, which is passed through anyway.
`svc_ghoulreliable` uses that same back door for its body, so its opcode is
written straight into the mailbox (see the table) and is not deferred.

## How it decides, step by step

Every delivery to a player's mailbox goes through one checkpoint with three
possible outcomes: **deliver now**, **hold for later**, or **throw away**
(only when it could never be delivered at all).

```
              delivery for a player's mailbox
                              |
              +---------------+---------------+
              |               |               |
         switched off     not really      checkpoint
         or not ready     our mailbox      below
              |               |               |
              v               v               v
         deliver as      deliver as      deliver now /
         stock does      stock does      hold / throw away
```

The checkpoint, in order (first match wins):

| # | Situation | Outcome | Why, in plain words |
|---|-----------|---------|---------------------|
| 0 | Player is still joining | **Deliver now** | Join traffic is sacred; don't touch it. |
| 1 | Empty delivery | **Deliver now** | Nothing to do; stock ignores it too. |
| 2 | Bigger than the whole mailbox (`msgMaxsize`) | **Throw away** | It could never fit, not now, not later. (On MP that means anything over 1384 bytes in one parcel — chunk big script output.) |
| 3 | A registered letter is still waiting for its signature (`reliable_length > 0`) | **Hold** | Only one registered parcel may be in flight. Piling more into the mailbox would glue many messages into one giant parcel. Wait for the ACK. |
| 4 | Other parcels are already waiting | **Hold** | First come, first served — a new delivery must not jump the queue, or mail arrives out of order. |
| 5 | `_sofbuddy_reldef_one_per_tick` is `1` | **Hold** | Only matters if you turned that setting on (don't). It means "wrap every delivery as a parcel first, no matter how tiny." Default **off** — leave it off. |
| 6 | The mailbox is nearly full (`cursize + length > msgMaxsize − reserve`) | **Hold** | Keep headroom for the engine's other writers (ghoul, configstrings) so the mailbox never bursts (burst = player kicked). |
| 7 | Already holding 32 parcels for this player | **Throw away** | Memory safety cap; dropping beats growing forever. |
| 8 | Already holding 256 KiB for this player | **Throw away** | Same, in bytes (oldest parcel is evicted once first to make room). |
| 9 | None of the above | **Deliver now** | Lane open, queue empty, room to spare — behave exactly like stock. |

(`msgMaxsize` and the reserves come from the engine's `buffersize`; see
"Size math" below.)

## Parcels, the waiting line, and delivery day

- A **parcel** (a "blob" in the code) is a piece of mail sitting in *our*
  post office instead of the player's mailbox: the same bytes, in the same
  order, as if they had been delivered immediately. Each player has their
  own waiting line (first in, first out).
- While a parcel for a player is being packed, every further delivery for
  that player *that tick* joins the same parcel — newer mail can never ship
  ahead of older mail.
- A parcel is **not** sealed when a text message ends. Bytes accumulate in
  the capture until the mailbox would overflow, or until the send tick.
  The cut (`RelDef_LastCompleteEnd`) walks opcodes the client understands
  and stops at the first one it cannot finish. `0x24` / `0x25` / `0x27`
  skip a counted payload; a NUL or `0x0B` inside that payload is not a cut.
- On overflow, a tail that does not start with an opcode (`0x01`–`0x28`) is
  **dropped**, not held. That tail is what used to ride out after a finished
  `svc_layout` and make the client report `Illegible server message (Last
  command was svc_layout)`. A tail that does start with an opcode stays in
  the capture so the rest of that message can join it.
- At the send tick the whole capture is sealed with `RelDef_SealEnd`. A
  leading fragment (first byte not an opcode) is dropped. A tail that starts
  with an opcode is shipped with the parcel, even if that message is not
  finished yet. An orphan string (`MSG_WriteString` while staging already
  ends on a complete message, and the capture is empty) is dropped instead
  of being appended after the layout.

**Delivery day** is the send tick, per player, after that frame's game
writes have already landed:

1. Seal the capture onto the waiting line (rules above).
2. Lift whatever is already in the mailbox (this frame's "deliver now"
   bytes) off to the side.
3. Load waiting parcels into the empty mailbox, oldest first, while the
   lane is open and the next parcel fits. Then put the lifted bytes back
   on the end. If they no longer fit, they rejoin the waiting line instead
   of being stuck in front of older mail.

Older held mail therefore leaves before this frame's direct writes. A
queued fragment is not glued on after a layout that was written now.

Then stock builds the snapshot and sends **mail + newspaper in one van**
(`Netchan_Transmit`), exactly as it always has.

### Letting the snapshot jump the queue (`frame_first`, on by default)

Big parcels (a 1000-character print is ~1000 bytes of a 1400-byte van)
leave no room for the snapshot, so stock skips that tick's frame. Normally
that is fine — one missed frame, then resync. But during a long print flood
it becomes continuous hitching for everyone.

So by default the snapshot gets priority: a parcel bigger than the snapshot
estimate (`maxDripBytes`, 692 bytes on MP) waits for a quieter tick instead
of eating someone's frame. Small mail (chat, commands, setup) is unaffected
— it always fits alongside the snapshot and flows as before. Big mail is
*delayed, never stuck*: if the oldest waiting parcel has waited
`_sofbuddy_reldef_max_drip_wait` ticks (default 10, about a second), it
ships anyway and sacrifices that one frame. In short:

- `1` (default): snapshot first — smooth game, big prints may arrive ~1 s later.
- `0`: mail first — faster chat, frames may skip during floods.

Turn it off (`set _sofbuddy_reldef_frame_first 0`) only if chat latency
matters more to your server than snapshot smoothness.

`changing` and `reconnect` do not take this wait. They are written into
the mailbox immediately, and the flush that `SV_Map` does before
`SpawnServer` leaves that mailbox alone — a drip must not lift `changing`
out to make room for a scoreboard. That flush is also the one send where
the client is still spawned, so the engine would attach a snapshot.
The client parses `changing` and then that snapshot, spends its one-shot
loading-menu close while the servercount is still the old map's, and the
menu stays up until a later `reconnect` — which waits on the ack, and an
alt-tabbed client acks slowly. For that send only, a spawned client whose
mailbox contains `changing` or `reconnect` is treated as connected, so the
engine transmits the mailbox with no snapshot, then the state is restored
before `SpawnServer`. `changing` is what makes the client run `menu loading`.
Once the client drops below spawned the rest of the old-map queue is
discarded, but a `changing` or `reconnect` still sitting on it is written
out first. Join traffic after that (`serverdata`, configstrings, `cmd begin`)
is written straight through, never appended onto a capture left from the
previous map, or the client sits on the loading menu with no `begin`.

#### For developers: `frame_first` mechanics (exact)

Drip gate per blob, evaluated oldest-first in `RelDef_CanDrip`
(`reliable_defer_logic.h`, called from `QueuePopWrite`):

```
drip  <=>  reliable_length == 0
         && msgCursize + blobLen <= msgMaxsize
         && ( !frameFirst
              || msgCursize + blobLen <= maxDripBytes
              || oldestWaitTicks >= maxDripWaitTicks )
```

- `oldestWaitTicks` is measured in server frames: every queued blob records
  `sv_framenum` (@ `0x203A1F30`) at enqueue time (`SlotQueue::enqueued`,
  parallel to `blobs`); wait = current `sv_framenum` − front enqueue frame,
  clamped at 0. Cost is one `int` per blob and one global read per drip
  attempt.
- `maxDripBytes` is a standing estimate, not a measurement: the frame is
  built *after* the drip (`SV_SendClientDatagram` runs later in the same
  send pass), so its true size is unknowable at drip time. The estimate is
  `buffersize − 8 − frameReserve` (692 B on MP); a blob under it leaves room
   for a typical snapshot. `maxDripBytes` never overrides the hard staging
   cap — a blob over `msgMaxsize` never drips, escape or not. (Degenerate
   non-positive caps are ignored rather than blocking everything:
   `msgMaxsize <= 0` skips the staging check, `maxDripBytes <= 0` skips the
   frame-first check.)
- `maxDripWaitTicks <= 0` disables the escape: an over-estimate blob is held
  indefinitely while the lane is open. Not recommended; the default 10
  (≈1 s at 10 ticks/s) bounds the worst-case chat delay instead.
- **Head-of-line blocking is real:** the drip loop only ever looks at the
  FIFO front. A held fat blob also holds every younger parcel behind it —
  including small chat that would fit. The escape bound is what keeps this
  from becoming a stall: at most `maxDripWaitTicks` ticks of full-queue
  silence, then the fat blob ships (one skipped frame) and the line drains.
- Untouched paths: connect handshake (rule 0 bypasses before any of this),
  `one_per_tick` classification, and the unreliable lane. `frame_first`
  only narrows the drip gate; it never reorders, splits, or drops.
- Numeric example (MP, `maxDripBytes` 692): 259 B prints drip freely, several
  per tick; a 1002 B print waits up to 10 ticks for a roomy moment, then
  ships whole.

#### For developers: `one_per_tick` mechanics (exact)

`_sofbuddy_reldef_one_per_tick` (default `0`) adds two extra capture rules
in `RelDef_Classify` (`reliable_defer_logic.h`), evaluated in order between
the queue-nonempty check and the staging-reserve check:

```
... queueCount > 0                    -> QUEUE   (unchanged)
onePerTick && msgCursize > 0          -> QUEUE   (extra rule A)
... staging reserve check                           (unchanged)
... queue cap checks                                (unchanged)
onePerTick                            -> QUEUE   (extra rule B: fallthrough)
WRITE_NOW                                            (unreachable when =1)
```

- **Rule B** makes `WRITE_NOW` unreachable for spawned clients: every hooked
  write to `message` — even 1 byte into empty staging with the lane open —
  is captured and becomes a blob. **Rule A** additionally stops coalescing:
  with the flag off, small writes still accumulate in staging until the
  reserve trips; with it on, the second write of a tick already diverts to
  capture.
- Downstream of classification: atomic `CaptureAppend`, message-boundary
  prefix splits, the orphan-string drop, FIFO drip (older parcels, then
  this frame's staging), and the full drip gate (including `frame_first`).
  There is no flush after `MSG_WriteString`. `=1` still depends on the
  cutter understanding every opcode in the capture. An opcode it does not
  parse can be sealed together with whatever follows.
- Costs, which is why it stays off: **+1 tick minimum latency** on all
  reliable mail (nothing ships the same tick it is written; everything waits
  for the next Pre + an open lane); **queue pressure** — a chatty tick that
  stock would coalesce into one staging becomes N blobs, so `maxQueue` (32)
  and `maxQueueBytes` (256 KiB) trip far earlier and excess mail is
  **dropped**, not delayed; and under a constant backlog the drip gate
  (`reliable_length == 0`) still serializes everything to lane speed.
- Legitimate use is **diagnostic**: with `=1`, 100% of spawned reliable
  traffic takes the capture → queue → drip path, which deterministically
  exercises FIFO order, blob sealing, prefix splits, the `frame_first` hold
  and the starvation escape. If a parse or ordering bug reproduces with `=1`
  but not `=0`, the trigger is in the queue/drip path; if it reproduces in
  both, look at the write-now path or outside this feature. Never ship `=1`
  to players.

## Size math (from IDA)

`SV_InitGame` @ `0x2005DB20` calls `NET_Config(multiplayer)`, which is true
when **`maxclients != 1.0`**. `NET_Config` @ `0x2004F2D0` writes
`buffersize` @ RVA `0x136A0C`:

| | `maxclients == 1` (solo) | `maxclients > 1` (dedicated MP) |
|---|------------------------------:|--------------------------------:|
| `buffersize` | **16384** | **1400** |
| `msgMaxsize` (mailbox) | **16368** | **1384** |
| `stagingReserve` (headroom) | **2046** | **256** |
| `frameReserve` (newspaper space) | **8192** | **700** |
| `maxDripBytes` (guidance only) | **8184** | **692** |

MP uses the **small** buffer so each van stays near one Ethernet frame
(~1500 MTU). A normal dedicated server (`maxclients` 4–16) is on the
**1400** row — not 16 KiB.

```
msgMaxsize       = buffersize - 16
stagingReserve   = max(_sofbuddy_reldef_reserve, msgMaxsize / 8)
frameReserve     = _sofbuddy_reldef_frame_reserve  (0 → buffersize / 2)
maxDripBytes     = buffersize - 8 - frameReserve
```

The **8-byte** header is the sequence + acknowledgement numbers on
server→client mail. `maxDripBytes` is only an estimate of how much mail can
ride along without squeezing the newspaper out. With `frame_first` on it
**does** hold a parcel that would exceed that estimate, until
`max_drip_wait` ticks have passed. It is not a reason to drop mail, and it
does not apply while a player is still joining (that path never queues).

Per-player mailbox layout:

| Mailbox | Offset | `maxsize` |
|---------|--------|----------:|
| `datagram` (newspaper drafts) | `+0x2C4` | `buffersize` |
| `netchan.message` (the mailbox) | `+0x52B4` | `msgMaxsize` |
| `reliable_buf` (in-flight copy) | `+0x92BC` | `msgMaxsize` |

## Settings

| Setting | Default | What it does, simply |
|---------|---------|----------------------|
| `_sofbuddy_reldef` | `1` | Master switch. `0` = post office closed, stock behaviour. |
| `_sofbuddy_reldef_reserve` | `256` | Mailbox headroom: how much space to always keep free. Raised automatically to `msgMaxsize/8` when that is bigger. |
| `_sofbuddy_reldef_frame_reserve` | `0` | How much van space to save for the newspaper; `0` = half the packet. |
| `_sofbuddy_reldef_one_per_tick` | `0` | **Leave this off.** Normally, small deliveries go straight to the mailbox with no waiting. Turning this on forces *every single delivery* — even a 1-byte hello — into a parcel first, one at a time. That slows everything down and used to scramble multi-part messages (a print's label arriving without its text). It exists only for diagnosing weird cases; it makes normal servers worse, not better. |
| `_sofbuddy_reldef_max_queue` | `32` | Max parcels waiting per player; beyond that, new mail is dropped. |
| `_sofbuddy_reldef_max_queue_bytes` | `262144` | Same cap in bytes (256 KiB). |
| `_sofbuddy_reldef_frame_first` | `1` | `1` = snapshots jump the queue (fat parcels wait for room — smooth game, slightly later big prints). `0` = mail first (big parcels ship at once, frames may skip — faster chat, possible hitching). |
| `_sofbuddy_reldef_max_drip_wait` | `10` | Ticks a held parcel waits before shipping anyway (starvation escape). |

Read-only gauges (updated each `CL_SendClientMessages` tick, `CVAR_NOSET`):

| cvar | meaning |
|------|---------|
| `_sofbuddy_reldef_queued` | Parcels pushed to the defer queue since boot |
| `_sofbuddy_reldef_dripped` | Parcels dripped into mailboxes since boot |
| `_sofbuddy_reldef_dropped` | Writes/parcels dropped since boot |
| `_sofbuddy_reldef_capture_bytes` | Bytes in the in-frame capture buffer (usually `0` after send) |
| `_sofbuddy_reldef_oldest_wait` | Max ticks the front parcel on any slot has waited |

## If something looks wrong

| What you see | What it means / what to try |
|--------------|-----------------------------|
| `Illegible server message (Last command was svc_layout)` | The layout itself parsed (opcode, text, NUL). The **next byte in that same message** was not a command. That is a fragment glued on after the layout, not the layout's opcode and text split apart (a split string starts the *next* message, and the last command would be `svc_bad`). Confirm with `set _sofbuddy_reldef 0`. Watch the debug log for `drop orphan string` and `drop tail`. |
| `Illegible server message (Last command was svc_bad)` | The first byte of the message was not a command. A parcel was queued that began mid-payload. |
| `Illegible server message` while joining | Restart the whole server after deploying (not just the map). Check `_sofbuddy_reldef_one_per_tick` is `0`. Join traffic should not be deferred; if `set _sofbuddy_reldef 0` stops it, report the log. |
| Chat/prints arrive late | Expected while earlier mail is still waiting for signatures or parcels are queued — the line is draining. Fix the script burst (fewer/smaller prints), or raise the queue caps. |
| Game hitches during big print floods | Fat reliable parcels are crowding out snapshots (`Netchan_Transmit: dumped unreliable` on the server). Snapshots already get priority by default; if you turned `_sofbuddy_reldef_frame_first` off, turn it back on (`1`). |
| `reldef: drop write` | One write was bigger than the mailbox (1384 B on MP), or it would not fit without cutting a message, or a queue cap was hit. |
| `reldef: drop tail` | Bytes after the last complete message did not start with an opcode and were discarded. |
| `reldef: drop orphan string` | A `MSG_WriteString` arrived with no unfinished message in staging. It was not appended after a finished layout. |
| `reldef: drop staging` | This frame's direct mailbox bytes did not fit after older parcels were dripped, and the queue would not take them either. |

Remember: on dedicated MP a single parcel can never hold more than **1384
bytes**. Five 1000-character prints will always need several ticks — that is
the feature working, not failing.

## Tests

`tools/tests/reliable_defer/run.sh`

## For developers: where the hooks sit

Hooks are on **`SZ_Write`** plus **`MSG_WriteByte` / `MSG_WriteShort` /
`MSG_WriteLong` / `MSG_WriteString`** (`SoF.exe` `0x2001CB00` / `0x2001CB70` /
`0x2001CBF0` / `0x2001CD00`). Only writes into a connected player's
`netchan.message` are classified; `sv.multicast` staging, `datagram`, demo
buffers, and delta-entity `SZ_GetSpace` traffic pass through untouched.

**`MSG_WriteLong` (classifier-only).** When a slot already has bytes in the
in-frame capture (typical mid-`svc_equip` / `svc_countdown`), the hook
absorbs any staged prefix and appends four bytes atomically instead of
letting the long land in the mailbox alone. If capture is empty, the write
is stock-immediate. No extra queue rules.

**Strings (`RelDef_CStrEnd`).** Matches `MSG_ReadString` @ `0x2001E3B0`: a
C string ends on `0x00` or `0xFF`. Counted payloads (`0x24` / `0x25` /
`0x1A` / `0x1C`) do not use that scan.

**Seal (`RelDef_SealEnd`).** A tail that starts with opcode `0x01`–`0x28`
is kept only when `RelDef_MsgEnd` can finish that message inside the
buffer. Otherwise the seal stops at the last complete message and drops
the orphan header (avoids `svc_bad` from a header-only parcel).

### Lockstep bypass: `svc_download` only

`svc_download` (`0x13`) is `Byte(op)` + `Short` + `Byte|Short` +
`SZ_Write(raw)` (`SV_NextDownload` @ `0x20063040`). `RelDef_IsLockstepOp`
is **`0x13` only**. `RelDef_DlStep` arms when that opcode is the first
byte of a new message (staging and capture both empty); the next header
writes and bulk `SZ_Write` bypass classification until the unit finishes
or disarms.

**`SP_Print` (`0x24` / `0x25`)** does not use lockstep. `SP_Print_@0x200583D5`
builds the packet and `PF_Unicast` copies it with one `SZ_Write@0x2005C27F`.
If that blob later shares a capture with other mail, `RelDef_MsgEnd` keeps
the counted payload whole (including embedded `0x0B`, `0xFF`, or NUL).

## What still bites

1. **Opaque opcodes in `RelDef_MsgEnd`.** `svc_equip` (`0x06`) is kept whole
   via capture + `MSG_WriteLong` continuation, not full parsing.
   `svc_rebuild_pred_inv` (`0x1E`), `svc_playernamecols` (`0x21`), baselines,
   and serverdata are still opaque at seal time (parcel ships as one unit
   when the tail looks like an opcode).

2. **No in-process test of `HandleMessageWrite` / `DripSlot`.** `run.sh`
   covers cutters, seal rules, queue FIFO, and policy math. Integration
   behaviour is build-time + live server.

3. **Queue pressure.** Full queues still drop new mail (`reldef: drop
   staging` / `drop write`). Evicting older chat before dropping in-flight
   staging is a separate policy change (not implemented).
