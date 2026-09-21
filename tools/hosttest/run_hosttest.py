#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_hosttest.py — build and run the firmware quantizer on the development host.

Compiles the REAL src/display/retro_quantize.cpp (via tools/hosttest/parity_main.cpp
and a small Arduino shim) into a host executable, then feeds it every test vector
and compares the resulting 30000-byte frame against the Python reference.

This is the third leg of the byte-parity check. Python and the browser can both be
run locally, but the firmware normally needs a board in the loop - so the C++ port
is the end that silently drifts. See tools/hosttest/parity_main.cpp for what this
does and does not prove.

Usage:
    python tools/hosttest/run_hosttest.py                # build + verify all vectors
    python tools/hosttest/run_hosttest.py --keep         # keep the built binary
    python tools/hosttest/run_hosttest.py --cxx <path>   # use a specific compiler

Requires a HOST C++ compiler (g++/clang++ for the build machine). The ESP32
toolchain cannot be used here - it only emits Xtensa objects.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
HOSTTEST = REPO / "tools" / "hosttest"
SHIM = HOSTTEST / "shim"

VECTORS = [
    "v1_flat_white_400x300",
    "v2_flat_blue_400x300",
    "v3_flat_green_400x300",
    "v4_quad_bands_400x300",
    "v5_gray_ramp_400x300",
    "v6_hue_wheel_400x300",
]


def find_compiler(explicit: str | None) -> str | None:
    if explicit:
        return explicit if Path(explicit).is_file() else None
    for name in ("g++", "clang++"):
        found = shutil.which(name)
        if found:
            return found
    # winlibs / MSYS2 / MinGW installs that are not on PATH, plus the copy
    # verify_parity setup drops under out/tmp (which keeps the toolchain inside
    # the workspace instead of requiring a system install).
    for pat in (
        REPO / "out" / "tmp" / "mingw64" / "bin" / "g++.exe",
        r"C:\msys64\mingw64\bin\g++.exe",
        r"C:\mingw64\bin\g++.exe",
        r"C:\ProgramData\mingw64\mingw64\bin\g++.exe",
    ):
        if Path(pat).is_file():
            return str(pat)
    return None


def build(cxx: str, out_exe: Path) -> bool:
    cmd = [
        cxx,
        "-std=gnu++17",
        "-O2",
        # Link the runtime statically. A dynamically-linked MinGW binary needs
        # libstdc++-6.dll / libgcc_s_seh-1.dll next to it or on PATH, and a
        # missing one fails as 0xC0000135 (STATUS_DLL_NOT_FOUND) with no stderr
        # at all - which looks like "the quantizer crashed" rather than "the
        # loader gave up". Static linking removes that whole failure mode.
        "-static",
        # -ffp-contract=off is what platformio.ini sets for the firmware build.
        # Without it the host compiler is allowed to fuse a*b+c into an FMA and
        # skip the rounding of the product, which is exactly the drift this
        # harness exists to catch - so the host must use the same rule.
        "-ffp-contract=off",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-DBOARD_NM_EPD_420_4C=1",
        f"-I{SHIM}",            # Arduino.h shim (Serial, ps_malloc, ps_calloc)
        f"-I{REPO / 'src'}",    # real board.h + display/retro_quantize.h
        str(HOSTTEST / "parity_main.cpp"),
        str(REPO / "src" / "display" / "retro_quantize.cpp"),
        "-o", str(out_exe),
    ]
    print("[hosttest] compiling the firmware quantizer for the host...")
    r = subprocess.run(cmd, cwd=str(REPO))
    return r.returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default=None, help="host C++ compiler to use")
    ap.add_argument("--keep", action="store_true", help="keep the built binary")
    ap.add_argument("--raw-dir", default="out/raw",
                    help="directory holding <vector>.rgb inputs")
    args = ap.parse_args()

    cxx = find_compiler(args.cxx)
    if not cxx:
        print("no host C++ compiler found (looked for g++, clang++).")
        print("Pass --cxx <path>, or install one. The ESP32 xtensa toolchain")
        print("cannot be used: it only emits Xtensa objects.")
        return 2
    print("[hosttest] compiler: %s" % cxx)

    build_dir = REPO / "out" / "hosttest"
    build_dir.mkdir(parents=True, exist_ok=True)
    exe = build_dir / ("parity_main" + (".exe" if os.name == "nt" else ""))
    if not build(cxx, exe):
        print("[hosttest] BUILD FAILED")
        return 2

    # PYTHONPATH so we can import the reference without installing anything
    sys.path.insert(0, str(REPO / "reference"))
    import numpy as np
    import retro_reference as rr

    raw_dir = REPO / args.raw_dir
    if not raw_dir.is_dir():
        print("raw dir not found: %s" % raw_dir)
        return 2

    print()
    print("=" * 72)
    print("firmware (host build) vs python reference")
    print("=" * 72)

    bad = ran = 0
    for name in VECTORS:
        rgb_path = raw_dir / (name + ".rgb")
        if not rgb_path.is_file():
            print("SKIP  %-28s (no %s)" % (name, rgb_path.name))
            continue
        out_bin = build_dir / (name + ".cpp.bin")
        r = subprocess.run([str(exe), str(rgb_path), str(out_bin)])
        if r.returncode != 0:
            print("FAIL  %-28s harness exit %d" % (name, r.returncode))
            bad += 1
            continue

        cpp_frame = out_bin.read_bytes()
        rgb = np.frombuffer(rgb_path.read_bytes(), np.uint8).reshape(300, 400, 3)
        py_frame = rr.retro_quantize(rgb)

        ran += 1
        if cpp_frame == py_frame:
            print("PASS  %-28s %s" % (name, hashlib.md5(cpp_frame).hexdigest()))
        else:
            bad += 1
            # First differing byte, expressed as a pixel so it is actionable.
            n = min(len(cpp_frame), len(py_frame))
            first = next((i for i in range(n) if cpp_frame[i] != py_frame[i]), n)
            print("FAIL  %-28s len %d/%d, first diff byte %d (pixel %d)"
                  % (name, len(cpp_frame), len(py_frame), first, first * 4))
            print("        cpp md5 %s" % hashlib.md5(cpp_frame).hexdigest())
            print("        py  md5 %s" % hashlib.md5(py_frame).hexdigest())

    if not args.keep and exe.exists():
        exe.unlink()

    print()
    if ran == 0:
        print("no vectors were checked - cannot claim parity")
        return 2
    if bad == 0:
        print("firmware port byte-identical on %d/%d vectors" % (ran, ran))
        print("(note: this is the HOST build; -ffp-contract=off in platformio.ini")
        print(" keeps the Xtensa build on the same arithmetic - verify on-device")
        print(" once per release, see docs/RETRO.md)")
        return 0
    print("%d/%d vectors differ" % (bad, ran))
    return 1


if __name__ == "__main__":
    sys.exit(main())
