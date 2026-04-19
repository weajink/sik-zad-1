#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

start_server "111" "$PORT" 10

echo "Test 1: MOVE with non-existent game_id gets MSG_WRONG_MSG"
# Player 42 tries MOVE_1 on game 99999 which doesn't exist
# First need to join so the player_id is known
run_client "$PORT" "0/42"
assert_exit_code 0

# Now try a move on a game that doesn't exist
run_client "$PORT" "1/42/99999/0"
assert_exit_code 0
assert_stdout_contains "status=255"
assert_stdout_contains "MessageWrong"

echo "Test 2: KEEP_ALIVE with unknown player for existing game gets MSG_WRONG_MSG"
# Extract game_id from the join
run_client "$PORT" "0/42"
assert_exit_code 0
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)
# Player 9999 is not in this game
run_client "$PORT" "3/9999/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "status=255"

echo "All wrong message tests passed."
