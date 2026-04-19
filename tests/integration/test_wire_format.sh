#!/usr/bin/env bash
# Verifies wire-format details of MSG_GAME_STATE (task.txt §3.1, §3.3):
#   * Network byte order (big-endian) for all 32-bit fields.
#   * status byte placement at offset 12.
#   * max_pawn byte at offset 13.
#   * pawn_row bitmap: pawn 0 is MSB of byte 0.
#   * excess bits (indexes > max_pawn) are zeroed.
#   * bitmap length is floor(max_pawn / 8) + 1 bytes.
#
# We bypass the client binary and open a raw UDP socket in python to inspect
# the exact bytes coming back from the server.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

# Send raw bytes and print server response as hex (one line). Times out after 2s.
# Usage: raw_udp <port> <hex_string>
raw_udp() {
    local port="$1"
    local hex="$2"
    python3 -c "
import socket, sys
port = int('$port')
data = bytes.fromhex('$hex')
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)
s.sendto(data, ('127.0.0.1', port))
try:
    resp, _ = s.recvfrom(4096)
    print(resp.hex())
except socket.timeout:
    print('TIMEOUT')
"
}

trap stop_server EXIT

# -------------------------------------------------------------------------
# Case 1: max_pawn=2 (row "111") — bitmap is 1 byte, value 0b11100000 = 0xE0.
# -------------------------------------------------------------------------
echo "Test 1: GAME_STATE wire format with max_pawn=2 (row=\"111\")"
PORT=$(get_random_port)
start_server "111" "$PORT" 30

# MSG_JOIN(msg_type=0, player_id=0x00000001) — expect GAME_STATE back.
# Wire: 00 00000001 (5 bytes).
RESP=$(raw_udp "$PORT" "0000000001")
if [[ "$RESP" == "TIMEOUT" ]]; then
    echo "  FAIL: server did not respond to MSG_JOIN"
    exit 1
