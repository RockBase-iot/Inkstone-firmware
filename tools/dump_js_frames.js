#!/usr/bin/env node
/* Dump RETRO frames for every vector in out/raw into out/<dir>/, so they can
 * be diffed against another port. See docs/RETRO.md §Troubleshooting. */
"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");
const ROOT = path.resolve(__dirname, "..");
class ID { constructor(w, h) { this.width = w; this.height = h; this.data = new Uint8ClampedArray(w * h * 4); } }
const sb = { ImageData: ID, Math, console };
vm.createContext(sb);
const p = path.join(ROOT, "web", "retro_core.js");
vm.runInContext(fs.readFileSync(p, "utf8"), sb, { filename: p });

const outDir = path.join(ROOT, "out", process.argv[2] || "js");
const rawDir = path.join(ROOT, "out", "raw");
fs.mkdirSync(outDir, { recursive: true });
for (const f of fs.readdirSync(rawDir)) {
  if (!f.endsWith(".rgb")) continue;
  const name = f.replace(".rgb", "");
  const rgb = new Uint8ClampedArray(fs.readFileSync(path.join(rawDir, f)));
  fs.writeFileSync(path.join(outDir, `${name}.retro.bin`), Buffer.from(sb.retroQuantize(rgb, true)));
}
console.log(`dumped ${fs.readdirSync(outDir).length} frames to ${path.relative(ROOT, outDir)}`);
