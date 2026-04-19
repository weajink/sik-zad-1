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

# Responses should differ: r1 shows WAITING (player_a=11, b=0),
# r2 shows TURN_B (player_a=11, player_b=22).
if r1[8:12] != bytes.fromhex('0000000b') or r1[12] != 0:
    print(f"FAIL: r1 unexpected: {r1.hex()}")
    sys.exit(1)
if r2[8:12] != bytes.fromhex('0000000b') or r2[12:13] != bytes.fromhex('02'):
    print(f"FAIL: r2 status should be TURN_B: {r2.hex()}")
    sys.exit(1)
# player_b in r2 should be 22.
if r2[12-4:12] != bytes.fromhex('00000016'):
    # offset 8..12 is player_b in GameState layout.
    print(f"FAIL: r2 player_b bytes wrong: {r2.hex()}")
    sys.exit(1)
print(f"OK: both responses came from server, correct payloads")
PY

echo "All response_source_addr tests passed."
