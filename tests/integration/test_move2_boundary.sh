#!/usr/bin/env bash
# MOVE_2 specification: knocks pawn and pawn+1 (task.txt §3.2).
# Boundary conditions:
#   * pawn = max_pawn ⇒ pawn+1 is out of range ⇒ illegal (state unchanged).
#   * pawn > max_pawn ⇒ illegal (state unchanged).
#   * pawn = 255 (wraparound candidate) ⇒ illegal unless max_pawn ≥ 256 (impossible).
#   * Either adjacent pin already empty ⇒ illegal (bitmap unchanged).
#   * MOVE_2 on final two remaining pins ⇒ WIN for the mover.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT

# -------------------------------------------------------------------------
# Test 1: MOVE_2 at pawn=max_pawn is illegal (pawn+1 out of range).
# Row "11111" ⇒ max_pawn=4. B tries MOVE_2 at pawn=4 ⇒ illegal.
# -------------------------------------------------------------------------
echo "Test 1: MOVE_2 at pawn=max_pawn is illegal (state unchanged)"
PORT=$(get_random_port)
start_server "11111" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
assert_stdout_contains "Status: player B's turn"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

run_client "$PORT" "2/2/$GAME_ID/4"
# Still TURN_B (illegal move doesn't flip turn), and pawn_row unchanged.
assert_stdout_contains "Status: player B's turn"
assert_stdout_contains "Pawns: #####"
# And it must NOT be a WRONG_MSG — it's a valid but illegal move.
assert_stdout_not_contains "Server rejected the message"
assert_stdout_not_contains "Server rejected the message"

# -------------------------------------------------------------------------
# Test 2: MOVE_2 at pawn=255 with row of size 255 ⇒ still illegal.
# Row of exactly 255 '1's ⇒ max_pawn=254. pawn=255 > max_pawn ⇒ illegal.
# -------------------------------------------------------------------------
echo "Test 2: MOVE_2 at pawn=255 with max_pawn=254 is illegal"
stop_server
PORT=$(get_random_port)
ROW_255=$(printf '1%.0s' {1..255})
start_server "$ROW_255" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

run_client "$PORT" "2/2/$GAME_ID/255"
# Turn should still be TURN_B — move was illegal.
assert_stdout_contains "Status: player B's turn"

# -------------------------------------------------------------------------
# Test 3: MOVE_2 where pawn+1 is empty ⇒ illegal.
# Row "101" ⇒ pins 0, 2 present, pin 1 missing. B tries MOVE_2 at pawn=0
# (needs pins 0 and 1, but 1 is empty).
# -------------------------------------------------------------------------
echo "Test 3: MOVE_2 where pawn+1 is empty is illegal (bitmap unchanged)"
stop_server
PORT=$(get_random_port)
start_server "101" "$PORT" 30

run_client "$PORT" "0/1"
run_client "$PORT" "0/2"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
assert_stdout_contains "Pawns: #\.#"

run_client "$PORT" "2/2/$GAME_ID/0"
# Illegal move: pin 1 is empty, so MOVE_2 at pawn 0 can't knock both.
# State should be unchanged: still TURN_B, still pawn_row=101.
assert_stdout_contains "Status: player B's turn"
assert_stdout_contains "Pawns: #\.#"
assert_stdout_not_contains "Server rejected the message"

# -------------------------------------------------------------------------
# Test 4: MOVE_2 where pawn is empty ⇒ illegal.
# After B knocks pin 2 via MOVE_1, A tries MOVE_2 at pawn=2 (empty).
# -------------------------------------------------------------------------
echo "Test 4: MOVE_2 where pawn itself is empty is illegal"
# B knocks pin 2.
run_client "$PORT" "1/2/$GAME_ID/2"
assert_stdout_contains "Status: player A's turn"
assert_stdout_contains "Pawns: #\.\."

# A tries MOVE_2 at pawn=2 (empty, and pawn=3 out of range anyway).
run_client "$PORT" "2/1/$GAME_ID/2"
assert_stdout_contains "Status: player A's turn"
assert_stdout_contains "Pawns: #\.\."

# -------------------------------------------------------------------------
# Test 5: MOVE_2 on the last two remaining adjacent pins wins the game.
# Row "11" ⇒ B does MOVE_2 at pawn=0 ⇒ WIN_B, pawn_row=00.
# -------------------------------------------------------------------------
echo "Test 5: MOVE_2 knocking the last two pins ⇒ WIN for mover"
stop_server
PORT=$(get_random_port)
start_server "11" "$PORT" 30

run_client "$PORT" "0/7"
run_client "$PORT" "0/8"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

run_client "$PORT" "2/8/$GAME_ID/0"
assert_stdout_contains "Status: player B wins"
assert_stdout_contains "Pawns: \.\."

# -------------------------------------------------------------------------
# Test 6: MOVE_2 at pawn=254 where max_pawn=255 (edge of 256-pin row).
# pawn=254 ⇒ knock 254 and 255. Legal if both present.
# -------------------------------------------------------------------------
echo "Test 6: MOVE_2 at pawn=254 with max_pawn=255 ⇒ legal (knocks last 2 pins)"
stop_server
PORT=$(get_random_port)
LONG=$(printf '1%.0s' {1..256})
start_server "$LONG" "$PORT" 30

run_client "$PORT" "0/10"
run_client "$PORT" "0/20"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')
assert_stdout_contains "Status: player B's turn"

# B makes MOVE_2 at pawn=254. This should succeed — pins 254 and 255 exist.
run_client "$PORT" "2/20/$GAME_ID/254"
# Turn should be TURN_A now (not WIN because 254 other pins remain).
assert_stdout_contains "Status: player A's turn"
# Extract the pawn_row field and check it's exactly 254 ones followed by "00".
ROW=$(echo "$CLIENT_STDOUT" | grep -oE 'Pawns: [.#]+' | head -1 | sed 's/^Pawns: //' | tr '.#' '01')
if [[ ${#ROW} -ne 256 ]]; then
    echo "  FAIL: pawn_row length should be 256, got ${#ROW}"
    exit 1
fi
# Positions 254, 255 (0-indexed) must be '0'; everything before must be '1'.
EXPECTED=$(printf '1%.0s' {1..254})"00"
if [[ "$ROW" != "$EXPECTED" ]]; then
    echo "  FAIL: pawn_row should be 254 ones then '00'"
    echo "  got: $ROW"
    exit 1
fi

echo "All move2_boundary tests passed."
