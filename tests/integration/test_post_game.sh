#!/usr/bin/env bash
# task.txt §5.3: "Serwer utrzymuje stan zakończonej rozgrywki przez
# server_timeout sekund od momentu otrzymania poprawnego komunikatu od
# któregoś z graczy, którzy uczestniczyli w tej rozgrywce."
#
# Verify: after WIN_X, both players can still query the game's final state
# via KEEP_ALIVE within the server_timeout window.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)
trap stop_server EXIT

# Use timeout=10 so the test has plenty of headroom.
start_server "11" "$PORT" 10

# Set up a quick game: A=42, B=99, B knocks both pins → WIN_B.
run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "Game [0-9]+" | head -1 | awk '{print $2}')

run_client "$PORT" "2/99/$GAME_ID/0"
assert_stdout_contains "Status: player B wins"  # WIN_B

echo "Test 1: KEEP_ALIVE from B (winner) on finished game still returns WIN_B"
run_client "$PORT" "3/99/$GAME_ID"
assert_exit_code 0
assert_stdout_not_contains "Server rejected the message"
assert_stdout_contains "Status: player B wins"
assert_stdout_contains "Player A: 42$"
assert_stdout_contains "Player B: 99$"

echo "Test 2: KEEP_ALIVE from A (loser) on finished game also returns WIN_B"
run_client "$PORT" "3/42/$GAME_ID"
assert_exit_code 0
assert_stdout_not_contains "Server rejected the message"
assert_stdout_contains "Status: player B wins"

echo "Test 3: KEEP_ALIVE from non-participant on finished game gets WRONG_MSG"
# Player 12345 was never in this game.
run_client "$PORT" "3/12345/$GAME_ID"
assert_exit_code 0
assert_stdout_contains "Server rejected the message"

echo "All post_game tests passed."
