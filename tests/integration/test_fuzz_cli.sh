#!/usr/bin/env bash
# CLI fuzz — random inputs to kayles_server and kayles_client.
#
# Axis 8 from the randomized integration suite spec.
#
# For each flag of each binary, generate random strings covering:
#   - non-digit chars, leading zeros, empty strings, very long strings,
#     Unicode, control chars, strings with slashes in -m, extreme numbers;
# Invalid inputs MUST produce non-zero exit (1) and NOT crash (no SIGSEGV,
# no SIGABRT). Valid-but-randomly-generated inputs MUST be accepted
# (exit 0 for the server we test with a brief spawn-then-kill, exit 0 or
# 1 for the client depending on whether we spun up a listener).
#
# The test never hangs: we use timeout() for every subprocess call.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

SEED="${RANDOM_SEED:-$RANDOM}"
echo "RANDOM_SEED=$SEED"

SERVER_BIN_PATH="$SERVER_BIN"
CLIENT_BIN_PATH="$CLIENT_BIN"

python3 - "$SERVER_BIN_PATH" "$CLIENT_BIN_PATH" "$SEED" <<'PYEOF'
import os, random, signal, socket, string, subprocess, sys, time

SERVER_BIN = sys.argv[1]
CLIENT_BIN = sys.argv[2]
SEED = int(sys.argv[3])
random.seed(SEED)

failures = []

def run(cmd, timeout=3):
    """Run cmd with timeout. Return (rc, stdout, stderr) or signal on kill."""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout,
                           text=True)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        return -1, e.stdout or '', e.stderr or ''
    except Exception as e:
        return -2, '', str(e)

def expect_reject(label, cmd):
    rc, _, err = run(cmd, timeout=3)
    # Reject = exit code 1. Anything else is bad.
    if rc < 0:
        failures.append(
            f"{label}: crashed or hung (rc={rc}) cmd={cmd!r} err={err[:200]!r}")
        return
    # Exit code 1 expected. Signal-death would show up as 128+signal (bash) or
    # -signal (python subprocess). Anything other than 1 is suspect.
    if rc != 1:
        # Some "valid" random inputs may leak through — we only flag
        # EXPECTED-INVALID cases here, so rc must be 1.
        failures.append(
            f"{label}: expected exit 1 for invalid input, got rc={rc} "
            f"cmd={cmd!r}")

def rand_gibberish(min_len=0, max_len=40):
    n = random.randint(min_len, max_len)
    # Mix of control chars, unicode, weirdness. Avoid NUL so argv works.
    pool = (string.ascii_letters + string.digits + string.punctuation
            + '\t\r\x01\x02\x03\x1b' + ' ' + '\u00e9\u00f1\u2603')
    return ''.join(random.choice(pool) for _ in range(n))

def rand_non_digit_string(n=10):
    pool = string.ascii_letters + string.punctuation + ' '
    return ''.join(random.choice(pool) for _ in range(n))

# ============================================================================
# CLIENT -p (port): 1..65535. Invalid: empty, 0, 65536, non-digits, huge
# numbers, leading +/-.
# ============================================================================
print("CLIENT -p invalid")
invalid_ports = [
    '', '0',                # spec says port 1..2^16-1 for client
    '65536', '100000',
    'abc', '1.5', '-1', '+1', ' 12', '12 ',
    '\t',
    'x' * 50,
]
# Add a handful of random non-digit strings.
for _ in range(10):
    invalid_ports.append(rand_non_digit_string(random.randint(1, 12)))

for p in invalid_ports:
    expect_reject(
        f"client -p {p!r}",
        [CLIENT_BIN, '-a', '127.0.0.1', '-p', p, '-m', '0/1', '-t', '1'])

# ============================================================================
# CLIENT -t (timeout): 1..99. Invalid: 0, 100, non-digit, huge.
# ============================================================================
print("CLIENT -t invalid")
invalid_t = [
    '', '0', '100', '255', '999',
    'abc', '1.0', '-1', '+1', '0x1', 'x' * 30,
]
for _ in range(5):
    invalid_t.append(rand_non_digit_string(random.randint(1, 10)))
for t in invalid_t:
    expect_reject(
        f"client -t {t!r}",
        [CLIENT_BIN, '-a', '127.0.0.1', '-p', '12345', '-m', '0/1', '-t', t])

