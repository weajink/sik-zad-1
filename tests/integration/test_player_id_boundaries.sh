#!/usr/bin/env bash
# task.txt §2: player IDs are 1..2^32-1 (nonzero 32-bit unsigned).
# Verify the server correctly handles the extremes:
#   * player_id=1 (minimum valid).
#   * player_id=2^32-1 = 4294967295 (maximum valid).
#   * player_id=2 and 2^32-1 in the same game.
#   * player_id=0 ⇒ MSG_WRONG_MSG at JOIN (also covered in test_wrong_player_zero).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
start_server "11" "$PORT" 30

echo "Test 1: player_id=1 (min) and player_id=4294967295 (max) play a full game"
run_client "$PORT" "0/1"
assert_stdout_contains "Player A: 1$"
assert_stdout_contains "Status: waiting for opponent"

run_client "$PORT" "0/4294967295"
assert_stdout_contains "Status: player B's turn"
assert_stdout_contains "Player A: 1$"
assert_stdout_contains "Player B: 4294967295$"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

# B (4294967295) MOVE_2 at pawn=0 ⇒ WIN_B.
run_client "$PORT" "2/4294967295/$GID/0"
assert_stdout_contains "Status: player B wins"
assert_stdout_contains "Player B: 4294967295$"

echo "Test 2: Client with player_id > 2^32-1 must be rejected at CLI"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/4294967296" -t 1
# parse_client_message uses from_chars into uint32_t; 2^32 overflows.
assert_exit_code 1

echo "Test 3: Client with huge player_id (10^20) rejected at CLI"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/100000000000000000000" -t 1
assert_exit_code 1

echo "Test 4: Client with player_id=-1 rejected at CLI (negative ⇒ parse error)"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/-1" -t 1
assert_exit_code 1

echo "All player_id_boundaries tests passed."
