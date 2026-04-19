#!/usr/bin/env bash
# Order / retransmit fuzz (what we can control on localhost).
#
# Axis 7 from the randomized integration suite spec.
#
# Localhost UDP is effectively lossless and in-order, so we test:
#   1. Back-to-back bursts of legitimate packets from a single socket —
#      every packet gets a response; no packets swallowed or aggregated.
#   2. Duplicate (retransmitted) legal MOVE_1: the second is illegal (pin
#      already knocked) and returns GAME_STATE with state unchanged and
#      turn NOT doubly-flipped.
#   3. Duplicate legal MOVE_2: same.
#   4. Random permuted retransmits of a mixed KA/MOVE sequence where each
#      MOVE is sent twice in a row. Verify state advances exactly one step
#      per legal move.
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

def make_join(pid): return bytes([0]) + struct.pack('>I', pid)
def make_ka(pid, gid):
    return bytes([3]) + struct.pack('>I', pid) + struct.pack('>I', gid)
def make_move1(pid, gid, pawn):
    return bytes([1]) + struct.pack('>I', pid) + struct.pack('>I', gid) + bytes([pawn])
def make_move2(pid, gid, pawn):
    return bytes([2]) + struct.pack('>I', pid) + struct.pack('>I', gid) + bytes([pawn])

failures = []

# ============================================================================
# 1. Burst of legitimate packets from a single socket.
# Fire 50 KEEP_ALIVEs in a tight loop; receive 50 responses.
# ============================================================================
print("1: burst of KAs — every packet must be answered")
proc, port = start_server('11111111', timeout_s=60)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    # Bigger socket buffer to avoid losing responses on localhost.
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)

    pa = 11; pb = 22
    sock.sendto(make_join(pa), addr); sock.recvfrom(4096)
    sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 7)
    if not s: failures.append(f"1: setup: {resp.hex()}")
    else:
        gid = s['game_id']
        # Fire KAs.
        BURST = 50
        for _ in range(BURST):
            sock.sendto(make_ka(pa, gid), addr)
        # Receive BURST responses.
        got = 0
        try:
            for _ in range(BURST):
                resp, _ = sock.recvfrom(4096)
                ps = parse_state(resp, 7)
                if not ps or ps['game_id'] != gid:
                    failures.append(f"1: burst response drift: {resp.hex()}")
                got += 1
        except socket.timeout:
            failures.append(f"1: burst — got {got}/{BURST} responses")
        else:
            print(f"  1: burst — received all {got}/{BURST} responses")
finally:
    stop_proc(proc)

# ============================================================================
# 2 & 3. Duplicate legal MOVE_1 / MOVE_2.
# After the first MOVE_1 pawn=0 from B, turn flips to A. A SECOND identical
# MOVE_1 from B (same pawn) is illegal: wrong turn AND already-knocked.
# State must not change. Turn must remain TURN_A.
# ============================================================================
print("2&3: duplicate MOVE_1/MOVE_2 retransmits")
proc, port = start_server('11111111', timeout_s=60)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)
    pa = 7; pb = 13
    sock.sendto(make_join(pa), addr); sock.recvfrom(4096)
    sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
    gid = parse_state(resp, 7)['game_id']

    # First MOVE_1 by B at pawn=0 — legal. Then a second identical from B.
    mv = make_move1(pb, gid, 0)
    sock.sendto(mv, addr); resp1, _ = sock.recvfrom(4096)
    s1 = parse_state(resp1, 7)
    if not s1 or s1['status'] != 1:  # TURN_A
        failures.append(f"2: first MOVE_1 didn't flip turn: {resp1.hex()}")
    sock.sendto(mv, addr); resp2, _ = sock.recvfrom(4096)  # retransmit
    s2 = parse_state(resp2, 7)
    if not s2:
        failures.append(f"2: retransmit got junk: {resp2.hex()}")
    elif s2['status'] != 1:
        failures.append(f"2: retransmit flipped turn! now status={s2['status']}")
    elif s2['pins'] != s1['pins']:
        failures.append(f"2: retransmit changed bitmap!")

    # Now A takes pawn=1 — legal.
    sock.sendto(make_move1(pa, gid, 1), addr); resp, _ = sock.recvfrom(4096)
    s3 = parse_state(resp, 7)
    if not s3 or s3['status'] != 2:
        failures.append(f"2: A's move didn't return TURN_B: {resp.hex()}")

    # Duplicate MOVE_2 test: B takes pawns 2,3 via MOVE_2 at pawn=2. Then
    # retransmit — state must not change.
    mv2 = make_move2(pb, gid, 2)
    sock.sendto(mv2, addr); resp1, _ = sock.recvfrom(4096)
    s4 = parse_state(resp1, 7)
    if not s4 or s4['status'] != 1:
        failures.append(f"3: first MOVE_2 failed: {resp1.hex()}")
    sock.sendto(mv2, addr); resp2, _ = sock.recvfrom(4096)
    s5 = parse_state(resp2, 7)
    if not s5:
        failures.append(f"3: retransmit MOVE_2 got junk: {resp2.hex()}")
    elif s5['status'] != 1:
        failures.append(f"3: retransmit MOVE_2 flipped turn!")
    elif s5['pins'] != s4['pins']:
        failures.append(f"3: retransmit MOVE_2 changed bitmap!")
