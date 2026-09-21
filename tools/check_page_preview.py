"""Verify the double-preview page's quantizers without a browser.

A syntax check cannot catch the three failures that matter here:

  1. the page's inline script and web/retro_core.js are BOTH classic scripts, so
     they share one top-level lexical scope. A name declared in both is a
     SyntaxError that blanks the entire page - and it would only show up when
     retro_core.js gets inlined at build time.
  2. the inline DEFAULT quantizer must still produce the bytes the firmware
     produces. Reorganising the preview code is exactly the kind of change that
     silently shifts this.
  3. retroQuantize() must be reachable as a global from the page script.

So this extracts the relevant top-level declarations from the page script and
evaluates them in a shared context alongside retro_core.js, then runs both
quantizers over the six real test vectors and compares MD5s against the frozen
baselines.

Run: python tools/check_page_preview.py
"""

import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PAGE = REPO / "web" / "uploader.html"
CORE = REPO / "web" / "retro_core.js"

# Top-level declarations the quantizers need, and nothing else. Listed
# explicitly rather than by slicing the file, so a failure names what moved.
# W/H/FRAME_BYTES come as one statement (`const W = 400, H = 300, ...`), so the
# list holds the whole statement's first name and extract() returns all of it.
PAGE_DECLS = [
    "W", "PAL_PACK", "PAL_VIEW",
    "clamp", "nearest", "spread", "quantizeDefault",
]


def top_level_names(src: str) -> set:
    """Names bound at the top level of a classic script."""
    return set(re.findall(
        r"^(?:const|let|var|function|async function)\s+([A-Za-z_$][\w$]*)", src, re.M))


def extract(js: str, name: str) -> str:
    """Pull one top-level declaration by name, tracking bracket nesting.

    Kept simple and strict: the page script only ever declares top-level things
    at column 0, and every declaration ends either at a `;` at nesting depth 0
    (`const clamp = v => ...;`, `const W = 400, H = 300;`) or at the closing `}`
    that returns to depth 0 (`function quantizeDefault(...) { ... }`).
    """
    m = re.search(r"^(?:const|let|var|function|async function)\s+%s\b" % re.escape(name),
                  js, re.M)
    if not m:
        raise SystemExit("FAIL: %s not found at top level of the page script" % name)
    start = m.start()

    depth = 0
    in_str = None
    saw_brace = False          # have we entered a block body yet?
    j = start
    while j < len(js):
        ch = js[j]
        if in_str:
            if ch == "\\":
                j += 2
                continue
            if ch == in_str:
                in_str = None
        elif ch in "\"'`":
            in_str = ch
        elif ch in "([{":
            depth += 1
            if ch == "{":
                saw_brace = True
        elif ch in ")]}":
            depth -= 1
            # A function is only complete when its BODY closes. Stopping at the
            # ')' that closes the parameter list would return just the
            # signature - which parses, then throws "Unexpected token function"
            # when the next declaration is appended right after it.
            if depth == 0 and saw_brace:
                return js[start:j + 1]
        elif ch == ";" and depth == 0:
            return js[start:j + 1]
        j += 1
    raise SystemExit("FAIL: could not find the end of %s" % name)


