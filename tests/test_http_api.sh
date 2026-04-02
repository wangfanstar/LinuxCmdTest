#!/usr/bin/env bash
# tests/test_http_api.sh  —  HTTP integration tests (curl)
#
# Starts the server on a temporary port, runs tests, then kills it.
# Exit code: 0 = all passed, 1 = any failure.
#
# Env vars (with defaults):
#   TEST_PORT   port to run the server on   (default: 18881)
#   SERVER_BIN  path to the compiled binary (default: ./bin/simplewebserver)

set -euo pipefail

PORT="${TEST_PORT:-18881}"
BIN="${SERVER_BIN:-./bin/simplewebserver}"
BASE="http://127.0.0.1:${PORT}"

PASS=0
FAIL=0
SERVER_PID=""

# ── Colour helpers ────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'

ok()   { echo -e "  ${GREEN}OK${NC}  $1"; PASS=$((PASS+1)); }
fail() { echo -e "  ${RED}FAIL${NC} $1"; FAIL=$((FAIL+1)); }

# ── Cleanup on exit ───────────────────────────────────────────────
cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ── Start server ──────────────────────────────────────────────────
echo "=== HTTP API Integration Tests ==="
echo "Binary : $BIN"
echo "Port   : $PORT"
echo

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: server binary not found at $BIN"
    exit 1
fi

# Ensure required directories exist
mkdir -p html/report logs

"$BIN" -p "$PORT" -t 2 -q 16 -l /tmp/wfserver_test_http_logs &
SERVER_PID=$!

# Wait for the server to be ready (up to 8 seconds)
MAX_WAIT=40
for ((i=0; i<MAX_WAIT; i++)); do
    if curl -sf --max-time 1 "${BASE}/" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "ERROR: server exited prematurely"
    exit 1
fi

echo "Server ready (pid $SERVER_PID)"
echo

# ── Test helpers ──────────────────────────────────────────────────
check_status() {
    local desc="$1" url="$2" expected_code="$3"
    local actual
    actual=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$url")
    if [[ "$actual" == "$expected_code" ]]; then
        ok "$desc (HTTP $actual)"
    else
        fail "$desc (expected $expected_code, got $actual)"
    fi
}

check_body_contains() {
    local desc="$1" url="$2" needle="$3"
    local body
    body=$(curl -sf --max-time 5 "$url" 2>/dev/null || echo "")
    if echo "$body" | grep -qF "$needle"; then
        ok "$desc"
    else
        fail "$desc (needle not found: '$needle')"
    fi
}

check_post_status() {
    local desc="$1" url="$2" data="$3" expected_code="$4"
    local actual
    actual=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
        -X POST -H 'Content-Type: application/json' -d "$data" "$url")
    if [[ "$actual" == "$expected_code" ]]; then
        ok "$desc (HTTP $actual)"
    else
        fail "$desc (expected $expected_code, got $actual)"
    fi
}

check_post_body() {
    local desc="$1" url="$2" data="$3" needle="$4"
    local body
    body=$(curl -s --max-time 5 \
        -X POST -H 'Content-Type: application/json' -d "$data" "$url" 2>/dev/null || echo "")
    if echo "$body" | grep -qF "$needle"; then
        ok "$desc"
    else
        fail "$desc (needle not found: '$needle' in '$body')"
    fi
}

# ── Static file serving ───────────────────────────────────────────
echo "-- Static files --"
check_status   "GET /              returns 200" "${BASE}/"              "200"
check_status   "GET /index.html    returns 200" "${BASE}/index.html"    "200"
check_status   "GET /nonexist.html returns 404" "${BASE}/nonexist.html" "404"
check_status   "GET /..illegal     returns 400 or 404" \
               "${BASE}/../etc/passwd" "400"

# ── GET API endpoints ─────────────────────────────────────────────
echo
echo "-- GET API --"
check_status       "GET /api/client-info  returns 200" \
                   "${BASE}/api/client-info" "200"
check_body_contains "GET /api/client-info  returns ok:true" \
                   "${BASE}/api/client-info" '"ok":true'
check_body_contains "GET /api/client-info  returns ip field" \
                   "${BASE}/api/client-info" '"ip"'

