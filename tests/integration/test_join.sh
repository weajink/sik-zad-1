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
assert_stdout_contains "Game "
assert_stdout_contains "Player A: 42$"
assert_stdout_contains "Status: waiting for opponent"

echo "Test 2: Second JOIN starts the game (status TURN_B = 2)"
run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "Game "
assert_stdout_contains "Player A: 42$"
assert_stdout_contains "Player B: 99$"
assert_stdout_contains "Status: player B's turn"

echo "All join tests passed."