def main() -> int:
    page = PAGE.read_text(encoding="utf-8")
    core = CORE.read_text(encoding="utf-8")

    m = re.search(r"<script>\n\"use strict\";([\s\S]*?)</script>\s*</body>", page)
    if not m:
        print("FAIL: could not locate the page's inline script")
        return 1
    js = m.group(1)

    shared = top_level_names(core) & top_level_names(js)
    if shared:
        print("FAIL: top-level name declared in both classic scripts, which blanks"
              " the whole page once retro_core.js is inlined: %s" % ", ".join(sorted(shared)))
        return 1
    print("OK    no top-level name collision (%d page names, %d core names)"
          % (len(top_level_names(js)), len(top_level_names(core))))

    if "retro_core.js" not in page:
        print("FAIL: page does not reference retro_core.js")
        return 1
    if not re.search(r"<script\s+src=[\"']retro_core\.js[\"']\s*>\s*</script>", page):
        print("FAIL: retro_core.js is not loaded as a plain classic <script>")
        return 1

    snippets = "\n".join(extract(js, n) for n in PAGE_DECLS)

    runner = REPO / "out" / "_page_preview_check.js"
    runner.parent.mkdir(parents=True, exist_ok=True)
    runner.write_text(
        "const fs=require('fs'),path=require('path'),vm=require('vm'),crypto=require('crypto');\n"
        "const ROOT=%r;\n"
        "class ImageDataShim{constructor(w,h){this.width=w;this.height=h;"
        "this.data=new Uint8ClampedArray(w*h*4);}}\n"
        "const sandbox={ImageData:ImageDataShim,Math,console,Uint8Array,Uint8ClampedArray,"
        "Int32Array,Float32Array,Number,Array,Object,JSON};\n"
        "vm.createContext(sandbox);\n"
        "vm.runInContext(fs.readFileSync(path.join(ROOT,'web','retro_core.js'),'utf8'),"
        "sandbox,{filename:'retro_core.js'});\n"
        "sandbox.document={createElement:()=>({getContext:()=>({})})};\n"
        "const pageDecls=%r;\n"
        "try{ vm.runInContext(pageDecls,sandbox,{filename:'uploader-inline.js'}); }\n"
        "catch(e){ console.log('PAGE_DECLS_THREW '+e.message); process.exit(3); }\n"
        "const qd=sandbox.quantizeDefault, rq=sandbox.retroQuantize, W=400,H=300;\n"
        "if(typeof qd!=='function'){console.log('FAIL: quantizeDefault not reachable');process.exit(4);}\n"
        "if(typeof rq!=='function'){console.log('FAIL: retroQuantize not reachable from page scope');process.exit(5);}\n"
        "const meta=JSON.parse(fs.readFileSync(path.join(ROOT,'reference','test_vectors.json'),'utf8'));\n"
        "const rawDir=path.join(ROOT,'out','raw');\n"
        "let bad=0,ran=0;\n"
        "for(const v of meta.vectors){\n"
        "  const rp=path.join(rawDir,v.name+'.rgb');\n"
        "  if(!fs.existsSync(rp)){console.log('SKIP  '+v.name);continue;}\n"
        "  const rgb=fs.readFileSync(rp);\n"
        "  const rgba=new Uint8ClampedArray(W*H*4);\n"
        "  for(let i=0;i<W*H;i++){rgba[i*4]=rgb[i*3];rgba[i*4+1]=rgb[i*3+1];"
        "rgba[i*4+2]=rgb[i*3+2];rgba[i*4+3]=255;}\n"
        "  const dFrame=qd(rgba);\n"
        "  const rFrame=rq(rgb,true);\n"
        "  ran++;\n"
        "  const dm=crypto.createHash('md5').update(Buffer.from(dFrame)).digest('hex');\n"
        "  const rm=crypto.createHash('md5').update(Buffer.from(rFrame)).digest('hex');\n"
        "  const dOk=(dFrame.length===30000)&&(dm===v.default_md5);\n"
        "  const rOk=(rFrame.length===30000)&&(rm===v.retro_md5);\n"
        "  if(dOk&&rOk) console.log('PASS  '+v.name+'  default '+dm.slice(0,12)+'  retro '+rm.slice(0,12));\n"
        "  else{ bad++;\n"
        "    if(!dOk) console.log('FAIL  '+v.name+'  DEFAULT '+dFrame.length+'B '+dm+' want '+v.default_md5);\n"
        "    if(!rOk) console.log('FAIL  '+v.name+'  RETRO   '+rFrame.length+'B '+rm+' want '+v.retro_md5);\n"
        "  }\n"
        "}\n"
        "console.log(bad===0&&ran>0?'page quantizers OK on '+ran+'/'+ran+' vectors':'FAILED '+bad);\n"
        "process.exit(bad===0&&ran>0?0:1);\n"
        % (str(REPO), snippets)
        , encoding="utf-8")

    r = subprocess.run(["node", str(runner)], cwd=str(REPO))
    if r.returncode == 0:
        print("OK    both quantizers reachable from page scope and byte-correct")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
