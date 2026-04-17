#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVER_BIN="$REPO_ROOT/src/kayles_server"
CLIENT_BIN="$REPO_ROOT/src/kayles_client"

# Verify binaries exist
if [[ ! -x "$SERVER_BIN" ]]; then
    echo "ERROR: Server binary not found at $SERVER_BIN"
    echo "Run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build"
    exit 1
fi
if [[ ! -x "$CLIENT_BIN" ]]; then
    echo "ERROR: Client binary not found at $CLIENT_BIN"
    exit 1
fi

# --- Helper functions ---

SERVER_PID=""

get_random_port() {
    echo $(( (RANDOM % 10000) + 20000 ))
}

start_server() {
    # Usage: start_server <pawn_row> <port> <timeout>
    local pawn_row="$1"
    local port="$2"
    local timeout="${3:-10}"

    "$SERVER_BIN" -r "$pawn_row" -a 127.0.0.1 -p "$port" -t "$timeout" &
    SERVER_PID=$!
    # Wait for server to start listening
    sleep 0.3
    # Verify server is still running
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "ERROR: Server failed to start"
        SERVER_PID=""
        return 1
    fi
}

stop_server() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

# Run client and capture output
# Usage: run_client <port> <message> [timeout]
# Sets: CLIENT_STDOUT, CLIENT_STDERR, CLIENT_EXIT
run_client() {
    local port="$1"
    local message="$2"
    local timeout="${3:-2}"

    local tmpout tmperr
    tmpout=$(mktemp)
    tmperr=$(mktemp)

    set +e
    "$CLIENT_BIN" -a 127.0.0.1 -p "$port" -m "$message" -t "$timeout" \
        >"$tmpout" 2>"$tmperr"
    CLIENT_EXIT=$?
    set -e

    CLIENT_STDOUT=$(cat "$tmpout")
    CLIENT_STDERR=$(cat "$tmperr")
    rm -f "$tmpout" "$tmperr"
}

# Run client without -m flag (for invalid args tests)
# Usage: run_client_raw <args...>
# Sets: CLIENT_STDOUT, CLIENT_STDERR, CLIENT_EXIT
run_client_raw() {
    local tmpout tmperr
    tmpout=$(mktemp)
    tmperr=$(mktemp)

    set +e
    "$CLIENT_BIN" "$@" >"$tmpout" 2>"$tmperr"
    CLIENT_EXIT=$?
    set -e

    CLIENT_STDOUT=$(cat "$tmpout")
    CLIENT_STDERR=$(cat "$tmperr")
    rm -f "$tmpout" "$tmperr"
}

# Assertion helpers
assert_exit_code() {
    local expected="$1"
    if [[ "$CLIENT_EXIT" -ne "$expected" ]]; then
        echo "  FAIL: Expected exit code $expected, got $CLIENT_EXIT"
        echo "  STDOUT: $CLIENT_STDOUT"
        echo "  STDERR: $CLIENT_STDERR"
        return 1
    fi
}

assert_stdout_contains() {
    local pattern="$1"
    if ! echo "$CLIENT_STDOUT" | grep -qE "$pattern"; then
        echo "  FAIL: stdout does not contain pattern: $pattern"
        echo "  STDOUT: $CLIENT_STDOUT"
        return 1
    fi
}

assert_stdout_not_contains() {
    local pattern="$1"
    if echo "$CLIENT_STDOUT" | grep -qE "$pattern"; then
        echo "  FAIL: stdout unexpectedly contains pattern: $pattern"
        echo "  STDOUT: $CLIENT_STDOUT"
        return 1
    fi
}

assert_stderr_contains() {
    local pattern="$1"
    if ! echo "$CLIENT_STDERR" | grep -qE "$pattern"; then
        echo "  FAIL: stderr does not contain pattern: $pattern"
        echo "  STDERR: $CLIENT_STDERR"
        return 1
    fi
}

# --- Test runner ---

# If sourced by a test script, just export helpers and return
if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
    return 0
fi

# Main test runner
echo "=== Kayles Integration Tests ==="
echo ""

TOTAL=0
PASSED=0
FAILED=0
FAILED_TESTS=""

for test_file in "$SCRIPT_DIR"/test_*.sh; do
    if [[ ! -f "$test_file" ]]; then
        continue
    fi

    test_name=$(basename "$test_file" .sh)
    TOTAL=$((TOTAL + 1))

    echo -n "Running $test_name ... "

    tmplog=$(mktemp)
    set +e
    bash "$test_file" > "$tmplog" 2>&1
    result=$?
    set -e

    if [[ $result -eq 0 ]]; then
        echo "PASS"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL"
        FAILED=$((FAILED + 1))
        FAILED_TESTS="$FAILED_TESTS  - $test_name\n"
        # Show output for failed tests
        echo "--- output ---"
        cat "$tmplog"
        echo "--- end ---"
        echo ""
    fi

    rm -f "$tmplog"
done

echo ""
echo "=== Summary ==="
echo "Total: $TOTAL  Passed: $PASSED  Failed: $FAILED"

if [[ $FAILED -gt 0 ]]; then
    echo ""
    echo "Failed tests:"
    echo -e "$FAILED_TESTS"
    exit 1
fi

echo "All tests passed."
exit 0
