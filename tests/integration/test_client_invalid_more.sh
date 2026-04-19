#!/usr/bin/env bash
# Additional client CLI validation cases (task.txt §4):
#   * -p 0 is invalid for the client (client needs a real destination port).
#   * -t 0 and -t 100 are out of range (valid: 1..99).
#   * -m with trailing / (empty token) is rejected.
#   * -m with leading / (empty msg_type) is rejected.
#   * -m with double slashes is rejected.
#   * -m with whitespace is rejected.
#   * -m with hex/octal numbers is rejected (spec requires base 10).
#   * Repeated -m or -p (behavior should be "reasonable" — last wins or error).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

echo "Test 1: Client -p 0 is invalid"
run_client_raw -a 127.0.0.1 -p 0 -m "0/42" -t 1
assert_exit_code 1

echo "Test 2: Client -t 0 is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42" -t 0
assert_exit_code 1

echo "Test 3: Client -t 100 is invalid (max 99)"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42" -t 100
assert_exit_code 1

echo "Test 4: Client -t -1 is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42" -t -1
assert_exit_code 1

echo "Test 5: Client -m trailing slash is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42/" -t 1
assert_exit_code 1

echo "Test 6: Client -m leading slash is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "/0/42" -t 1
assert_exit_code 1

echo "Test 7: Client -m double slashes is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0//42" -t 1
assert_exit_code 1

echo "Test 8: Client -m with spaces is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0 / 42" -t 1
assert_exit_code 1

echo "Test 9: Client -m with hex (0xFF) msg_type is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0x1/42/0/0" -t 1
assert_exit_code 1

echo "Test 10: Client -m MOVE_1 missing pawn field is invalid"
# type=1 (MOVE_1) requires 4 fields: type/player/game/pawn.
run_client_raw -a 127.0.0.1 -p 12345 -m "1/42/7" -t 1
assert_exit_code 1

echo "Test 11: Client -m KEEP_ALIVE with extra pawn field is invalid"
# type=3 (KEEP_ALIVE) requires exactly 3 fields, no pawn.
run_client_raw -a 127.0.0.1 -p 12345 -m "3/42/7/0" -t 1
assert_exit_code 1

echo "Test 12: Client -m with msg_type > 4 is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "5/42" -t 1
assert_exit_code 1

echo "Test 13: Client -m JOIN with extra field is invalid"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42/0" -t 1
assert_exit_code 1

echo "Test 14: Client -m with pawn > 255 is invalid (pawn is uint8)"
run_client_raw -a 127.0.0.1 -p 12345 -m "1/42/0/256" -t 1
assert_exit_code 1

echo "Test 15: Client missing -a argument"
run_client_raw -p 12345 -m "0/42" -t 1
assert_exit_code 1

echo "Test 16: Client missing -m argument"
run_client_raw -a 127.0.0.1 -p 12345 -t 1
assert_exit_code 1

echo "Test 17: Client missing -t argument"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42"
assert_exit_code 1

echo "Test 18: Client with non-integer -p"
run_client_raw -a 127.0.0.1 -p 12.5 -m "0/42" -t 1
assert_exit_code 1

echo "Test 19: Client with -p 65536 (overflow for uint16)"
run_client_raw -a 127.0.0.1 -p 65536 -m "0/42" -t 1
assert_exit_code 1

echo "Test 20: Client with trailing extra positional argument"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42" -t 1 extra_garbage
assert_exit_code 1

echo "All client_invalid_more tests passed."
