// Minimal BSP v46 parser test. Builds one triangle by hand so the offsets in
// bsp.js are exercised without shipping a game asset.
import assert from "node:assert/strict";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const Bsp = require("../../svdemo_viewer/bsp.js");

const HEADER_LUMPS = 22;
const HEADER_SIZE = 8 + HEADER_LUMPS * 8;

function buildFixture() {
  const lumps = new Map();
  const entities = Buffer.from(
    '{\n"classname" "worldspawn"\n}\n' +
      '{\n"classname" "info_player_deathmatch"\n"origin" "10 20 30"\n"angle" "90"\n}\n',
    "ascii"
  );
  lumps.set(0, entities);

  const planes = Buffer.alloc(20);
  planes.writeFloatLE(0, 0);
  planes.writeFloatLE(0, 4);
  planes.writeFloatLE(1, 8);
  planes.writeFloatLE(0, 12);
  planes.writeInt32LE(2, 16); // PLANE_Z
  lumps.set(1, planes);

  const verts = Buffer.alloc(3 * 12);
  const coords = [
    [0, 0, 0],
    [64, 0, 0],
    [0, 64, 0],
  ];
  coords.forEach((v, i) => {
    verts.writeFloatLE(v[0], i * 12);
    verts.writeFloatLE(v[1], i * 12 + 4);
    verts.writeFloatLE(v[2], i * 12 + 8);
  });
  lumps.set(2, verts);

  const edges = Buffer.alloc(4 * 4);
  edges.writeUInt16LE(0, 0 * 4);
  edges.writeUInt16LE(0, 0 * 4 + 2);
  edges.writeUInt16LE(0, 1 * 4);
  edges.writeUInt16LE(1, 1 * 4 + 2);
  edges.writeUInt16LE(1, 2 * 4);
  edges.writeUInt16LE(2, 2 * 4 + 2);
  edges.writeUInt16LE(2, 3 * 4);
  edges.writeUInt16LE(0, 3 * 4 + 2);
  lumps.set(11, edges);

  const surfedges = Buffer.alloc(3 * 4);
  [1, 2, 3].forEach((v, i) => surfedges.writeInt32LE(v, i * 4));
  lumps.set(12, surfedges);

  const texinfo = Buffer.alloc(76);
  texinfo.writeInt32LE(0, 32); // flags
  texinfo.writeInt32LE(0, 36); // value
  texinfo.write("test/floor", 40, "ascii");
  texinfo.writeInt32LE(-1, 72);
  lumps.set(5, texinfo);

  const face = Buffer.alloc(44);
  face.writeUInt16LE(0, 0); // planenum
  face.writeInt16LE(0, 2); // side
  face.writeInt32LE(0, 4); // firstedge
  face.writeInt16LE(3, 8); // numedges
  face.writeInt16LE(0, 10); // texinfo
  face.writeInt32LE(-1, 28); // lightofs
  lumps.set(6, face);

  const model = Buffer.alloc(48);
  model.writeFloatLE(0, 0);
  model.writeFloatLE(0, 4);
  model.writeFloatLE(0, 8);
  model.writeFloatLE(64, 12);
  model.writeFloatLE(64, 16);
  model.writeFloatLE(0, 20);
  model.writeInt32LE(0, 40); // firstface
  model.writeInt32LE(1, 44); // numfaces
  lumps.set(13, model);

  let total = HEADER_SIZE;
  for (const [index, data] of lumps) {
    if (index >= HEADER_LUMPS) throw new Error("bad lump index");
    total += data.length;
  }
  const out = Buffer.alloc(total);
  out.write("IBSP", 0, "ascii");
  out.writeInt32LE(46, 4);
  let offset = HEADER_SIZE;
  for (let i = 0; i < HEADER_LUMPS; i++) {
    const data = lumps.get(i);
    const length = data ? data.length : 0;
    out.writeInt32LE(offset, 8 + i * 8);
    out.writeInt32LE(length, 8 + i * 8 + 4);
    if (data) {
      data.copy(out, offset);
      offset += length;
    }
  }
  return out;
}

const fixture = buildFixture();
const map = Bsp.parse(fixture);
assert.equal(map.version, 46);
assert.equal(map.faces.length, 1);
assert.equal(map.vertices.length, 3);
assert.equal(map.rawFaces ?? map.stats.rawFaces, 1);
assert.equal(map.stats.rawFaces, 1);
assert.equal(map.stats.faces, 1);
assert.equal(map.stats.skipped, 0);
assert.equal(map.faces[0].texture, "test/floor");
assert.deepEqual(map.faces[0].normal, [0, 0, 1]);
assert.equal(map.faces[0].vertices.length, 3);
assert.equal(map.faces[0].vertices[0][0], 0);
assert.equal(map.faces[0].vertices[1][0], 64);
assert.equal(map.faces[0].vertices[2][1], 64);
assert.match(map.faces[0].style.color, /^hsl\(/);
assert.equal(map.spawn.origin[0], 10);
assert.equal(map.spawn.origin[1], 20);
assert.equal(map.spawn.origin[2], 30);
assert.equal(map.spawn.yaw, 90);
assert.equal(map.intermission, null);

const badMagic = Buffer.from(fixture);
badMagic[0] = 0x58;
assert.throws(() => Bsp.parse(badMagic), /BSP/);

const badVersion = Buffer.from(fixture);
badVersion.writeInt32LE(38, 4);
assert.throws(() => Bsp.parse(badVersion), /version/);

console.log("svdemo viewer BSP parser: all tests passed");