# ============================================================================
# CLIENT -m (message): invalid formats.
# ============================================================================
print("CLIENT -m invalid")
invalid_m = [
    '',
    '/',              # empty token
    '//',             # multiple empty
    '0/',             # trailing slash
    '/0/1',           # leading slash
    '0/0',            # player_id=0 is invalid
    '5/1',            # msg_type 5 is invalid (>4)
    '-1/1',           # negative msg_type
    '99/1',           # huge msg_type
    'abc/1',
    '0',              # missing player_id
    '0/1/2',          # JOIN doesn't take game_id
    '1/1/2',          # MOVE_1 missing pawn
    '1/1/2/3/4',      # MOVE_1 too many fields
    '2/1/2',          # MOVE_2 missing pawn
    '3/1',            # KA missing game_id
    '3/1/2/3',        # KA too many fields
    '4/1',            # GU missing game_id
    '4/1/2/3',        # GU too many fields
    '0/4294967296',   # player_id overflow (>UINT32_MAX)
    '1/1/4294967296/0',
    '1/1/2/256',      # pawn > 255
    '1/1/2/-1',
    '0/+1',           # unary plus not allowed by from_chars
]
# Some random gibberish.
for _ in range(20):
    invalid_m.append(rand_gibberish(1, 30))
for m in invalid_m:
    expect_reject(
        f"client -m {m!r}",
        [CLIENT_BIN, '-a', '127.0.0.1', '-p', '12345', '-m', m, '-t', '1'])

# ============================================================================
# CLIENT -a (address): truly malformed addresses. We pick strings that
# getaddrinfo cannot parse. Note that "localhost" etc. ARE valid per spec
# (parse_address uses getaddrinfo), so avoid strings that happen to resolve.
# ============================================================================
print("CLIENT -a invalid")
invalid_a = [
    '',
    '256.256.256.256',
    '1.2.3.4.5',
    '999.999.999.999',
    'not_a_host_@@@',
    'http://127.0.0.1',
    '\x01',
    '-invalid-domain-name-that-cannot-possibly-exist-123456789.invalid',
]
for a in invalid_a:
    # Some of these may succeed on DNS (unlikely). If they do, expect_reject
    # flags it. We accept that as a legitimate concern — invalid-looking
    # addresses shouldn't resolve.
    # Use a larger timeout since getaddrinfo can take several seconds on
    # hostnames that don't resolve.
    rc, _, err = run(
        [CLIENT_BIN, '-a', a, '-p', '12345', '-m', '0/1', '-t', '1'],
        timeout=10)
    if rc != 1:
        failures.append(
            f"client -a {a!r}: expected rc=1 for invalid address, got rc={rc}")

# ============================================================================
# CLIENT missing required flags.
# ============================================================================
print("CLIENT missing args")
for cmd in [
    [CLIENT_BIN],
    [CLIENT_BIN, '-a', '127.0.0.1'],
    [CLIENT_BIN, '-a', '127.0.0.1', '-p', '12345'],
    [CLIENT_BIN, '-a', '127.0.0.1', '-p', '12345', '-m', '0/1'],
    [CLIENT_BIN, '-a', '127.0.0.1', '-p', '12345', '-t', '1'],
    [CLIENT_BIN, '-p', '12345', '-m', '0/1', '-t', '1'],
]:
    expect_reject(f"client missing args {cmd!r}", cmd)

# ============================================================================
# CLIENT positional args / unknown flags.
# ============================================================================
print("CLIENT positional / unknown")
for cmd in [
    [CLIENT_BIN, '-a', '127.0.0.1', '-p', '1', '-m', '0/1', '-t', '1', 'garbage'],
    [CLIENT_BIN, '-x', 'foo', '-a', '127.0.0.1', '-p', '1', '-m', '0/1', '-t', '1'],
]:
    expect_reject(f"client extra {cmd!r}", cmd)

# ============================================================================
# SERVER -r invalid pawn_row.
# ============================================================================
print("SERVER -r invalid")
invalid_r = [
    '',
    '0',             # must have first and last '1'
    '01',            # first must be '1'
    '10',            # last must be '1'
    '011',           # first must be '1'
    '1' * 257,       # too long
    '2',             # non-0/1 char
    '1a1',
    '1 1',
    '\t',
    'abc',
]
# Random gibberish of random length.
for _ in range(10):
    invalid_r.append(rand_gibberish(1, 20))
# Generate a random alphanum string that will fail (not 0/1 chars).
for _ in range(5):
    n = random.randint(1, 30)
    s = ''.join(random.choice('abcXYZ23456789') for _ in range(n))
    invalid_r.append(s)
