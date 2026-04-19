#!/usr/bin/env bash
# Timeout / retention fuzz.
#
# Axis 5 from the randomized integration suite spec.
#
# Runs:
#   A. During a TURN_X game: one player goes silent longer than
#      server_timeout. Verify: silent player loses when server wakes up.
#   B. KEEP_ALIVE by either participant preserves an active game across a
#      silence interval shorter than server_timeout.
#   C. Finished games are queryable for `server_timeout` seconds after the
#      last valid message, then purged (queries return WRONG_MSG).
#   D. Client -t timeout fuzz: send a MSG_JOIN to a dead port and measure
#      actual wall-clock time. Assert client returns within a sane window
#      around the configured timeout.
#
# `server_timeout` is randomized in {1, 2, 3} per A/B/C to exercise the
# boundary. Minimum allowed by spec is 1 second. We generously account for
# scheduling jitter with explicit sleeps.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

SEED="${RANDOM_SEED:-$RANDOM}"
echo "RANDOM_SEED=$SEED"

SERVER_BIN_PATH="$SERVER_BIN"
CLIENT_BIN_PATH="$CLIENT_BIN"

python3 - "$SERVER_BIN_PATH" "$CLIENT_BIN_PATH" "$SEED" <<'PYEOF'
import atexit, random, socket, struct, subprocess, sys, time

SERVER_BIN = sys.argv[1]
CLIENT_BIN = sys.argv[2]
SEED = int(sys.argv[3])
random.seed(SEED)

_servers = []
def cleanup():
    for p in _servers:
        try: p.terminate(); p.wait(timeout=3)
        except Exception:
            try: p.kill()
            except Exception: pass
atexit.register(cleanup)

def pick_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p

