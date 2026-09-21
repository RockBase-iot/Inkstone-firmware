#!/usr/bin/env python3
"""Render the double-preview panes with a real image, headlessly.

The first smoke test (tools/_shot_page.py) only proves the page does not throw
and that both quantizers are reachable. The preview card starts hidden until an
image is picked, so that test cannot see the panes at all. This one drives the
page: it synthesizes a 400x300 test image, feeds it to the same code path the
file-input handler uses, lets both quantizers run, then screenshots the result.

It also asserts the two frames are NOT identical -- a colour-blocked image
should look very different under Floyd-Steinberg and under OKLab+hue-mask, so
identical frames would mean one pane is silently reusing the other's output.

Usage:  python tools/_shot_preview.py [--keep]
"""
from __future__ import annotations

import argparse
import base64
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
SHOT = OUT / "page_preview_driven.png"
LOG = OUT / "page_preview_driven.log"

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
]


def find_chrome() -> str:
    for c in CHROME_CANDIDATES:
        if Path(c).is_file():
            return c
    raise SystemExit("FAIL: no Chrome/Edge found")


def synth_png() -> bytes:
    """A 400x300 image with saturated colour blocks, a gradient and a face-ish
    warm blotch -- enough to make the two profiles diverge visibly."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        raise SystemExit("FAIL: this tool needs Pillow (pip install pillow)")

    im = Image.new("RGB", (400, 300), (240, 238, 230))
    d = ImageDraw.Draw(im)
    # saturated primaries / secondaries across the top
    for i, col in enumerate([(220, 40, 40), (240, 190, 30), (40, 160, 70),
                             (40, 90, 200), (150, 60, 180), (30, 30, 30)]):
        d.rectangle([i * 66, 0, i * 66 + 64, 90], fill=col)
    # horizontal grey ramp
    for x in range(0, 400, 2):
        v = int(x * 255 / 400)
        d.rectangle([x, 95, x + 1, 150], fill=(v, v, v))
    # warm skin-ish ellipse (the hue mask should treat this specially)
    d.ellipse([60, 165, 190, 285], fill=(226, 168, 138))
    d.ellipse([230, 165, 360, 285], fill=(90, 120, 100))
    e = im.resize((400, 300))
    buf = tempfile.NamedTemporaryFile(suffix=".png", delete=False)
    e.save(buf, format="PNG")
    buf.close()
    return Path(buf.name).read_bytes()


DRIVER = """
<script>
(function () {
  var b64 = %(b64)s;
  var img = new Image();
  img.onload = function () {
    try {
      // Drive the page the way loadImage() does: set srcBitmap, let the PAGE
      // initialise the crop state via initCrop(), then quantize. Skipping
      // initCrop() leaves crop at its {s:1,x:0,y:0} default, which makes
      // renderCropped() scale wrongly and paints a black band across the
      // preview -- tools/_probe_drive.py confirmed the quantizers themselves
      // are fine, the bug was in this driver.
      window.__driveResult = {};
      srcBitmap = img;
      initCrop();

      ["cropCard", "prevCard", "upCard"].forEach(function (id) {
        var el = document.getElementById(id);
        if (el) el.classList.remove("hidden");
      });
      quantize();

      var a = frames["default"], b = frames["retro"];
      window.__driveResult.hasDefault = !!a;
      window.__driveResult.hasRetro = !!b;
      window.__driveResult.defaultLen = a ? a.length : -1;
      window.__driveResult.retroLen = b ? b.length : -1;
      var diff = -1;
      if (a && b && a.length === b.length) {
        diff = 0;
        for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) diff++;
      }
      window.__driveResult.byteDiff = diff;
      window.__driveResult.defaultMd5 = a ? md5hex(a) : null;
      window.__driveResult.retroMd5 = b ? md5hex(b) : null;
      window.__driveResult.chosen = chosen;
      document.title = "DRIVEN:" + JSON.stringify(window.__driveResult);
    } catch (e) {
      document.title = "JSERROR:" + e.message;
    }
  };
  img.onerror = function () { document.title = "JSERROR:image decode failed"; };
  img.src = "data:image/png;base64," + b64;
})();
</script>
"""

# A tiny self-contained MD5 so the probe does not depend on any page global.
MD5_JS = """
<script>
function md5hex(u8) {
  function rl(n, c) { return (n << c) | (n >>> (32 - c)); }
  function au(x, y) { var l = (x & 0xFFFF) + (y & 0xFFFF); return (((x >> 16) + (y >> 16) + (l >> 16)) << 16) | (l & 0xFFFF); }
  function cmn(q, a, b, x, s, t) { return au(rl(au(au(a, q), au(x, t)), s), b); }
  function ff(a,b,c,d,x,s,t){return cmn((b&c)|((~b)&d),a,b,x,s,t);}
  function gg(a,b,c,d,x,s,t){return cmn((b&d)|(c&(~d)),a,b,x,s,t);}
  function hh(a,b,c,d,x,s,t){return cmn(b^c^d,a,b,x,s,t);}
  function ii(a,b,c,d,x,s,t){return cmn(c^(b|(~d)),a,b,x,s,t);}
  function blk(s) {
    var i, n = ((s.length + 8) >> 6) + 1, bl = new Array(n * 16).fill(0);
    for (i = 0; i < s.length; i++) bl[i >> 2] |= s[i] << ((i % 4) * 8);
    bl[i >> 2] |= 0x80 << ((i % 4) * 8);
    bl[n * 16 - 2] = s.length * 8;
    return bl;
  }
  var s = Array.from(u8), b = blk(s);
  var a = 1732584193, bb = -271733879, c = -1732584194, d = 271733878;
  for (var i = 0; i < b.length; i += 16) {
    var oa=a, ob=bb, oc=c, od=d;
    a=ff(a,bb,c,d,b[i+0],7,-680876936);  d=ff(d,a,bb,c,b[i+1],12,-389564586);
    c=ff(c,d,a,bb,b[i+2],17,606105819);  bb=ff(bb,c,d,a,b[i+3],22,-1044525330);
    a=ff(a,bb,c,d,b[i+4],7,-176418897);  d=ff(d,a,bb,c,b[i+5],12,1200080426);
    c=ff(c,d,a,bb,b[i+6],17,-1473231341);bb=ff(bb,c,d,a,b[i+7],22,-45705983);
    a=ff(a,bb,c,d,b[i+8],7,1770035416);  d=ff(d,a,bb,c,b[i+9],12,-1958414417);
    c=ff(c,d,a,bb,b[i+10],17,-42063);    bb=ff(bb,c,d,a,b[i+11],22,-1990404162);
    a=ff(a,bb,c,d,b[i+12],7,1804603682); d=ff(d,a,bb,c,b[i+13],12,-40341101);
    c=ff(c,d,a,bb,b[i+14],17,-1502002290);bb=ff(bb,c,d,a,b[i+15],22,1236535329);
    a=gg(a,bb,c,d,b[i+1],5,-165796510);  d=gg(d,a,bb,c,b[i+6],9,-1069501632);
    c=gg(c,d,a,bb,b[i+11],14,643717713); bb=gg(bb,c,d,a,b[i+0],20,-373897302);
    a=gg(a,bb,c,d,b[i+5],5,-701558691);  d=gg(d,a,bb,c,b[i+10],9,38016083);
    c=gg(c,d,a,bb,b[i+15],14,-660478335);bb=gg(bb,c,d,a,b[i+4],20,-405537848);
    a=gg(a,bb,c,d,b[i+9],5,568446438);   d=gg(d,a,bb,c,b[i+14],9,-1019803690);
    c=gg(c,d,a,bb,b[i+3],14,-187363961); bb=gg(bb,c,d,a,b[i+8],20,1163531501);
    a=gg(a,bb,c,d,b[i+13],5,-1444681467);d=gg(d,a,bb,c,b[i+2],9,-51403784);
    c=gg(c,d,a,bb,b[i+7],14,1735328473); bb=gg(bb,c,d,a,b[i+12],20,-1926607734);
    a=hh(a,bb,c,d,b[i+5],4,-378558);     d=hh(d,a,bb,c,b[i+8],11,-2022574463);
    c=hh(c,d,a,bb,b[i+11],16,1839030562);bb=hh(bb,c,d,a,b[i+14],23,-35309556);
    a=hh(a,bb,c,d,b[i+1],4,-1530992060); d=hh(d,a,bb,c,b[i+4],11,1272893353);
    c=hh(c,d,a,bb,b[i+7],16,-155497632); bb=hh(bb,c,d,a,b[i+10],23,-1094730640);
    a=hh(a,bb,c,d,b[i+13],4,681279174);  d=hh(d,a,bb,c,b[i+0],11,-358537222);
    c=hh(c,d,a,bb,b[i+3],16,-722521979); bb=hh(bb,c,d,a,b[i+6],23,76029189);
    a=hh(a,bb,c,d,b[i+9],4,-640364487);  d=hh(d,a,bb,c,b[i+12],11,-421815835);
    c=hh(c,d,a,bb,b[i+15],16,530742520); bb=hh(bb,c,d,a,b[i+2],23,-995338651);
    a=ii(a,bb,c,d,b[i+0],6,-198630844);  d=ii(d,a,bb,c,b[i+7],10,1126891415);
    c=ii(c,d,a,bb,b[i+14],15,-1416354905);bb=ii(bb,c,d,a,b[i+5],21,-57434055);
    a=ii(a,bb,c,d,b[i+12],6,1700485571); d=ii(d,a,bb,c,b[i+3],10,-1894986606);
    c=ii(c,d,a,bb,b[i+10],15,-1051523);  bb=ii(bb,c,d,a,b[i+1],21,-2054922799);
    a=ii(a,bb,c,d,b[i+8],6,1873313359);  d=ii(d,a,bb,c,b[i+15],10,-30611744);
    c=ii(c,d,a,bb,b[i+6],15,-1560198380);bb=ii(bb,c,d,a,b[i+13],21,1309151649);
    a=ii(a,bb,c,d,b[i+4],6,-145523070);  d=ii(d,a,bb,c,b[i+11],10,-1120210379);
    c=ii(c,d,a,bb,b[i+2],15,718787259);  bb=ii(bb,c,d,a,b[i+9],21,-343485551);
    a=au(a,oa); bb=au(bb,ob); c=au(c,oc); d=au(d,od);
  }
  var h = "";
  [a, bb, c, d].forEach(function (n) {
    for (var j = 0; j < 4; j++) h += ("0" + ((n >> (j * 8)) & 255).toString(16)).slice(-2);
  });
  return h;
}
</script>
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    opts = ap.parse_args()

    OUT.mkdir(exist_ok=True)
    chrome = find_chrome()

    png = synth_png()
    b64 = base64.b64encode(png).decode("ascii")
    print("[drv] test image: %d bytes png" % len(png))

    html = PAGE.read_text(encoding="utf-8")
    inject = MD5_JS + (DRIVER % {"b64": json_dumps(b64)})
    # Inject right before </body> so both page scripts and retro_core.js have
    # already executed and defined their globals.
    if "</body>" in html.lower():
        html = re.sub(r"</body>", inject + "\n</body>", html, count=1, flags=re.I)
    else:
        html += inject

    tmpdir = Path(tempfile.mkdtemp(prefix="pagedrv_"))
    page = tmpdir / "uploader.html"
    page.write_text(html, encoding="utf-8")
    shutil.copy2(WEB / "retro_core.js", tmpdir / "retro_core.js")

    base = [chrome, "--headless=new", "--disable-gpu", "--no-sandbox",
            "--allow-file-access-from-files", "--hide-scrollbars",
            "--user-data-dir=" + tempfile.mkdtemp(prefix="chromeprof_")]

    dom = subprocess.run(base + ["--virtual-time-budget=15000", "--dump-dom",
                                 page.as_uri()],
           capture_output=True, text=True, encoding="utf-8",
           errors="replace", timeout=180)
    h = dom.stdout or ""
    m = re.search(r"<title[^>]*>(.*?)</title>", h, re.S | re.I)
    title = m.group(1).strip() if m else ""

    log = ["title: " + title]
    ok = True
    if title.startswith("JSERROR:"):
        print("[drv] JS ERROR: %s" % title[len("JSERROR:"):])
        ok = False
    elif title.startswith("DRIVEN:"):
        import json
        r = json.loads(title[len("DRIVEN:"):])
        for k in ("hasDefault", "hasRetro"):
            good = bool(r.get(k))
            print("  [%s] %s = %s" % ("PASS" if good else "FAIL", k, r.get(k)))
            ok &= good
            log.append("%s=%s" % (k, r.get(k)))
        # Both frames must be full-size, and they must DIFFER: a saturated test
        # image looks nothing alike under Floyd-Steinberg vs OKLab+hue-mask, so
        # zero differing bytes would mean one pane silently reused the other.
        for k in ("defaultLen", "retroLen"):
            good = r.get(k) == 30000
            print("  [%s] %s = %s" % ("PASS" if good else "FAIL", k, r.get(k)))
            ok &= good
            log.append("%s=%s" % (k, r.get(k)))
        d = r.get("byteDiff")
        good = isinstance(d, int) and d > 0
        print("  [%s] frames differ across profiles (byteDiff=%s)"
              % ("PASS" if good else "FAIL", d))
        ok &= good
        log.append("byteDiff=%s" % d)
        print("        DEFAULT md5 %s" % r.get("defaultMd5"))
        print("        RETRO   md5 %s" % r.get("retroMd5"))
        log.append("default_md5=%s" % r.get("defaultMd5"))
        log.append("retro_md5=%s" % r.get("retroMd5"))
    else:
        print("[drv] unexpected title: %r" % title[:200])
        ok = False

    LOG.write_text("\n".join(log) + "\n", encoding="utf-8")

    shot = subprocess.run(base + ["--virtual-time-budget=15000",
                                  "--screenshot=" + str(SHOT),
                                  "--window-size=1100,2000", page.as_uri()],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=180)
    if SHOT.is_file():
        print("[drv] screenshot: %s (%d bytes)" % (SHOT, SHOT.stat().st_size))
    else:
        print("[drv] FAIL: no screenshot")
        ok = False

    if not opts.keep:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print("[drv] %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def json_dumps(s: str) -> str:
    import json
    return json.dumps(s)


if __name__ == "__main__":
    sys.exit(main())
