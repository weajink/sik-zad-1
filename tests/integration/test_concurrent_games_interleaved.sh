#!/usr/bin/env bash
# Verify the server correctly handles multiple concurrent games with
# interleaved messages (task.txt §2: "Serwer obsługuje symultanicznie wiele
# rozgrywek.").
#
# This is distinct from test_many_players.sh: instead of finishing games one
# after another, we interleave moves across several games to catch any
# state-crosstalk bugs (e.g., if the server incorrectly used the last game
# modified, or mixed up player IDs across game states).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
start_server "1111" "$PORT" 60

declare -a GAMES=()
declare -a PLAYERS_A=()
declare -a PLAYERS_B=()

NUM=4  # 4 concurrent games, 8 distinct players.

echo "Phase 1: Create $NUM games with distinct player IDs"
for ((i = 0; i < NUM; i++)); do
    pa=$((100 + 2*i))
    pb=$((101 + 2*i))
    run_client "$PORT" "0/$pa"
    assert_stdout_contains "status=WAITING_FOR_OPPONENT"
    run_client "$PORT" "0/$pb"
    assert_stdout_contains "status=TURN_B"
    assert_stdout_contains "player_a=$pa"
    assert_stdout_contains "player_b=$pb"
    gid=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
    GAMES+=("$gid")
    PLAYERS_A+=("$pa")
    PLAYERS_B+=("$pb")
done

# All game IDs must be distinct.
uniq_count=$(printf '%s\n' "${GAMES[@]}" | sort -u | wc -l)
if [[ "$uniq_count" -ne "$NUM" ]]; then
    echo "  FAIL: expected $NUM unique game IDs, got $uniq_count (ids: ${GAMES[*]})"
    exit 1
fi

echo "Phase 2: Each game's B player knocks pin 0 (interleaved)"
# Interleave: game0-B, game1-B, game2-B, game3-B, all MOVE_1 pawn=0.
for ((i = 0; i < NUM; i++)); do
    gid="${GAMES[i]}"
    pb="${PLAYERS_B[i]}"
    pa="${PLAYERS_A[i]}"
    run_client "$PORT" "1/$pb/$gid/0"
    assert_stdout_contains "status=TURN_A"
    assert_stdout_contains "player_a=$pa"
    assert_stdout_contains "player_b=$pb"
    assert_stdout_contains "pawn_row=0111"
    # game_id must match the one we queried — server must not mix up games.
    if ! echo "$CLIENT_STDOUT" | grep -qE "game_id=$gid\b"; then
        echo "  FAIL: wrong game_id in response for game $gid"
        echo "  STDOUT: $CLIENT_STDOUT"
        exit 1
    fi
done

echo "Phase 3: Each game's A player MOVE_2 at pawn=2 (knocks pins 2,3); all games end WIN_A"
for ((i = 0; i < NUM; i++)); do
    gid="${GAMES[i]}"
    pa="${PLAYERS_A[i]}"
    # A knocks pin 1 via MOVE_1 ⇒ TURN_B again (so game doesn't end yet).
    run_client "$PORT" "1/$pa/$gid/1"
    assert_stdout_contains "status=TURN_B"
    assert_stdout_contains "pawn_row=0011"
done

echo "Phase 4: Each B finishes via MOVE_2 at pawn=2 (knocks 2,3) ⇒ WIN_B"
for ((i = 0; i < NUM; i++)); do
    gid="${GAMES[i]}"
    pb="${PLAYERS_B[i]}"
    run_client "$PORT" "2/$pb/$gid/2"
    assert_stdout_contains "status=WIN_B"
    assert_stdout_contains "pawn_row=0000"
done

echo "Phase 5: KEEP_ALIVE on each finished game returns its own WIN_B state"
for ((i = 0; i < NUM; i++)); do
    gid="${GAMES[i]}"
    pa="${PLAYERS_A[i]}"
    pb="${PLAYERS_B[i]}"
    run_client "$PORT" "3/$pa/$gid"
    assert_stdout_contains "status=WIN_B"
    # Verify correct A and B IDs for THIS game, not some other one.
    assert_stdout_contains "player_a=$pa"
    assert_stdout_contains "player_b=$pb"
    if ! echo "$CLIENT_STDOUT" | grep -qE "game_id=$gid\b"; then
        echo "  FAIL: game_id mismatch — wanted $gid, got $(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1)"
        echo "  STDOUT: $CLIENT_STDOUT"
        exit 1
    fi
done

echo "Phase 6: Cross-contamination check — player A of game 0 cannot KEEP_ALIVE game 1"
pa0="${PLAYERS_A[0]}"
gid1="${GAMES[1]}"
run_client "$PORT" "3/$pa0/$gid1"
# Player $pa0 is not in game $gid1 ⇒ WRONG_MSG.
assert_stdout_contains "MessageWrong"
assert_stdout_contains "status=255"

echo "All concurrent_games_interleaved tests passed."
