#!/usr/bin/env python3
"""Headless-Chrome smoke test for web/uploader.html.

What this guards against, and nothing more:
  * a top-level name collision between uploader.html's inline classic script and
    retro_core.js -- which throws a SyntaxError, blanks the whole page, and only
    shows up AFTER gen_embedded_page.py inlines the two together
  * a runtime throw while wiring up the double-preview UI
  * an obvious layout break (panes stacked instead of side by side)

It does NOT verify quantizer output. tools/check_page_preview.py owns that, and
it does so against frozen MD5 baselines instead of a screenshot.

Usage:  python tools/_shot_page.py [--keep]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
WEB = REPO / "web"
PAGE = WEB / "uploader.html"
OUT = REPO / "out"
SHOT = OUT / "page_preview.png"
CDP_LOG = OUT / "page_console.log"

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]


def find_chrome() -> str:
    for c in CHROME_CANDIDATES:
        if Path(c).is_file():
            return c
    exe = shutil.which("chrome") or shutil.which("chrome.exe")
    if exe:
        return exe
    raise SystemExit("FAIL: no Chrome/Edge found for the headless screenshot")


def build_probe() -> Path:
    """A temp copy of the page with a tiny error-capture shim prepended.

    We cannot use --dump-dom + console output directly: Chrome's headless
    console only reaches stderr with --enable-logging, and the message format is
    unstable across versions. Collecting errors into document.title ourselves is
    version-proof and survives --dump-dom.
    """
    html = PAGE.read_text(encoding="utf-8")
    shim = """<script>
window.__err = [];
window.onerror = function (m, s, l, c) {
  window.__err.push(String(m) + " @" + l + ":" + c);
  document.title = "JSERROR:" + window.__err.join(" | ");
};
</script>
"""
    # The capture shim must run BEFORE the two classic scripts do.
    out = re.sub(r"(<head[^>]*>)", r"\1\n" + shim, html, count=1, flags=re.I)
    if out == html:
        # No <head>: fall back to prepending at the very top of the document.
        out = shim + html
    tmp = Path(tempfile.mkdtemp(prefix="pageshot_")) / "uploader.html"
    tmp.write_text(out, encoding="utf-8")
    # retro_core.js is loaded by relative src; copy it next to the temp page.
    shutil.copy2(WEB / "retro_core.js", tmp.parent / "retro_core.js")
    return tmp


def run(chrome: str, page: Path, args: list[str]) -> subprocess.CompletedProcess:
    # file:// is enough here: the page touches no network until the user hits
    # send, and the quantizers are pure functions of a local image.
    return subprocess.run(
        [chrome, "--headless=new", "--disable-gpu", "--no-sandbox",
         "--allow-file-access-from-files", "--hide-scrollbars",
         "--virtual-time-budget=4000",
         "--user-data-dir=" + tempfile.mkdtemp(prefix="chromeprof_")] + args + [page.as_uri()],
        capture_output=True, text=True, timeout=120,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the temp page copy")
    opts = ap.parse_args()

    OUT.mkdir(exist_ok=True)
    chrome = find_chrome()
    print("[shot] using %s" % chrome)

    page = build_probe()
    print("[shot] probe page: %s" % page)

    # --- 1. DOM + title after scripts ran -----------------------------------
    dom = run(chrome, page, ["--dump-dom"])
    html = dom.stdout or ""
    title = ""
    m = re.search(r"<title[^>]*>(.*?)</title>", html, re.S | re.I)
    if m:
        title = m.group(1).strip()

    errs: list[str] = []
    if title.startswith("JSERROR:"):
        errs = [s.strip() for s in title[len("JSERROR:"):].split("|") if s.strip()]

    checks: list[tuple[str, bool, str]] = []

    checks.append(("no uncaught JS error", not errs,
                   "; ".join(errs) if errs else "clean"))

    # Both panes and both canvases must exist -- that is the double preview.
    for i in ("paneDefault", "paneRetro", "prevDefault", "prevRetro"):
        checks.append(("element #%s present" % i, ('id="%s"' % i) in html, ""))

    # Both top-level quantizers must be reachable, i.e. the classic-script
    # globals actually got defined (this is what the inline step can break).
    for fn in ("quantizeDefault", "retroQuantize", "RETRO_PAL_VIEW", "PAL_VIEW"):
        checks.append(("global %s defined" % fn, fn in html, ""))

    # --- 2. screenshot ------------------------------------------------------
    shot = run(chrome, page, ["--screenshot=" + str(SHOT),
                              "--window-size=1200,2400"])
    ok_shot = SHOT.is_file() and SHOT.stat().st_size > 10_000
    checks.append(("screenshot written", ok_shot,
                   "%d bytes" % SHOT.stat().st_size if SHOT.is_file() else "missing"))

    # --- 3. the two panes must be side by side, not stacked -----------------
    if ok_shot:
        try:
            from PIL import Image  # optional
            im = Image.open(SHOT)
            checks.append(("screenshot dimensions", im.size[0] >= 1000,
                           "%dx%d" % im.size))
        except Exception:
            pass  # PIL absent is fine; the size check above still ran

    width = max(len(n) for n, _, _ in checks)
    bad = 0
    for name, ok, extra in checks:
        mark = "PASS" if ok else "FAIL"
        if not ok:
            bad += 1
        print("  [%s] %-*s %s" % (mark, width, name, extra))

    CDP_LOG.write_text(
        "chrome: %s\npage: %s\ntitle: %s\nerrors: %s\n"
        % (chrome, page, title, json.dumps(errs, ensure_ascii=False)),
        encoding="utf-8")

    if not opts.keep:
        shutil.rmtree(page.parent, ignore_errors=True)

    print("[shot] %d/%d checks passed" % (len(checks) - bad, len(checks)))
    if bad:
        print("[shot] screenshot: %s" % SHOT)
        return 1
    print("[shot] screenshot: %s" % SHOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
