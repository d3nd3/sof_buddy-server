// Cross-checks the browser parser against a fixture written by the real C++
// writer, then checks the viewer's synthetic sample path.
import assert from "node:assert/strict";
import fs from "node:fs";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const Svdemo = require("../../svdemo_viewer/svdemo.js");

const fixturePath = process.argv[2];
assert.ok(fixturePath, "fixture path argument is required");
const data = fs.readFileSync(fixturePath);
const demo = Svdemo.parse(data);

assert.equal(demo.header.mapName, "dm/arena");
assert.equal(demo.header.levelName, "The Arena");
assert.equal(demo.header.serverName, "fixture");
assert.equal(demo.header.gamedir, "base");
assert.equal(demo.header.tickMs, 100);
assert.equal(demo.frames.length, 6);
assert.equal(demo.groups.length, 3);
assert.deepEqual(demo.slots, [1, 2]);
assert.equal(demo.clientFrames, 6);
assert.equal(demo.durationMs, 200);

const grim = demo.rosterBySlot.get(1);
assert.equal(grim.name, "Grim");
assert.match(grim.userinfo, /Grim/);
assert.equal(demo.rosterBySlot.get(2).name, "Vex");

assert.equal(demo.configstringCount, 3);
assert.equal(demo.configstrings.get(Svdemo.CS.SKY), "sky1");
assert.equal(demo.configstrings.get(Svdemo.CS.MODELS), "models/player/grim");
assert.equal(demo.configstrings.get(Svdemo.CS.PLAYERSKINS), "Grim/male/base");

assert.equal(demo.events.length, 1);
assert.equal(demo.events[0].text, "fixture mark");
assert.equal(demo.events[0].timeMs, 100);

assert.equal(demo.end.reason, 0);
assert.equal(demo.end.firstFrame, 0);
assert.equal(demo.end.lastFrame, 2);
assert.equal(demo.end.clientFrames, 6);
assert.equal(demo.end.durationMs, 200);

const first = demo.groups[0].slots.get(1);
assert.equal(first.ps.pmove.origin[0], 100);
assert.equal(first.ps.pmove.origin[1], -200);
assert.equal(first.ps.eye[2], 22);
assert.equal(first.ps.stats[Svdemo.STAT.HEALTH], 100);
assert.equal(first.ps.stats[Svdemo.STAT.ARMOR], 50);
assert.equal(first.ps.stats[Svdemo.STAT.FRAGS], 7);
assert.equal(first.ps.viewangles[1], 90);
assert.equal(first.ps.fov, 90);
assert.equal(first.es.origin[0], 100);
assert.equal(first.es.modelindex, 1);

const last = demo.groups[2].slots.get(1);
assert.equal(last.ps.pmove.origin[0], 116);
assert.equal(demo.slotTrails[1].length, 3);
assert.equal(demo.slotTrails[2][2].x, -116);

assert.ok(demo.bounds.minX <= 100);
assert.ok(demo.bounds.maxX >= 116);
assert.ok(demo.bounds.minY <= -208);
assert.ok(demo.bounds.maxY >= -200);

// The parser must reject a truncated or wrong-magic file.
let rejected = false;
try {
  Svdemo.parse(data.subarray(0, data.length - 4));
} catch {
  rejected = true;
}
assert.equal(rejected, true, "truncated file rejected");

const badMagic = Buffer.from(data);
badMagic[0] = 0x58;
try {
  Svdemo.parse(badMagic);
  assert.fail("bad magic accepted");
} catch (error) {
  assert.match(String(error.message), /\.sbd/);
}

// Synthetic sample path used by the viewer button.
const sample = Svdemo.parse(Svdemo.buildSample());
assert.equal(sample.header.mapName, "dm/arena");
assert.deepEqual(sample.slots, [1, 2, 3]);
assert.ok(sample.groups.length > 100);
assert.ok(sample.events.length === 1);
assert.equal(sample.configstrings.get(Svdemo.CS.SKY), "sky1");
assert.equal(sample.groups[0].slots.get(1).ps.stats[Svdemo.STAT.HEALTH] > 0, true);

console.log("svdemo viewer parser: all tests passed");
