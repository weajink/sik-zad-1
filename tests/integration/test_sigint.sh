#!/usr/bin/env bash
# Verify that the server exits cleanly (exit code 0, socket released) when
# it receives SIGINT or SIGTERM. An unhandled signal yields exit code 130/143
# and can leave the UDP port in TIME_WAIT / unreleased state.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

echo "Test 1: SIGINT → server exits with code 0"
"$SERVER_BIN" -r "1111" -a 127.0.0.1 -p "$PORT" -t 10 &
pid=$!
sleep 0.3
if ! kill -0 "$pid" 2>/dev/null; then
    echo "  FAIL: server died before we could signal it"
    exit 1
fi
kill -INT "$pid"
set +e
wait "$pid"
rc=$?
set -e
if [[ "$rc" -ne 0 ]]; then
    echo "  FAIL: expected exit code 0 after SIGINT, got $rc"
    exit 1
fi

echo "Test 2: SIGTERM → server exits with code 0"
PORT2=$(get_random_port)
"$SERVER_BIN" -r "1111" -a 127.0.0.1 -p "$PORT2" -t 10 &
pid=$!
sleep 0.3
kill -TERM "$pid"
set +e
wait "$pid"
rc=$?
set -e
if [[ "$rc" -ne 0 ]]; then
    echo "  FAIL: expected exit code 0 after SIGTERM, got $rc"
    exit 1
fi

echo "Test 3: SIGINT mid-conversation — server releases port"
PORT3=$(get_random_port)
"$SERVER_BIN" -r "1111" -a 127.0.0.1 -p "$PORT3" -t 10 &
pid=$!
sleep 0.3
# Send one message so the server has interacted at least once.
run_client "$PORT3" "0/42"
assert_exit_code 0
kill -INT "$pid"
set +e
wait "$pid"
rc=$?
set -e
if [[ "$rc" -ne 0 ]]; then
    echo "  FAIL: expected exit code 0 after mid-conversation SIGINT, got $rc"
    exit 1
fi
# Re-bind the same port immediately — fails if the old server still holds it.
"$SERVER_BIN" -r "1111" -a 127.0.0.1 -p "$PORT3" -t 10 &
pid=$!
sleep 0.3
if ! kill -0 "$pid" 2>/dev/null; then
    echo "  FAIL: fresh server could not bind the port the old server released"
    exit 1
fi
kill -INT "$pid"
wait "$pid" 2>/dev/null || true

echo "All SIGINT / SIGTERM tests passed."
