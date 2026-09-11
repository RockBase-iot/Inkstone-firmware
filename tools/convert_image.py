#!/usr/bin/env python3
"""
convert_image.py — NM-EPD-420-4C reference converter (golden baseline)

Converts any image into a 400x300 4-color 2bpp bitstream (30000 bytes) for
direct writing via POST /display or /api/v1/display. The algorithm is
byte-identical across this file, web/uploader.html (JS), and the firmware
image_pipeline:

  Palette (packing RGB): black(0,0,0) white(255,255,255) yellow(255,210,0) red(200,30,30)
  Distance: d = 299*dr^2 + 587*dg^2 + 114*db^2 (integer)
  Dither: Floyd-Steinberg serpentine, error accumulated as 1/16 integers,
          weights right=7, down-left=3, down=5, down-right=1;
          pixel value = floor((v*16 + err + 8) / 16), clamped to [0,255]
  Packing: byte = c0<<6 | c1<<4 | c2<<2 | c3, row-major, 30000 bytes total

Note: byte-level parity is strictly guaranteed only when the input is already
400x300 (no resampling differences); other sizes are center-cropped and
LANCZOS-resized, which differs from browser drawImage at sub-pixel level.

Usage:
  python tools/convert_image.py input.jpg out.bin [--preview out.png] [--no-dither]
  python tools/convert_image.py --selftest     # CI consistency self-check
"""

import argparse
import sys

from PIL import Image

W, H, FRAME_BYTES = 400, 300, 30000

PAL_PACK = [(0, 0, 0), (255, 255, 255), (255, 210, 0), (200, 30, 30)]
PAL_VIEW = [(20, 20, 20), (245, 243, 235), (225, 190, 25), (185, 45, 40)]


def nearest(r: int, g: int, b: int) -> int:
    best, bd = 0, None
    for i, (pr, pg, pb) in enumerate(PAL_PACK):
        d = 299 * (r - pr) ** 2 + 587 * (g - pg) ** 2 + 114 * (b - pb) ** 2
        if bd is None or d < bd:
            best, bd = i, d
    return best


def quantize_pack(px: bytes, dither: bool = True) -> bytes:
    """px: W*H*3 RGB888 row-major -> 30000B 2bpp"""
    out = bytearray(FRAME_BYTES)
    if not dither:
        for i in range(W * H):
            c = nearest(px[i * 3], px[i * 3 + 1], px[i * 3 + 2])
            out[i // 4] |= c << (6 - 2 * (i % 4))
        return bytes(out)

    err_r = [0] * (W * H)
    err_g = [0] * (W * H)
    err_b = [0] * (W * H)
    for y in range(H):
        fwd = y % 2 == 0
        for xi in range(W):
            x = xi if fwd else W - 1 - xi
            i = y * W + x
            r = min(255, max(0, (px[i * 3] * 16 + err_r[i] + 8) // 16))
            g = min(255, max(0, (px[i * 3 + 1] * 16 + err_g[i] + 8) // 16))
            b = min(255, max(0, (px[i * 3 + 2] * 16 + err_b[i] + 8) // 16))
            c = nearest(r, g, b)
            out[i // 4] |= c << (6 - 2 * (i % 4))
            er, eg, eb = r - PAL_PACK[c][0], g - PAL_PACK[c][1], b - PAL_PACK[c][2]
            xr = x + 1 if fwd else x - 1
            xl = x - 1 if fwd else x + 1

            def spread(ni, w):
                err_r[ni] += er * w
                err_g[ni] += eg * w
                err_b[ni] += eb * w

            if 0 <= xr < W:
                spread(y * W + xr, 7)
            if y + 1 < H:
                if 0 <= xl < W:
                    spread((y + 1) * W + xl, 3)
                spread((y + 1) * W + x, 5)
                if 0 <= xr < W:
                    spread((y + 1) * W + xr, 1)
    return bytes(out)


def convert_image_to_2bpp(path: str, dither: bool = True) -> bytes:
    """Shared with push_image.py: image file -> 30000B raw"""
    im = Image.open(path).convert("RGB")
    if im.size != (W, H):
        # center-crop to 4:3, then resize
        cw, ch = im.width, im.width * H // W
        if ch > im.height:
            ch, cw = im.height, im.height * W // H
        left, top = (im.width - cw) // 2, (im.height - ch) // 2
        im = im.crop((left, top, left + cw, top + ch)).resize((W, H), Image.LANCZOS)
    return quantize_pack(im.tobytes(), dither)


def make_preview(raw: bytes, path: str) -> None:
    im = Image.new("RGB", (W, H))
    px = im.load()
    for i in range(W * H):
        c = (raw[i // 4] >> (6 - 2 * (i % 4))) & 3
        px[i % W, i // W] = PAL_VIEW[c]
    im.save(path)


def selftest() -> int:
    """CI golden baseline: build a 400x300 test image and verify packing."""
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = (x * 255 // (W - 1), y * 255 // (H - 1), 128)

    # 1) Non-dithered four-quadrant image: byte-predictable output
    q = Image.new("RGB", (W, H))
    qp = q.load()
    quad = [(0, 0, 0), (255, 255, 255), (255, 210, 0), (200, 30, 30)]
    for y in range(H):
        for x in range(W):
            qp[x, y] = quad[(y * 2 // H) * 2 + (x * 2 // W)]
    raw = quantize_pack(q.tobytes(), dither=False)
    assert len(raw) == FRAME_BYTES, "frame size"
    row = W // 4
    # Quadrant layout: TL black(0x00) TR white(0x55) BL yellow(0xAA) BR red(0xFF)
    assert raw[0] == 0x00                          # top-left = black
    assert raw[row // 2] == 0x55                   # top-right = white
    assert raw[(H // 2) * row] == 0xAA             # bottom-left = yellow
    assert raw[(H // 2) * row + row // 2] == 0xFF  # bottom-right = red
    assert all(b in (0x00, 0x55, 0xAA, 0xFF) for b in raw), "pure quadrant bytes"

    # 2) Dither path determinism: same input twice -> same output
    a = quantize_pack(im.tobytes(), dither=True)
    b = quantize_pack(im.tobytes(), dither=True)
    assert a == b and len(a) == FRAME_BYTES

    # 3) All-white image -> all 0x55
    white = bytes([255, 255, 255] * W * H)
    assert quantize_pack(white, dither=False) == bytes([0x55]) * FRAME_BYTES
    print("selftest OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--preview", help="write a preview PNG (simulated panel colors)")
    ap.add_argument("--no-dither", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.input or not args.output:
        ap.error("need input and output (or --selftest)")

    raw = convert_image_to_2bpp(args.input, dither=not args.no_dither)
    with open(args.output, "wb") as f:
        f.write(raw)
    print(f"wrote {args.output}: {len(raw)} bytes")
    if args.preview:
        make_preview(raw, args.preview)
        print(f"wrote {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
