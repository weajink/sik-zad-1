#!/usr/bin/env bash
# A battery of full game plays covering varied pawn_row layouts and move
# sequences. Each plays to completion, verifying the final winner and the
# all-zero pawn_row at WIN.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT

# -------------------------------------------------------------------------
# Scenario 1: "111" — odd number of pins, A wins with the last pin.
#   Moves: B knocks 0 → A knocks 1 → B knocks 2 ⇒ WIN_B
#   Wait — let's force a WIN_A. B knocks 0 → A knocks 1 → B MOVE_1 knocks 2 ⇒ WIN_B.
#   Actually 3 pins, 3 moves, last mover wins. So B wins.
# Let's do a different scenario.
# -------------------------------------------------------------------------
echo "Scenario 1: \"111\" (3 pins), B wins via single pin moves"
PORT=$(get_random_port)
start_server "111" "$PORT" 30
run_client "$PORT" "0/10"
run_client "$PORT" "0/20"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

run_client "$PORT" "1/20/$GID/0"
assert_stdout_contains "status=TURN_A"
run_client "$PORT" "1/10/$GID/1"
assert_stdout_contains "status=TURN_B"
run_client "$PORT" "1/20/$GID/2"
assert_stdout_contains "status=WIN_B"
assert_stdout_contains "pawn_row=000"

# -------------------------------------------------------------------------
# Scenario 2: "1111" — B MOVE_2 + A MOVE_2 ⇒ WIN_A.
# -------------------------------------------------------------------------
echo "Scenario 2: \"1111\" — B MOVE_2(0) + A MOVE_2(2) ⇒ WIN_A"
stop_server
PORT=$(get_random_port)
start_server "1111" "$PORT" 30
run_client "$PORT" "0/10"
run_client "$PORT" "0/20"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

run_client "$PORT" "2/20/$GID/0"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=0011"
run_client "$PORT" "2/10/$GID/2"
assert_stdout_contains "status=WIN_A"
assert_stdout_contains "pawn_row=0000"

# -------------------------------------------------------------------------
# Scenario 3: "10101" (3 isolated pins). Each must be MOVE_1. 3 moves.
#   B: 0, A: 2, B: 4 ⇒ WIN_B.
# -------------------------------------------------------------------------
echo "Scenario 3: \"10101\" — all MOVE_1, B wins last pin"
stop_server
PORT=$(get_random_port)
start_server "10101" "$PORT" 30
run_client "$PORT" "0/10"
run_client "$PORT" "0/20"
assert_stdout_contains "pawn_row=10101"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

# MOVE_2 at pawn=0 is illegal here (pin 1 empty).
run_client "$PORT" "2/20/$GID/0"
assert_stdout_contains "status=TURN_B"  # unchanged
assert_stdout_contains "pawn_row=10101"

# Legal: B MOVE_1 at 0.
run_client "$PORT" "1/20/$GID/0"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=00101"

# A MOVE_1 at 2.
run_client "$PORT" "1/10/$GID/2"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=00001"

# B MOVE_1 at 4 ⇒ WIN_B.
run_client "$PORT" "1/20/$GID/4"
assert_stdout_contains "status=WIN_B"
assert_stdout_contains "pawn_row=00000"

# -------------------------------------------------------------------------
# Scenario 4: "11011" — 4 pins, pin 2 missing. MOVE_2(0) and MOVE_2(3) legal.
# -------------------------------------------------------------------------
echo "Scenario 4: \"11011\" — MOVE_2(0) + MOVE_2(3) ⇒ WIN_A"
stop_server
PORT=$(get_random_port)
start_server "11011" "$PORT" 30
run_client "$PORT" "0/10"
run_client "$PORT" "0/20"
GID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)
assert_stdout_contains "pawn_row=11011"

# B MOVE_2 at 0 (knock 0,1) ⇒ TURN_A, pawn_row=00011.
run_client "$PORT" "2/20/$GID/0"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=00011"

# MOVE_2 at 2 is illegal (pin 2 empty).
run_client "$PORT" "2/10/$GID/2"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=00011"

# A MOVE_2 at 3 (knock 3,4) ⇒ WIN_A.
run_client "$PORT" "2/10/$GID/3"
assert_stdout_contains "status=WIN_A"
assert_stdout_contains "pawn_row=00000"

echo "All full_game_various tests passed."
