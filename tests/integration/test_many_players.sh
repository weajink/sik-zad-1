#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

# 20 players → 10 sequential games. pawn_row "1111" = 4 pins so games can
# play out fully (knock 2 + knock 2 = WIN_B). Server timeout long enough
# that no game is reaped during the test.
start_server "1111" "$PORT" 30

NUM_PLAYERS=20
NUM_GAMES=$((NUM_PLAYERS / 2))

# Player IDs: use 1001..1020 so they're easy to distinguish in logs.
# Pair (1001,1002) → game 1, (1003,1004) → game 2, ...
declare -a GAME_IDS=()

echo "Phase 1: $NUM_PLAYERS players JOIN sequentially, forming $NUM_GAMES games"

for ((i = 0; i < NUM_GAMES; i++)); do
    player_a=$((1001 + 2 * i))
    player_b=$((1002 + 2 * i))

    # First player of the pair: creates a game in WAITING state.
    run_client "$PORT" "0/$player_a"
    assert_exit_code 0
    assert_stdout_contains "Player A: $player_a$"
    assert_stdout_contains "Status: waiting for opponent"

    # Small delay to spread joins out "over time".
    sleep 0.05

    # Second player: game starts, status TURN_B (2).
    run_client "$PORT" "0/$player_b"
    assert_exit_code 0
    assert_stdout_contains "Player A: $player_a$"
    assert_stdout_contains "Player B: $player_b$"
    assert_stdout_contains "Status: player B's turn"

    game_id=$(echo "$CLIENT_STDOUT" | grep -oE "Game [0-9]+" | head -1 | awk '{print $2}')
    if [[ -z "$game_id" ]]; then
        echo "  FAIL: could not parse game_id for pair ($player_a,$player_b)"
        exit 1
    fi
    GAME_IDS+=("$game_id")
    echo "  Game $((i + 1)): players $player_a vs $player_b → game_id=$game_id"

    sleep 0.05
done

# Game IDs must be unique — server should not reuse them while games are live.
unique_count=$(printf '%s\n' "${GAME_IDS[@]}" | sort -u | wc -l)
if [[ "$unique_count" -ne "$NUM_GAMES" ]]; then
    echo "  FAIL: expected $NUM_GAMES unique game IDs, got $unique_count"
    printf '  IDs: %s\n' "${GAME_IDS[*]}"
    exit 1
fi
echo "  All $NUM_GAMES game IDs are unique."

echo "Phase 2: KEEP_ALIVE on every live game (interleaved across games)"

# Hit each game with a keep-alive from player A. Verifies the server
# correctly routes messages to the right game when many are in flight.
for ((i = 0; i < NUM_GAMES; i++)); do
    player_a=$((1001 + 2 * i))
    player_b=$((1002 + 2 * i))
    game_id="${GAME_IDS[i]}"

    run_client "$PORT" "3/$player_a/$game_id"
    assert_exit_code 0
    assert_stdout_contains "Status: player B's turn"
    assert_stdout_contains "Player A: $player_a$"
    assert_stdout_contains "Player B: $player_b$"
done

echo "Phase 3: finish each game — mix of full play, give-up, and one in-progress"

# pawn_row "1111" → 4 pins (indices 0..3). MOVE_2 at pawn 0 knocks 0,1;
# MOVE_2 at pawn 2 knocks 2,3 → WIN_B.
finished_a=0
finished_b=0
in_progress=0

for ((i = 0; i < NUM_GAMES; i++)); do
    player_a=$((1001 + 2 * i))
    player_b=$((1002 + 2 * i))
    game_id="${GAME_IDS[i]}"

    case $((i % 3)) in
        0)
            # B knocks pins 0,1 → TURN_A. A knocks pins 2,3 → WIN_A.
            run_client "$PORT" "2/$player_b/$game_id/0"
            assert_exit_code 0
            assert_stdout_contains "Status: player A's turn"

            run_client "$PORT" "2/$player_a/$game_id/2"
            assert_exit_code 0
            assert_stdout_contains "Status: player A wins"
            finished_a=$((finished_a + 1))
            ;;
        1)
            # B gives up immediately → WIN_A (status 3).
            run_client "$PORT" "4/$player_b/$game_id"
            assert_exit_code 0
            assert_stdout_contains "Status: player A wins"
            finished_a=$((finished_a + 1))
            ;;
        2)
            # B makes one move only — game stays in progress (TURN_A).
            run_client "$PORT" "1/$player_b/$game_id/0"
            assert_exit_code 0
            assert_stdout_contains "Status: player A's turn"
            in_progress=$((in_progress + 1))
            ;;
    esac
done

echo "  WIN_A: $finished_a, WIN_B: $finished_b, in-progress: $in_progress"

echo "Phase 4: finished games still respond to KEEP_ALIVE within timeout window"

# Pick the first game (which finished as WIN_A) and confirm it's still queryable.
first_game="${GAME_IDS[0]}"
run_client "$PORT" "3/1001/$first_game"
assert_exit_code 0
assert_stdout_contains "Status: player A wins"

echo "Phase 5: a fresh JOIN after all the activity still creates a new game"

# Player 9999 joins; should create a brand-new WAITING game with a unique ID.
run_client "$PORT" "0/9999"
assert_exit_code 0
assert_stdout_contains "Player A: 9999$"
assert_stdout_contains "Status: waiting for opponent"
new_game_id=$(echo "$CLIENT_STDOUT" | grep -oE "Game [0-9]+" | head -1 | awk '{print $2}')
for existing in "${GAME_IDS[@]}"; do
    if [[ "$new_game_id" == "$existing" ]]; then
        echo "  FAIL: new game reused existing game_id=$new_game_id"
        exit 1
    fi
done
echo "  New game_id=$new_game_id is distinct from all prior games."

echo "All many-players tests passed."
