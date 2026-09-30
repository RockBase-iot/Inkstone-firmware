"use strict";
/* Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
 * ============================================================================
 * e6_core.js — Spectra E6 6-color quantization profile (browser port)
 *
 * Byte-identical with the firmware's quantizePack6() in
 * src/display/image_pipeline.cpp:
 *
 *   Palette (packing RGB, wire order = GxEPD2 7-color indices):
 *     0 black(0,0,0) 1 white(255,255,255) 2 green(0,204,0)
 *     3 blue(0,51,204) 4 red(204,0,0) 5 yellow(230,230,0)
 *   Distance: d = 299*dr² + 587*dg² + 114*db² (integer)
 *   Dithering: Floyd-Steinberg serpentine, error /16 integer accumulation,
 *              weights: right 7, lower-left 3, below 5, lower-right 1,
 *              pixel value = floor((v*16 + err + 8) / 16) clamped to [0,255]
 *   Error buffers: rolling two rows (same as the firmware, which cannot afford
 *              a full-frame error field at 1200x1600 in 4MB PSRAM)
 *   Packing: 4bpp, byte = c0<<4 | c1, row-major, 1200*1600/2 = 960000 bytes.
 *            The EPD driver translates these indices to panel-native codes
 *            (convertByte in src/drivers/epd1200x1600_e6.cpp).
 * ============================================================================ */

const E6_W = 1200, E6_H = 1600, E6_FRAME_BYTES = 960000;

/* Packing palette (RGB used for distance + error). Wire order! */
const E6_PAL_PACK = [
  [0, 0, 0],        /* 0 black  */
  [255, 255, 255],  /* 1 white  */
  [0, 204, 0],      /* 2 green  */
  [0, 51, 204],     /* 3 blue   */
  [204, 0, 0],      /* 4 red    */
  [230, 230, 0]     /* 5 yellow */
];

/* Preview-only palette (simulated Spectra 6 panel tone; never packed) */
const E6_PAL_VIEW = [
  [24, 24, 24],
  [241, 238, 227],
  [70, 138, 78],
  [58, 92, 165],
  [196, 52, 42],
  [222, 188, 40]
];

function e6Nearest(r, g, b) {
  let best = 0, bd = Infinity;
  for (let i = 0; i < 6; i++) {
    const dr = r - E6_PAL_PACK[i][0], dg = g - E6_PAL_PACK[i][1], db = b - E6_PAL_PACK[i][2];
    const d = 299 * dr * dr + 587 * dg * dg + 114 * db * db;
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

const e6Clamp = v => v < 0 ? 0 : v > 255 ? 255 : v;

/* Floyd-Steinberg, serpentine, rolling two-row error buffers.
 * rgba: Uint8ClampedArray of E6_W*E6_H*4 (canvas ImageData data).
 * Returns the packed 960000-byte frame. */
function e6QuantizeFS(rgba) {
  const W = E6_W, H = E6_H;
  const frame = new Uint8Array(E6_FRAME_BYTES);
  const errR = new Int32Array(2 * W), errG = new Int32Array(2 * W), errB = new Int32Array(2 * W);
  for (let y = 0; y < H; y++) {
    const rowOff = y * W * 4;
    const curR = y & 1 ? W : 0, nxtR = y & 1 ? 0 : W;
    errR.fill(0, nxtR, nxtR + W); errG.fill(0, nxtR, nxtR + W); errB.fill(0, nxtR, nxtR + W);
    const fwd = y % 2 === 0;
    const orow = y * (W / 2);
    for (let xi = 0; xi < W; xi++) {
      const x = fwd ? xi : W - 1 - xi, p = rowOff + x * 4;
      const r = e6Clamp(Math.floor((rgba[p]     * 16 + errR[curR + x] + 8) / 16));
      const g = e6Clamp(Math.floor((rgba[p + 1] * 16 + errG[curR + x] + 8) / 16));
      const b = e6Clamp(Math.floor((rgba[p + 2] * 16 + errB[curR + x] + 8) / 16));
      const c = e6Nearest(r, g, b);
      if (x & 1) frame[orow + (x >> 1)] |= c;
      else       frame[orow + (x >> 1)] |= c << 4;
      const er = r - E6_PAL_PACK[c][0], eg = g - E6_PAL_PACK[c][1], eb = b - E6_PAL_PACK[c][2];
      const xr = fwd ? x + 1 : x - 1, xl = fwd ? x - 1 : x + 1;
      if (xr >= 0 && xr < W) { errR[curR + xr] += er * 7; errG[curR + xr] += eg * 7; errB[curR + xr] += eb * 7; }
      if (xl >= 0 && xl < W) { errR[nxtR + xl] += er * 3; errG[nxtR + xl] += eg * 3; errB[nxtR + xl] += eb * 3; }
      errR[nxtR + x] += er * 5; errG[nxtR + x] += eg * 5; errB[nxtR + x] += eb * 5;
      if (xr >= 0 && xr < W) { errR[nxtR + xr] += er; errG[nxtR + xr] += eg; errB[nxtR + xr] += eb; }
    }
  }
  return frame;
}

/* Packed 4bpp frame -> canvas ImageData through the preview palette. */
function e6FrameToImageData(frame) {
  const img = new ImageData(E6_W, E6_H);
  const d = img.data;
  for (let i = 0; i < E6_W * E6_H; i++) {
    const c = (frame[i >> 1] >> ((i & 1) ? 0 : 4)) & 0x0F;
    const p = i * 4, v = E6_PAL_VIEW[c] || E6_PAL_VIEW[1];
    d[p] = v[0]; d[p + 1] = v[1]; d[p + 2] = v[2]; d[p + 3] = 255;
  }
  return img;
}
