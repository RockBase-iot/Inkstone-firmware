#!/usr/bin/env bash
# Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
#
# ============================================================================
# verify_parity.sh — run every byte-parity check this repo can run locally
#
# The RETRO quantizer claims the same 400x300 RGB888 input produces the same
# 30000-byte frame in three independent ports: the Python reference, the
# browser, and the ESP32 firmware. This script runs the two ends that do not
# need a board, plus a compile check on the third.
#
#   1. sRGB lookup table   tools/sync_srgb_table.py   (all four copies agree)
#   2. Python reference    six vectors, self-consistency
#   3. Browser port        tools/check_js_parity.js   vs Python
#   4. Upload page         tools/check_page_preview.py
#                          (both quantizers reachable + byte-correct, and the
#                           page's two classic scripts do not name-collide)
#   5. Firmware port       tools/hosttest/run_hosttest.py vs Python
#                          (compiles the real .cpp for the host; skipped with a
#                           clear message if no host compiler is installed)
#
# The one thing this cannot check is that the Xtensa compiler actually emits the
# arithmetic as written - that needs a flashed board. See docs/RETRO.md
# §Verifying parity for the on-device procedure and why -ffp-contract=off
# matters.
#
# Usage:  bash scripts/verify_parity.sh
# Exit:   0 = everything that could run, passed; 1 = something failed
# ============================================================================

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

# ── locate a Python with numpy (+ Pillow for the baseline check) ────────────
# The repo's own venv is not assumed; several system Pythons may be present and
# only some have numpy. Probe rather than guess.
find_python() {
  local cand winuser
  winuser="${USERNAME:-$USER}"
  for cand in "${PYTHON:-}" python3 python \
              "C:/Users/$winuser/AppData/Local/Programs/Python/Python311/python.exe" \
              "C:/Users/$winuser/AppData/Local/Programs/Python/Python312/python.exe"; do
    [ -z "$cand" ] && continue
    if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then
      if "$cand" -c "import numpy" >/dev/null 2>&1; then
        printf '%s' "$cand"; return 0
      fi
    fi
  done
  return 1
}

PY="$(find_python)" || {
  echo "ERROR: no Python with numpy found. Set PYTHON=/path/to/python and retry."
  exit 2
}
echo "python: $PY"

FAILED=0
note() { printf '\n\033[1m── %s\033[0m\n' "$1"; }
ok()   { printf '   \033[92mOK\033[0m   %s\n' "$1"; }
bad()  { printf '   \033[91mFAIL\033[0m %s\n' "$1"; FAILED=1; }

# ── 1. shared sRGB table ───────────────────────────────────────────────────
note "1/5  sRGB lookup table (4 copies must be identical)"
if "$PY" tools/sync_srgb_table.py; then
  ok "sync_srgb_table.py"
else
  bad "sync_srgb_table.py — run with --write to resync"
fi

# ── 2. Python reference ────────────────────────────────────────────────────
note "2/5  python reference self-consistency"
if "$PY" reference/retro_reference.py >/dev/null 2>&1; then
  ok "retro_reference.py selftest"
else
  bad "retro_reference.py selftest"
fi

# ── 3. browser port ────────────────────────────────────────────────────────
note "3/5  browser port vs python"
if command -v node >/dev/null 2>&1; then
  if [ -d out/raw ]; then
    if node tools/check_js_parity.js --raw-dir out/raw; then
      ok "check_js_parity.js"
    else
      bad "check_js_parity.js"
    fi
  else
    printf '   \033[93mSKIP\033[0m out/raw missing — regenerate with:\n'
    printf '        python tools/convert_image.py <each test png> ...\n'
    printf '        (see docs/RETRO.md §Verifying parity)\n'
  fi
else
  printf '   \033[93mSKIP\033[0m node not on PATH\n'
fi

# ── 4. the upload page's own two quantizers ────────────────────────────────
# The page carries two: the inline DEFAULT path (what it has always pushed) and
# retro_core.js inlined by the build. Both must be reachable from the page scope
# and byte-correct, and the two classic scripts must not share a top-level name
# (that would be a SyntaxError blanking the page, only visible after inlining).
note "4/5  upload page double preview"
if "$PY" tools/check_page_preview.py; then
  ok "check_page_preview.py"
else
  bad "check_page_preview.py"
fi

# ── 5. firmware port (host build) ──────────────────────────────────────────
note "5/5  firmware port vs python (host build of the real .cpp)"
# Record the exit code before `if` consumes it - `$?` inside the else branch is
# the status of the `if` itself, not of the script that just ran.
"$PY" tools/hosttest/run_hosttest.py
rc=$?
if [ "$rc" -eq 0 ]; then
  ok "run_hosttest.py"
elif [ "$rc" -eq 2 ]; then
  printf '   \033[93mSKIP\033[0m no host C++ compiler or no vectors (see message above)\n'
else
  bad "run_hosttest.py"
fi

# ── summary ────────────────────────────────────────────────────────────────
echo
if [ "$FAILED" -eq 0 ]; then
  printf '\033[92mAll runnable parity checks passed.\033[0m\n'
  printf 'Third-end caveat: the host build proves the C++ *source* matches Python.\n'
  printf 'It does not prove Xtensa codegen preserves float32 — for that, flash a\n'
  printf 'board and compare the frame (docs/RETRO.md §Verifying parity).\n'
else
  printf '\033[91mSome parity checks failed.\033[0m See docs/RETRO.md §Troubleshooting.\n'
fi
exit "$FAILED"
