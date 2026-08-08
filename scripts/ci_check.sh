#!/usr/bin/env bash
# Phase 0 "WASM cadence" check (phi.md): build both targets and confirm
# they link. Doesn't run either binary — that needs a browser (wasm) or a
# display (native) neither of which a CI runner necessarily has. Run this
# after any change that touches client/, before moving on to the next
# milestone, to catch GL-feature or platform-abstraction drift early.
#
# Usage: scripts/ci_check.sh
# Exit status: 0 if both targets build, non-zero otherwise.

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
if [ "$fail" -eq 0 ]; then
    echo "ci_check: PASS"
else
    echo "ci_check: FAIL"
fi
exit $fail