finally:
    stop_proc(proc)

# ============================================================================
# 4. Mixed KA/MOVE sequence where each MOVE is sent TWICE in a row. Verify
# state advances exactly once per unique legal move.
# Use an 8-pin board; play out some random but reproducible sequence.
# ============================================================================
print("4: each legal MOVE sent twice — state advances exactly once")
proc, port = start_server('11111111', timeout_s=60)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    pa = 100; pb = 200
    sock.sendto(make_join(pa), addr); sock.recvfrom(4096)
    sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 7); gid = s['game_id']
    pins = [True] * 8
    status = 2  # TURN_B

    legal_moves = 0
    while status in (1, 2):
        me = pa if status == 1 else pb
        me_label = 'A' if status == 1 else 'B'
        m1s = [i for i in range(8) if pins[i]]
        m2s = [i for i in range(7) if pins[i] and pins[i + 1]]
        if not m1s: break  # should never hit — every game ends in WIN
        if m1s and m2s and random.random() < 0.5:
            p = random.choice(m2s); data = make_move2(me, gid, p)
            dp = [p, p + 1]
        else:
            p = random.choice(m1s); data = make_move1(me, gid, p)
            dp = [p]

        # First send — legal.
        sock.sendto(data, addr); resp1, _ = sock.recvfrom(4096)
        s1 = parse_state(resp1, 7)
        if not s1:
            failures.append(f"4 move#{legal_moves}: first got junk {resp1.hex()}")
            break
        for i in dp: pins[i] = False
        expected = (3 if me_label == 'A' else 4) if all(not x for x in pins) \
            else (1 if status == 2 else 2)
        if s1['status'] != expected:
            failures.append(
                f"4 move#{legal_moves}: first expected status {expected}, "
                f"got {s1['status']}")
            break

        # Second send — SHOULD BE IDEMPOTENT (identical packet).
        sock.sendto(data, addr); resp2, _ = sock.recvfrom(4096)
        s2 = parse_state(resp2, 7)
        if not s2:
            failures.append(
                f"4 move#{legal_moves}: retransmit got junk {resp2.hex()}")
            break
        if s2['status'] != s1['status']:
            failures.append(
                f"4 move#{legal_moves}: retransmit changed status "
                f"{s1['status']} -> {s2['status']}")
            break
        if s2['pins'] != s1['pins']:
            failures.append(
                f"4 move#{legal_moves}: retransmit changed bitmap")
            break

        # Optional KA — same state.
        if random.random() < 0.3:
            sock.sendto(make_ka(me, gid), addr); resp, _ = sock.recvfrom(4096)
            sk = parse_state(resp, 7)
            if not sk or sk['status'] != s2['status'] or sk['pins'] != s2['pins']:
                failures.append(
                    f"4 move#{legal_moves}: KA changed state!")
                break

        status = s2['status']
        legal_moves += 1
        if legal_moves > 20:
            break

    if status not in (3, 4):
        failures.append(f"4: game didn't finish; final status={status}")
    else:
        print(f"  4: played {legal_moves} legal moves with duplicate sends, "
              f"final status={status}")
finally:
    stop_proc(proc)

if failures:
    print(f"\n{len(failures)} FAILURES")
    for f in failures:
        print("  " + f)
    sys.exit(1)

print("\nAll retransmit fuzz tests OK.")
PYEOF

echo "All fuzz_retransmit tests passed."
