#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

start_server "111" "$PORT" 10

echo "Test: Player B gives up -> WIN_A (status 3)"

# Create a game
run_client "$PORT" "0/42"
assert_exit_code 0

run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

# Player B (99) gives up
# Format: 4/player_id/game_id
run_client "$PORT" "4/99/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "status=WIN_A"

echo "All give_up tests passed."
