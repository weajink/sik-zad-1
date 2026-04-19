#!/usr/bin/env bash
# Illegal moves return MSG_GAME_STATE with unchanged state (NOT MSG_WRONG_MSG).
# task.txt §3.3: "Komunikat zawierający niepoprawną wartość pola pawn uznaje
# się za poprawny, ale taki ruch jest nielegalny." and "Ruch jest nielegalny
# również wtedy, gdy nie może być wykonany w aktualnym stanie gry lub gdy
# próbuje go wykonać gracz, którego nie jest kolej (dotyczy to także
# komunikatu MSG_GIVE_UP)."
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

trap stop_server EXIT

# 4-pin board so we have room for legal moves and out-of-range pawns.
start_server "1111" "$PORT" 30

# Set up: A=42, B=99, game_id captured.
run_client "$PORT" "0/42"
assert_exit_code 0
run_client "$PORT" "0/99"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE "game_id=[0-9]+" | head -1 | cut -d= -f2)

echo "Test 1: A tries MOVE_1 on B's turn — illegal, state unchanged"
# Status starts at TURN_B (2). A is not the active player.
run_client "$PORT" "1/42/$GAME_ID/0"
assert_exit_code 0
# Server returns GAME_STATE (not WRONG_MSG status 255).
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"
# Pawn row should still be all-1s (no pin knocked).
assert_stdout_contains "pawn_row=1111"

echo "Test 2: A tries GIVE_UP on B's turn — illegal, state unchanged"
run_client "$PORT" "4/42/$GAME_ID"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"

echo "Test 3: B tries MOVE_1 with pawn > max_pawn — illegal, state unchanged"
# max_pawn=3 (4 pins, 0..3). pawn=99 is out of range.
run_client "$PORT" "1/99/$GAME_ID/99"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=1111"

echo "Test 4: B tries MOVE_2 with pawn=max_pawn (pawn+1 out of range) — illegal"
# pawn=3, pawn+1=4 doesn't exist.
run_client "$PORT" "2/99/$GAME_ID/3"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_B"
assert_stdout_contains "pawn_row=1111"

echo "Test 5: B legally knocks pin 0 (MOVE_1), then A tries to knock pin 0 again — illegal"
run_client "$PORT" "1/99/$GAME_ID/0"
assert_exit_code 0
assert_stdout_contains "status=TURN_A"  # turn flipped to A
assert_stdout_contains "pawn_row=0111"

# A tries to knock the already-empty pin 0.
run_client "$PORT" "1/42/$GAME_ID/0"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_A"  # still A's turn
assert_stdout_contains "pawn_row=0111"  # unchanged

echo "Test 6: A tries MOVE_2 where pawn is empty — illegal"
# A wants to MOVE_2 at pawn=0 (already knocked) — illegal.
run_client "$PORT" "2/42/$GAME_ID/0"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=TURN_A"
assert_stdout_contains "pawn_row=0111"

echo "Test 7: A legally finishes via MOVE_2 — both pins knocked at once is fine"
# A knocks pins 1,2 with MOVE_2 at pawn 1.
run_client "$PORT" "2/42/$GAME_ID/1"
assert_exit_code 0
assert_stdout_contains "status=TURN_B"  # B's turn now
assert_stdout_contains "pawn_row=0001"

# B knocks pin 3 with MOVE_1.
run_client "$PORT" "1/99/$GAME_ID/3"
assert_exit_code 0
assert_stdout_contains "status=WIN_B"  # WIN_B
assert_stdout_contains "pawn_row=0000"

echo "Test 8: After WIN, MOVE is illegal — state unchanged"
run_client "$PORT" "1/99/$GAME_ID/0"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=WIN_B"  # still WIN_B
assert_stdout_contains "pawn_row=0000"

echo "Test 9: After WIN, GIVE_UP is illegal — state unchanged"
run_client "$PORT" "4/99/$GAME_ID"
assert_exit_code 0
assert_stdout_not_contains "status=255"
assert_stdout_contains "status=WIN_B"

echo "All illegal_moves tests passed."