fi
# GAME_STATE_HEADER_SIZE=14, + 1 bitmap byte = 15 bytes = 30 hex chars.
if [[ ${#RESP} -ne 30 ]]; then
    echo "  FAIL: expected 30 hex chars (15 bytes), got ${#RESP}: $RESP"
    exit 1
fi
# Layout: gameid(4) playerA(4) playerB(4) status(1) maxpawn(1) bitmap(1)
# Offset 0..3: game_id (big-endian) — first allocation is 0 ⇒ 00000000.
if [[ "${RESP:0:8}" != "00000000" ]]; then
    echo "  FAIL: game_id bytes wrong: ${RESP:0:8}"
    exit 1
fi
# Offset 4..7: player_a_id = 1 ⇒ 00000001. Verifies network byte order.
if [[ "${RESP:8:8}" != "00000001" ]]; then
    echo "  FAIL: player_a_id not big-endian 1: ${RESP:8:8}"
    exit 1
fi
# Offset 8..11: player_b_id = 0 (no player B yet) ⇒ 00000000.
if [[ "${RESP:16:8}" != "00000000" ]]; then
    echo "  FAIL: player_b_id should be 0: ${RESP:16:8}"
    exit 1
fi
# Offset 12: status = WAITING_FOR_OPPONENT (0).
if [[ "${RESP:24:2}" != "00" ]]; then
    echo "  FAIL: status should be 0x00 (WAITING): ${RESP:24:2}"
    exit 1
fi
# Offset 13: max_pawn = 2.
if [[ "${RESP:26:2}" != "02" ]]; then
    echo "  FAIL: max_pawn should be 0x02: ${RESP:26:2}"
    exit 1
fi
# Offset 14: bitmap byte — pins at 0,1,2 → 0b11100000 = 0xE0 (MSB-first).
if [[ "${RESP:28:2}" != "e0" ]]; then
    echo "  FAIL: bitmap should be 0xE0 (MSB-first for pins 0,1,2): ${RESP:28:2}"
    exit 1
fi
echo "  OK: wire format correct for 3-pin board"

# -------------------------------------------------------------------------
# Case 2: max_pawn=0 (row "1") — bitmap is 1 byte, value 0b10000000 = 0x80.
# Excess bits (1..7) must be zero.
# -------------------------------------------------------------------------
stop_server
echo "Test 2: max_pawn=0 ⇒ bitmap 0x80 with excess bits zeroed"
PORT=$(get_random_port)
start_server "1" "$PORT" 30

RESP=$(raw_udp "$PORT" "0000000001")
# 14 header + 1 bitmap = 15 bytes = 30 hex chars.
if [[ ${#RESP} -ne 30 ]]; then
    echo "  FAIL: expected 30 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:26:2}" != "00" ]]; then
    echo "  FAIL: max_pawn should be 0x00: ${RESP:26:2}"
    exit 1
fi
if [[ "${RESP:28:2}" != "80" ]]; then
    echo "  FAIL: bitmap should be 0x80 (only pin 0, rest zero): ${RESP:28:2}"
    exit 1
fi

# -------------------------------------------------------------------------
# Case 3: max_pawn=4 (row "10001") — bitmap 1 byte, MSB pin 0 and bit 4 set;
# bits 5,6,7 (out of range) must be 0.
# 0b10001000 = 0x88.
# -------------------------------------------------------------------------
stop_server
echo "Test 3: max_pawn=4 with gaps ⇒ bitmap 0x88"
PORT=$(get_random_port)
start_server "10001" "$PORT" 30

RESP=$(raw_udp "$PORT" "0000000001")
if [[ ${#RESP} -ne 30 ]]; then
    echo "  FAIL: expected 30 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:26:2}" != "04" ]]; then
    echo "  FAIL: max_pawn should be 0x04: ${RESP:26:2}"
    exit 1
fi
if [[ "${RESP:28:2}" != "88" ]]; then
    echo "  FAIL: bitmap should be 0x88: ${RESP:28:2}"
    exit 1
fi

# -------------------------------------------------------------------------
# Case 4: max_pawn=8 (row "111111111") — bitmap 2 bytes. All 9 pins set.
# Byte 0 = 0xFF (pins 0..7). Byte 1 = 0x80 (pin 8, rest are excess ⇒ zero).
# -------------------------------------------------------------------------
stop_server
echo "Test 4: max_pawn=8 ⇒ 2-byte bitmap 0xFF 0x80"
PORT=$(get_random_port)
start_server "111111111" "$PORT" 30

RESP=$(raw_udp "$PORT" "0000000001")
# 14 header + 2 bitmap = 16 bytes = 32 hex chars.
if [[ ${#RESP} -ne 32 ]]; then
    echo "  FAIL: expected 32 hex chars (16 bytes), got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:26:2}" != "08" ]]; then
    echo "  FAIL: max_pawn should be 0x08: ${RESP:26:2}"
    exit 1
fi
if [[ "${RESP:28:4}" != "ff80" ]]; then
    echo "  FAIL: bitmap should be 0xFF 0x80: ${RESP:28:4}"
    exit 1
fi

# -------------------------------------------------------------------------
# Case 5: max_pawn=15 (row "1111111111111111") — exactly 2 bytes, all set.
# Byte 0 = 0xFF, byte 1 = 0xFF (no excess bits).
# -------------------------------------------------------------------------
stop_server
echo "Test 5: max_pawn=15 ⇒ 2-byte bitmap 0xFF 0xFF (no excess bits)"
PORT=$(get_random_port)
start_server "1111111111111111" "$PORT" 30

RESP=$(raw_udp "$PORT" "0000000001")
if [[ ${#RESP} -ne 32 ]]; then
    echo "  FAIL: expected 32 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:26:2}" != "0f" ]]; then
    echo "  FAIL: max_pawn should be 0x0F: ${RESP:26:2}"
    exit 1
fi
if [[ "${RESP:28:4}" != "ffff" ]]; then
    echo "  FAIL: bitmap should be 0xFF 0xFF: ${RESP:28:4}"
    exit 1
fi

# -------------------------------------------------------------------------
# Case 6: max_pawn=255 (256-pin row, all-1s) — bitmap must be 32 bytes of 0xFF.
# -------------------------------------------------------------------------
stop_server
echo "Test 6: max_pawn=255 ⇒ 32-byte bitmap of 0xFF"
PORT=$(get_random_port)
LONG=$(printf '1%.0s' {1..256})
start_server "$LONG" "$PORT" 30

RESP=$(raw_udp "$PORT" "0000000001")
# 14 header + 32 bitmap = 46 bytes = 92 hex chars.
if [[ ${#RESP} -ne 92 ]]; then
    echo "  FAIL: expected 92 hex chars (46 bytes), got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:26:2}" != "ff" ]]; then
    echo "  FAIL: max_pawn should be 0xFF: ${RESP:26:2}"
    exit 1
fi
EXPECTED_BITMAP=$(printf 'ff%.0s' {1..32})
if [[ "${RESP:28:64}" != "$EXPECTED_BITMAP" ]]; then
    echo "  FAIL: bitmap should be 32 0xFF bytes, got: ${RESP:28:64}"
    exit 1
fi

# -------------------------------------------------------------------------
# Case 7: Large player_id verifies endianness of 32-bit fields.
# player_id=0xDEADBEEF — expect to see "deadbeef" at offset 4..7.
# -------------------------------------------------------------------------
stop_server
echo "Test 7: player_id=0xDEADBEEF ⇒ network byte order verified"
PORT=$(get_random_port)
start_server "1" "$PORT" 30

# MSG_JOIN with player_id=0xDEADBEEF
RESP=$(raw_udp "$PORT" "00deadbeef")
if [[ ${#RESP} -ne 30 ]]; then
    echo "  FAIL: expected 30 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:8:8}" != "deadbeef" ]]; then
    echo "  FAIL: player_a_id should be deadbeef (big-endian): ${RESP:8:8}"
    exit 1
fi

echo "All wire_format tests passed."
