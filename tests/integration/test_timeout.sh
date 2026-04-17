#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

echo "Test: Client times out when no server is running"

# Use a port where nothing is listening
PORT=$(get_random_port)

# Use -t 1 for fast timeout
run_client "$PORT" "0/42" 1
assert_exit_code 0
assert_stdout_contains "timeout"

echo "All timeout tests passed."
