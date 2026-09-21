# SCons entry point that patches the sandbox-blocked os.remove() call.
import sys, os
sys.path.insert(0, r"C:/Users/RockBase-007/.platformio/packages/tool-scons/scons-local-4.8.1")

_real_remove = os.remove
def _safe_remove(path, *a, **kw):
    if str(path).endswith("pioarduino-build.py.esp32s3"):
        sys.stderr.write("[wrap] skipped os.remove(%s)\n" % path)
        return
    return _real_remove(path, *a, **kw)
os.remove = _safe_remove

import SCons.Script
sys.exit(SCons.Script.main())
