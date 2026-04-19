#!/usr/bin/env bash
# Adversarial game_id / player_id probing.
#
# Axis 6 from the randomized integration suite spec.
#
# Given a known active game:
#   - Spray random (game_id, player_id) tuples where both are nonzero and
#     wrong (either game_id unknown, or player_id not in the specified
#     game). Verify all elicit MSG_WRONG_MSG with the correct error_index
#     (4 + 1 = 5 for INVALID_GAME_ID or INVALID_PLAYER_ID, which both live
#     after the msg_type byte).
#   - After each such probe, the real game state must be unchanged (query
#     with a legitimate KEEP_ALIVE).
#
# Additionally:
#   - From a fresh server, fire many random JOINs from distinct player IDs
#     interleaved with the known spec rule "at most one WAITING game at a
#     time". Verify the pairing order is strictly FIFO (first unpaired JOIN
#     pairs with the next JOIN; the JOIN after that creates a new WAITING
#     game; etc).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

SEED="${RANDOM_SEED:-$RANDOM}"
echo "RANDOM_SEED=$SEED"

SERVER_BIN_PATH="$SERVER_BIN"

python3 - "$SERVER_BIN_PATH" "$SEED" <<'PYEOF'
import atexit, random, socket, struct, subprocess, sys, time

SERVER_BIN = sys.argv[1]
SEED = int(sys.argv[2])
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

def start_server(pawn_row, timeout_s=60):
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
def make_gu(pid, gid):
    return bytes([4]) + struct.pack('>I', pid) + struct.pack('>I', gid)
def make_move1(pid, gid, pawn):
    return bytes([1]) + struct.pack('>I', pid) + struct.pack('>I', gid) + bytes([pawn])

failures = []

# ============================================================================
# Part A: probe with random wrong (game_id, player_id) pairs.
# Real game: pa=12345, pb=67890 on game board "1111".
# ============================================================================
print("Part A: spraying wrong (gid, pid) probes at a known game")
proc, port = start_server('1111', timeout_s=60)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    pa = 12345; pb = 67890
    sock.sendto(make_join(pa), addr); sock.recvfrom(4096)
    sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 3)
    if not s or s['status'] != 2:
        failures.append(f"A: setup failed: {resp.hex()}")
    else:
        gid = s['game_id']
        initial_pins = list(s['pins'])  # [True]*4

        # Probe with many random wrong tuples.
        # Per kayles_game.h KaylesGameMap::{move,keep_alive,give_up}:
        #   - If gid not found -> invalid_game_id() with error_index =
        #     MSG_TYPE_SIZE + PLAYER_ID_SIZE = 5.
        #   - If player not in that game -> invalid_player_id() with
        #     error_index = 5.
        N = 500
        for i in range(N):
            # Choose what to mess up: game_id, player_id, or both.
            bad_game = random.choice([True, False])
            if bad_game:
                bad_gid = random.randint(0, 0xFFFFFFFF)
                while bad_gid == gid:
                    bad_gid = random.randint(0, 0xFFFFFFFF)
            else:
                bad_gid = gid
            # For player, pick a nonzero that's NOT in our game.
            bad_pid = random.randint(1, 0xFFFFFFFE)
            while bad_pid in (pa, pb):
                bad_pid = random.randint(1, 0xFFFFFFFE)
            # Randomly pick msg type from {KA, GU, MOVE_1}. MOVE_1 needs a pawn.
            mtype = random.choice(['ka', 'gu', 'mv'])
            if mtype == 'ka':
                data = make_ka(bad_pid, bad_gid)
            elif mtype == 'gu':
                data = make_gu(bad_pid, bad_gid)
            else:
                data = make_move1(bad_pid, bad_gid, random.randint(0, 255))
            sock.sendto(data, addr); resp, _ = sock.recvfrom(4096)
            if not is_wrong(resp):
                failures.append(
                    f"A[{i}] {mtype} bad_gid={bad_gid} bad_pid={bad_pid}: "
                    f"expected WRONG_MSG, got {resp.hex()}")
                continue
            # error_index at offset 13 should be 5 (MSG_TYPE_SIZE + PLAYER_ID_SIZE)
            # for both invalid_game_id and invalid_player_id.
            if resp[13] != 5:
                failures.append(
                    f"A[{i}] {mtype}: expected error_index=5, got {resp[13]}")

        # Between probes, verify real state is unchanged.
        sock.sendto(make_ka(pa, gid), addr); resp, _ = sock.recvfrom(4096)
        s2 = parse_state(resp, 3)
        if not s2 or s2['status'] != 2 or s2['pins'] != initial_pins \
                or s2['player_a'] != pa or s2['player_b'] != pb \
                or s2['game_id'] != gid:
            failures.append(f"A: real state changed after probes: {resp.hex()}")
        else:
            print(f"  A: {N} probes survived, real game intact.")

        # Extra check: sending MSG_GIVE_UP / KA / MOVE with the CORRECT (gid,
        # pid) combo but swapping A<->B (so pid = pa and gid is correct) is
        # VALID and must return a GAME_STATE (not WRONG). We pick A (on B's
        # turn) so the GIVE_UP / MOVE is illegal but the message is valid.
        sock.sendto(make_gu(pa, gid), addr); resp, _ = sock.recvfrom(4096)
        s3 = parse_state(resp, 3)
        if not s3:
            failures.append("A: valid-but-illegal GIVE_UP from A got "
                            f"{resp.hex()}")
        elif s3['status'] != 2 or s3['pins'] != initial_pins:
            failures.append("A: illegal GIVE_UP from A changed state")
