#!/usr/bin/env bash
# Phase 0 "WASM cadence" check (phi.md): build every target this machine
# can and confirm they link. Doesn't run any of them — that needs a
# browser (wasm), a display (native), or (win32) actually being under
# WSL with the Windows-side MinGW-w64 toolchain from phi.md's Windows
# verification note, none of which a CI runner necessarily has. Run this
# after any change that touches client/, before moving on to the next
# milestone, to catch GL-feature or platform-abstraction drift early.
#
# Usage: scripts/ci_check.sh
# Exit status: 0 if every attempted target builds, non-zero otherwise.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

fail=0

echo "== make native =="
if make native; then
    echo "native: OK ($(du -h build/phi_native | cut -f1))"
else
    echo "native: FAILED"
    fail=1
fi

echo
echo "== make wasm =="
if command -v emcc >/dev/null 2>&1; then
    if make wasm; then
        echo "wasm: OK ($(du -h www/game.wasm | cut -f1) + $(du -h www/game.js | cut -f1))"
    else
        echo "wasm: FAILED"
        fail=1
    fi
else
    echo "wasm: SKIPPED (emcc not on PATH — source emsdk_env.sh first)"
fi

echo
echo "== make win32 =="
if [ -x /mnt/c/msys64/mingw64/bin/gcc.exe ]; then
    if make win32; then
        echo "win32: OK ($(du -h build/phi_win32.exe | cut -f1))"
    else
        echo "win32: FAILED"
        fail=1
    fi
else
    echo "win32: SKIPPED (not under WSL with MinGW-w64 at /mnt/c/msys64/mingw64 — expected on other machines)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "ci_check: PASS"
else
    echo "ci_check: FAIL"
fi
exit $fail
