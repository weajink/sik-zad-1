#!/usr/bin/env bash
# Stress the largest allowed board: max_pawn=255 (256 pins).
# Verify:
#   * Game state round-trips the full 256-bit bitmap.
#   * Moves deep in the middle of the board work.
#   * Pin at index 255 (last) is addressable via MOVE_1 pawn=255.
#   * Final state shows correct bitmap after a sequence of moves.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)

# 256-pin row, all 1s.
LONG=$(printf '1%.0s' {1..256})
start_server "$LONG" "$PORT" 60

echo "Test 1: 256-pin board advertises max_pawn=255"
run_client "$PORT" "0/1"
assert_stdout_contains "max_pawn=255"
assert_stdout_contains "status=WAITING_FOR_OPPONENT"

echo "Test 2: Second JOIN starts TURN_B"
run_client "$PORT" "0/2"
assert_stdout_contains "status=TURN_B"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'game_id=[0-9]+' | head -1 | cut -d= -f2)

echo "Test 3: B knocks pin 128 (middle) via MOVE_1"
run_client "$PORT" "1/2/$GAME_ID/128"
assert_stdout_contains "status=TURN_A"
# The bitmap should have a 0 at index 128.
# Extract pawn_row and verify.
ROW=$(echo "$CLIENT_STDOUT" | grep -oE 'pawn_row=[01]+' | cut -d= -f2)
if [[ ${#ROW} -ne 256 ]]; then
    echo "  FAIL: pawn_row length should be 256, got ${#ROW}: $ROW"
    exit 1
fi
if [[ "${ROW:128:1}" != "0" ]]; then
    echo "  FAIL: pin at index 128 should be 0, got ${ROW:128:1}"
    echo "  ROW: $ROW"
    exit 1
fi
# Other pins should still be 1.
if [[ "${ROW:0:1}" != "1" || "${ROW:255:1}" != "1" || "${ROW:127:1}" != "1" || "${ROW:129:1}" != "1" ]]; then
    echo "  FAIL: expected pins 0,127,129,255 to still be 1"
    echo "  ROW: $ROW"
    exit 1
fi

echo "Test 4: A knocks pin 255 (last pin) via MOVE_1"
run_client "$PORT" "1/1/$GAME_ID/255"
assert_stdout_contains "status=TURN_B"
ROW=$(echo "$CLIENT_STDOUT" | grep -oE 'pawn_row=[01]+' | cut -d= -f2)
if [[ "${ROW:255:1}" != "0" ]]; then
    echo "  FAIL: pin at 255 should be 0"
    echo "  ROW: $ROW"
    exit 1
fi

echo "Test 5: B knocks pin 0 (first pin) via MOVE_1"
run_client "$PORT" "1/2/$GAME_ID/0"
assert_stdout_contains "status=TURN_A"
ROW=$(echo "$CLIENT_STDOUT" | grep -oE 'pawn_row=[01]+' | cut -d= -f2)
if [[ "${ROW:0:1}" != "0" ]]; then
    echo "  FAIL: pin at 0 should be 0"
    exit 1
fi

echo "Test 6: A does MOVE_2 at pawn=1 (knocks pins 1,2)"
run_client "$PORT" "2/1/$GAME_ID/1"
assert_stdout_contains "status=TURN_B"
ROW=$(echo "$CLIENT_STDOUT" | grep -oE 'pawn_row=[01]+' | cut -d= -f2)
if [[ "${ROW:1:1}" != "0" || "${ROW:2:1}" != "0" ]]; then
    echo "  FAIL: pins 1,2 should be 0"
    echo "  ROW: $ROW"
    exit 1
fi

echo "Test 7: B gives up ⇒ WIN_A on 256-pin board"
run_client "$PORT" "4/2/$GAME_ID"
assert_stdout_contains "status=WIN_A"

echo "All max_pawn_255 tests passed."
