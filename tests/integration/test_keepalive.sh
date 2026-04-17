#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

start_server "111" "$PORT" 10

echo "Test: KEEP_ALIVE returns unchanged game state"

# Create a game with two joins
run_client "$PORT" "0/42"
assert_exit_code 0

run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "Status: 2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep "Game ID:" | head -1 | awk '{print $3}')
SAVED_STDOUT="$CLIENT_STDOUT"

# Send KEEP_ALIVE from player B (99)
# Format: 3/player_id/game_id
run_client "$PORT" "3/99/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "Status: 2"
assert_stdout_contains "Player A ID: 42"
assert_stdout_contains "Player B ID: 99"

# Send KEEP_ALIVE from player A (42) too
run_client "$PORT" "3/42/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "Status: 2"

echo "All keepalive tests passed."
