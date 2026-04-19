#!/usr/bin/env bash
# task.txt §5.3 bullet 3: "któryś z graczy nie przysłał żadnego poprawnego
# komunikatu z identyfikatorem tej rozgrywki przez server_timeout – ten
# gracz przegrywa."
#
# Verify: during a TURN_X game, if one player goes silent for longer than
# server_timeout, they LOSE. The game transitions to WIN for the OTHER
# player.
#
# We establish the game, then the timing-out player stops sending messages
# while the other keeps sending KEEP_ALIVEs. After server_timeout elapses
# relative to the silent player's last valid message, the next arriving
# message should cause the server to mark the silent player as loser.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
# 2-second server timeout.
start_server "1111" "$PORT" 2

echo "Test: B joins, then B stops sending messages for > 2s ⇒ B loses ⇒ WIN_A"
run_client "$PORT" "0/42"
run_client "$PORT" "0/99"
assert_stdout_contains "status=TURN_B"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

# A keeps sending KEEP_ALIVEs; B does not.
# Sleep 2.5s to let B's timeout expire.
sleep 2.5

# A sends KEEP_ALIVE — this should trigger the server's timeout check and
# transition the game to WIN_A (because B timed out).
run_client "$PORT" "3/42/$GID"
# Expected: status=WIN_A. If the server incorrectly marked A as the loser
# (because A didn't send messages either), we'd see WIN_B — which would be
# wrong because A just sent the KA we received on.
assert_stdout_contains "status=WIN_A"

echo "All server_timeout_during_game tests passed."
