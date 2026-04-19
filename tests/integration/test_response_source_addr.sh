#!/usr/bin/env bash
# task.txt §3.3: "Serwer odsyła komunikat do klienta na adres i numer
# portu, z których odebrał komunikat od tego klienta."
#
# Additionally, the client has a check to reject responses from unexpected
# sources (kayles_client.cpp). Verify:
#   * Response source is always the server's bind address/port.
#   * If we send from port P and receive on port P, the server replies to P.
#   * Multiple clients on different ephemeral ports each get their own reply.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

trap stop_server EXIT
PORT=$(get_random_port)
start_server "111" "$PORT" 30

echo "Test 1: Response comes from the server's bind port"
# Send a JOIN from an explicit ephemeral port and check the source.
python3 <<PY
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 0))  # any ephemeral port
local_port = s.getsockname()[1]
s.settimeout(2.0)
data = bytes.fromhex('0000000001')  # JOIN player=1
s.sendto(data, ('127.0.0.1', $PORT))
try:
    resp, src = s.recvfrom(4096)
except socket.timeout:
    print("TIMEOUT"); sys.exit(1)
# Source must be 127.0.0.1:<server port>, not some other port.
if src != ('127.0.0.1', $PORT):
    print(f"FAIL: response from {src}, expected 127.0.0.1:$PORT")
    sys.exit(1)
print(f"OK: response from {src}")
PY

echo "Test 2: Two clients on different local ports each get their own response"
python3 <<PY
import socket, sys
s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s1.bind(('127.0.0.1', 0))
s1.settimeout(2.0)
s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s2.bind(('127.0.0.1', 0))
s2.settimeout(2.0)

# Client 1 sends JOIN (player 11). Client 2 sends JOIN (player 22).
s1.sendto(bytes.fromhex('000000000b'), ('127.0.0.1', $PORT))
r1, src1 = s1.recvfrom(4096)
s2.sendto(bytes.fromhex('0000000016'), ('127.0.0.1', $PORT))
r2, src2 = s2.recvfrom(4096)

# Both sources must be the server.
if src1 != ('127.0.0.1', $PORT) or src2 != ('127.0.0.1', $PORT):
    print(f"FAIL: srcs {src1} {src2}, expected 127.0.0.1:$PORT")
    sys.exit(1)

# GameState wire layout:
#   [0:4]  = game_id
#   [4:8]  = player_a_id
#   [8:12] = player_b_id
#   [12]   = status
#
# Expected for r1 (WAITING): player_a=11 (0x0000000b), player_b=0, status=0.
# Expected for r2 (TURN_B):  player_a=11 (0x0000000b), player_b=22 (0x00000016), status=2.
if r1[4:8] != bytes.fromhex('0000000b'):
    print(f"FAIL: r1 player_a should be 11, got r1={r1.hex()}")
    sys.exit(1)
if r1[8:12] != bytes.fromhex('00000000'):
    print(f"FAIL: r1 player_b should be 0, got r1={r1.hex()}")
    sys.exit(1)
if r1[12] != 0:
    print(f"FAIL: r1 status should be 0 (WAITING), got r1={r1.hex()}")
    sys.exit(1)

if r2[4:8] != bytes.fromhex('0000000b'):
    print(f"FAIL: r2 player_a should be 11, got r2={r2.hex()}")
    sys.exit(1)
if r2[8:12] != bytes.fromhex('00000016'):
    print(f"FAIL: r2 player_b should be 22, got r2={r2.hex()}")
    sys.exit(1)
if r2[12] != 2:
    print(f"FAIL: r2 status should be 2 (TURN_B), got r2={r2.hex()}")
    sys.exit(1)

print("OK: both responses came from server, correct payloads")
PY

echo "All response_source_addr tests passed."
