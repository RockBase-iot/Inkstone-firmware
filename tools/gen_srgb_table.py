"""Generate the shared sRGB->linear byte lookup table for all three ports.

SRGB2LIN[b] = linearize(b / 255)  for b in 0..255

The curve is the standard sRGB EOTF::

    c  = b / 255                        (as float64, exact enough here)
    y  = c / 12.92                      for c <= 0.04045
         ((c + 0.055) / 1.055) ** 2.4   otherwise

Each entry is then narrowed to float32 ONCE, here, and the resulting bit
patterns are what the firmware, the browser and the Python reference all use.
Computing it here (instead of at runtime on each target) is the whole point:
xtensa newlib, V8 and numpy do not agree on `pow`, so any target that evaluates
the curve itself drifts from the other two.

The 0.04045 threshold sits between b=10 (c=0.0392) and b=11 (c=0.0431), so the
table crosses from the linear branch to the power branch exactly once, between
index 10 and 11. A generator that picks the branch from a *pre-narrowing* value
gets index 11 wrong (0.000529 instead of 0.003347) — the table is then
non-monotonic and every mid-grey pixel in the image is crushed to near-black.
The monotonicity assertion below exists to catch exactly that class of bug.

Usage::

    python tools/gen_srgb_table.py            # verify the checked-in tables
    python tools/gen_srgb_table.py --write    # rewrite the three copies
"""

import argparse
import re
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent

JS_PATH = REPO / "web" / "retro_core.js"
PY_REF_PATH = REPO / "reference" / "retro_reference.py"
PY_CORE_PATH = REPO / "tools" / "retro_core.py"
CPP_PATH = REPO / "src" / "display" / "retro_quantize.cpp"
SNIPPET_PATH = REPO / "reference" / "srgb2lin_table.inc"

LINEAR_THRESHOLD = 0.04045


def build_table() -> np.ndarray:
    """Return the 256-entry float32 table, built in float64 then narrowed once."""
    b = np.arange(256, dtype=np.float64)
    c = b / 255.0
    y = np.where(
        c <= LINEAR_THRESHOLD,
        c / 12.92,
        np.power((c + 0.055) / 1.055, 2.4),
    )
    table = y.astype(np.float32)

    # The curve is strictly increasing; a non-monotonic table means the branch
    # selection above is broken and mid-tones are being crushed.
    d = np.diff(table.astype(np.float64))
    if np.any(d <= 0):
        bad = np.where(d <= 0)[0]
        raise SystemExit(
            "BUG: generated table is not strictly increasing at %s" % bad.tolist()
        )
    if table[0] != 0.0 or table[255] != 1.0:
        raise SystemExit(
            "BUG: endpoints wrong (got %r .. %r)" % (table[0], table[255])
        )
    return table


def bits(table: np.ndarray) -> list:
    return list(struct.unpack("<%dI" % table.size, table.tobytes()))


def fmt_js(bits_list: list) -> str:
    lines = []
    for i in range(0, len(bits_list), 6):
        chunk = ", ".join("0x%08X" % v for v in bits_list[i : i + 6])
        lines.append("  " + chunk + ",")
    return "\n".join(lines)


def fmt_py(bits_list: list) -> str:
    f = np.frombuffer(
        struct.pack("<%dI" % len(bits_list), *bits_list), dtype=np.float32
    )
    lines = []
    for i in range(0, f.size, 4):
        chunk = ", ".join("np.float32(%r)" % float(v) for v in f[i : i + 4])
        lines.append("    " + chunk + ",")
    return "\n".join(lines)


def fmt_pybits(bits_list: list) -> str:
    """Python form that round-trips bit-exactly through np.array(..., uint32).view(float32)."""
    lines = []
    for i in range(0, len(bits_list), 6):
        chunk = ", ".join("0x%08X" % v for v in bits_list[i : i + 6])
        lines.append("    " + chunk + ",")
    return "\n".join(lines)


def cpp_float(v: float) -> str:
    """Render one float as a valid C++ float literal that round-trips exactly.

    '%g' is wrong twice over: it prints '0' and '1' for the endpoints (and
    appending 'f' then yields the invalid tokens '0f'/'1f'), and 6 significant
    digits do not round-trip a float32, so the firmware would parse a different
    value than Python and the browser. repr() always carries enough digits; only
    the exponent spelling and the suffix need fixing up.
    """
    s = repr(float(v))
    if "e" in s or "E" in s:
        s = s.replace("E", "e")
    elif "." not in s:
        s += ".0"
    return s + "f"


def fmt_cpp(bits_list: list) -> str:
    f = np.frombuffer(
        struct.pack("<%dI" % len(bits_list), *bits_list), dtype=np.float32
    )
    lines = []
    for i in range(0, f.size, 4):
        chunk = ", ".join(cpp_float(float(v)) for v in f[i : i + 4])
        lines.append("    " + chunk + ",")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="write reference/srgb2lin_table.inc")
    args = ap.parse_args()

    table = build_table()
    print("generated 256-entry sRGB->linear table (float32)")
    print("  [0]=%r [10]=%r [11]=%r [255]=%r"
          % tuple(float(table[i]) for i in (0, 10, 11, 255)))
    print()

    if not args.write:
        print("This tool only generates the values. To verify or propagate them")
        print("into the three ports, run:")
        print("    python tools/sync_srgb_table.py --write")
        print("    python tools/sync_srgb_table.py --check")
        return 0

    b = bits(table)
    SNIPPET_PATH.write_text(
        "// AUTO-GENERATED by tools/gen_srgb_table.py - do not edit by hand.\n"
        "static const float SRGB2LIN[256] = {\n" + fmt_cpp(b) + "\n};\n",
        encoding="utf-8",
    )
    print("wrote %s" % SNIPPET_PATH.relative_to(REPO))
    print("now run: python tools/sync_srgb_table.py --write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
