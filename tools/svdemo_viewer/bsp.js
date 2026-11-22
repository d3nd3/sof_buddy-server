/*
 * Minimal Soldier of Fortune BSP v46 reader for the state-demo viewer.
 *
 * Only what the renderer needs: world vertices/edges/faces/planes/texinfo and
 * the entity lump for a spawn point. Textures/lightmaps/PVS are not loaded;
 * faces get a deterministic flat colour from their texture name.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory();
  } else {
    root.SofBsp = factory();
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const MAGIC = [0x49, 0x42, 0x53, 0x50]; // "IBSP"
  const VERSION = 46;
  const HEADER_LUMPS = 22;
  const HEADER_SIZE = 8 + HEADER_LUMPS * 8;

  const LUMP = {
    ENTITIES: 0,
    PLANES: 1,
    VERTEXES: 2,
    VISIBILITY: 3,
    NODES: 4,
    TEXINFO: 5,
    FACES: 6,
    LIGHTING: 7,
    LEAFS: 8,
    LEAFFACES: 9,
    LEAFBRUSHES: 10,
    EDGES: 11,
    SURFEDGES: 12,
    MODELS: 13,
    BRUSHES: 14,
    BRUSHSIDES: 15,
    POP: 16,
    AREAS: 17,
    AREAPORTALS: 18,
    REGIONFACES: 19,
    LIGHTS: 20,
    REGIONS: 21,
  };

  const LUMP_NAMES = [
    "entities",
    "planes",
    "vertexes",
    "visibility",
    "nodes",
    "texinfo",
    "faces",
    "lighting",
    "leafs",
    "leaffaces",
    "leafbrushes",
    "edges",
    "surfedges",
    "models",
    "brushes",
    "brushsides",
    "pop",
    "areas",
    "areaportals",
    "regionfaces",
    "lights",
    "regions",
  ];

  const SURF = {
    SKY: 0x00000004,
    WARP: 0x00000008,
    TRANS33: 0x00000010,
    TRANS66: 0x00000020,
    FLOWING: 0x00000040,
    NODRAW: 0x00000080,
  };

  const TEXINFO_SIZE = 76;
  const FACE_SIZE = 44;
  const PLANE_SIZE = 20;
  const VERTEX_SIZE = 12;
  const EDGE_SIZE = 4;
  const SURFEDGE_SIZE = 4;
  const MODEL_SIZE = 48;

  const utf8 =
    typeof TextDecoder !== "undefined" ? new TextDecoder("utf-8") : null;

  class FormatError extends Error {
    constructor(message) {
      super(message);
      this.name = "FormatError";
    }
  }

  function readFixedString(bytes, offset, length) {
    let end = offset;
    const max = offset + length;
    while (end < max && bytes[end] !== 0) end++;
    const slice = bytes.subarray(offset, end);
    if (utf8) return utf8.decode(slice);
    let out = "";
    for (let i = 0; i < slice.length; i++)
      out += String.fromCharCode(slice[i]);
    return out;
  }

  function hashString(value) {
    let h = 2166136261;
    for (let i = 0; i < value.length; i++) {
      h ^= value.charCodeAt(i);
      h = Math.imul(h, 16777619);
    }
    return h >>> 0;
  }

  function faceStyle(texture, flags, value) {
    const lower = texture.toLowerCase();
    const liquid =
      flags & SURF.WARP ||
      lower.includes("water") ||
      lower.includes("lava") ||
      lower.includes("slime");
    if (flags & SURF.SKY) return { kind: "sky", color: null, alpha: 1 };
    if (flags & SURF.NODRAW) return { kind: "nodraw", color: null, alpha: 1 };

    const h = hashString(texture || "unknown");
    let hue = h % 360;
    let sat = 12 + ((h >>> 9) % 22);
    let light = 36 + ((h >>> 17) % 20);

    // The first pass has no textures, so give common material families a
    // plausible base colour instead of a random hash. Lightmaps still do the
    // actual shading.
    if (lower.includes("grass") || lower.includes("moss")) {
      hue = 100;
      sat = 30;
      light = 38;
    } else if (
      lower.includes("sand") ||
      lower.includes("dirt") ||
      lower.includes("desert") ||
      lower.includes("ground")
    ) {
      hue = 42;
      sat = 32;
      light = 48;
    } else if (
      lower.includes("metal") ||
      lower.includes("crate") ||
      lower.includes("armory") ||
      lower.includes("steel")
    ) {
      hue = 38;
      sat = 10;
      light = 42;
    } else if (lower.includes("wood") || lower.includes("plank")) {
      hue = 28;
      sat = 34;
      light = 34;
    } else if (
      lower.includes("brick") ||
      lower.includes("wall") ||
      lower.includes("stone") ||
      lower.includes("concrete") ||
      lower.includes("castle")
    ) {
      hue = 26;
      sat = 14;
      light = 44;
    } else if (lower.includes("snow") || lower.includes("ice")) {
      hue = 200;
      sat = 12;
      light = 72;
    }
    let alpha = 1;
    if (flags & SURF.TRANS33) alpha = 0.66;
    if (flags & SURF.TRANS66) alpha = 0.33;
    if (liquid) {
      if (lower.includes("lava")) {
        hue = 8;
        sat = 70;
        light = 45;
      } else if (lower.includes("slime")) {
        hue = 95;
        sat = 45;
        light = 36;
      } else {
        hue = 195;
        sat = 45;
        light = 38;
      }
      alpha = 0.55;
      return {
        kind: "liquid",
        color: `hsla(${hue} ${sat}% ${light}% / ${alpha})`,
        alpha,
      };
    }
    return {
      kind: "solid",
      color:
        alpha < 1
          ? `hsla(${hue} ${sat}% ${light}% / ${alpha})`
          : `hsl(${hue} ${sat}% ${light}%)`,
      alpha,
      hue,
      sat,
      light,
    };
  }

  function parseEntities(text) {
    const entities = [];
    const blocks = text.split("}");
    for (const block of blocks) {
      if (!block.includes("{")) continue;
      const body = block.slice(block.indexOf("{") + 1);
      const fields = {};
      const re = /"([^"]+)"\s+"([^"]*)"/g;
      let match;
      while ((match = re.exec(body))) fields[match[1].toLowerCase()] = match[2];
      if (Object.keys(fields).length === 0) continue;
      entities.push(fields);
    }
    return entities;
  }

  function parseSpawn(entities) {
    const order = [
      "info_player_deathmatch",
      "info_player_team1",
      "info_player_team2",
      "info_player_start",
    ];
    let preferred = null;
    for (const classname of order) {
      preferred = entities.find(
        (e) =>
          (e.classname || "").toLowerCase() === classname &&
          typeof e.origin === "string"
      );
      if (preferred) break;
    }
    if (!preferred && !entities.length) return null;
    if (!preferred)
      preferred = entities.find(
        (e) => /info_player/i.test(e.classname || "") && e.origin
      );
    if (!preferred || !preferred.origin) return null;
    const origin = preferred.origin
      .trim()
      .split(/\s+/)
      .map((v) => Number(v));
    if (origin.length !== 3 || origin.some((v) => !Number.isFinite(v)))
      return null;
    let yaw = Number(preferred.angle);
    if (!Number.isFinite(yaw)) yaw = 0;
    return {
      origin,
      yaw,
      classname: preferred.classname || "",
    };
  }

  function parsePointEntity(entities, classname) {
    const entity = entities.find(
      (e) =>
        (e.classname || "").toLowerCase() === classname &&
        typeof e.origin === "string"
    );
    if (!entity) return null;
    const origin = entity.origin
      .trim()
      .split(/\s+/)
      .map((v) => Number(v));
    if (origin.length !== 3 || origin.some((v) => !Number.isFinite(v)))
      return null;
    let yaw = Number(entity.angle);
    if (!Number.isFinite(yaw)) yaw = null;
    return { origin, yaw, classname };
  }

  class Reader {
    constructor(bytes) {
      this.bytes = bytes;
      this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    }

    need(offset, size) {
      if (offset < 0 || offset + size > this.bytes.length)
        throw new FormatError("truncated BSP lump");
    }

    i32(offset) {
      this.need(offset, 4);
      return this.view.getInt32(offset, true);
    }

    u16(offset) {
      this.need(offset, 2);
      return this.view.getUint16(offset, true);
    }

    i16(offset) {
      this.need(offset, 2);
      return this.view.getInt16(offset, true);
    }

    f32(offset) {
      this.need(offset, 4);
      return this.view.getFloat32(offset, true);
    }

    u32(offset) {
      this.need(offset, 4);
      return this.view.getUint32(offset, true);
    }
  }

  function parse(input) {
    const bytes =
      input instanceof Uint8Array ? input : new Uint8Array(input);
    const r = new Reader(bytes);

    for (let i = 0; i < 4; i++) {
      if (bytes[i] !== MAGIC[i]) throw new FormatError("not a SoF BSP file");
    }
    const version = r.i32(4);
    if (version !== VERSION)
      throw new FormatError(`unsupported BSP version ${version} (need 46)`);

    const lumps = [];
    for (let i = 0; i < HEADER_LUMPS; i++) {
      const offset = r.i32(8 + i * 8);
      const length = r.i32(8 + i * 8 + 4);
      if (offset < 0 || length < 0 || offset + length > bytes.length)
        throw new FormatError(`bad lump ${LUMP_NAMES[i]} bounds`);
      lumps.push({ offset, length });
    }

    const planes = [];
    {
      const l = lumps[LUMP.PLANES];
      for (let off = l.offset; off + PLANE_SIZE <= l.offset + l.length; off += PLANE_SIZE) {
        planes.push({
          normal: [r.f32(off), r.f32(off + 4), r.f32(off + 8)],
          dist: r.f32(off + 12),
          type: r.i32(off + 16),
        });
      }
    }

    const vertices = [];
    {
      const l = lumps[LUMP.VERTEXES];
      for (let off = l.offset; off + VERTEX_SIZE <= l.offset + l.length; off += VERTEX_SIZE) {
        vertices.push([r.f32(off), r.f32(off + 4), r.f32(off + 8)]);
      }
    }

    const edges = [];
    {
      const l = lumps[LUMP.EDGES];
      for (let off = l.offset; off + EDGE_SIZE <= l.offset + l.length; off += EDGE_SIZE) {
        edges.push([r.u16(off), r.u16(off + 2)]);
      }
    }

    const surfedges = [];
    {
      const l = lumps[LUMP.SURFEDGES];
      for (let off = l.offset; off + SURFEDGE_SIZE <= l.offset + l.length; off += SURFEDGE_SIZE) {
        surfedges.push(r.i32(off));
      }
    }

    const texinfo = [];
    {
      const l = lumps[LUMP.TEXINFO];
      for (let off = l.offset; off + TEXINFO_SIZE <= l.offset + l.length; off += TEXINFO_SIZE) {
        texinfo.push({
          flags: r.i32(off + 32),
          value: r.i32(off + 36),
          texture: readFixedString(bytes, off + 40, 32),
          nexttexinfo: r.i32(off + 72),
        });
      }
    }

    const models = [];
    {
      const l = lumps[LUMP.MODELS];
      for (let off = l.offset; off + MODEL_SIZE <= l.offset + l.length; off += MODEL_SIZE) {
        models.push({
          mins: [r.f32(off), r.f32(off + 4), r.f32(off + 8)],
          maxs: [r.f32(off + 12), r.f32(off + 16), r.f32(off + 20)],
          origin: [r.f32(off + 24), r.f32(off + 28), r.f32(off + 32)],
          headnode: r.i32(off + 36),
          firstface: r.i32(off + 40),
          numfaces: r.i32(off + 44),
        });
      }
    }

    const rawFaces = [];
    {
      const l = lumps[LUMP.FACES];
      for (let off = l.offset; off + FACE_SIZE <= l.offset + l.length; off += FACE_SIZE) {
        rawFaces.push({
          planenum: r.u16(off),
          side: r.i16(off + 2),
          firstedge: r.i32(off + 4),
          numedges: r.i16(off + 8),
          texinfo: r.i16(off + 10),
          styles: [
            bytes[off + 22],
            bytes[off + 23],
            bytes[off + 24],
            bytes[off + 25],
          ],
          lightofs: r.i32(off + 28),
          texturemins: [r.i16(off + 36), r.i16(off + 38)],
          extents: [r.i16(off + 40), r.i16(off + 42)],
        });
      }
    }

    const lightingLump = lumps[LUMP.LIGHTING];
    const lighting = bytes.subarray(
      lightingLump.offset,
      lightingLump.offset + lightingLump.length
    );

    const sampleFaceLight = (face) => {
      if (
        face.lightofs < 0 ||
        !lighting.length ||
        face.styles[0] === 255 ||
        face.extents[0] < 0 ||
        face.extents[1] < 0
      )
        return null;
      const w = (face.extents[0] >> 4) + 1;
      const h = (face.extents[1] >> 4) + 1;
      const count = w * h;
      if (count <= 0 || count > 65536) return null;
      const start = face.lightofs;
      const end = start + count * 3;
      if (end > lighting.length) return null;
      let r = 0;
      let g = 0;
      let b = 0;
      for (let i = 0; i < count; i++) {
        const p = start + i * 3;
        r += lighting[p];
        g += lighting[p + 1];
        b += lighting[p + 2];
      }
      return [r / count / 255, g / count / 255, b / count / 255];
    };

    const world = models[0] || { firstface: 0, numfaces: rawFaces.length };
    const faces = [];
    let skipped = 0;
    for (
      let fi = world.firstface;
      fi < world.firstface + world.numfaces && fi < rawFaces.length;
      fi++
    ) {
      const inFace = rawFaces[fi];
      if (inFace.numedges < 3) {
        skipped++;
        continue;
      }
      if (inFace.firstedge < 0 || inFace.firstedge + inFace.numedges > surfedges.length) {
        skipped++;
        continue;
      }
      if (inFace.planenum >= planes.length || inFace.texinfo < 0 || inFace.texinfo >= texinfo.length) {
        skipped++;
        continue;
      }
      const faceVerts = [];
      let bad = false;
      for (let i = 0; i < inFace.numedges; i++) {
        const se = surfedges[inFace.firstedge + i];
        const edgeIndex = Math.abs(se);
        if (edgeIndex <= 0 || edgeIndex >= edges.length) {
          bad = true;
          break;
        }
        const edge = edges[edgeIndex];
        const vertexIndex = se >= 0 ? edge[0] : edge[1];
        if (vertexIndex < 0 || vertexIndex >= vertices.length) {
          bad = true;
          break;
        }
        faceVerts.push(vertices[vertexIndex]);
      }
      if (bad) {
        skipped++;
        continue;
      }

      const plane = planes[inFace.planenum];
      const normal = inFace.side
        ? [-plane.normal[0], -plane.normal[1], -plane.normal[2]]
        : plane.normal.slice();
      let cx = 0;
      let cy = 0;
      let cz = 0;
      for (const v of faceVerts) {
        cx += v[0];
        cy += v[1];
        cz += v[2];
      }
      cx /= faceVerts.length;
      cy /= faceVerts.length;
      cz /= faceVerts.length;
      const ti = texinfo[inFace.texinfo];
      const style = faceStyle(ti.texture, ti.flags, ti.value);
      if (style.kind === "nodraw") {
        skipped++;
        continue;
      }
      faces.push({
        index: fi,
        vertices: faceVerts,
        normal,
        center: [cx, cy, cz],
        texinfo: inFace.texinfo,
        texture: ti.texture,
        flags: ti.flags,
        value: ti.value,
        lightofs: inFace.lightofs,
        light: sampleFaceLight(inFace),
        style,
      });
    }

    let minX = Infinity;
    let minY = Infinity;
    let minZ = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    let maxZ = -Infinity;
    for (const f of faces) {
      for (const v of f.vertices) {
        if (v[0] < minX) minX = v[0];
        if (v[1] < minY) minY = v[1];
        if (v[2] < minZ) minZ = v[2];
        if (v[0] > maxX) maxX = v[0];
        if (v[1] > maxY) maxY = v[1];
        if (v[2] > maxZ) maxZ = v[2];
      }
    }
    if (!Number.isFinite(minX)) {
      minX = minY = minZ = -512;
      maxX = maxY = maxZ = 512;
    }

    const entityLump = lumps[LUMP.ENTITIES];
    const entityText = readFixedString(
      bytes,
      entityLump.offset,
      entityLump.length
    );
    const entities = parseEntities(entityText);
    const spawn = parseSpawn(entities);
    const intermission = parsePointEntity(entities, "info_player_intermission");

    return {
      version,
      lumps,
      planes,
      vertices,
      edges,
      surfedges,
      texinfo,
      models,
      faces,
      entities,
      spawn,
      intermission,
      bounds: {
        minX: minX - 16,
        minY: minY - 16,
        minZ: minZ - 16,
        maxX: maxX + 16,
        maxY: maxY + 16,
        maxZ: maxZ + 16,
      },
      stats: {
        rawFaces: rawFaces.length,
        faces: faces.length,
        skipped,
        vertices: vertices.length,
        edges: edges.length,
        texinfo: texinfo.length,
        models: models.length,
        lumps: HEADER_LUMPS,
        lumpBytes: bytes.length,
      },
    };
  }

  return { parse, FormatError, LUMP, LUMP_NAMES, SURF, VERSION };
});
