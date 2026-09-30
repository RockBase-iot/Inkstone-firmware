# Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
"""
check_e6_parity.py — byte-parity check for the Spectra E6 6-color quantizer

Generates a deterministic synthetic 1200x1600 RGB image, quantizes it twice:
  1. web/e6_core.js        (browser implementation, run under Node)
  2. the NumPy port below  (mirrors src/display/image_pipeline.cpp quantizePack6)
and compares the two packed 4bpp frames byte for byte.

Run:  python tools/check_e6_parity.py
"""

import os
import subprocess
import sys
import tempfile

import numpy as np

W, H, FRAME_BYTES = 1200, 1600, 960000

# Packing palette, wire order (0 black 1 white 2 green 3 blue 4 red 5 yellow)
PAL = np.array([[0, 0, 0], [255, 255, 255], [0, 204, 0],
                [0, 51, 204], [204, 0, 0], [230, 230, 0]], dtype=np.float64)


def reference_fs(rgb):
    """NumPy Floyd-Steinberg with the exact integer arithmetic of
    quantizePack6(): error accumulated as 1/16 integers, pixel value
    floor((v*16 + err + 8) / 16) clamped to [0,255], serpentine scan,
    weights 7/3/5/1, rolling two-row error buffers."""
    err = np.zeros((2, W, 3), dtype=np.int64)
    out = np.zeros(H * W // 2, dtype=np.uint8)
    for y in range(H):
        cur, nxt = y & 1, (y + 1) & 1
        err[nxt].fill(0)
        fwd = y % 2 == 0
        xs = range(W) if fwd else range(W - 1, -1, -1)
        orow = out[y * (W // 2):(y + 1) * (W // 2)]
        for x in xs:
            v = np.floor((rgb[y, x].astype(np.int64) * 16 + err[cur, x] + 8) / 16)
            v = np.clip(v, 0, 255)
            d = (PAL - v) ** 2 @ np.array([299, 587, 114])
            c = int(np.argmin(d))
            if x & 1:
                orow[x >> 1] |= c
            else:
                orow[x >> 1] |= c << 4
            e = v.astype(np.int64) - PAL[c].astype(np.int64)
            xr = x + 1 if fwd else x - 1
            xl = x - 1 if fwd else x + 1
            if 0 <= xr < W:
                err[cur, xr] += e * 7
            if 0 <= xl < W:
                err[nxt, xl] += e * 3
            err[nxt, x] += e * 5
            if 0 <= xr < W:
                err[nxt, xr] += e
        if y % 200 == 0:
            print(f"  reference row {y}/{H}", flush=True)
    return out.tobytes()


JS_RUNNER = r"""
const fs = require("fs");
const path = require("path");
const src = fs.readFileSync(path.join(process.argv[2], "web", "e6_core.js"), "utf8");
// e6_core.js is "use strict", so its consts stay inside eval's scope: append
// the runner body to the same eval string instead of evaluating separately.
const runner = `
const rgb = fs.readFileSync(process.argv[3]);   // W*H*3
const rgba = new Uint8ClampedArray(E6_W * E6_H * 4);
for (let i = 0; i < E6_W * E6_H; i++) {
  rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1];
  rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
}
const frame = e6QuantizeFS(rgba);
if (frame.length !== E6_FRAME_BYTES) { console.error("bad length"); process.exit(1); }
fs.writeFileSync(process.argv[4], Buffer.from(frame));
console.log("js frame written");
`;
eval(src + runner);
"""


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Deterministic synthetic image: smooth gradients + saturation sweeps so
    # every palette entry and error path gets exercised.
    yy, xx = np.mgrid[0:H, 0:W]
    r = (xx * 255 // (W - 1)).astype(np.uint8)
    g = (yy * 255 // (H - 1)).astype(np.uint8)
    b = ((xx // 7 + yy // 5) * 37 % 256).astype(np.uint8)
    rgb = np.stack([r, g, b], axis=-1)  # H, W, 3

    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, "in.rgb")
        outp = os.path.join(td, "js.frame")
        rgb.tofile(inp)
        runner = os.path.join(td, "run.js")
        with open(runner, "w") as f:
            f.write(JS_RUNNER)
        subprocess.run(["node", runner, root, inp, outp], check=True)
        js_frame = open(outp, "rb").read()

    ref_frame = reference_fs(rgb)

    if js_frame == ref_frame:
        print(f"PARITY OK: {FRAME_BYTES} bytes identical")
        return 0
    diffs = sum(1 for a, b in zip(js_frame, ref_frame) if a != b)
    print(f"PARITY FAIL: {diffs}/{FRAME_BYTES} bytes differ")
    return 1


if __name__ == "__main__":
    sys.exit(main())
