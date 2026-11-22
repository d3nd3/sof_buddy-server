/*
 * .sbd parser for the browser and Node.
 *
 * This mirrors src/features/server_demo/server_demo_format.h. Keep the two in
 * sync; the Node fixture test compiles the C++ writer and feeds its output to
 * this parser.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory();
  } else {
    root.Svdemo = factory();
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const MAGIC = [0x53, 0x4f, 0x46, 0x44, 0x45, 0x4d, 0x4f, 0x00];
  const VERSION = 1;
  const PS_SIZE = 200;
  const ES_SIZE = 116;

  const CHUNK = {
    CLIENT_FRAME: 1,
    ROSTER: 2,
    EVENT: 3,
    END: 4,
    CONFIGSTRING: 5,
    PACKET_FRAME: 6,
  };

  // SoF configstring.h indexes.
  const CS = {
    SKY: 2,
    MODELS: 35,
    PLAYERSKINS: 1415,
  };

  const CLIENT_FLAG = {
    HAS_ENTITY: 1,
    SPECTATOR: 2,
  };

  const EVENT_KIND = {
    MARK: 0,
    CHAT: 1,
    JOIN: 2,
    LEAVE: 3,
  };

  const END_REASON = {
    0: "stopped",
    1: "map change",
    2: "disabled",
    3: "size cap",
    4: "shutdown",
    5: "error",
  };

  // player_state_t->stats[] indexes (SoF q_sh_interface.h)
  const STAT = {
    INV_TYPE: 0,
    HEALTH: 1,
    CLIP_AMMO: 2,
    AMMO: 3,
    CLIP_MAX: 4,
    ARMOR: 5,
    WEAPON: 6,
    INV_COUNT: 7,
    FLAGS: 8,
    LAYOUTS: 9,
    FRAGS: 10,
    STEALTH: 11,
    FORCEHUD: 12,
    DAMAGELOC: 14,
    DAMAGEDIR: 15,
  };

  const utf8 =
    typeof TextDecoder !== "undefined" ? new TextDecoder("utf-8") : null;

  function decode(bytes) {
    if (utf8) return utf8.decode(bytes);
    let out = "";
    for (let i = 0; i < bytes.length; i++) out += String.fromCharCode(bytes[i]);
    return out;
  }

  class FormatError extends Error {
    constructor(message) {
      super(message);
      this.name = "FormatError";
    }
  }

  class Reader {
    constructor(bytes) {
      this.bytes = bytes;
      this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      this.pos = 0;
    }

    need(n) {
      if (this.pos + n > this.bytes.length)
        throw new FormatError("truncated .sbd payload");
    }

    u8() {
      this.need(1);
      return this.view.getUint8(this.pos++);
    }

    i8() {
      this.need(1);
      return this.view.getInt8(this.pos++);
    }

    u16() {
      this.need(2);
      const v = this.view.getUint16(this.pos, true);
      this.pos += 2;
      return v;
    }

    i16() {
      this.need(2);
      const v = this.view.getInt16(this.pos, true);
      this.pos += 2;
      return v;
    }

    u32() {
      this.need(4);
      const v = this.view.getUint32(this.pos, true);
      this.pos += 4;
      return v;
    }

    i32() {
      this.need(4);
      const v = this.view.getInt32(this.pos, true);
      this.pos += 4;
      return v;
    }

    f32() {
      this.need(4);
      const v = this.view.getFloat32(this.pos, true);
      this.pos += 4;
      return v;
    }

    skip(n) {
      this.need(n);
      this.pos += n;
    }

    sub(n) {
      this.need(n);
      const out = this.bytes.subarray(this.pos, this.pos + n);
      this.pos += n;
      return out;
    }

    string16() {
      return decode(this.sub(this.u16()));
    }

    remaining() {
      return this.bytes.length - this.pos;
    }
  }

  class Writer {
    constructor() {
      this.bytes = [];
    }

    u8(v) {
      this.bytes.push(v & 0xff);
    }

    u16(v) {
      this.u8(v);
      this.u8(v >>> 8);
    }

    i16(v) {
      this.u16(v & 0xffff);
    }

    u32(v) {
      this.u8(v >>> 0);
      this.u8(v >>> 8);
      this.u8(v >>> 16);
      this.u8(v >>> 24);
    }

    i32(v) {
      this.u32(v >>> 0);
    }

    f32(v) {
      const b = new ArrayBuffer(4);
      new DataView(b).setFloat32(0, v, true);
      this.bytes.push(...new Uint8Array(b));
    }

    raw(bytes) {
      for (let i = 0; i < bytes.length; i++) this.u8(bytes[i]);
    }

    string16(s) {
      const b =
        typeof TextEncoder !== "undefined"
          ? new TextEncoder().encode(String(s))
          : Array.from(String(s), (c) => c.charCodeAt(0));
      this.u16(b.length);
      this.raw(b);
    }

    patchU16(offset, value) {
      this.bytes[offset] = value & 0xff;
      this.bytes[offset + 1] = (value >>> 8) & 0xff;
    }

    chunk(type, flags, fn) {
      this.u8(type);
      this.u8(flags);
      const sizePos = this.bytes.length;
      this.u16(0);
      const start = this.bytes.length;
      fn();
      const size = this.bytes.length - start;
      if (size > 0xffff) throw new FormatError("chunk too large");
      this.patchU16(sizePos, size);
    }

    finish() {
      return new Uint8Array(this.bytes).buffer;
    }
  }

  function readVec3(r) {
    return [r.f32(), r.f32(), r.f32()];
  }

  function readPlayerState(r) {
    const start = r.pos;
    const ps = { pmove: {} };
    ps.pmove.pmType = r.i32();
    ps.pmove.origin = [r.i16() / 8, r.i16() / 8, r.i16() / 8];
    ps.pmove.velocity = [r.i16() / 8, r.i16() / 8, r.i16() / 8];
    ps.pmove.pmFlags = r.u8();
    ps.pmove.pmTime = r.u8();
    ps.pmove.gravity = r.i16();
    ps.pmove.deltaAngles = [r.i16(), r.i16(), r.i16()];
    ps.pmove.moveScale = r.u8();
    r.skip(1); // pad

    ps.viewangles = readVec3(r);
    ps.viewoffset = readVec3(r);
    ps.kickAngles = readVec3(r);
    ps.weaponkickAngles = readVec3(r);
    ps.remoteVieworigin = readVec3(r);
    ps.remoteViewangles = readVec3(r);
    ps.remoteId = r.i32();
    ps.remoteType = r.u8();
    r.skip(3);

    ps.gun = r.u32();
    ps.gunUUID = r.i16();
    ps.gunType = r.i16();
    ps.gunClip = r.i16();
    ps.gunAmmo = r.i16();
    ps.gunReload = r.u8();
    ps.restartCount = r.u8();
    ps.buttonsInhibit = r.u8();
    r.skip(1);

    ps.bod = r.u32();
    ps.bodUUID = r.i16();
    r.skip(2);

    ps.blend = [r.f32(), r.f32(), r.f32(), r.f32()];
    ps.fov = r.f32();
    ps.rdflags = r.i32();
    ps.soundID = r.i16();
    ps.musicID = r.u8();
    r.skip(1);
    ps.damageLoc = r.i16();
    ps.damageDir = r.i16();
    ps.stats = new Array(16);
    for (let i = 0; i < 16; i++) ps.stats[i] = r.i16();
    ps.dmRank = r.u8();
    ps.dmRankedPlyrs = r.u8();
    ps.spectatorId = r.u8();
    ps.cinematicfreeze = r.u8();

    if (r.pos - start !== PS_SIZE)
      throw new FormatError("player_state_t size mismatch");

    // Convenience aliases for renderers.
    ps.origin = ps.pmove.origin;
    ps.eye = [
      ps.pmove.origin[0] + ps.viewoffset[0],
      ps.pmove.origin[1] + ps.viewoffset[1],
      ps.pmove.origin[2] + ps.viewoffset[2],
    ];
    return ps;
  }

  function readEntityState(r) {
    const start = r.pos;
    const es = {};
    es.number = r.i32();
    es.origin = readVec3(r);
    es.angles = readVec3(r);
    es.angleDiff = r.f32();
    es.modelindex = r.i32();
    es.renderindex = r.i32();
    es.frame = r.i32();
    es.skinnum = r.i32();
    es.effects = r.i32();
    es.renderfx = r.i32();
    es.solid = r.i32();
    es.sound = r.i32();
    es.soundData = r.i32();
    es.event = r.i32();
    es.event2 = r.i32();
    es.event3 = r.i32();
    es.data = r.i32();
    es.data2 = r.i32();
    es.data3 = r.i32();
    es.effectData = new Array(4);
    for (let i = 0; i < 4; i++) {
      es.effectData[i] = {
        effectId: r.i16(),
        bolt: r.i16(),
        size: r.i16(),
      };
    }
    if (r.pos - start !== ES_SIZE)
      throw new FormatError("entity_state_t size mismatch");
    return es;
  }

  function parseHeader(r) {
    const magic = r.sub(8);
    for (let i = 0; i < 8; i++) {
      if (magic[i] !== MAGIC[i]) throw new FormatError("not a .sbd file");
    }
    const version = r.u16();
    if (version !== VERSION)
      throw new FormatError("unsupported .sbd version " + version);
    const headerSize = r.u16();
    const flags = r.u32();
    const psSize = r.u32();
    const esSize = r.u32();
    const maxClients = r.u32();
    const tickMs = r.u32();
    const createdUnix = r.u32();
    r.skip(12); // reserved u32[3]
    if (psSize !== PS_SIZE || esSize !== ES_SIZE)
      throw new FormatError("unexpected state sizes in .sbd header");
    const header = {
      version,
      flags,
      psSize,
      esSize,
      maxClients,
      tickMs,
      createdUnix,
      mapName: r.string16(),
      levelName: r.string16(),
      gamedir: r.string16(),
      serverName: r.string16(),
    };
    if (r.pos !== headerSize)
      throw new FormatError("bad .sbd header size");
    return header;
  }

  function parseClientFrame(payload) {
    const r = new Reader(payload);
    const frame = {
      frame: r.u32(),
      timeMs: r.u32(),
      slot: r.u8(),
      flags: r.u8(),
    };
    r.skip(2);
    frame.ps = readPlayerState(r);
    frame.es = frame.flags & CLIENT_FLAG.HAS_ENTITY ? readEntityState(r) : null;
    if (r.remaining() !== 0)
      throw new FormatError("client frame trailing bytes");
    return frame;
  }

  function parseRoster(payload) {
    const r = new Reader(payload);
    const entry = {
      frame: r.u32(),
      timeMs: r.u32(),
      slot: r.u8(),
      flags: r.u8(),
      name: r.string16(),
      userinfo: r.string16(),
    };
    if (r.remaining() !== 0) throw new FormatError("roster trailing bytes");
    return entry;
  }

  function parseEvent(payload) {
    const r = new Reader(payload);
    const event = {
      frame: r.u32(),
      timeMs: r.u32(),
      kind: r.u8(),
      text: "",
    };
    r.skip(1);
    event.text = r.string16();
    if (r.remaining() !== 0) throw new FormatError("event trailing bytes");
    return event;
  }

  function parseConfigString(payload) {
    const r = new Reader(payload);
    const index = r.u16();
    const value = r.string16();
    if (r.remaining() !== 0)
      throw new FormatError("configstring trailing bytes");
    return { index, value };
  }

  function parseEnd(payload) {
    const r = new Reader(payload);
    const end = {
      firstFrame: r.u32(),
      lastFrame: r.u32(),
      clientFrames: r.u32(),
      maxClients: r.u32(),
      durationMs: r.u32(),
      reason: r.u8(),
    };
    r.skip(3);
    if (r.remaining() !== 0) throw new FormatError("end trailing bytes");
    return end;
  }

  function finalize(demo) {
    const rosterBySlot = new Map();
    for (const entry of demo.rosters) rosterBySlot.set(entry.slot, entry);
    demo.rosterBySlot = rosterBySlot;

    const byTime = new Map();
    for (const f of demo.frames) {
      let group = byTime.get(f.timeMs);
      if (!group) {
        group = { timeMs: f.timeMs, frame: f.frame, slots: new Map() };
        byTime.set(f.timeMs, group);
      }
      group.slots.set(f.slot, f);
      if (f.frame > group.frame) group.frame = f.frame;
    }
    demo.groups = Array.from(byTime.values()).sort((a, b) => a.timeMs - b.timeMs);

    const slotSet = new Set();
    for (const entry of demo.rosters) slotSet.add(entry.slot);
    for (const f of demo.frames) slotSet.add(f.slot);
    demo.slots = Array.from(slotSet).sort((a, b) => a - b);

    demo.slotTrails = {};
    for (const slot of demo.slots) {
      const trail = [];
      for (const group of demo.groups) {
        const f = group.slots.get(slot);
        if (!f) continue;
        trail.push({
          timeMs: group.timeMs,
          frame: group.frame,
          x: f.ps.pmove.origin[0],
          y: f.ps.pmove.origin[1],
          z: f.ps.pmove.origin[2],
          yaw: f.ps.viewangles[1],
        });
      }
      demo.slotTrails[slot] = trail;
    }

    let minX = Infinity,
      minY = Infinity,
      maxX = -Infinity,
      maxY = -Infinity;
    const add = (p) => {
      if (!p) return;
      if (!Number.isFinite(p[0]) || !Number.isFinite(p[1])) return;
      if (p[0] < minX) minX = p[0];
      if (p[0] > maxX) maxX = p[0];
      if (p[1] < minY) minY = p[1];
      if (p[1] > maxY) maxY = p[1];
    };
    for (const f of demo.frames) {
      add(f.ps.pmove.origin);
      if (f.es) add(f.es.origin);
    }
    if (!Number.isFinite(minX)) {
      minX = minY = -512;
      maxX = maxY = 512;
    }
    const pad = 64;
    demo.bounds = {
      minX: minX - pad,
      minY: minY - pad,
      maxX: maxX + pad,
      maxY: maxY + pad,
    };

    demo.firstTime = demo.groups.length ? demo.groups[0].timeMs : 0;
    demo.lastTime = demo.groups.length
      ? demo.groups[demo.groups.length - 1].timeMs
      : 0;
    demo.durationMs = demo.lastTime - demo.firstTime;
    demo.clientFrames = demo.frames.length;
    demo.configstringCount = demo.configstrings.size;
    return demo;
  }

  function parse(input) {
    const bytes =
      input instanceof Uint8Array ? input : new Uint8Array(input);
    const r = new Reader(bytes);
    const header = parseHeader(r);
    const demo = {
      header,
      frames: [],
      rosters: [],
      events: [],
      configstrings: new Map(),
      end: null,
    };

    while (r.remaining() > 0) {
      const type = r.u8();
      const flags = r.u8();
      const size = r.u16();
      const payload = r.sub(size);
      switch (type) {
        case CHUNK.CLIENT_FRAME:
          demo.frames.push(parseClientFrame(payload));
          break;
        case CHUNK.ROSTER:
          demo.rosters.push(parseRoster(payload));
          break;
        case CHUNK.EVENT:
          demo.events.push(parseEvent(payload));
          break;
        case CHUNK.END:
          demo.end = parseEnd(payload);
          break;
        case CHUNK.CONFIGSTRING: {
          const cs = parseConfigString(payload);
          demo.configstrings.set(cs.index, cs.value);
          break;
        }
        default:
          // Reserved side-channel chunks (configstrings/packets) are skipped
          // by this v1 state viewer.
          void flags;
          break;
      }
    }

    demo.events.sort((a, b) => a.timeMs - b.timeMs);
    return finalize(demo);
  }

  function findGroupIndex(groups, timeMs) {
    if (!groups.length) return 0;
    let lo = 0;
    let hi = groups.length - 1;
    let ans = 0;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (groups[mid].timeMs <= timeMs) {
        ans = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    return ans;
  }

  // ---------------------------------------------------------------------
  // Synthetic sample. Used by the viewer's "Load sample" button and by the
  // Node test so the viewer is usable before a real recording exists.
  // ---------------------------------------------------------------------

  function makePS(overrides) {
    const o = overrides || {};
    const pm = Object.assign(
      {
        pmType: 0,
        origin: [0, 0, 0],
        velocity: [0, 0, 0],
        pmFlags: 0,
        pmTime: 0,
        gravity: 800,
        deltaAngles: [0, 0, 0],
        moveScale: 0,
      },
      o.pmove || {}
    );
    return Object.assign(
      {
        pmove: pm,
        viewangles: [0, 0, 0],
        viewoffset: [0, 0, 22],
        kickAngles: [0, 0, 0],
        weaponkickAngles: [0, 0, 0],
        remoteVieworigin: [0, 0, 0],
        remoteViewangles: [0, 0, 0],
        remoteId: -1,
        remoteType: 0,
        gun: 0,
        gunUUID: 0,
        gunType: 1,
        gunClip: 30,
        gunAmmo: 90,
        gunReload: 0,
        restartCount: 0,
        buttonsInhibit: 0,
        bod: 0,
        bodUUID: 0,
        blend: [0, 0, 0, 0],
        fov: 90,
        rdflags: 0,
        soundID: 0,
        musicID: 0,
        damageLoc: 0,
        damageDir: 0,
        stats: new Array(16).fill(0),
        dmRank: 0,
        dmRankedPlyrs: 0,
        spectatorId: 0,
        cinematicfreeze: 0,
      },
      o,
      { pmove: pm }
    );
  }

  function makeES(overrides) {
    const o = overrides || {};
    return Object.assign(
      {
        number: 0,
        origin: [0, 0, 0],
        angles: [0, 0, 0],
        angleDiff: 0,
        modelindex: 1,
        renderindex: 0,
        frame: 0,
        skinnum: 0,
        effects: 0,
        renderfx: 0,
        solid: 0,
        sound: 0,
        soundData: 0,
        event: 0,
        event2: 0,
        event3: 0,
        data: 0,
        data2: 0,
        data3: 0,
        effectData: [
          { effectId: 0, bolt: 0, size: 0 },
          { effectId: 0, bolt: 0, size: 0 },
          { effectId: 0, bolt: 0, size: 0 },
          { effectId: 0, bolt: 0, size: 0 },
        ],
      },
      o
    );
  }

  function writePS(w, ps) {
    w.i32(ps.pmove.pmType);
    for (const v of ps.pmove.origin) w.i16(Math.round(v * 8));
    for (const v of ps.pmove.velocity) w.i16(Math.round(v * 8));
    w.u8(ps.pmove.pmFlags);
    w.u8(ps.pmove.pmTime);
    w.i16(ps.pmove.gravity);
    for (const v of ps.pmove.deltaAngles) w.i16(v);
    w.u8(ps.pmove.moveScale);
    w.u8(0);
    for (const v of ps.viewangles) w.f32(v);
    for (const v of ps.viewoffset) w.f32(v);
    for (const v of ps.kickAngles) w.f32(v);
    for (const v of ps.weaponkickAngles) w.f32(v);
    for (const v of ps.remoteVieworigin) w.f32(v);
    for (const v of ps.remoteViewangles) w.f32(v);
    w.i32(ps.remoteId);
    w.u8(ps.remoteType);
    w.u8(0);
    w.u8(0);
    w.u8(0);
    w.u32(ps.gun);
    w.i16(ps.gunUUID);
    w.i16(ps.gunType);
    w.i16(ps.gunClip);
    w.i16(ps.gunAmmo);
    w.u8(ps.gunReload);
    w.u8(ps.restartCount);
    w.u8(ps.buttonsInhibit);
    w.u8(0);
    w.u32(ps.bod);
    w.i16(ps.bodUUID);
    w.u8(0);
    w.u8(0);
    for (const v of ps.blend) w.f32(v);
    w.f32(ps.fov);
    w.i32(ps.rdflags);
    w.i16(ps.soundID);
    w.u8(ps.musicID);
    w.u8(0);
    w.i16(ps.damageLoc);
    w.i16(ps.damageDir);
    for (let i = 0; i < 16; i++) w.i16(ps.stats[i] || 0);
    w.u8(ps.dmRank);
    w.u8(ps.dmRankedPlyrs);
    w.u8(ps.spectatorId);
    w.u8(ps.cinematicfreeze);
  }

  function writeES(w, es) {
    w.i32(es.number);
    for (const v of es.origin) w.f32(v);
    for (const v of es.angles) w.f32(v);
    w.f32(es.angleDiff);
    w.i32(es.modelindex);
    w.i32(es.renderindex);
    w.i32(es.frame);
    w.i32(es.skinnum);
    w.i32(es.effects);
    w.i32(es.renderfx);
    w.i32(es.solid);
    w.i32(es.sound);
    w.i32(es.soundData);
    w.i32(es.event);
    w.i32(es.event2);
    w.i32(es.event3);
    w.i32(es.data);
    w.i32(es.data2);
    w.i32(es.data3);
    for (const e of es.effectData) {
      w.i16(e.effectId);
      w.i16(e.bolt);
      w.i16(e.size);
    }
  }

  function buildSample() {
    const w = new Writer();
    w.raw(MAGIC);
    w.u16(VERSION);
    const headerSizePos = w.bytes.length;
    w.u16(0);
    w.u32(1); // flags: entity states
    w.u32(PS_SIZE);
    w.u32(ES_SIZE);
    w.u32(3);
    w.u32(100);
    w.u32(Math.floor(Date.now() / 1000));
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.string16("dm/arena");
    w.string16("Synthetic Arena");
    w.string16("base");
    w.string16("svdemo sample");
    w.patchU16(headerSizePos, w.bytes.length);

    const names = ["Grim", "Vex", "Mako"];
    for (let slot = 1; slot <= 3; slot++) {
      w.chunk(CHUNK.ROSTER, 0, () => {
        w.u32(0);
        w.u32(0);
        w.u8(slot);
        w.u8(0);
        w.string16(names[slot - 1]);
        w.string16(
          "\\name\\" +
            names[slot - 1] +
            "\\model\\male\\skin\\" +
            (slot === 1 ? "base" : slot === 2 ? "red" : "blue")
        );
      });
    }

    w.chunk(CHUNK.CONFIGSTRING, 0, () => {
      w.u16(CS.SKY);
      w.string16("sky1");
    });
    w.chunk(CHUNK.CONFIGSTRING, 0, () => {
      w.u16(CS.MODELS);
      w.string16("models/player/grim");
    });
    w.chunk(CHUNK.CONFIGSTRING, 0, () => {
      w.u16(CS.MODELS + 1);
      w.string16("models/player/vex");
    });
    w.chunk(CHUNK.CONFIGSTRING, 0, () => {
      w.u16(CS.PLAYERSKINS);
      w.string16("Grim/male/base");
    });

    const ticks = 120;
    for (let i = 0; i <= ticks; i++) {
      const timeMs = i * 100;
      for (let slot = 1; slot <= 3; slot++) {
        const a = (timeMs / 1000) * 0.7 + slot * 2.1;
        let x;
        let y;
        if (slot === 2) {
          x = Math.sin(a) * 520;
          y = Math.sin(a * 2) * 260;
        } else {
          const radius = slot === 1 ? 360 : 240;
          x = Math.cos(a) * radius + (slot - 2) * 90;
          y = Math.sin(a) * radius;
        }
        const yaw = (Math.atan2(Math.cos(a), -Math.sin(a)) * 180) / Math.PI;
        const speed = slot === 2 ? 220 : 180;
        const ps = makePS({
          pmove: {
            pmType: slot === 3 ? 2 : 0,
            origin: [x, y, 0],
            velocity: [
              -Math.sin(a) * speed,
              Math.cos(a) * speed,
              0,
            ],
          },
          viewangles: [
            Math.sin(a * 0.7) * 8,
            yaw,
            0,
          ],
          viewoffset: [0, 0, slot === 3 ? 30 : 22],
          fov: 90,
          stats: [
            0,
            100 - ((i * (slot + 1)) % 45),
            30,
            90,
            30,
            50,
            slot,
            0,
            0,
            0,
            slot - 1,
            0,
            0,
            0,
            0,
            0,
          ],
          gunType: slot,
          gunClip: 30,
          gunAmmo: 90,
        });
        const es = makeES({
          number: slot,
          origin: [x, y, 0],
          angles: [0, yaw, 0],
          frame: Math.floor(timeMs / 100) % 10,
          // Player bodies resolve through CS_PLAYERSKINS, not s.modelindex.
          modelindex: 0,
        });
        w.chunk(CHUNK.CLIENT_FRAME, 1, () => {
          w.u32(i);
          w.u32(timeMs);
          w.u8(slot);
          w.u8(1);
          w.u16(0);
          writePS(w, ps);
          writeES(w, es);
        });
      }
      if (timeMs === 4000) {
        w.chunk(CHUNK.EVENT, 0, () => {
          w.u32(i);
          w.u32(timeMs);
          w.u8(EVENT_KIND.MARK);
          w.u8(0);
          w.string16("sample mark");
        });
      }
    }

    w.chunk(CHUNK.END, 0, () => {
      w.u32(0);
      w.u32(ticks);
      w.u32((ticks + 1) * 3);
      w.u32(3);
      w.u32(ticks * 100);
      w.u8(0);
      w.u8(0);
      w.u16(0);
    });
    return w.finish();
  }

  return {
    parse,
    buildSample,
    findGroupIndex,
    CHUNK,
    CLIENT_FLAG,
    EVENT_KIND,
    END_REASON,
    STAT,
    CS,
    VERSION,
    PS_SIZE,
    ES_SIZE,
    FormatError,
  };
});