def start_server(pawn_row, timeout_s):
    port = pick_port()
    proc = subprocess.Popen(
        [SERVER_BIN, '-r', pawn_row, '-a', '127.0.0.1', '-p', str(port),
         '-t', str(timeout_s)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _servers.append(proc); time.sleep(0.2)
    if proc.poll() is not None:
        raise RuntimeError("server exited on startup")
    return proc, port

def stop_proc(proc):
    try: proc.terminate(); proc.wait(timeout=3)
    except Exception:
        try: proc.kill()
        except Exception: pass
    if proc in _servers: _servers.remove(proc)

def parse_state(resp, max_pawn):
    header_size = 14
    bitmap_size = max_pawn // 8 + 1
    if len(resp) != header_size + bitmap_size: return None
    if resp[12] == 0xFF: return None
    return {
        'game_id': struct.unpack('>I', resp[0:4])[0],
        'player_a': struct.unpack('>I', resp[4:8])[0],
        'player_b': struct.unpack('>I', resp[8:12])[0],
        'status': resp[12], 'max_pawn': resp[13],
        'pins': [bool((resp[14 + i // 8] >> (7 - (i % 8))) & 1)
                 for i in range(resp[13] + 1)],
    }

def is_wrong(resp):
    return len(resp) == 14 and resp[12] == 0xFF

def make_join(pid): return bytes([0]) + struct.pack('>I', pid)
def make_ka(pid, gid):
    return bytes([3]) + struct.pack('>I', pid) + struct.pack('>I', gid)

failures = []

# ============================================================================
# Sub-test A: silent player during TURN_X loses.
# Pick server_timeout in {1, 2, 3} and a random player to be silent.
# ============================================================================
for trial in range(3):
    st = random.choice([1, 2, 3])
    silent = random.choice(['A', 'B'])
    print(f"A[{trial}]: server_timeout={st}, silent={silent}")
    proc, port = start_server('1111', timeout_s=st)
    try:
        addr = ('127.0.0.1', port)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(3.0)
        pa = random.randint(1, 0xFFFFFFFE)
        pb = random.randint(1, 0xFFFFFFFE)
        while pb == pa: pb = random.randint(1, 0xFFFFFFFE)

        sock.sendto(make_join(pa), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if not s or s['status'] != 0:
            failures.append(f"A[{trial}] JOIN A: {resp.hex()}"); continue
        gid = s['game_id']
        sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if not s or s['status'] != 2:
            failures.append(f"A[{trial}] JOIN B: {resp.hex()}"); continue

        # Now wait longer than server_timeout while the NON-silent player keeps
        # sending KEEP_ALIVEs. That way only the silent player's last-message
        # timestamp grows stale.
        #
        # Sleep pattern: total_wait = st + 0.5 seconds. During it, non-silent
        # sends KEEP_ALIVE every 0.3s.
        active_player = pa if silent == 'B' else pb
        end_time = time.monotonic() + st + 0.8
        while time.monotonic() < end_time:
            sock.sendto(make_ka(active_player, gid), addr)
            try:
                sock.recvfrom(4096)
            except socket.timeout:
                pass
            time.sleep(0.3)

        # Now the silent player's time has expired. Next message from the
        # active player should trigger the timeout check and transition to
        # WIN for the ACTIVE player.
        sock.sendto(make_ka(active_player, gid), addr)
        resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if s is None:
            failures.append(
                f"A[{trial}]: expected GAME_STATE post-timeout, got "
                f"{resp.hex()}")
            continue
        # Silent player lost. Winner:
        #   silent=A -> WIN_B (4), silent=B -> WIN_A (3).
        expected = 4 if silent == 'A' else 3
        if s['status'] != expected:
            failures.append(
                f"A[{trial}] silent={silent}: expected status {expected}, "
                f"got {s['status']}")
    finally:
        stop_proc(proc)

# ============================================================================
# Sub-test B: KEEP_ALIVE from either participant preserves an active game.
# With server_timeout=2, send alternating KEEP_ALIVEs from A and B every 0.5s
# for 4 seconds (2 * server_timeout). Game must remain in TURN_B.
# ============================================================================
print("B: alternating KAs keep game alive across 2*server_timeout window")
proc, port = start_server('1111', timeout_s=2)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    pa = 42; pb = 99
    sock.sendto(make_join(pa), addr); sock.recvfrom(4096)
    sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 3)
    if not s or s['status'] != 2:
        failures.append(f"B: JOIN B: {resp.hex()}")
    else:
        gid = s['game_id']
        end = time.monotonic() + 4.0
        last_status = None
        while time.monotonic() < end:
            for pid in (pa, pb):
                sock.sendto(make_ka(pid, gid), addr)
                resp, _ = sock.recvfrom(4096)
                s = parse_state(resp, 3)
                if s is None:
                    failures.append(f"B: unexpected WRONG/garbage: {resp.hex()}")
                    break
                last_status = s['status']
            time.sleep(0.4)
        if last_status != 2:
            failures.append(f"B: game did not stay in TURN_B; last={last_status}")
finally:
    stop_proc(proc)

# ============================================================================
# Sub-test C: finished games are queryable for server_timeout seconds, then
# purged. Finish a game via GIVE_UP, wait server_timeout + slack, query again.
# ============================================================================
print("C: finished game purged after server_timeout")
st = 2
proc, port = start_server('1111', timeout_s=st)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    pa = 42; pb = 99
    sock.sendto(make_join(pa), addr); sock.recvfrom(4096)
    sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 3); gid = s['game_id']
    # GIVE_UP from B -> WIN_A.
    give_up_b = bytes([4]) + struct.pack('>I', pb) + struct.pack('>I', gid)
    sock.sendto(give_up_b, addr); resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 3)
    if not s or s['status'] != 3:
        failures.append(f"C: GIVE_UP: expected WIN_A, got "
                        f"{s['status'] if s else 'wrong/garbage'}")
    else:
        # Immediately queryable.
        sock.sendto(make_ka(pa, gid), addr); resp, _ = sock.recvfrom(4096)
        s2 = parse_state(resp, 3)
        if not s2 or s2['status'] != 3:
            failures.append(f"C: immediate post-game KA: {resp.hex()}")
        # Wait longer than server_timeout.
        time.sleep(st + 1.2)
        # Game should now be purged. KA returns WRONG_MSG.
        sock.sendto(make_ka(pa, gid), addr); resp, _ = sock.recvfrom(4096)
        if not is_wrong(resp):
            failures.append(
                f"C: game not purged after {st + 1.2}s; resp={resp.hex()}")
finally:
    stop_proc(proc)

# ============================================================================
# Sub-test D: client -t timeout fuzz — send to a dead port, measure actual
# wait. Client should wait ~client_timeout seconds then exit with code 0
# (spec: "wypisuje na standardowe wyjście stosowny komunikat i kończy się
# kodem 0").
# We bind a UDP socket to hold the port open (no reads) so the client gets
# nothing back but the port is valid.
# ============================================================================
print("D: client -t timeout tolerance")
for t in (1, 2, 3):
    # We need a port that responds ... oh wait, we want the port to exist
    # (not ICMP "port unreachable") but never answer. Bind a socket and
    # don't read.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    # Send a valid-looking JOIN so the sink just silently absorbs it.
    start = time.monotonic()
    try:
        r = subprocess.run(
            [CLIENT_BIN, '-a', '127.0.0.1', '-p', str(port),
             '-m', f'0/{random.randint(1, 10000)}', '-t', str(t)],
            capture_output=True, timeout=t + 5, text=True)
    except subprocess.TimeoutExpired:
        failures.append(f"D t={t}: client hung past {t + 5}s")
        s.close(); continue
    elapsed = time.monotonic() - start
    s.close()
    # Per spec, client prints a message and exits 0.
    if r.returncode != 0:
        failures.append(f"D t={t}: client exit code {r.returncode} "
                        f"(stderr={r.stderr[:200]!r})")
    # Elapsed should be ~t seconds: lower bound t - 0.3s (slight leeway for
    # SO_RCVTIMEO rounding), upper bound t + 2s (generous for fork/exec/etc).
    if elapsed < t - 0.3 or elapsed > t + 2:
        failures.append(
            f"D t={t}: client waited {elapsed:.3f}s (expected ~{t}s)")
    else:
        print(f"  D t={t}: elapsed {elapsed:.3f}s OK")

if failures:
    print(f"\n{len(failures)} FAILURES")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("\nAll timeout fuzz tests OK.")
PYEOF

echo "All fuzz_timeouts tests passed."
