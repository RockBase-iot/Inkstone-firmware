const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.join(__dirname, "..");
const page = fs.readFileSync(path.join(root, "web/uploader.html"), "utf8");
const inline = page.match(/<script>\s*"use strict";([\s\S]*?)<\/script>\s*<\/body>/);
assert(inline, "page inline script missing");
new vm.Script(inline[1]);
assert(page.includes('<script src="color_adjust.js"></script>'));
assert(page.includes("retroQuantize(adjusted.rgb, true, adjustments.contrast === 1 && !toneCurve)"));

const sandbox = { Uint8ClampedArray, Uint8Array, Int32Array, Float32Array, Uint32Array,
                  Math, ImageData: class {} };
vm.createContext(sandbox);
for (const script of ["retro_core.js", "color_adjust.js"]) {
  vm.runInContext(fs.readFileSync(path.join(root, "web", script), "utf8"), sandbox);
}
const declarations = ["W", "PAL_PACK", "clamp"].map(name => {
  const line = inline[1].match(new RegExp(`^const ${name}\\s*=.*$`, "m"));
  assert(line, `missing ${name} declaration`);
  return line[0];
});
for (const name of ["nearest", "spread", "quantizeDefault"]) {
  const block = inline[1].match(new RegExp(`^function ${name}\\([\\s\\S]*?^\\}`, "m"));
  assert(block, `missing ${name} function`);
  declarations.push(block[0]);
}
vm.runInContext(declarations.join("\n"), sandbox);

const width = 400, height = 300;
const source = new Uint8ClampedArray(width * height * 4);
for (let row = 0; row < height; row++) {
  for (let column = 0; column < width; column++) {
    const offset = (row * width + column) * 4;
    source[offset] = Math.round(column * 255 / (width - 1));
    source[offset + 1] = Math.round(row * 255 / (height - 1));
    source[offset + 2] = (row + column) % 256;
    source[offset + 3] = 255;
  }
}
const original = Buffer.from(source);
const neutral = sandbox.adjustColorPixels(source, 0, 1, 1);
assert.strictEqual(neutral.rgba, source);
assert.equal(neutral.rgb.length, width * height * 3);
assert.deepEqual(Buffer.from(neutral.rgba), original);
assert.equal(sandbox.makeToneCurve([0, 64, 128, 192, 255]), null);

const curve = sandbox.makeToneCurve([0, 90, 128, 180, 255]);
assert.equal(curve.length, 256);
assert.equal(curve[0], 0);
assert.equal(curve[255], 255);
for (let index = 1; index < curve.length; index++) {
  assert(curve[index] >= curve[index - 1], `tone curve decreased at ${index}`);
}
assert(curve[64] > 64);
for (const anchors of [[0, 0, 0, 255, 255], [0, 128, 128, 128, 255], [0, 10, 20, 30, 255]]) {
  const lookup = sandbox.makeToneCurve(anchors);
  for (let index = 1; index < lookup.length; index++) {
    assert(lookup[index] >= lookup[index - 1], `tone curve decreased at ${index} with ${anchors}`);
  }
}
const curved = sandbox.adjustColorPixels(source, 0, 1, 1, curve);
assert.notDeepEqual(Buffer.from(curved.rgba), original);
assert.deepEqual(Buffer.from(source), original);

for (const [exposure, saturation, contrast] of [[0.8, 1, 1], [0, 0.5, 1], [0, 1, 1.2]]) {
  const result = sandbox.adjustColorPixels(source, exposure, saturation, contrast);
  assert.notDeepEqual(Buffer.from(result.rgba), original);
  assert.deepEqual(Buffer.from(source), original);
  for (let offset = 0; offset < source.length; offset += 4) {
    assert.equal(result.rgba[offset + 3], 255);
  }
}

const adjusted = sandbox.adjustColorPixels(source, 0.8, 0.5, 1.2);
const defaultBefore = Buffer.from(sandbox.quantizeDefault(neutral.rgba));
const defaultAfter = Buffer.from(sandbox.quantizeDefault(adjusted.rgba));
const retroBefore = Buffer.from(sandbox.retroQuantize(neutral.rgb, true));
const retroLegacy = Buffer.from(sandbox.retroQuantize(neutral.rgb, true, true));
const retroAfter = Buffer.from(sandbox.retroQuantize(adjusted.rgb, true, false));
const defaultCurved = Buffer.from(sandbox.quantizeDefault(curved.rgba));
const retroCurved = Buffer.from(sandbox.retroQuantize(curved.rgb, true, false));
assert.equal(defaultBefore.length, 30000);
assert.equal(retroBefore.length, 30000);
assert.deepEqual(retroBefore, retroLegacy);
assert.notDeepEqual(defaultBefore, defaultAfter);
assert.notDeepEqual(retroBefore, retroAfter);
assert.notDeepEqual(defaultBefore, defaultCurved);
assert.notDeepEqual(retroBefore, retroCurved);
console.log("PASS: neutral RGB, monotone tone curve, independent adjustments, and both quantizers (30000 bytes)");