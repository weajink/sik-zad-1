#!/usr/bin/env bash
# task.txt §3.3: "Komunikat MSG_JOIN jest poprawny, jeśli ma poprawną długość
# i zawiera niezerową wartość w polu player_id."
#
# The client CLI rejects player_id=0, but a raw UDP packet with player_id=0
# must be answered by the server with MSG_WRONG_MSG (wire-level validation).
# Also: MOVE/KEEP_ALIVE/GIVE_UP with player_id=0 are not participants of any
# game ⇒ server returns WRONG_MSG with INVALID_PLAYER_ID (error_index=1).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

raw_udp() {
    local port="$1"
    local hex="$2"
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)
s.sendto(bytes.fromhex('$hex'), ('127.0.0.1', int('$port')))
try:
    resp, _ = s.recvfrom(4096)
    print(resp.hex())
except socket.timeout:
    print('TIMEOUT')
"
}

assert_wrong_msg_status() {
    local resp="$1"
    local label="$2"
    if [[ "$resp" == "TIMEOUT" ]]; then
        echo "  FAIL [$label]: server did not respond"
        return 1
    fi
    if [[ ${#resp} -ne 28 ]]; then
        echo "  FAIL [$label]: expected 28 hex chars, got ${#resp}: $resp"
        return 1
    fi
    if [[ "${resp:24:2}" != "ff" ]]; then
        echo "  FAIL [$label]: status byte should be 0xFF, got ${resp:24:2}"
        return 1
    fi
    echo "  OK   [$label]"
}

trap stop_server EXIT
PORT=$(get_random_port)
start_server "111" "$PORT" 30

echo "Test 1: MSG_JOIN with player_id=0 ⇒ MSG_WRONG_MSG"
# 00 00000000 — msg_type=JOIN, player_id=0.
RESP=$(raw_udp "$PORT" "0000000000")
assert_wrong_msg_status "$RESP" "JOIN player_id=0"

echo "Test 2: MSG_KEEP_ALIVE with player_id=0 on a non-existent game ⇒ WRONG_MSG"
# Note: the server first looks up the game, so error may be INVALID_GAME_ID
# (error_index=5) rather than INVALID_PLAYER_ID. Either way, status must be 255.
RESP=$(raw_udp "$PORT" "030000000000000000")
assert_wrong_msg_status "$RESP" "KEEP_ALIVE player_id=0"

echo "Test 3: After a real game is created, KEEP_ALIVE player_id=0 for that game ⇒ WRONG_MSG"
# Create a WAITING game (player A=42). Its game_id will be 0 (first alloc).
run_client "$PORT" "0/42"
assert_exit_code 0

# Raw KEEP_ALIVE: msg_type=03, player_id=0, game_id=0 (all zero u32s).
# Wire: 03 00000000 00000000 = 9 bytes = 18 hex chars.
# Expect WRONG_MSG because player_id=0 is not a valid participant id. If the
# server accepted it because player_b_id is internally 0 in a WAITING game,
# that's a spec violation (player IDs must be 1..2^32-1).
RESP=$(raw_udp "$PORT" "030000000000000000")
assert_wrong_msg_status "$RESP" "KEEP_ALIVE player_id=0 on existing WAITING game"

echo "Test 4: MSG_MOVE_1 with player_id=0 on existing game ⇒ WRONG_MSG"
# Wire: 01 00000000 00000000 00 = 10 bytes = 20 hex chars.
RESP=$(raw_udp "$PORT" "01000000000000000000")
assert_wrong_msg_status "$RESP" "MOVE_1 player_id=0"

echo "Test 5: MSG_GIVE_UP with player_id=0 on existing game ⇒ WRONG_MSG"
# Wire: 04 00000000 00000000 = 9 bytes = 18 hex chars.
RESP=$(raw_udp "$PORT" "040000000000000000")
assert_wrong_msg_status "$RESP" "GIVE_UP player_id=0"

echo "All wrong_player_zero tests passed."
