#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

# Start server with pawn_row "111" (3 pins)
start_server "111" "$PORT" 10

echo "Test 1: First JOIN creates a game in WAITING state"
run_client "$PORT" "0/42"
assert_exit_code 0
assert_stdout_contains "Game ID"
assert_stdout_contains "Player A ID: 42"
assert_stdout_contains "Status: 0"

echo "Test 2: Second JOIN starts the game (status TURN_B = 2)"
run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "Game ID"
assert_stdout_contains "Player A ID: 42"
assert_stdout_contains "Player B ID: 99"
assert_stdout_contains "Status: 2"

echo "All join tests passed."
