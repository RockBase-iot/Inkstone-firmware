#!/usr/bin/env python3
"""
convert_image.py — NM-EPD-420-4C reference converter (golden baseline)

Converts any image into a 400x300 4-color 2bpp bitstream (30000 bytes) for
direct writing via POST /display or /api/v1/display. Each profile is
byte-identical across this file, web/uploader.html (JS), and the firmware
image_pipeline.

── default (DEFAULT) ──────────────────────────────────────────────────────
  Palette (packing RGB): black(0,0,0) white(255,255,255) yellow(255,210,0) red(200,30,30)
  Distance: d = 299*dr^2 + 587*dg^2 + 114*db^2 (integer)
  Dither: Floyd-Steinberg serpentine, error accumulated as 1/16 integers,
          weights right=7, down-left=3, down=5, down-right=1;
          pixel value = floor((v*16 + err + 8) / 16), clamped to [0,255]
  Packing: byte = c0<<6 | c1<<4 | c2<<2 | c3, row-major, 30000 bytes total

── retro (RETRO) ──────────────────────────────────────────────────────────
    RETRO matches internally as black/white/RED/YELLOW, then emits the panel's
    fixed black/white/YELLOW/RED 2bpp codes (see tools/retro_core.py).
  OKLab perceptual matching + warm calibrated palette + hue-aware candidate
  masking (cool hues dither black/white only) + stylized preprocessing +
  serpentine Atkinson diffusion.  Implemented in tools/retro_core.py.

── none ───────────────────────────────────────────────────────────────────
  Nearest colour, no error diffusion.

Note: byte-level parity is strictly guaranteed only when the input is already
400x300 (no resampling differences); other sizes are center-cropped and
LANCZOS-resized, which differs from browser drawImage at sub-pixel level.

Usage:
  python tools/convert_image.py input.jpg out.bin [--preview out.png]
                                [--profile default|retro|none]
  python tools/convert_image.py --selftest     # CI consistency self-check

Compatibility: --no-dither is equivalent to --profile=none.
"""

import argparse
import hashlib
import os
import sys
from enum import Enum

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

W, H, FRAME_BYTES = 400, 300, 30000

PAL_PACK = [(0, 0, 0), (255, 255, 255), (255, 210, 0), (200, 30, 30)]
PAL_VIEW = [(20, 20, 20), (245, 243, 235), (225, 190, 25), (185, 45, 40)]


class Profile(Enum):
    """Quantization profile. Values are the public `?profile=` tokens."""
    NONE = "none"        # nearest colour, no diffusion
    DEFAULT = "default"  # existing luma-weighted Floyd-Steinberg (byte-stable)
    RETRO = "retro"      # OKLab + hue mask + serpentine Atkinson


def nearest(r: int, g: int, b: int) -> int:
    best, bd = 0, None
    for i, (pr, pg, pb) in enumerate(PAL_PACK):
        d = 299 * (r - pr) ** 2 + 587 * (g - pg) ** 2 + 114 * (b - pb) ** 2
        if bd is None or d < bd:
            best, bd = i, d
    return best


def quantize_pack(px: bytes, dither: bool = True) -> bytes:
    """DEFAULT path: px W*H*3 RGB888 row-major -> 30000B 2bpp.

    DO NOT MODIFY. This is the byte-stable legacy path; its output is pinned by
    reference/regression_baseline/*.default.bin.
    """
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


def _quantize_retro(px: bytes) -> bytes:
    """RETRO profile. Kept in tools/retro_core.py so the port contract lives in
    one place; imported lazily so DEFAULT-only callers do not pay for numpy."""
    import numpy as np
    import retro_core
    arr = np.frombuffer(px, dtype=np.uint8).reshape(H, W, 3).copy()
    return retro_core.retro_quantize(arr)


def quantize_pack_profile(px: bytes, profile: Profile = Profile.DEFAULT) -> bytes:
    """px: W*H*3 RGB888 row-major -> 30000B 2bpp, routed by profile."""
    if profile == Profile.NONE:
        return quantize_pack(px, dither=False)
    if profile == Profile.DEFAULT:
        return quantize_pack(px, dither=True)
    if profile == Profile.RETRO:
        return _quantize_retro(px)
    raise ValueError(f"unknown profile: {profile}")


def convert_image_to_2bpp(path: str, dither: bool = True,
                          profile: Profile = None) -> bytes:
    """Shared with push_image.py: image file -> 30000B raw.

    `dither` is the legacy flag and maps to DEFAULT/NONE; passing an explicit
    `profile` takes precedence (mirrors the API's profile/dither precedence).
    """
    if profile is None:
        profile = Profile.DEFAULT if dither else Profile.NONE
    im = Image.open(path).convert("RGB")
    if im.size != (W, H):
        # center-crop to 4:3, then resize
        cw, ch = im.width, im.width * H // W
        if ch > im.height:
            ch, cw = im.height, im.height * W // H
        left, top = (im.width - cw) // 2, (im.height - ch) // 2
        im = im.crop((left, top, left + cw, top + ch)).resize((W, H), Image.LANCZOS)
    return quantize_pack_profile(im.tobytes(), profile)


