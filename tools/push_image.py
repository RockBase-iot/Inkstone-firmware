#!/usr/bin/env python3
"""
push_image.py — AI / script call example: image -> raw -> POST /api/v1/display

Usage:
    python tools/push_image.py photo.jpg --token <32hex> [--host inkstone.local]
  python tools/push_image.py photo.jpg                            # open mode, no token
  python tools/push_image.py photo.jpg --token <t> --direct-jpeg  # on-device decode

Status semantics: 202 = accepted, refreshing; 409 = busy (retry per
retry_after_s); 429 = rate limited (retry per Retry-After); 401 = bad token;
no response within ~3 s = device is in deep sleep.
"""

import argparse
import os
import sys

import requests

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_image import convert_image_to_2bpp  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--token", default="",
                    help="API token; optional once open mode is enabled (/api/v1/auth open)")
    ap.add_argument("--host", default="inkstone.local",
                    help="device host, e.g. inkstone.local")
    ap.add_argument("--direct-jpeg", action="store_true",
                    help="send the JPEG as-is; device decodes + FS-dithers")
    args = ap.parse_args()

    base = f"http://{args.host}"
    hdr = {"Authorization": f"Bearer {args.token}"} if args.token else {}

    # 1) Probe first: no response within 3 s means deep sleep
    try:
        s = requests.get(f"{base}/api/v1/status", headers=hdr, timeout=3)
        s.raise_for_status()
        info = s.json()
        print(f"device: {info['ip']} busy={info['busy']} "
              f"next_allowed_in={info.get('next_allowed_update')}")
    except requests.RequestException:
        print("No response (device likely in deep sleep). "
              "Press the BOOT key to wake it, then retry.")
        return 2

    # 2) Convert + push
    if args.direct_jpeg:
        data = open(args.image, "rb").read()
        hdr["Content-Type"] = "image/jpeg"
    else:
        data = convert_image_to_2bpp(args.image)
        hdr["Content-Type"] = "application/octet-stream"

    r = requests.post(f"{base}/api/v1/display", headers=hdr, data=data, timeout=30)
    print(f"POST /api/v1/display -> {r.status_code} {r.text}")
    if r.status_code == 202:
        print("Accepted, device is refreshing (~30 s).")
        return 0
    if r.status_code == 429:
        print(f"Rate limited: retry in {r.headers.get('Retry-After', '?')} s.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
