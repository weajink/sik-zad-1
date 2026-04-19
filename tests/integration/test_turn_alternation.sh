#!/usr/bin/env bash
# task.txt §1: "Gracze wykonują ruchy na przemian."
# §5.2: implicit — turn flips after a legal move.
#
# Verify the turn strictly alternates TURN_B → TURN_A → TURN_B → ... on every
# legal move, and that illegal moves do NOT flip the turn.
#
# Also verify that after MOVE_2, the turn still flips (not double-flip).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
start_server "11111111" "$PORT" 60  # 8 pins for lots of alternation

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

echo "Move 1: B MOVE_1 pawn=0 ⇒ TURN_A"
run_client "$PORT" "1/2/$GAME_ID/0"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=01111111"

echo "Move 2: A MOVE_1 pawn=1 ⇒ TURN_B"
run_client "$PORT" "1/1/$GAME_ID/1"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=00111111"

echo "Illegal (B tries pawn=0 again) — turn must NOT flip"
run_client "$PORT" "1/2/$GAME_ID/0"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=00111111"

echo "Move 3: B MOVE_2 pawn=2 ⇒ TURN_A (not TURN_B again — single flip)"
run_client "$PORT" "2/2/$GAME_ID/2"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=00001111"

echo "Illegal by A (GIVE_UP? no — let's try MOVE_1 pawn=99 out of range)"
run_client "$PORT" "1/1/$GAME_ID/99"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=00001111"

echo "Move 4: A MOVE_1 pawn=4 ⇒ TURN_B"
run_client "$PORT" "1/1/$GAME_ID/4"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=00000111"

echo "Move 5: B MOVE_1 pawn=5 ⇒ TURN_A"
run_client "$PORT" "1/2/$GAME_ID/5"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=00000011"

echo "Move 6: A MOVE_2 pawn=6 ⇒ WIN_A (knocks last two pins)"
run_client "$PORT" "2/1/$GAME_ID/6"
assert_stdout_contains "status=WIN_A"
assert_stdout_contains "pawn_row=00000000"

echo "After WIN, next MOVE_1 from A must NOT change status (still WIN_A)"
run_client "$PORT" "1/1/$GAME_ID/0"
assert_stdout_contains "status=WIN_A"

echo "After WIN, next MOVE_1 from B must NOT change status"
run_client "$PORT" "1/2/$GAME_ID/0"
assert_stdout_contains "status=WIN_A"

echo "All turn_alternation tests passed."