check_status       "GET /api/reports      returns 200" \
                   "${BASE}/api/reports" "200"
check_body_contains "GET /api/reports      returns ok:true" \
                   "${BASE}/api/reports" '"ok":true'
check_body_contains "GET /api/reports      returns files array" \
                   "${BASE}/api/reports" '"files"'

check_status       "GET /api/list-all-configs returns 200" \
                   "${BASE}/api/list-all-configs" "200"
check_body_contains "GET /api/list-all-configs returns ok:true" \
                   "${BASE}/api/list-all-configs" '"ok":true'

check_status       "GET /unknown/endpoint returns 404" \
                   "${BASE}/api/no-such-endpoint" "404"

# ── POST /api/cancel ──────────────────────────────────────────────
echo
echo "-- POST /api/cancel --"
check_post_body   "POST /api/cancel     returns ok:true" \
                  "${BASE}/api/cancel" '{}' '"ok":true'

# ── POST /api/ssh-exec  (invalid body → 400) ─────────────────────
echo
echo "-- POST /api/ssh-exec (validation) --"
check_post_status "POST /api/ssh-exec   empty body → 400" \
                  "${BASE}/api/ssh-exec" '' "400"
check_post_status "POST /api/ssh-exec-stream empty body → 400" \
                  "${BASE}/api/ssh-exec-stream" '' "400"
check_post_status "POST /api/ssh-exec-one  empty body → 400" \
                  "${BASE}/api/ssh-exec-one" '' "400"

# ── POST /api/save-report (no body → 400) ────────────────────────
echo
echo "-- POST /api/save-report (validation) --"
check_post_status "POST /api/save-report no body → 400" \
                  "${BASE}/api/save-report" '' "400"

# ── POST /api/save-config (no body → 400) ────────────────────────
echo
echo "-- POST /api/save-config (validation) --"
check_post_status "POST /api/save-config no body → 400" \
                  "${BASE}/api/save-config" '' "400"

# ── POST /api/delete-report (no body → 400) ──────────────────────
echo
echo "-- POST /api/delete-report (validation) --"
check_post_status "POST /api/delete-report no body → 400" \
                  "${BASE}/api/delete-report" '' "400"

# ── POST /api/kill (invalid pid → 400) ───────────────────────────
echo
echo "-- POST /api/kill (validation) --"
check_post_body   "POST /api/kill  pid=0   → invalid pid error" \
                  "${BASE}/api/kill" '{"pid":0}' '"ok":false'
check_post_body   "POST /api/kill  pid=-1  → invalid pid error" \
                  "${BASE}/api/kill" '{"pid":-1}' '"ok":false'

# ── Content-Type header check ─────────────────────────────────────
echo
echo "-- Response headers --"
CT=$(curl -s -I --max-time 5 "${BASE}/api/client-info" \
     | grep -i content-type | head -1 | tr -d '\r')
if echo "$CT" | grep -qi "application/json"; then
    ok "GET /api/client-info Content-Type: application/json"
else
    fail "GET /api/client-info Content-Type not application/json (got: $CT)"
fi

CT_HTML=$(curl -s -I --max-time 5 "${BASE}/index.html" \
     | grep -i content-type | head -1 | tr -d '\r')
if echo "$CT_HTML" | grep -qi "text/html"; then
    ok "GET /index.html Content-Type: text/html"
else
    fail "GET /index.html Content-Type not text/html (got: $CT_HTML)"
fi

# ── CORS header check ─────────────────────────────────────────────
echo
echo "-- CORS headers --"
CORS=$(curl -s -I --max-time 5 "${BASE}/api/client-info" \
      | grep -i access-control | head -1 | tr -d '\r')
if echo "$CORS" | grep -qi "Access-Control-Allow-Origin"; then
    ok "GET /api/client-info has Access-Control-Allow-Origin header"
else
    fail "GET /api/client-info missing Access-Control-Allow-Origin (got: $CORS)"
fi

# ── Summary ───────────────────────────────────────────────────────
echo
echo "Results: ${PASS} passed, ${FAIL} failed"
echo

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
