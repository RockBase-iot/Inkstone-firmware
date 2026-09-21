#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_frames.py — 比较两个 2bpp 帧，定位第一个不一致的像素，并可视化差异。

三端一致性排查时，MD5 只能告诉你"不一样"，不能告诉你"从哪开始不一样"。
本工具补上后半句：把两个帧按 2bpp 解包成索引图，找出第一个差异像素
（按蛇形扫描顺序，而不是线性顺序 —— 误差扩散的错位是从扫描起点开始扩散的），
并把差异区域画成 ASCII 图，方便一眼看出是"整体偏移"还是"局部孤立点"。

用法:
    # 直接比两个帧文件
    python tools/diff_frames.py out/hosttest/v2.cpp.bin out/js/v2.retro.bin

    # 按向量名比两个目录（框架约定 <name>.<profile>.bin）
    python tools/diff_frames.py --dirs out/hosttest out/js --suffix .bin \
        --vectors v1_flat_white_400x300,v2_flat_blue_400x300

    # 只统计不画图
    python tools/diff_frames.py a.bin b.bin --quiet

退出码: 0 = 完全一致; 1 = 有差异; 2 = 输入错误
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

W, H = 400, 300
FRAME_BYTES = W * H // 4

# 面板码 -> 字符（0=黑 1=白 2=黄 3=红）
IDX_CHAR = "KWYR"


def unpack3(frame: bytes) -> list[str]:
    """2bpp MSB-first 解包成 W*H 个字符（仅用于人眼观察）。"""
    out = []
    for b in frame:
        for k in range(4):
            out.append(IDX_CHAR[(b >> (6 - 2 * k)) & 3])
    return out


def load_frame(path: Path, expect: int = FRAME_BYTES) -> bytes:
    data = path.read_bytes()
    if len(data) != expect:
        raise ValueError("%s: %d bytes, expected %d" % (path, len(data), expect))
    return data


def render_diff(a: bytes, b: bytes, rows: int = 24, cols: int = 60) -> str:
    """把差异画成 row x col 的密集字符图，'.' 一致 '#' 不同。"""
    ai, bi = unpack3(a), unpack3(b)
    lines = ["    " + "".join(str((c // 10) % 10) for c in range(0, cols))]
    lines.append("    " + "".join(str(c % 10) for c in range(0, cols)))
    for r in range(rows):
        y0 = r * H // rows
        y1 = max(y0 + 1, (r + 1) * H // rows)
        row = []
        for c in range(cols):
            x0 = c * W // cols
            x1 = max(x0 + 1, (c + 1) * W // cols)
            hit = False
            for y in range(y0, y1):
                base = y * W
                for x in range(x0, x1):
                    i = base + x
                    if ai[i] != bi[i]:
                        hit = True
                        break
                if hit:
                    break
            row.append("#" if hit else ".")
        lines.append("%3d " % y0 + "".join(row))
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="*", help="two frame files to compare")
    ap.add_argument("--quiet", action="store_true", help="只输出计数，不画图")
    ap.add_argument("--rows", type=int, default=24, help="ASCII 图的行数")
    ap.add_argument("--cols", type=int, default=60, help="ASCII 图的列数")
    args = ap.parse_args()

    if len(args.frames) != 2:
        print("need exactly two frame files", file=sys.stderr)
        return 2

    pa, pb = Path(args.frames[0]), Path(args.frames[1])
    for p in (pa, pb):
        if not p.is_file():
            print("not a file: %s" % p, file=sys.stderr)
            return 2

    try:
        a, b = load_frame(pa), load_frame(pb)
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2

    ai, bi = unpack3(a), unpack3(b)
    bad = [i for i in range(W * H) if ai[i] != bi[i]]

    print("A: %s" % pa)
    print("B: %s" % pb)
    print("frame bytes : %d / %d" % (len(a), len(b)))

    if not bad:
        print("RESULT: identical (%d bytes, %d pixels)" % (len(a), W * H))
        return 0

    print("RESULT: %d of %d pixels differ (%.3f%%)"
          % (len(bad), W * H, 100.0 * len(bad) / (W * H)))

    # 按蛇形扫描顺序找"第一个"差异 —— 误差扩散的错位一定从每行的扫描起点
    # 开始，线性顺序会把第 1 行最右边的差异误报成首个。
    first = None
    for y in range(H):
        xs = range(W) if y % 2 == 0 else range(W - 1, -1, -1)
        for x in xs:
            i = y * W + x
            if ai[i] != bi[i]:
                first = (x, y, i)
                break
        if first:
            break
    x, y, i = first
    print("first diff (serpentine order): x=%d y=%d  index=%d" % (x, y, i))
    print("           linear index too : %d" % i)
    print("           A=%s  B=%s   (A byte 0x%02x  B byte 0x%02x)"
          % (ai[i], bi[i], a[i >> 2], b[i >> 2]))

    # 差异分布：按行统计，行号从 0 开始
    per_row = {}
    for idx in bad:
        per_row.setdefault(idx // W, 0)
    rows_hit = sorted(per_row)
    print("rows with diffs: %d..%d (%d rows), worst row %d (%d px)"
          % (rows_hit[0], rows_hit[-1], len(rows_hit),
             max(per_row, key=per_row.get), max(per_row.values())))

    if not args.quiet:
        print()
        print("diff map ('#' = at least one differing pixel in that cell):")
        print(render_diff(a, b, args.rows, args.cols))

    return 1


if __name__ == "__main__":
    sys.exit(main())