finally:
    stop_proc(proc)

# ============================================================================
# Part B: random JOIN sequence with shuffled distinct player IDs. Verify
# strict FIFO pairing and the at-most-one-WAITING invariant.
# ============================================================================
print("Part B: random JOIN sequence and FIFO pairing")
proc, port = start_server('11111', timeout_s=60)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    # 40 distinct player IDs drawn without replacement.
    pids = random.sample(range(1, 10_000_000), 40)

    waiting_pa = None
    waiting_gid = None
    seen_gids = set()

    for i, pid in enumerate(pids):
        sock.sendto(make_join(pid), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 4)
        if s is None:
            failures.append(f"B[{i}] JOIN pid={pid}: {resp.hex()}"); continue

        if waiting_pa is None:
            # Expect this JOIN to create a new WAITING game.
            if s['status'] != 0:
                failures.append(
                    f"B[{i}]: expected WAITING, got status={s['status']}")
            if s['player_a'] != pid:
                failures.append(
                    f"B[{i}]: player_a {s['player_a']} != pid {pid}")
            if s['player_b'] != 0:
                failures.append(f"B[{i}]: player_b should be 0")
            if s['game_id'] in seen_gids:
                failures.append(f"B[{i}]: reused game_id {s['game_id']}")
            seen_gids.add(s['game_id'])
            waiting_pa = pid
            waiting_gid = s['game_id']
        else:
            # Expect this JOIN to pair with waiting_pa.
            if s['status'] != 2:
                failures.append(
                    f"B[{i}]: expected TURN_B, got status={s['status']}")
            if s['player_a'] != waiting_pa:
                failures.append(
                    f"B[{i}]: player_a {s['player_a']} (expected "
                    f"{waiting_pa})")
            if s['player_b'] != pid:
                failures.append(
                    f"B[{i}]: player_b {s['player_b']} != pid {pid}")
            if s['game_id'] != waiting_gid:
                failures.append(
                    f"B[{i}]: game_id {s['game_id']} != waiting "
                    f"{waiting_gid}")
            waiting_pa = None
            waiting_gid = None
    print(f"  B: {len(seen_gids)} distinct game_ids across {len(pids)} JOINs")
finally:
    stop_proc(proc)

if failures:
    print(f"\n{len(failures)} FAILURES (first 20)")
    for f in failures[:20]:
        print("  " + f)
    sys.exit(1)

print("\nAll adversarial-ID fuzz tests OK.")
PYEOF

echo "All fuzz_adversarial_ids tests passed."
