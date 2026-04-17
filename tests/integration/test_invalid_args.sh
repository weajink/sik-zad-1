#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

echo "Test 1: Client with missing args exits with code 1"
run_client_raw -a 127.0.0.1
assert_exit_code 1

echo "Test 2: Client with invalid port exits with code 1"
run_client_raw -a 127.0.0.1 -p abc -m "0/42" -t 1
assert_exit_code 1

echo "Test 3: Client with invalid message format exits with code 1"
run_client_raw -a 127.0.0.1 -p 12345 -m "invalid" -t 1
assert_exit_code 1

echo "Test 4: Client with player_id=0 exits with code 1"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/0" -t 1
assert_exit_code 1

echo "Test 5: Client with wrong field count exits with code 1"
run_client_raw -a 127.0.0.1 -p 12345 -m "0/42/extra" -t 1
assert_exit_code 1

echo "All invalid_args tests passed."
