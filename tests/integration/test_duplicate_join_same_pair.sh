#!/usr/bin/env bash
# Additional JOIN edge cases:
#   * Two JOINs from the same player_id when no WAITING game exists ⇒ player
#     acts as both A and B in a single game (already covered by
#     test_same_player.sh, but we also verify the bitmap and subsequent
#     play).
#   * After a game reaches TURN_B, a duplicate JOIN from player A (via raw
#     UDP to bypass client-side validation) creates a NEW game, NOT re-joins
#     the existing one. This is because "JOIN" just asks to join a WAITING
#     slot; since there's no WAITING game, a new one is created.
#   * A JOIN after the previous game reaches a final state (WIN_X) also
#     creates a new WAITING game.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT

# -------------------------------------------------------------------------
# Test 1: Duplicate JOIN after game is in TURN_B creates a new WAITING game.
# -------------------------------------------------------------------------
echo "Test 1: JOIN from same player after pairing creates a NEW WAITING game"
PORT=$(get_random_port)
start_server "1111" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
assert_stdout_contains "status=TURN_B"
GID1=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

# Player 1 (already playing as A in game 1) sends JOIN again.
# No WAITING game exists right now, so this creates a new WAITING game with
# player 1 as A.
run_client "$PORT" "0/1"
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
assert_stdout_contains "player_a=1"
assert_stdout_contains "player_b=0"
GID2=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
if [[ "$GID2" == "$GID1" ]]; then
    echo "  FAIL: duplicate JOIN must create a new game, got same game_id=$GID1"
    exit 1
fi

echo "Test 2: Player 1 can then play in both games independently"
# KA on game 1 — still TURN_B.
run_client "$PORT" "3/1/$GID1"
assert_stdout_contains "status=TURN_B"
if ! echo "$CLIENT_STDOUT" | grep -qE "game_id=$GID1\b"; then
    echo "  FAIL: wrong game in KA response"
    exit 1
fi

# KA on game 2 — still WAITING.
run_client "$PORT" "3/1/$GID2"
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
if ! echo "$CLIENT_STDOUT" | grep -qE "game_id=$GID2\b"; then
    echo "  FAIL: wrong game in KA response"
    exit 1
fi

# -------------------------------------------------------------------------
# Test 3: JOIN into the (now only) WAITING game from player 2.
# Player 2 is already B in game 1, but can also be B in game 2.
# -------------------------------------------------------------------------
echo "Test 3: Player 2 joins game 2 as B (distinct from game 1)"
run_client "$PORT" "0/2"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "player_a=1"
assert_stdout_contains "player_b=2"
# Must be game 2 (game 1 was already paired).
if ! echo "$CLIENT_STDOUT" | grep -qE "game_id=$GID2\b"; then
    echo "  FAIL: expected game_id=$GID2 in second pairing"
    echo "  STDOUT: $CLIENT_STDOUT"
    exit 1
fi

# -------------------------------------------------------------------------
# Test 4: JOIN after previous game ends ⇒ creates a new WAITING game.
# -------------------------------------------------------------------------
echo "Test 4: JOIN after prior game ends creates new WAITING game"
stop_server
PORT=$(get_random_port)
start_server "11" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
# B wins immediately.
run_client "$PORT" "2/2/$GID/0"
assert_stdout_contains "status=WIN_B"

# New JOIN from player 3 — must create a new WAITING game.
run_client "$PORT" "0/3"
assert_stdout_contains "status=WAITING_FOR_OPPONENT"
assert_stdout_contains "player_a=3"
NEW_GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
if [[ "$NEW_GID" == "$GID" ]]; then
    echo "  FAIL: new game must have distinct ID from finished game"
    exit 1
fi

echo "All duplicate_join_same_pair tests passed."
