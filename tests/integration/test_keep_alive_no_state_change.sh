#!/usr/bin/env bash
# MSG_KEEP_ALIVE must never change game state (other than the retention
# clock). Verify:
#   * Multiple KEEP_ALIVEs during TURN_B leave status=TURN_B and pawn_row
#     unchanged.
#   * KEEP_ALIVE does NOT flip the turn.
#   * KEEP_ALIVE on a WAITING game returns WAITING (not TURN_B).
#   * KEEP_ALIVE on a finished game returns the same WIN_X.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT

echo "Test 1: Multiple KEEP_ALIVEs during TURN_B leave everything unchanged"
PORT=$(get_random_port)
start_server "1111" "$PORT" 60

run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
assert_stdout_contains "Status: player B's turn"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

# 5 KEEP_ALIVEs, alternating between A and B.
for i in 1 2 3 4 5; do
    if ((i % 2 == 0)); then
        run_client "$PORT" "3/42/$GID"
    else
        run_client "$PORT" "3/99/$GID"
    fi
    assert_stdout_contains "Status: player B's turn"
    assert_stdout_contains "Pawns: ####"
    assert_stdout_contains "Player A: 42$"
    assert_stdout_contains "Player B: 99$"
done

echo "Test 2: KEEP_ALIVE during WAITING returns WAITING (no side effects)"
stop_server
PORT=$(get_random_port)
start_server "1111" "$PORT" 30

run_client "$PORT" "0/42"
assert_stdout_contains "Status: waiting for opponent"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

run_client "$PORT" "3/42/$GID"
assert_stdout_contains "Status: waiting for opponent"
assert_stdout_contains "Player A: 42$"
assert_stdout_contains "Player B: <none>"
assert_stdout_contains "Pawns: ####"

echo "Test 3: KEEP_ALIVE after a real move doesn't un-do the move"
# B knocks pin 0.
run_client "$PORT" "0/99"
assert_stdout_contains "Status: player B's turn"
run_client "$PORT" "1/99/$GID/0"
assert_stdout_contains "Status: player A's turn"
assert_stdout_contains "Pawns: \.###"

# KA from A — state must remain TURN_A with pawn_row=0111.
run_client "$PORT" "3/42/$GID"
assert_stdout_contains "Status: player A's turn"
assert_stdout_contains "Pawns: \.###"

# KA from B — same.
run_client "$PORT" "3/99/$GID"
assert_stdout_contains "Status: player A's turn"
assert_stdout_contains "Pawns: \.###"

echo "Test 4: KEEP_ALIVE after WIN doesn't flip/reset the winner"
stop_server
PORT=$(get_random_port)
start_server "11" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
run_client "$PORT" "2/2/$GID/0"
assert_stdout_contains "Status: player B wins"

# Multiple KAs from both players on finished game.
for i in 1 2 3; do
    run_client "$PORT" "3/1/$GID"
    assert_stdout_contains "Status: player B wins"
    run_client "$PORT" "3/2/$GID"
    assert_stdout_contains "Status: player B wins"
done

echo "All keep_alive_no_state_change tests passed."
