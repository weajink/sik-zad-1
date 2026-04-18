#!/usr/bin/env bash
# Server -r pawn_row validation rules from task.txt §4:
#   * sequence of '0'/'1' only
#   * length 1..256
#   * first and last chars must be '1'
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

# Run server in foreground and capture exit code (for CLI-validation tests
# that expect immediate exit). Returns 0 on expected failure.
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

echo "Test: server rejects invalid pawn_row strings"
expect_server_exit_1 "empty"            -r ""              -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "leading zero"     -r "0111"          -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "trailing zero"    -r "1110"          -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "both edges zero"  -r "0110"          -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "non 0/1 char"     -r "11211"         -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "letter"           -r "11a11"         -a 127.0.0.1 -p "$PORT" -t 5
expect_server_exit_1 "space inside"     -r "11 11"         -a 127.0.0.1 -p "$PORT" -t 5

# Build a 257-char pawn_row of all 1s (over the 256 limit).
TOO_LONG=$(printf '1%.0s' {1..257})
expect_server_exit_1 "length 257"       -r "$TOO_LONG"     -a 127.0.0.1 -p "$PORT" -t 5

echo "Test: server accepts valid edge-case pawn_row strings"

# Length 1: just "1" (max_pawn=0, a single pin).
trap stop_server EXIT
start_server "1" "$PORT" 5
stop_server

# Length 256: max-size board (max_pawn=255).
PORT=$(get_random_port)
LONG=$(printf '1%.0s' {1..256})
start_server "$LONG" "$PORT" 5
stop_server

echo "All pawn_row validation tests passed."
