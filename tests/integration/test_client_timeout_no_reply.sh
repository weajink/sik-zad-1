#!/usr/bin/env bash
# Client SO_RCVTIMEO behavior (task.txt §4):
#   "Klient czeka co najwyżej client_timeout sekund na odpowiedź serwera.
#    Jeśli nie otrzyma odpowiedzi, wypisuje na standardowe wyjście stosowny
#    komunikat i kończy się kodem 0."
#
# Additionally verify that the client does NOT use a fixed hard-coded
# timeout — if we pass -t 3, it should wait approximately 3 seconds before
# giving up.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

PORT=$(get_random_port)

echo "Test 1: With no server, client -t 1 exits 0 with timeout message within ~2s"
start_ts=$(date +%s%N)
run_client "$PORT" "0/42" 1
end_ts=$(date +%s%N)
elapsed_ms=$(( (end_ts - start_ts) / 1000000 ))
assert_exit_code 0
assert_stdout_contains "timeout|No response"
# Must be at least 900ms (near the 1s timeout) and at most ~2500ms to catch
# accidentally-huge timeouts.
if [[ "$elapsed_ms" -lt 900 ]]; then
    echo "  FAIL: client returned after only ${elapsed_ms}ms — timeout too short?"
    exit 1
fi
if [[ "$elapsed_ms" -gt 2500 ]]; then
    echo "  FAIL: client took ${elapsed_ms}ms with -t 1 — timeout not honored"
    exit 1
fi
echo "  OK: client timed out in ${elapsed_ms}ms with -t 1"

echo "Test 2: With no server, client -t 3 waits ~3s before giving up"
start_ts=$(date +%s%N)
run_client "$PORT" "0/42" 3
end_ts=$(date +%s%N)
elapsed_ms=$(( (end_ts - start_ts) / 1000000 ))
assert_exit_code 0
assert_stdout_contains "timeout|No response"
if [[ "$elapsed_ms" -lt 2900 ]]; then
    echo "  FAIL: client returned after only ${elapsed_ms}ms with -t 3"
    exit 1
fi
if [[ "$elapsed_ms" -gt 4500 ]]; then
    echo "  FAIL: client took ${elapsed_ms}ms with -t 3 — way too long"
    exit 1
fi
echo "  OK: client timed out in ${elapsed_ms}ms with -t 3"

echo "All client_timeout_no_reply tests passed."
