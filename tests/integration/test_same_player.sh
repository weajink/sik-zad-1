#!/usr/bin/env bash
# task.txt §2:
#   "Gracz może grać jednocześnie w wielu rozgrywkach."
#   "Gracz może grać jako obaj gracze w rozgrywce."
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

start_server "1111" "$PORT" 30

echo "Test 1: Same player_id joins both sides of one game"
# Player 7 sends two JOINs. Result should be A=7, B=7, status TURN_B.
run_client "$PORT" "0/7"
assert_exit_code 0
assert_stdout_contains "player_a=7 "
assert_stdout_contains "status=WAITING_FOR_OPPONENT"

run_client "$PORT" "0/7"
assert_exit_code 0
assert_stdout_contains "player_a=7 "
assert_stdout_contains "player_b=7"
assert_stdout_contains "status=TURN_B"
SAME_GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

echo "Test 2: Player 7 (acting as B) makes a legal move in the same-player game"
# B's turn — knock pin 0.
run_client "$PORT" "1/7/$SAME_GAME_ID/0"
assert_exit_code 0
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=0111"

# Now A's turn — same player_id.
run_client "$PORT" "1/7/$SAME_GAME_ID/1"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=0011"

echo "Test 3: Same player can join multiple distinct games"
# Player 7 starts a NEW game (was already A and B in SAME_GAME_ID). Since
# no WAITING game exists right now, this creates one.
run_client "$PORT" "0/7"
assert_exit_code 0
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
GAME_2_ID_WAIT=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)
if [[ "$GAME_2_ID_WAIT" == "$SAME_GAME_ID" ]]; then
    echo "  FAIL: expected new game_id, got same as $SAME_GAME_ID"
    exit 1
fi

# Player 8 joins as B → game 2 starts.
run_client "$PORT" "0/8"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "player_a=7 "
assert_stdout_contains "player_b=8"
GAME_2_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)
if [[ "$GAME_2_ID" != "$GAME_2_ID_WAIT" ]]; then
    echo "  FAIL: game_id changed between WAITING and TURN_B"
    exit 1
fi

echo "Test 4: Player 7 still has SAME_GAME_ID active and game 2 active simultaneously"
# KEEP_ALIVE on the original game returns its (still-in-progress) state.
run_client "$PORT" "3/7/$SAME_GAME_ID"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=0011"

# KEEP_ALIVE on game 2 returns its (just-started) state.
run_client "$PORT" "3/7/$GAME_2_ID"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=1111"

echo "Test 5: Player 8 (in game 2 only) cannot send KEEP_ALIVE for SAME_GAME_ID"
# Player 8 is not in SAME_GAME_ID; should get MSG_WRONG_MSG.
run_client "$PORT" "3/8/$SAME_GAME_ID"
assert_exit_code 0
assert_stdout_contains "status=255"

echo "All same_player tests passed."
