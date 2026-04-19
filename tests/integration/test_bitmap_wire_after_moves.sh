#!/usr/bin/env bash
# After moves, verify at the wire level that the pawn_row bitmap bytes are
# updated correctly, in MSB-first order, with excess bits still zero.
#
# This is distinct from test_wire_format.sh (which tests the initial state);
# here we mutate the state and check the bytes change as expected.
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

trap stop_server EXIT

# -------------------------------------------------------------------------
# Row "11111" (max_pawn=4). After B knocks pin 0 (MSB), bitmap should be
# 0b01111000 = 0x78.
# -------------------------------------------------------------------------
echo "Test 1: After knocking pin 0 (MSB), bitmap MSB is zeroed"
PORT=$(get_random_port)
start_server "11111" "$PORT" 30

# JOIN A=1. player_id=1 big-endian = 00000001.
RESP=$(raw_udp "$PORT" "0000000001")
# JOIN B=2 and capture the GAME_ID.
RESP=$(raw_udp "$PORT" "0000000002")
# GameState layout: gameid(4) playerA(4) playerB(4) status(1) maxpawn(1) bitmap
GID_HEX="${RESP:0:8}"

# B (id=2) MOVE_1 at pawn=0: type=1, player=2, game=<GID>, pawn=0.
RESP=$(raw_udp "$PORT" "010000000200000000")
# The real packet must include the real GID, not a placeholder.
RESP=$(raw_udp "$PORT" "0100000002${GID_HEX}00")
# Expected: 15 bytes = 30 hex chars.
if [[ ${#RESP} -ne 30 ]]; then
    echo "  FAIL: expected 30 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
# Offset 14: bitmap byte. Pins 1,2,3,4 set (pin 0 cleared) ⇒ 0b01111000 = 0x78.
if [[ "${RESP:28:2}" != "78" ]]; then
    echo "  FAIL: bitmap after knocking pin 0 should be 0x78, got ${RESP:28:2}"
    echo "  FULL: $RESP"
    exit 1
fi
# Status should be TURN_A = 1.
if [[ "${RESP:24:2}" != "01" ]]; then
    echo "  FAIL: status should be 0x01 (TURN_A), got ${RESP:24:2}"
    exit 1
fi

# -------------------------------------------------------------------------
# A knocks pin 4 (last) via MOVE_1: bitmap should become 0b01110000 = 0x70.
# -------------------------------------------------------------------------
echo "Test 2: After knocking last pin (index=max_pawn), bit at max_pawn position is 0"
# A=1, game=<GID>, pawn=4.
RESP=$(raw_udp "$PORT" "0100000001${GID_HEX}04")
if [[ "${RESP:28:2}" != "70" ]]; then
    echo "  FAIL: bitmap should be 0x70 after knocking pins 0 and 4, got ${RESP:28:2}"
    exit 1
fi
# Status should be TURN_B = 2.
if [[ "${RESP:24:2}" != "02" ]]; then
    echo "  FAIL: status should be 0x02 (TURN_B), got ${RESP:24:2}"
    exit 1
fi

# -------------------------------------------------------------------------
# Excess-bits-zeroed check: start a 9-pin board, knock pin 8 (only pin in
# byte 1), and verify byte 1 is 0x00 (not some garbage).
# -------------------------------------------------------------------------
echo "Test 3: max_pawn=8, knock pin 8 ⇒ byte 1 becomes 0x00 (excess bits stay 0)"
stop_server
PORT=$(get_random_port)
start_server "111111111" "$PORT" 30

RESP=$(raw_udp "$PORT" "0000000001")
RESP=$(raw_udp "$PORT" "0000000002")
GID_HEX="${RESP:0:8}"
# B MOVE_1 at pawn=8.
RESP=$(raw_udp "$PORT" "0100000002${GID_HEX}08")
# Response should be 14 header + 2 bitmap bytes = 16 bytes = 32 hex chars.
if [[ ${#RESP} -ne 32 ]]; then
    echo "  FAIL: expected 32 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
# Byte 0 of bitmap = 0xFF (all 8 original pins), byte 1 = 0x00 (pin 8 was
# the only one in byte 1, now knocked, excess bits still 0).
if [[ "${RESP:28:4}" != "ff00" ]]; then
    echo "  FAIL: expected bitmap 0xFF 0x00, got ${RESP:28:4}"
    exit 1
fi

echo "All bitmap_wire_after_moves tests passed."
