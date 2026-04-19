#!/usr/bin/env bash
# task.txt §5.1: "Jeśli upłynie limit czasu server_timeout, gracz A nie
# przyśle MSG_KEEP_ALIVE i nie pojawi się MSG_JOIN od gracza B, rozgrywka
# jest kasowana."
#
# Verify: a WAITING game is discarded after server_timeout. The next JOIN
# after timeout creates a brand-new game (different game_id), not a
# continuation of the discarded one.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)
trap stop_server EXIT

# Minimum allowed server_timeout = 1s. Keep test snappy.
start_server "111" "$PORT" 1

echo "Test 1: A joins → WAITING game created"
run_client "$PORT" "0/42"
assert_exit_code 0
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
assert_stdout_contains "player_a=42 "
FIRST_GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

echo "Test 2: After server_timeout, A's WAITING game is discarded"
# Sleep > server_timeout to let A's slot expire.
sleep 2

# Next JOIN from a different player should create a NEW WAITING game,
# NOT join the (now-discarded) one as B.
run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "status=WAITING_FOR_OPPONENT"        # WAITING again — not TURN_B
assert_stdout_contains "player_a=99 "  # 99 is the new A, not the new B
NEW_GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

if [[ "$NEW_GAME_ID" == "$FIRST_GAME_ID" ]]; then
    echo "  FAIL: new game reused discarded game_id $FIRST_GAME_ID"
    exit 1
fi

echo "Test 3: KEEP_ALIVE on the discarded game returns WRONG_MSG"
# The first game no longer exists.
run_client "$PORT" "3/42/$FIRST_GAME_ID"
assert_exit_code 0
assert_stdout_contains "status=255"

echo "All waiting_timeout tests passed."
