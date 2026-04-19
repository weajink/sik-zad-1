#!/usr/bin/env bash
# task.txt §5.3: "Serwer utrzymuje stan zakończonej rozgrywki przez
# server_timeout sekund od momentu otrzymania poprawnego komunikatu od
# któregoś z graczy, którzy uczestniczyli w tej rozgrywce."
#
# We verify:
#   * Immediately after WIN_X, KEEP_ALIVE returns the final state.
#   * After sleeping less than server_timeout, state is still available.
#   * After sleeping more than server_timeout since the last valid message
#     from any participant, the game must be gone (WRONG_MSG on access).
#   * KEEP_ALIVE from a participant on a finished game resets the retention
#     clock (as implied by §5.3 wording "from receipt of a valid message").
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT

# -------------------------------------------------------------------------
# Test 1: After WIN, still accessible within timeout window.
# -------------------------------------------------------------------------
echo "Test 1: Immediately after WIN_B, KEEP_ALIVE returns WIN_B"
PORT=$(get_random_port)
start_server "11" "$PORT" 3

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
run_client "$PORT" "2/2/$GAME_ID/0"
assert_stdout_contains "Status: player B wins"

run_client "$PORT" "3/1/$GAME_ID"
assert_stdout_contains "Status: player B wins"

# -------------------------------------------------------------------------
# Test 2: After sleeping > server_timeout since the LAST valid message (no
# KEEP_ALIVEs refreshing it), the finished game must be discarded.
# -------------------------------------------------------------------------
echo "Test 2: After server_timeout, finished game is purged"
stop_server
PORT=$(get_random_port)
# 1-second server timeout — smallest allowed.
start_server "11" "$PORT" 1

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
run_client "$PORT" "2/2/$GAME_ID/0"
assert_stdout_contains "Status: player B wins"

# Sleep > 1s; game should be reaped.
sleep 2.5

# Probe for the game. The next valid client message triggers the reaper.
# KEEP_ALIVE from a participant on a purged game ⇒ WRONG_MSG (invalid_game_id).
run_client "$PORT" "3/1/$GAME_ID"
assert_stdout_contains "Server rejected the message"

# Also from the winner.
run_client "$PORT" "3/2/$GAME_ID"
assert_stdout_contains "Server rejected the message"

# -------------------------------------------------------------------------
# Test 3: KEEP_ALIVE refreshes the retention window.
# Start timeout=2s. Win the game. Sleep 1.2s. KEEP_ALIVE ⇒ still alive.
# Sleep another 1.2s (total >2s since original WIN, but <2s since KA) ⇒
# game must still be alive.
# -------------------------------------------------------------------------
echo "Test 3: KEEP_ALIVE on finished game refreshes retention"
stop_server
PORT=$(get_random_port)
start_server "11" "$PORT" 2

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
run_client "$PORT" "2/2/$GAME_ID/0"
assert_stdout_contains "Status: player B wins"

sleep 1.2
run_client "$PORT" "3/1/$GAME_ID"
assert_stdout_contains "Status: player B wins"

sleep 1.2
# Total sleep since WIN: ~2.4s + RTT, but only ~1.2s since last KA.
run_client "$PORT" "3/2/$GAME_ID"
assert_stdout_contains "Status: player B wins"

echo "All finished_game_retention tests passed."
