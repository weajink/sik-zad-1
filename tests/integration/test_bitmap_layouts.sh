#!/usr/bin/env bash
# Initial pin-row corner cases: max_pawn=0 (single pin), non-contiguous
# starting layout, and the MOVE_1 single-pin win condition.
# task.txt §3.1, §1.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)
trap stop_server EXIT

echo "Test 1: pawn_row \"1\" (single pin, max_pawn=0) — MOVE_1 by B wins immediately"
start_server "1" "$PORT" 30
run_client "$PORT" "0/1"
assert_exit_code 0
assert_stdout_contains "max_pawn=0"

run_client "$PORT" "0/2"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

# B (id=2) knocks the only pin via MOVE_1.
run_client "$PORT" "1/2/$GAME_ID/0"
assert_exit_code 0
assert_stdout_contains "status=WIN_B"  # WIN_B

echo "Test 2: pawn_row \"1\" — MOVE_2 at pawn 0 is illegal (pawn+1 doesn't exist)"
stop_server
PORT=$(get_random_port)
start_server "1" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

run_client "$PORT" "2/2/$GAME_ID/0"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"        # still B's turn
assert_stdout_contains "pawn_row=1"      # pin still there (or "10000000" — be tolerant)

echo "Test 3: non-contiguous layout \"10101\" — MOVE_2 at pawn 0 illegal (pin 1 missing)"
stop_server
PORT=$(get_random_port)
start_server "10101" "$PORT" 30

run_client "$PORT" "0/1"
assert_stdout_contains "max_pawn=4"

run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)
assert_stdout_contains "status=TURN_B"

# B tries MOVE_2 at pawn 0 — pin 1 is already empty in initial layout. Illegal.
run_client "$PORT" "2/2/$GAME_ID/0"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"

# B tries MOVE_2 at pawn 1 — pin 1 itself is empty. Illegal.
run_client "$PORT" "2/2/$GAME_ID/1"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"

# B legally knocks pin 0 (MOVE_1).
run_client "$PORT" "1/2/$GAME_ID/0"
assert_exit_code 0
assert_stdout_contains "status=TURN_A"

# A legally knocks pin 2 (MOVE_1).
run_client "$PORT" "1/1/$GAME_ID/2"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"

# B legally knocks pin 4 (MOVE_1) → only pin 4 was the last? No — pin 4 was
# the rightmost '1' in "10101". After knocking 0 and 2, pins remaining are
# {4}. Knocking 4 wins.
run_client "$PORT" "1/2/$GAME_ID/4"
assert_exit_code 0
assert_stdout_contains "status=WIN_B"        # WIN_B

echo "Test 4: max-size pawn_row (256 chars) — game state advertises max_pawn=255"
stop_server
PORT=$(get_random_port)
LONG=$(printf '1%.0s' {1..256})
start_server "$LONG" "$PORT" 30

run_client "$PORT" "0/1"
assert_exit_code 0
assert_stdout_contains "max_pawn=255"

echo "All bitmap_layouts tests passed."
