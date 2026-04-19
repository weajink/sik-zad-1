#!/usr/bin/env bash
# task.txt §5.1: "W danej chwili jest co najwyżej jedna rozgrywka w stanie
# WAITING_FOR_OPPONENT."
#
# Verify: when three distinct players JOIN in sequence, the 3rd JOIN must NOT
# create a second WAITING game. Instead, the 2nd JOIN pairs with player A, and
# the 3rd JOIN creates a brand-new WAITING game (since there's no more waiting
# game after pairing).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
start_server "1111" "$PORT" 30

echo "Test 1: A joins ⇒ status=WAITING_FOR_OPPONENT"
run_client "$PORT" "0/11"
assert_exit_code 0
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
assert_stdout_contains "player_a=11"
assert_stdout_contains "player_b=0"
GAME_ID_1=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

echo "Test 2: B joins ⇒ game_1 becomes TURN_B (not a second WAITING game)"
run_client "$PORT" "0/22"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "player_a=11"
assert_stdout_contains "player_b=22"
GAME_ID_2=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
if [[ "$GAME_ID_2" != "$GAME_ID_1" ]]; then
    echo "  FAIL: second JOIN should have paired with game $GAME_ID_1, got $GAME_ID_2"
    exit 1
fi

echo "Test 3: Third player joins ⇒ creates a NEW WAITING game (distinct game_id)"
run_client "$PORT" "0/33"
assert_exit_code 0
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
assert_stdout_contains "player_a=33"
assert_stdout_contains "player_b=0"
GAME_ID_3=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
if [[ "$GAME_ID_3" == "$GAME_ID_1" ]]; then
    echo "  FAIL: third JOIN should create a new game_id, not reuse $GAME_ID_1"
    exit 1
fi

echo "Test 4: 4th player joins the 3rd player ⇒ pairs with game 3, status TURN_B"
run_client "$PORT" "0/44"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "player_a=33"
assert_stdout_contains "player_b=44"
GAME_ID_4=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
if [[ "$GAME_ID_4" != "$GAME_ID_3" ]]; then
    echo "  FAIL: 4th JOIN should have paired with game $GAME_ID_3, got $GAME_ID_4"
    exit 1
fi

echo "Test 5: Third pair creates a third distinct game"
run_client "$PORT" "0/55"
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
GAME_ID_5=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
if [[ "$GAME_ID_5" == "$GAME_ID_1" || "$GAME_ID_5" == "$GAME_ID_3" ]]; then
    echo "  FAIL: 5th JOIN reused an existing game_id ($GAME_ID_5)"
    exit 1
fi

echo "All at_most_one_waiting tests passed."