def make_preview(raw: bytes, path: str, profile: Profile = Profile.DEFAULT) -> None:
    pal = PAL_VIEW
    if profile == Profile.RETRO:
        import retro_core
        pal = [tuple(int(v) for v in row) for row in retro_core.RETRO_PANEL_PAL_VIEW]
    im = Image.new("RGB", (W, H))
    px = im.load()
    for i in range(W * H):
        c = (raw[i // 4] >> (6 - 2 * (i % 4))) & 3
        px[i % W, i // W] = pal[c]
    im.save(path)


def selftest() -> int:
    """CI golden baseline: packing correctness for DEFAULT, plus the RETRO set."""
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

    # 4) profile routing must not disturb the legacy flag
    assert quantize_pack_profile(white, Profile.NONE) == \
        quantize_pack(white, dither=False)
    assert quantize_pack_profile(im.tobytes(), Profile.DEFAULT) == \
        quantize_pack(im.tobytes(), dither=True)

    retro_selftest()
    print("selftest OK")
    return 0


def retro_selftest() -> None:
    """RETRO cases R1-R8 (see docs/RETRO.md §Testing).

    RETRO matches against black/white/RED/YELLOW internally, then maps output
    to the panel's black/white/YELLOW/RED code order.
    """
    import numpy as np
    import retro_core as R

    def solid(r, g, b):
        img = np.zeros((H, W, 3), dtype=np.uint8)
        img[:, :, 0], img[:, :, 1], img[:, :, 2] = r, g, b
        return np.ascontiguousarray(img)

    def colors(raw):
        return [((raw[i >> 2] >> (6 - 2 * (i & 3))) & 3) for i in range(W * H)]

    # R1 frame length
    white = solid(255, 255, 255)
    raw = R.retro_quantize(white)
    assert len(raw) == FRAME_BYTES, f"R1 fail: {len(raw)}"

    # R3 flat white -> all 0x55 (white is index 1 in both palettes)
    assert raw == bytes([0x55]) * FRAME_BYTES, "R3 fail: white not all 0x55"

    # R4 cool hue -> black/white particles only (no red, no yellow)
    blue = solid(0, 0, 255)
    cs = colors(R.retro_quantize(blue))
    assert all(c in (0, 1) for c in cs), "R4 fail: cool mask leaked colour"

    # R5 green hue -> black/yellow only (panel codes 0/2)
    green = solid(0, 255, 0)
    cs = colors(R.retro_quantize(green))
    assert all(c in (0, 2) for c in cs), "R5 fail: green mask leaked red"

    # R6 warm hue -> unrestricted (must produce some coloured pixels)
    red = solid(255, 0, 0)
    cs = colors(R.retro_quantize(red))
    assert any(c in (2, 3) for c in cs), "R6 fail: warm region should allow colour"

    # R7 determinism
    assert R.retro_quantize(green) == R.retro_quantize(green), "R7 fail"

    # R8 grey ramp monotonicity: more white as luminance rises
    prev, ok = -1, True
    for gval in range(0, 256, 64):
        flat = solid(gval, gval, gval)
        cnt = colors(R.retro_quantize(flat)).count(1)
        if cnt < prev:
            ok = False
        prev = cnt
    assert ok, "R8 fail: white ratio not monotonic"

    # R2 RETRO output uses the fixed hardware order: black / white / yellow / red.
    q = np.zeros((H, W, 3), dtype=np.uint8)
    top, bot = np.zeros((H // 2, W, 3), np.uint8), np.zeros((H // 2, W, 3), np.uint8)
    top[:, : W // 2] = (35, 31, 32)       # TL -> RETRO black
    top[:, W // 2:] = (233, 231, 222)     # TR -> RETRO white
    bot[:, : W // 2] = (193, 80, 62)      # BL -> RETRO red
    bot[:, W // 2:] = (229, 197, 79)      # BR -> RETRO yellow
    q = np.ascontiguousarray(np.vstack([top, bot]))
    raw = R.retro_quantize(q, dither=False)
    row = W // 4
    assert raw[0] == 0x00, f"R2 fail TL: {raw[0]:#04x} (dither-free path lost)"
    assert raw[row // 2] == 0x55, f"R2 fail TR: {raw[row // 2]:#04x}"
    assert raw[(H // 2) * row] == 0xFF, f"R2 fail BL: {raw[(H // 2) * row]:#04x}"
    assert raw[(H // 2) * row + row // 2] == 0xAA, \
        f"R2 fail BR: {raw[(H // 2) * row + row // 2]:#04x}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--preview", help="write a preview PNG (simulated panel colors)")
    ap.add_argument("--profile", choices=[p.value for p in Profile],
                    help="quantization profile (default: default)")
    ap.add_argument("--no-dither", action="store_true",
                    help="legacy alias for --profile=none")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.input or not args.output:
        ap.error("need input and output (or --selftest)")

    prof = Profile(args.profile) if args.profile else None
    if prof is None:
        prof = Profile.NONE if args.no_dither else Profile.DEFAULT
    if args.no_dither and args.profile and args.profile != "none":
        print(f"[warn] --no-dither ignored: --profile={args.profile} takes precedence")

    raw = convert_image_to_2bpp(args.input, profile=prof)
    with open(args.output, "wb") as f:
        f.write(raw)
    print(f"[info] profile: {prof.value}")
    print(f"wrote {args.output}: {len(raw)} bytes  md5={hashlib.md5(raw).hexdigest()}")
    if args.preview:
        make_preview(raw, args.preview, prof)
        print(f"wrote {args.preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
