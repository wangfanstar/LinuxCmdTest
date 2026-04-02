#!/usr/bin/env bash
# tests/run_tests.sh  —  Master test runner
#
# Usage:
#   bash tests/run_tests.sh [unit|integration|all]
#
# Default: all
# Env vars forwarded to sub-scripts: TEST_PORT, SERVER_BIN

set -euo pipefail

MODE="${1:-all}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "$ROOT_DIR"

TOTAL_FAIL=0

run_binary() {
    local bin="$1"
    echo
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not found or not executable"
        TOTAL_FAIL=$((TOTAL_FAIL+1))
        return
    fi
    "$bin" || TOTAL_FAIL=$((TOTAL_FAIL+1))
}

# ── Unit tests ────────────────────────────────────────────────────
run_unit() {
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  UNIT TESTS"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    run_binary tests/test_log
    run_binary tests/test_threadpool
    run_binary tests/test_helpers
}

# ── Integration tests ─────────────────────────────────────────────
run_integration() {
    echo
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  INTEGRATION TESTS"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    bash tests/test_http_api.sh || TOTAL_FAIL=$((TOTAL_FAIL+1))
}

# ── Dispatch ──────────────────────────────────────────────────────
case "$MODE" in
    unit)        run_unit ;;
    integration) run_integration ;;
    all)         run_unit; run_integration ;;
    *)
        echo "Usage: $0 [unit|integration|all]"
        exit 1
        ;;
esac

# ── Final summary ─────────────────────────────────────────────────
echo
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [[ $TOTAL_FAIL -eq 0 ]]; then
    echo "  ALL TESTS PASSED"
else
    echo "  FAILURES: $TOTAL_FAIL test suite(s) failed"
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

exit "$TOTAL_FAIL"
