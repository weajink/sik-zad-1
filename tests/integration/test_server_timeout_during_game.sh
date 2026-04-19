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

# A must keep its own clock fresh so it isn't the one timing out; otherwise
# both sides are silent and the tie goes to whoever was silent first (A).
# We send a KEEP_ALIVE from A before B's timeout would be reached.
sleep 1.2
run_client "$PORT" "3/42/$GID"
sleep 1.2

# Now B has been silent for ~2.4s (> 2s), A has been silent for ~1.2s (< 2s).
# A's final KEEP_ALIVE should trigger the server's timeout check; since A is
# still fresh and B is stale, B loses → WIN_A.
run_client "$PORT" "3/42/$GID"
assert_stdout_contains "status=WIN_A"

echo "All server_timeout_during_game tests passed."
