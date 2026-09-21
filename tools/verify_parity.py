#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_parity.py — 三端一致性验证工具
=====================================
拿 reference/test_vectors.json 里记录的期望 MD5，去校验任意一端的实际输出。

支持三种接入方式：
  A. Python 参考实现（本仓库自带）
  B. 固件端导出的 .bin 帧文件（在设备/模拟器上跑出来后拷回来）
  C. 浏览器端导出的 .bin 帧文件（页面提供"导出帧"按钮，或从 DevTools 取出）

用法：
  # A. 校验 Python 参考实现自身（快速冒烟，约 8 分钟）
  python3 tools/verify_parity.py --impl python

  # B/C. 校验导出的帧文件（推荐，秒级）
  python3 tools/verify_parity.py --frames ./out/cpp
  python3 tools/verify_parity.py --frames ./out/js --profile retro

  # 单张图快速比对
  python3 tools/verify_parity.py --input test_vectors/v1_flat_white_400x300.png \
                                 --frame out/py_white.bin --profile retro

目录约定（--frames 模式）：
  <dir>/<vector_name>.<profile>.bin
  例：out/cpp/v2_flat_blue_400x300.retro.bin

退出码：0 = 全部一致；1 = 有不一致；2 = 输入缺失
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF_DIR = os.path.join(os.path.dirname(HERE), "reference")
VECTORS_JSON = os.path.join(REF_DIR, "test_vectors.json")

GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
RESET = "\033[0m"


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_vectors():
    if not os.path.exists(VECTORS_JSON):
        print(f"{RED}找不到 {VECTORS_JSON}{RESET}")
        print("请先运行 reference/gen_test_vectors.py 生成测试向量。")
        sys.exit(2)
    with open(VECTORS_JSON, encoding="utf-8") as f:
        return json.load(f)


def check(actual, expect, label):
    ok = actual == expect
    mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
    print(f"  {mark}  {label}")
    if not ok:
        print(f"        expect {expect}")
        print(f"        actual {actual}")
    return ok


def run_python_impl(vectors, profiles):
    """在子进程里跑参考实现，避免污染当前环境；从 stdout 读 MD5。"""
    script = os.path.join(REF_DIR, "retro_reference.py")
    helper = os.path.join(HERE, "_parity_python_worker.py")
    cmd = [sys.executable, helper, "--impl", script,
           "--vectors", VECTORS_JSON, "--profiles", ",".join(profiles)]
    print(f"{YELLOW}→ 运行 Python 参考实现（约 8 分钟，逐像素 Python 循环较慢）…{RESET}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
    out = {}
    for line in proc.stdout.splitlines():
        if line.startswith("RESULT "):
            _, name, prof, digest = line.split()
            out[(name, prof)] = digest
    return out


def main():
    ap = argparse.ArgumentParser(description="RETRO / DEFAULT 三端一致性验证")
    ap.add_argument("--impl", choices=["python"], help="直接跑本仓库的 Python 参考实现")
    ap.add_argument("--frames", help="帧文件目录（<name>.<profile>.bin）")
    ap.add_argument("--input", help="单图 PNG 路径（配合 --frame）")
    ap.add_argument("--frame", help="单图对应的帧文件")
    ap.add_argument("--profile", default="retro", choices=["retro", "default"],
                    help="--frames / 单图模式默认 profile")
    ap.add_argument("--profiles", default="retro,default",
                    help="--impl 模式要校验的 profile 列表")
    a = ap.parse_args()

    meta = load_vectors()
    vectors = {v["name"]: v for v in meta["vectors"]}

    print("=" * 68)
    print(" RETRO 三端一致性验证")
    print(f" 测试向量：{len(vectors)} 个  |  帧长：{meta['frame_bytes']} 字节")
    print("=" * 68)

    if a.impl == "python":
        profiles = [p.strip() for p in a.profiles.split(",") if p.strip()]
        actual = run_python_impl(vectors, profiles)
        all_ok = True
        for name, v in vectors.items():
            print(f"\n[{name}]  {v['desc']}")
            for prof in profiles:
                expect = v.get(f"{prof}_md5")
                got = actual.get((name, prof))
                if got is None:
                    print(f"  {RED}FAIL{RESET}  {prof}: 未产出结果")
                    all_ok = False
                    continue
                all_ok &= check(got, expect, f"{prof}")
    elif a.frames:
        all_ok = True
        checked = 0
        skipped = 0
        for name, v in vectors.items():
            print(f"\n[{name}]  {v['desc']}")
            for prof in ("retro", "default"):
                fp = os.path.join(a.frames, f"{name}.{prof}.bin")
                if not os.path.exists(fp):
                    print(f"  {YELLOW}SKIP{RESET}  {prof}: 缺 {fp}")
                    skipped += 1
                    continue
                checked += 1
                n = os.path.getsize(fp)
                if n != meta["frame_bytes"]:
                    print(f"  {RED}FAIL{RESET}  {prof}: 长度 {n} != {meta['frame_bytes']}")
                    all_ok = False
                    continue
                all_ok &= check(md5_of(fp), v[f"{prof}_md5"], prof)
    elif a.input and a.frame:
        base = os.path.splitext(os.path.basename(a.input))[0]
        v = vectors.get(base)
        if v is None:
            print(f"{YELLOW}警告：{base} 不在 test_vectors.json 中，仅做长度检查{RESET}")
            n = os.path.getsize(a.frame)
            print(f"长度 {n}  {'OK' if n == meta['frame_bytes'] else 'FAIL'}")
            sys.exit(0 if n == meta["frame_bytes"] else 1)
        print(f"\n[{base}]  profile={a.profile}")
        all_ok = check(md5_of(a.frame), v[f"{a.profile}_md5"], a.profile)
    else:
        ap.print_help()
        sys.exit(2)

    print("\n" + "=" * 68)
    if a.frames and checked == 0:
        print(f"{RED}❌ 没有任何可比对的帧文件（全部缺失），不能判定通过{RESET}")
        print("=" * 68)
        sys.exit(2)
    if a.frames and skipped:
        print(f"{YELLOW}⚠️  已比对 {checked} 项，跳过 {skipped} 项"
              f"（跳过项无法判定一致性）{RESET}")
    if all_ok:
        print(f"{GREEN}✅ 全部一致（byte-identical）{RESET}")
    else:
        print(f"{RED}❌ 存在不一致，请按 docs/03-三端一致性.md §排查顺序定位{RESET}")
    print("=" * 68)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
