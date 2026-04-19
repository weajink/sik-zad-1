#!/usr/bin/env bash
# task.txt §3.3: "Inne komunikaty klienta są poprawne, jeśli mają poprawną
# długość i zawierają poprawne wartości w polach msg_type, player_id i
# game_id (istnieje podana gra i uczestniczy w niej podany gracz)."
#
# Therefore a MOVE/KEEP_ALIVE/GIVE_UP for a nonexistent game_id ⇒ WRONG_MSG.
# The error_index for INVALID_GAME_ID is offset-of-game_id = 1 (type) + 4
# (player_id) = 5.
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
PORT=$(get_random_port)
start_server "111" "$PORT" 30

echo "Test 1: MOVE_1 with nonexistent game_id ⇒ WRONG_MSG, error_index=5"
# MOVE_1 type=1, player_id=42, game_id=99999, pawn=0. 42=0x2A. 99999=0x0001869F.
RESP=$(raw_udp "$PORT" "010000002a0001869f00")
if [[ "$RESP" == "TIMEOUT" ]]; then
    echo "  FAIL: server did not respond"
    exit 1
fi
if [[ ${#RESP} -ne 28 ]]; then
    echo "  FAIL: expected 28 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:24:2}" != "ff" ]]; then
    echo "  FAIL: status should be 0xFF, got ${RESP:24:2}"
    exit 1
fi
if [[ "${RESP:26:2}" != "05" ]]; then
    echo "  FAIL: error_index should be 0x05 (game_id offset), got ${RESP:26:2}"
    exit 1
fi

echo "Test 2: KEEP_ALIVE with nonexistent game_id ⇒ WRONG_MSG, error_index=5"
# KEEP_ALIVE type=3, player_id=42, game_id=99999.
RESP=$(raw_udp "$PORT" "030000002a0001869f")
if [[ "${RESP:26:2}" != "05" ]]; then
    echo "  FAIL: error_index should be 5, got ${RESP:26:2}"
    exit 1
fi

echo "Test 3: GIVE_UP with nonexistent game_id ⇒ WRONG_MSG, error_index=5"
RESP=$(raw_udp "$PORT" "040000002a0001869f")
if [[ "${RESP:26:2}" != "05" ]]; then
    echo "  FAIL: error_index should be 5, got ${RESP:26:2}"
    exit 1
fi

echo "Test 4: MOVE_2 where player is not a participant ⇒ WRONG_MSG, error_index=1"
# Create a game with players 11 and 22.
run_client "$PORT" "0/11"
run_client "$PORT" "0/22"
GAME_ID=$(echo "$CLIENT_STDOUT" | grep -oE 'Game [0-9]+' | head -1 | awk '{print $2}')

# Encode game_id and the non-participating player_id into 8 hex chars each
# (big-endian u32). Player 99 is not in this game.
GID_HEX=$(printf '%08x' "$GAME_ID")
PID_HEX=$(printf '%08x' 99)

# Wire: type(02) + player_id(4) + game_id(4) + pawn(00) = 10 bytes = 20 hex chars.
HEX="02${PID_HEX}${GID_HEX}00"
if [[ ${#HEX} -ne 20 ]]; then
    echo "  FAIL [setup]: constructed hex should be 20 chars, got ${#HEX}: $HEX"
    exit 1
fi

RESP=$(raw_udp "$PORT" "$HEX")
if [[ ${#RESP} -ne 28 ]]; then
    echo "  FAIL: expected 28 hex chars, got ${#RESP}: $RESP"
    exit 1
fi
if [[ "${RESP:24:2}" != "ff" ]]; then
    echo "  FAIL: status should be 0xFF, got ${RESP:24:2}"
    exit 1
fi
# Updated spec: nonzero player_id is never "invalid" — a player-not-in-game
# mismatch is attributed to the game_id field at MSG_TYPE_SIZE + PLAYER_ID_SIZE = 5.
if [[ "${RESP:26:2}" != "05" ]]; then
    echo "  FAIL: error_index should be 0x05 (game_id offset), got ${RESP:26:2}"
    exit 1
fi

echo "All wrong_game_id tests passed."
