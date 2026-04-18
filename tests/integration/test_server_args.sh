#!/usr/bin/env bash
# Server CLI validation: -p, -t, -a, missing args (task.txt §4).
#   * -p: 0..65535 inclusive (server allows 0 = any port)
#   * -t: 1..99 inclusive
#   * -a: must be parseable as IPv4 address or domain name
#   * all four args are required
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

expect_server_exit_1() {
    local label="$1"
    shift
    set +e
    "$SERVER_BIN" "$@" >/dev/null 2>&1
    local rc=$?
    set -e
    if [[ $rc -ne 1 ]]; then
        echo "  FAIL [$label]: expected exit 1, got $rc"
        return 1
    fi
    echo "  OK   [$label]"
}

PORT=$(get_random_port)

echo "Test: server rejects invalid -t (1..99 only)"
expect_server_exit_1 "t=0"       -r "111" -a 127.0.0.1 -p "$PORT" -t 0
expect_server_exit_1 "t=100"     -r "111" -a 127.0.0.1 -p "$PORT" -t 100
expect_server_exit_1 "t=-1"      -r "111" -a 127.0.0.1 -p "$PORT" -t -1
expect_server_exit_1 "t=abc"     -r "111" -a 127.0.0.1 -p "$PORT" -t abc

echo "Test: server rejects invalid -p"
expect_server_exit_1 "p=65536"   -r "111" -a 127.0.0.1 -p 65536 -t 5
expect_server_exit_1 "p=-1"      -r "111" -a 127.0.0.1 -p -1    -t 5
expect_server_exit_1 "p=abc"     -r "111" -a 127.0.0.1 -p abc   -t 5

echo "Test: server rejects invalid -a"
expect_server_exit_1 "a=garbage" -r "111" -a not_a_real_address_zzz_qqq -p "$PORT" -t 5

echo "Test: server rejects missing required args"
expect_server_exit_1 "missing -r"        -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "missing -a" -r "111"             -p "$PORT" -t 5
expect_server_exit_1 "missing -p" -r "111" -a 127.0.0.1            -t 5
expect_server_exit_1 "missing -t" -r "111" -a 127.0.0.1 -p "$PORT"
expect_server_exit_1 "no args at all"

echo "Test: server accepts -p 0 (any port) — task.txt explicitly allows this"
PORT=$(get_random_port)
trap stop_server EXIT
"$SERVER_BIN" -r "111" -a 127.0.0.1 -p 0 -t 5 >/dev/null 2>&1 &
SERVER_PID=$!
sleep 0.3
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "  FAIL: server with -p 0 exited immediately"
    SERVER_PID=""
    exit 1
fi
echo "  OK: server with -p 0 is running"

echo "All server_args tests passed."