# Server needs a port it can bind; pick an ephemeral one per call.
def with_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p
for r in invalid_r:
    p = with_free_port()
    expect_reject(
        f"server -r {r!r}",
        [SERVER_BIN, '-r', r, '-a', '127.0.0.1', '-p', str(p), '-t', '10'])

# ============================================================================
# SERVER -t invalid (1..99).
# ============================================================================
print("SERVER -t invalid")
invalid_t_srv = [
    '', '0', '100', '255', '999',
    'abc', '1.0', '-1', '+1', '0x1',
]
for _ in range(5):
    invalid_t_srv.append(rand_non_digit_string(random.randint(1, 10)))
for t in invalid_t_srv:
    p = with_free_port()
    expect_reject(
        f"server -t {t!r}",
        [SERVER_BIN, '-r', '1', '-a', '127.0.0.1', '-p', str(p), '-t', t])

# ============================================================================
# SERVER missing required flags.
# ============================================================================
print("SERVER missing args")
for cmd in [
    [SERVER_BIN],
    [SERVER_BIN, '-r', '1'],
    [SERVER_BIN, '-r', '1', '-a', '127.0.0.1'],
    [SERVER_BIN, '-r', '1', '-a', '127.0.0.1', '-p', '1234'],
]:
    expect_reject(f"server missing args {cmd!r}", cmd)

# ============================================================================
# SERVER positional / unknown flags.
# ============================================================================
print("SERVER positional / unknown")
for cmd in [
    [SERVER_BIN, '-r', '1', '-a', '127.0.0.1', '-p', '1', '-t', '1', 'extra'],
    [SERVER_BIN, '-q', 'q', '-r', '1', '-a', '127.0.0.1', '-p', '1', '-t', '1'],
]:
    expect_reject(f"server extra {cmd!r}", cmd)

# ============================================================================
# VALID random client CLI — must be accepted (or gracefully exit on timeout).
# Spec: "Jeśli nie otrzyma odpowiedzi, wypisuje na standardowe wyjście
# stosowny komunikat i kończy się kodem 0."
# We point the client at a port nobody listens on; it should exit 0 after
# the timeout.
# ============================================================================
print("CLIENT valid random accepted")
for _ in range(20):
    # Valid: msg_type in 0..4, valid number of fields, player_id != 0.
    mt = random.randint(0, 4)
    pid = random.randint(1, 10**9)
    gid = random.randint(0, 10**9)
    pawn = random.randint(0, 255)
    if mt == 0:
        m = f"{mt}/{pid}"
    elif mt in (3, 4):
        m = f"{mt}/{pid}/{gid}"
    else:
        m = f"{mt}/{pid}/{gid}/{pawn}"
    t = random.randint(1, 3)
    p = random.randint(1, 65535)
    rc, _, err = run(
        [CLIENT_BIN, '-a', '127.0.0.1', '-p', str(p), '-m', m, '-t', str(t)],
        timeout=t + 3)
    if rc != 0:
        failures.append(
            f"valid client -m={m!r} -p={p} -t={t}: expected rc=0, got {rc}. "
            f"stderr={err[:200]!r}")

# ============================================================================
# VALID random server CLI — spawn, confirm it binds, kill. We use free port.
# ============================================================================
print("SERVER valid random accepted")
for _ in range(10):
    # Valid pawn_row: length 1..256, first and last '1', chars 0/1.
    n = random.randint(1, 32)
    mid = ''.join(random.choice('01') for _ in range(max(0, n - 2)))
    r = '1' + mid + ('1' if n >= 2 else '')
    t = random.randint(1, 99)
    port = with_free_port()
    proc = subprocess.Popen(
        [SERVER_BIN, '-r', r, '-a', '127.0.0.1', '-p', str(port),
         '-t', str(t)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.15)
    if proc.poll() is not None:
        # Server exited unexpectedly.
        out, err = proc.communicate(timeout=2)
        failures.append(
            f"valid server -r={r!r} -t={t} -p={port}: exit "
            f"{proc.returncode}, err={err[:200]!r}")
        continue
    proc.terminate()
    try: proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait()

if failures:
    print(f"\n{len(failures)} FAILURES (first 30)")
    for f in failures[:30]:
        print("  " + f)
    sys.exit(1)

print("\nAll CLI fuzz tests OK.")
PYEOF

echo "All fuzz_cli tests passed."
