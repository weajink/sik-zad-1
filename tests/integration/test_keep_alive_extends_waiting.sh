#!/usr/bin/env bash
# task.txt §5.1: "Jeśli upłynie limit czasu server_timeout, gracz A nie
# przyśle MSG_KEEP_ALIVE i nie pojawi się MSG_JOIN od gracza B, rozgrywka
# jest kasowana."
#
# Conversely: if player A DOES send MSG_KEEP_ALIVE within server_timeout,
# the WAITING game is preserved. We verify this by:
#   1. Starting a 2-second timeout server.
#   2. Joining as A.
#   3. Sleeping 1.2s (less than timeout).
#   4. Sending KEEP_ALIVE from A.
#   5. Sleeping another 1.2s (so total > 2s since JOIN, but < 2s since KA).
#   6. Verifying a follow-up JOIN from B pairs with the still-WAITING game.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
# 2-second server timeout.
start_server "111" "$PORT" 2

echo "Test 1: A joins ⇒ WAITING game created"
run_client "$PORT" "0/42"
assert_exit_code 0
assert_stdout_contains "Status: waiting for opponent"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

echo "Test 2: Sleep 1.2s (< 2s timeout), then A sends KEEP_ALIVE"
sleep 1.2
run_client "$PORT" "3/42/$GAME_ID"
assert_exit_code 0
# KEEP_ALIVE should return the same WAITING game state.
assert_stdout_contains "Status: waiting for opponent"
if ! echo "$CLIENT_STDOUT" | grep -qE "Game $GAME_ID\b"; then
    echo "  FAIL: KEEP_ALIVE did not return game_id=$GAME_ID"
    echo "  STDOUT: $CLIENT_STDOUT"
    exit 1
fi

echo "Test 3: Sleep another 1.2s (total >2s since JOIN but <2s since KA); game still alive"
sleep 1.2
# B joins. If KEEP_ALIVE kept the game alive, B should pair with A. If the
# game was incorrectly discarded, B would create a new WAITING game instead.
run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "Status: player B's turn"
assert_stdout_contains "Player A: 42$"
assert_stdout_contains "Player B: 99$"
NEW_GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
if [[ "$NEW_GAME_ID" != "$GAME_ID" ]]; then
    echo "  FAIL: expected B to pair with game_id=$GAME_ID, got $NEW_GAME_ID"
    exit 1
fi

echo "All keep_alive_extends_waiting tests passed."
