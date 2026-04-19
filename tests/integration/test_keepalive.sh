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
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)
SAVED_STDOUT="$CLIENT_STDOUT"

# Send KEEP_ALIVE from player B (99)
# Format: 3/player_id/game_id
run_client "$PORT" "3/99/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "player_a=42 "
assert_stdout_contains "player_b=99"

# Send KEEP_ALIVE from player A (42) too
run_client "$PORT" "3/42/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"

echo "All keepalive tests passed."
