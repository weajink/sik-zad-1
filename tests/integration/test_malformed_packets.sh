#!/usr/bin/env bash
# Malformed wire-level packets should elicit MSG_WRONG_MSG from the server
# (task.txt §3.3). We verify:
#   * Correct response size (14 bytes: 12 echo + status + error_index).
#   * status byte at offset 12 == 0xFF (255).
#   * error_index at offset 13 matches the spec's indexing rules.
#   * The 12-byte echo contains up to 12 bytes of the offending datagram,
#     zero-padded. (Extra bytes beyond 12 are NOT echoed.)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

# Send raw bytes and return hex response; "TIMEOUT" on timeout.
# Usage: raw_udp <port> <hex>
raw_udp() {
    local port="$1"
    local hex="$2"
    python3 -c "
import socket
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

# Asserts a hex response is a MSG_WRONG_MSG with:
#   * 14 bytes (28 hex chars) total
#   * status byte (offset 12) == 0xFF
#   * error_index (offset 13) == expected
# Usage: assert_wrong_msg <response_hex> <expected_error_index_decimal> <label>
assert_wrong_msg() {
    local resp="$1"
    local expected_idx="$2"
    local label="$3"
    if [[ "$resp" == "TIMEOUT" ]]; then
        echo "  FAIL [$label]: server did not respond"
        return 1
    fi
    if [[ ${#resp} -ne 28 ]]; then
        echo "  FAIL [$label]: expected 28 hex chars (14 bytes), got ${#resp}: $resp"
        return 1
    fi
    if [[ "${resp:24:2}" != "ff" ]]; then
        echo "  FAIL [$label]: status byte should be 0xFF (WRONG), got: ${resp:24:2} (resp=$resp)"
        return 1
    fi
    local actual_idx_hex="${resp:26:2}"
    local actual_idx=$((16#$actual_idx_hex))
    if [[ "$actual_idx" -ne "$expected_idx" ]]; then
        echo "  FAIL [$label]: expected error_index=$expected_idx, got $actual_idx (hex=$actual_idx_hex)"
        return 1
    fi
    echo "  OK   [$label] (error_index=$expected_idx)"
}

# Asserts the 12-byte echo matches a given hex prefix (and zeros afterward).
# Usage: assert_echo_prefix <resp_hex> <expected_prefix_hex> <label>
# The expected prefix may be shorter than 24 hex chars (12 bytes); the remaining
# bytes must be zero.
assert_echo_prefix() {
    local resp="$1"
    local expected="$2"
    local label="$3"
    local echo_part="${resp:0:24}"
    # Right-pad expected with zeros to 24 hex chars.
    local padded="$expected"
    while [[ ${#padded} -lt 24 ]]; do padded="${padded}0"; done
    # Truncate in case expected was >24 (shouldn't happen, but be safe).
    padded="${padded:0:24}"
    if [[ "$echo_part" != "$padded" ]]; then
        echo "  FAIL [$label]: echo wrong"
        echo "    expected: $padded"
        echo "    got:      $echo_part"
        return 1
    fi
    echo "  OK   [$label] echo correct"
}

trap stop_server EXIT

PORT=$(get_random_port)
start_server "1111" "$PORT" 30

# -------------------------------------------------------------------------
# 1. Invalid msg_type (unknown type byte).
# Spec: max valid msg_type is 4. 5, 99, 255 are all invalid.
# error_index for invalid_msg_type is 0 (the msg_type byte).
# -------------------------------------------------------------------------
echo "Test 1: msg_type=5 (> MAX=4) ⇒ WRONG_MSG, error_index=0"
# 5-byte JOIN-sized with msg_type=5 ⇒ wrong msg_type.
RESP=$(raw_udp "$PORT" "0500000001")
assert_wrong_msg "$RESP" 0 "msg_type=5"
# Echo should be: 05 00000001 00 00 00 00 00 00 00 (padded to 12 bytes).
assert_echo_prefix "$RESP" "0500000001" "msg_type=5 echo"

echo "Test 2: msg_type=99 ⇒ WRONG_MSG, error_index=0"
RESP=$(raw_udp "$PORT" "6300000001")
assert_wrong_msg "$RESP" 0 "msg_type=99"

echo "Test 3: msg_type=255 (0xFF) ⇒ WRONG_MSG, error_index=0"
RESP=$(raw_udp "$PORT" "ff00000001")
assert_wrong_msg "$RESP" 0 "msg_type=255"

# -------------------------------------------------------------------------
# 2. Truncated messages (correct msg_type, insufficient bytes).
# error_index for invalid_length(n) is `n` (the actual received byte count).
# -------------------------------------------------------------------------
echo "Test 4: truncated MSG_JOIN (only msg_type byte, no player_id) ⇒ error_index=1"
# Send a single byte: msg_type=0 (JOIN). JOIN needs 5 bytes. Got 1.
RESP=$(raw_udp "$PORT" "00")
# deserialize consumes msg_type (1 byte), then sees bytes.size()=0, expected=4.
# invalid_length gets `bytes.size() + MSG_TYPE_SIZE = 0 + 1 = 1`.
assert_wrong_msg "$RESP" 1 "truncated JOIN len=1"

echo "Test 5: truncated MSG_MOVE_1 (5 bytes instead of 10) ⇒ error_index=5"
# MOVE_1 requires 10 bytes (type+player+game+pawn = 1+4+4+1).
# Send: 01 00000001 (6 bytes? No, 5 bytes = type + 4 for player).
# Actually "0100000001" is 5 bytes. After reading type, 4 bytes remain.
RESP=$(raw_udp "$PORT" "0100000001")
# invalid_length gets 1 (type) + 4 (rest) = 5.
assert_wrong_msg "$RESP" 5 "truncated MOVE_1 len=5"

echo "Test 6: truncated MSG_KEEP_ALIVE (5 bytes instead of 9) ⇒ error_index=5"
# KEEP_ALIVE requires 9 bytes (type+player+game).
RESP=$(raw_udp "$PORT" "0300000001")
assert_wrong_msg "$RESP" 5 "truncated KEEP_ALIVE len=5"

echo "Test 7: truncated MSG_GIVE_UP (5 bytes instead of 9) ⇒ error_index=5"
RESP=$(raw_udp "$PORT" "0400000001")
assert_wrong_msg "$RESP" 5 "truncated GIVE_UP len=5"

# -------------------------------------------------------------------------
# 3. Oversized messages (correct msg_type but extra trailing bytes).
# -------------------------------------------------------------------------
echo "Test 8: oversized MSG_JOIN (6 bytes: type+player+1 extra) ⇒ WRONG_MSG"
# Send 6 bytes: type=0 + 4-byte player_id + 1 trailing garbage.
RESP=$(raw_udp "$PORT" "0000000001ff")
# Expected size for JOIN is 5; received 6. First unparseable byte is at
# index 5 (the trailing garbage) ⇒ error_index = min(5, 6) = 5.
assert_wrong_msg "$RESP" 5 "oversized JOIN len=6"

echo "Test 9: oversized MSG_MOVE_1 (11 bytes instead of 10) ⇒ WRONG_MSG"
# MOVE_1 format = 10 bytes. Server buffer is 12, so it can read 11.
# bytes: 01 00000001 00000000 00 + 1 trailing = 11 bytes.
RESP=$(raw_udp "$PORT" "0100000001000000000000")
# Expected=10, received=11. First unparseable byte is at index 10 ⇒
# error_index = min(10, 11) = 10.
assert_wrong_msg "$RESP" 10 "oversized MOVE_1 len=11"

echo "Test 10: very oversized datagram (30 bytes) — server reads only buffer size"
# Server's recvfrom buffer is CLIENT_MESSAGE_SIZE_WITH_BUF = 12 bytes.
# A 30-byte datagram's first 12 bytes are read. Type byte = 0 (JOIN).
# Server sees 12 bytes, expects 5 for JOIN. First unparseable byte is at
# index 5 ⇒ error_index = min(5, 12) = 5.
BIG=$(printf '00%.0s' {1..30})
RESP=$(raw_udp "$PORT" "$BIG")
assert_wrong_msg "$RESP" 5 "oversized 30-byte datagram"

# -------------------------------------------------------------------------
# 4. Empty datagram.
# -------------------------------------------------------------------------
echo "Test 11: empty datagram (0 bytes)"
python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2.0)
s.sendto(b'', ('127.0.0.1', int('$PORT')))
try:
    resp, _ = s.recvfrom(4096)
    print(resp.hex())
except socket.timeout:
    print('TIMEOUT')
" >/tmp/empty_resp.$$ 2>&1
RESP=$(cat /tmp/empty_resp.$$)
rm -f /tmp/empty_resp.$$
assert_wrong_msg "$RESP" 0 "empty datagram"
assert_echo_prefix "$RESP" "" "empty datagram echo (all zeros)"

echo "All malformed_packets tests passed."
