#!/usr/bin/env node
/* Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
 * ============================================================================
 * check_js_parity.js — headless parity check for the browser RETRO port
 *
 * Feeds out/raw/<vector>.rgb (400x300 RGB888) through web/retro_core.js and
 * compares the resulting frame MD5 against reference/test_vectors.json.
 *
 * Usage:
 *   node tools/check_js_parity.js [--raw-dir out/raw]
 *
 * The browser needs ImageData for preview, so this script shims the two DOM
 * globals retro_core.js touches before loading it via vm.
 * ============================================================================ */

"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");
const crypto = require("crypto");

const ROOT = path.resolve(__dirname, "..");

/* Minimal ImageData shim so retro_core.js can be loaded outside a browser. */
class ImageDataShim {
  constructor(w, h) {
    this.width = w;
    this.height = h;
    this.data = new Uint8ClampedArray(w * h * 4);
  }
}

const corePath = path.join(ROOT, "web", "retro_core.js");
const sandbox = { ImageData: ImageDataShim, Math, console };
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(corePath, "utf8"), sandbox, { filename: corePath });

const meta = JSON.parse(fs.readFileSync(path.join(ROOT, "reference", "test_vectors.json"), "utf8"));

const argIdx = process.argv.indexOf("--raw-dir");
const rawDir = path.join(ROOT, argIdx >= 0 ? process.argv[argIdx + 1] : "out/raw");

if (!fs.existsSync(rawDir)) {
  console.error(`raw dir not found: ${rawDir}`);
  console.error("Generate it first (see docs/RETRO.md §Verifying the browser port).");
  process.exit(2);
}

let bad = 0, ran = 0;
for (const v of meta.vectors) {
  const rgbPath = path.join(rawDir, `${v.name}.rgb`);
  if (!fs.existsSync(rgbPath)) {
    console.log(`SKIP  ${v.name} (no ${path.relative(ROOT, rgbPath)})`);
    continue;
  }
  const rgb = new Uint8ClampedArray(fs.readFileSync(rgbPath));
  const t0 = Date.now();
  const frame = sandbox.retroQuantize(rgb, true);
  const ms = Date.now() - t0;

  const lenOk = frame.length === meta.frame_bytes;
  const md5 = crypto.createHash("md5").update(Buffer.from(frame)).digest("hex");
  const ok = lenOk && md5 === v.retro_md5;
  ran++;
  if (!ok) bad++;
  console.log(`${ok ? "PASS" : "FAIL"}  ${v.name}  ${ms}ms`);
  if (!lenOk) console.log(`      length ${frame.length} != ${meta.frame_bytes}`);
  if (md5 !== v.retro_md5) {
    console.log(`      expect ${v.retro_md5}`);
    console.log(`      actual ${md5}`);
  }
}

if (ran === 0) {
  console.error("no vectors were checked — cannot claim parity");
  process.exit(2);
}
console.log(bad === 0
  ? `\n✅ JS port byte-identical on ${ran}/${ran} vectors`
  : `\n❌ ${bad}/${ran} vectors differ — see docs/RETRO.md §Troubleshooting`);
process.exit(bad === 0 ? 0 : 1);
