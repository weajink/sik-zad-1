#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

# Start server with pawn_row "11" (2 pins, indices 0 and 1)
start_server "11" "$PORT" 10

echo "Test: Two JOINs, then Player B knocks both pins -> WIN_B"

# First JOIN
run_client "$PORT" "0/42"
assert_exit_code 0
assert_stdout_contains "status=WAITING_FOR_OPPONENT"

# Second JOIN - game starts, get game_id
run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
# Extract game_id from output
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)
echo "  Game ID: $GAME_ID"

# Player B (99) makes MOVE_2 at pawn 0 (knocks pawn 0 and 1)
# Message format: 2/player_id/game_id/pawn
run_client "$PORT" "2/99/$GAME_ID/0"
assert_exit_code 0
# Should be WIN_B (status 4) since all pins knocked down
assert_stdout_contains "status=WIN_B"

echo "All move tests passed."
