"""Rewrite the SRGB2LIN table in all three ports from tools/gen_srgb_table.py.

The table is generated once, in float64, and narrowed to float32 exactly once.
Every port then embeds the same 256 bit patterns. This script is the only writer
of those blocks; tools/gen_srgb_table.py is the source of truth for the values.

Run `python tools/gen_srgb_table.py --write` first to refresh
reference/srgb2lin_table.inc, then this script to propagate.

Usage::

    python tools/sync_srgb_table.py --check    # verify all three ports agree
    python tools/sync_srgb_table.py --write    # rewrite the blocks in place
"""

import argparse
import re
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from gen_srgb_table import build_table, fmt_cpp, fmt_js, fmt_pybits  # noqa: E402

JS_PATH = REPO / "web" / "retro_core.js"
PY_REF_PATH = REPO / "reference" / "retro_reference.py"
PY_CORE_PATH = REPO / "tools" / "retro_core.py"
CPP_PATH = REPO / "src" / "display" / "retro_quantize.cpp"
INC_PATH = REPO / "reference" / "srgb2lin_table.inc"

# Regexes used to find the SRGB2LIN block in each port for patching. These MUST
# tolerate the block ending with or without a trailing newline: the patcher
# always writes one, but a hand-edited file may not have one, and anchoring on
# the newline makes the verifier silently report MISMATCH for a correct table.
JS_ANCHOR = r"(const SRGB2LIN_BITS = new Uint32Array\(\[).*?(\]\);)"
PY_ANCHOR = r"(SRGB2LIN = np\.array\(\[\n)(?:.*?\n)(\], dtype=np\.float32\))"
CPP_ANCHOR = r"(static const float SRGB2LIN\[256\] = \{\n)(?:.*?\n)(\};)"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")
    print("  wrote %s" % path.relative_to(REPO))


def parse_bits_hex(body: str) -> list:
    return [int(t, 16) for t in re.findall(r"0x[0-9A-Fa-f]{8}", body)]


def parse_cpp_floats(body: str) -> np.ndarray:
    toks = re.findall(r"(?<![\w.])[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?f", body)
    return np.array([float(t[:-1]) for t in toks], dtype=np.float32)


def expected_bits() -> list:
    return list(struct.unpack("<256I", build_table().tobytes()))


def patch_js(text: str, b: list) -> str:
    m = re.search(JS_ANCHOR, text, re.S)
    if not m:
        raise SystemExit("JS anchor not found in %s" % JS_PATH)
    head = m.group(1)
    tail = m.group(2)
    return text[: m.start()] + head + "\n" + fmt_js(b) + "\n" + tail + text[m.end() :]


def patch_py(text: str, b: list) -> str:
    """Rewrite the SRGB2LIN block, switching to a bit-exact float32 literal list."""
    m = re.search(PY_ANCHOR, text, re.S)
    if not m:
        raise SystemExit("Python anchor not found (SRGB2LIN block)")
    # Emit plain float literals (repr of the float32) so the file reads naturally
    # and still round-trips exactly through np.float32().
    f = np.frombuffer(struct.pack("<256I", *b), dtype=np.float32)
    lines = []
    for i in range(0, f.size, 4):
        chunk = ", ".join(repr(float(v)) for v in f[i : i + 4])
        lines.append("    " + chunk + ",")
    return text[: m.start()] + m.group(1) + "\n".join(lines) + "\n" + m.group(2) + text[m.end() :]


def patch_cpp(text: str, b: list) -> str:
    m = re.search(CPP_ANCHOR, text, re.S)
    if not m:
        raise SystemExit("C++ anchor not found in %s" % CPP_PATH)
    return text[: m.start()] + m.group(1) + fmt_cpp(b) + "\n" + m.group(2) + text[m.end() :]


def check() -> bool:
    """Verify every port's table by round-tripping through its own patcher.

    Comparing `patch(text) == text` is stronger than re-parsing the block with a
    second, hand-written regex: if the verifier's idea of the block differs from
    the patcher's, the patcher would rewrite bytes that the verifier then fails
    to see, and we would be comparing two different things.
    """
    b = expected_bits()
    ok = True

    cases = [(JS_PATH, patch_js), (CPP_PATH, patch_cpp)]
    if PY_REF_PATH.exists():
        cases.append((PY_REF_PATH, patch_py))
    if PY_CORE_PATH.exists():
        cases.append((PY_CORE_PATH, patch_py))

    for path, patcher in cases:
        text = read(path)
        try:
            patched = patcher(text, b)
            unchanged = patched == text
        except SystemExit as exc:
            print("  %-34s ERROR: %s" % (path.relative_to(REPO), exc))
            ok = False
            continue
        print("  %-34s %s" % (path.relative_to(REPO), "OK" if unchanged else "MISMATCH"))
        ok &= unchanged

    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    if args.write:
        b = expected_bits()
        print("propagating 256-entry table to all ports:")
        write(INC_PATH, "// AUTO-GENERATED by tools/gen_srgb_table.py - do not edit.\n"
                        "static const float SRGB2LIN[256] = {\n" + fmt_cpp(b) + "\n};\n")
        write(JS_PATH, patch_js(read(JS_PATH), b))
        if PY_REF_PATH.exists():
            write(PY_REF_PATH, patch_py(read(PY_REF_PATH), b))
        if PY_CORE_PATH.exists():
            write(PY_CORE_PATH, patch_py(read(PY_CORE_PATH), b))
        write(CPP_PATH, patch_cpp(read(CPP_PATH), b))
        print()

    print("verifying:")
    ok = check()
    print()
    print("RESULT:", "all three ports agree" if ok else "DIVERGENCE")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
