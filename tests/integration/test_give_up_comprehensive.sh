#!/usr/bin/env bash
# MSG_GIVE_UP edge cases (task.txt §3.3, §5.3):
#   * GIVE_UP by the player whose turn it is ⇒ opponent wins.
#   * GIVE_UP by the player whose turn it is NOT ⇒ illegal (state unchanged).
#   * GIVE_UP after game ended ⇒ illegal (state unchanged).
#   * GIVE_UP on a WAITING game ⇒ illegal (no turn to give up; state unchanged).
#   * GIVE_UP from a player not in the game ⇒ WRONG_MSG.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT

# -------------------------------------------------------------------------
# Test 1: B gives up on B's turn ⇒ WIN_A (spec: §5.3 bullet 2)
# -------------------------------------------------------------------------
echo "Test 1: B gives up on TURN_B ⇒ WIN_A"
PORT=$(get_random_port)
start_server "111" "$PORT" 30

run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

run_client "$PORT" "4/99/$GAME_ID"
assert_stdout_contains "status=WIN_A"

# -------------------------------------------------------------------------
# Test 2: A gives up on A's turn ⇒ WIN_B
# Need to get to TURN_A first: B makes a legal MOVE_1.
# -------------------------------------------------------------------------
echo "Test 2: A gives up on TURN_A ⇒ WIN_B"
stop_server
PORT=$(get_random_port)
start_server "111" "$PORT" 30

run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

# B knocks pin 0 ⇒ TURN_A.
run_client "$PORT" "1/99/$GAME_ID/0"
assert_stdout_contains "status=TURN_A"

# A gives up on TURN_A ⇒ WIN_B.
run_client "$PORT" "4/42/$GAME_ID"
assert_stdout_contains "status=WIN_B"

# -------------------------------------------------------------------------
# Test 3: A tries to GIVE_UP on B's turn ⇒ illegal, state unchanged.
# -------------------------------------------------------------------------
echo "Test 3: GIVE_UP from non-active player is illegal (state unchanged)"
stop_server
PORT=$(get_random_port)
start_server "111" "$PORT" 30

run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

# A (not on turn) tries GIVE_UP.
run_client "$PORT" "4/42/$GAME_ID"
# Still TURN_B — no change.
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=111"
# Must not be a WRONG_MSG; A is a valid participant.
assert_stdout_not_contains "MessageWrong"
assert_stdout_not_contains "status=255"

# -------------------------------------------------------------------------
# Test 4: GIVE_UP on a WAITING game (no TURN_X yet) from player A.
# There is no active turn (status=WAITING_FOR_OPPONENT), so GIVE_UP should
# NOT end the game.
# -------------------------------------------------------------------------
echo "Test 4: GIVE_UP during WAITING is illegal (state unchanged)"
stop_server
PORT=$(get_random_port)
start_server "111" "$PORT" 30

run_client "$PORT" "0/42"
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

run_client "$PORT" "4/42/$GAME_ID"
# Still WAITING — GIVE_UP is illegal here; server returns unchanged state.
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
assert_stdout_not_contains "status=255"
assert_stdout_not_contains "status=WIN_"

# -------------------------------------------------------------------------
# Test 5: GIVE_UP after WIN is illegal (state unchanged).
# -------------------------------------------------------------------------
echo "Test 5: GIVE_UP after WIN is illegal (state unchanged)"
stop_server
PORT=$(get_random_port)
start_server "11" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
# B wins immediately via MOVE_2.
run_client "$PORT" "2/2/$GAME_ID/0"
assert_stdout_contains "status=WIN_B"

# B tries to GIVE_UP after WIN_B ⇒ state stays WIN_B.
run_client "$PORT" "4/2/$GAME_ID"
assert_stdout_contains "status=WIN_B"
# A tries to GIVE_UP after WIN_B ⇒ still WIN_B.
run_client "$PORT" "4/1/$GAME_ID"
assert_stdout_contains "status=WIN_B"

# -------------------------------------------------------------------------
# Test 6: GIVE_UP from a player NOT in the game ⇒ MSG_WRONG_MSG.
# -------------------------------------------------------------------------
echo "Test 6: GIVE_UP from non-participant ⇒ WRONG_MSG"
stop_server
PORT=$(get_random_port)
start_server "111" "$PORT" 30

run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

run_client "$PORT" "4/12345/$GAME_ID"
assert_stdout_contains "MessageWrong"
assert_stdout_contains "status=255"

echo "All give_up_comprehensive tests passed."
