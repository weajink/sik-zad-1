#!/usr/bin/env bash
# Mixed legal/illegal moves under a random schedule.
#
# Axis 3 from the randomized integration suite spec.
#
# Interleave legal moves, illegal moves, KEEP_ALIVEs, and stray packets.
# After every exchange verify:
#   - illegal moves and KEEP_ALIVEs never change turn/bitmap;
#   - wrong-player moves never change state;
#   - out-of-range pawn never changes state;
#   - knocking an already-empty pin never changes state;
#   - MOVE_2 on (pawn, pawn+1) where either is empty never changes state;
#   - turn alternation holds across legal moves only.
#
# Uses a single medium-sized board per run, with random interleavings. Runs
# several independent runs for coverage.
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

N_RUNS = 8
N_EXCHANGES_PER_RUN = 80

_servers = []
def cleanup():
    for p in _servers:
        try: p.terminate(); p.wait(timeout=2)
        except Exception:
            try: p.kill()
            except Exception: pass
atexit.register(cleanup)

def pick_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p

def start_server(pawn_row, timeout_s=30):
    port = pick_port()
    proc = subprocess.Popen(
        [SERVER_BIN, '-r', pawn_row, '-a', '127.0.0.1', '-p', str(port),
         '-t', str(timeout_s)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _servers.append(proc)
    time.sleep(0.15)
    if proc.poll() is not None:
        raise RuntimeError("server exited on startup")
    return proc, port

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
def make_move1(pid, gid, pawn):
    return bytes([1]) + struct.pack('>I', pid) + struct.pack('>I', gid) + bytes([pawn])
def make_move2(pid, gid, pawn):
    return bytes([2]) + struct.pack('>I', pid) + struct.pack('>I', gid) + bytes([pawn])
def make_ka(pid, gid):
    return bytes([3]) + struct.pack('>I', pid) + struct.pack('>I', gid)
def make_gu(pid, gid):
    return bytes([4]) + struct.pack('>I', pid) + struct.pack('>I', gid)

failures = []
def fail(run, msg):
    failures.append(f"[run {run}] {msg}")
    print(f"  FAIL run {run}: {msg}")

def run_one(run_idx):
    max_pawn = random.randint(3, 40)
    n = max_pawn + 1
    row = ['1']
    for _ in range(n - 2):
        row.append(random.choice(['0', '1']))
    if n >= 2:
        row.append('1')
    pawn_row = ''.join(row)

    proc, port = start_server(pawn_row, timeout_s=30)
    try:
        addr = ('127.0.0.1', port)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2.0)

        pa = random.randint(1, 0xFFFFFFFE)
        pb = random.randint(1, 0xFFFFFFFE)
        while pb == pa: pb = random.randint(1, 0xFFFFFFFE)

        # JOIN A, JOIN B.
        sock.sendto(make_join(pa), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, max_pawn)
        if not s or s['status'] != 0:
            fail(run_idx, "JOIN A failed"); return
        gid = s['game_id']
        sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, max_pawn)
        if not s or s['status'] != 2:
            fail(run_idx, "JOIN B failed"); return

        pins = list(s['pins'])
        status = s['status']  # TURN_B
        moves_done = 0
        # Keep track of who moved last to recompute expected status when game ends.
        last_mover_label = None

        for ex in range(N_EXCHANGES_PER_RUN):
            if status in (3, 4):
                break  # game over; move on to post-game section
            expected_pins = list(pins)
            expected_status = status
            current_player_id = pa if status == 1 else pb
            current_label = 'A' if status == 1 else 'B'
            other_player_id = pb if status == 1 else pa

            # Pick an action category.
            r = random.random()
            # 25% legal move, 10% KA current, 10% KA other, 20% illegal (wrong
            # turn / out-of-range / already-knocked), 20% totally stray packet,
            # 10% stray KEEP_ALIVE from unrelated id, 5% MOVE_2 on empty pair.
            action = random.choices(
                ['legal', 'ka_cur', 'ka_other', 'wrong_turn', 'out_of_range',
                 'already_knocked', 'stray', 'stray_ka', 'bad_move2'],
                weights=[25, 10, 10, 10, 10, 10, 10, 5, 10])[0]

            move1s = [i for i in range(max_pawn + 1) if pins[i]]
            move2s = [i for i in range(max_pawn) if pins[i] and pins[i + 1]]

            data = None
            expect_wrong_msg = False
            applied_pins_knocked = []  # if action leads to a legal move

            if action == 'legal':
                if not move1s and not move2s:
                    continue
                if move1s and (not move2s or random.random() < 0.5):
                    pawn = random.choice(move1s)
                    data = make_move1(current_player_id, gid, pawn)
                    applied_pins_knocked = [pawn]
                else:
                    pawn = random.choice(move2s)
                    data = make_move2(current_player_id, gid, pawn)
                    applied_pins_knocked = [pawn, pawn + 1]

            elif action == 'ka_cur':
                data = make_ka(current_player_id, gid)
            elif action == 'ka_other':
                data = make_ka(other_player_id, gid)

            elif action == 'wrong_turn':
                # Send MOVE from the OTHER player (not current).
                if move1s:
                    pawn = random.choice(move1s)
                    data = make_move1(other_player_id, gid, pawn)
                else:
                    continue  # skip this iteration
            elif action == 'out_of_range':
                # pawn > max_pawn. Still a valid message — illegal move, state
                # unchanged, returns GAME_STATE.
                pawn = random.randint(max_pawn + 1, 255)
                data = make_move1(current_player_id, gid, pawn)
            elif action == 'already_knocked':
                empty = [i for i in range(max_pawn + 1) if not pins[i]]
                if not empty: continue
                pawn = random.choice(empty)
                data = make_move1(current_player_id, gid, pawn)
            elif action == 'bad_move2':
                # MOVE_2 at a pawn where (pawn, pawn+1) is not fully up.
                bad_pairs = [i for i in range(max_pawn)
                             if not (pins[i] and pins[i + 1])]
                if not bad_pairs: continue
                pawn = random.choice(bad_pairs)
                data = make_move2(current_player_id, gid, pawn)
            elif action == 'stray':
                # Totally random garbage — server should reply WRONG_MSG.
                n_bytes = random.randint(0, 20)
                data = bytes(random.randint(0, 255) for _ in range(n_bytes))
                # Skip if by chance we produce a VALID KEEP_ALIVE / GIVE_UP
                # etc. for this game (state would change).
                if (len(data) in (5, 9, 10) and len(data) >= 1 and data[0] <= 4):
                    t = data[0]
                    if ((t == 0 and len(data) == 5) or
                        (t in (3, 4) and len(data) == 9) or
                        (t in (1, 2) and len(data) == 10)):
                        # Possibly interpretable — skip this iteration to
                        # keep the invariant test clean.
                        continue
                expect_wrong_msg = True
            elif action == 'stray_ka':
                # KEEP_ALIVE from an unrelated player on this game —> WRONG_MSG.
                stray_pid = random.randint(1, 0xFFFFFFFE)
                while stray_pid in (pa, pb):
                    stray_pid = random.randint(1, 0xFFFFFFFE)
                data = make_ka(stray_pid, gid)
                expect_wrong_msg = True
            else:
                continue

            sock.sendto(data, addr)
            resp, _ = sock.recvfrom(4096)

            if expect_wrong_msg:
                # Must be a 14-byte WRONG_MSG.
                if len(resp) != 14 or resp[12] != 0xFF:
                    fail(run_idx,
                         f"ex {ex}: expected WRONG_MSG for {action}, got "
                         f"{resp.hex()}")
                    return
                # State invariants hold — nothing to update locally.
                continue

            # We expected GAME_STATE. Parse.
            ns = parse_state(resp, max_pawn)
            if ns is None:
                fail(run_idx,
                     f"ex {ex}: expected GAME_STATE for {action}, got "
                     f"{resp.hex()}")
                return

            if ns['game_id'] != gid:
                fail(run_idx, f"ex {ex}: game_id drift {ns['game_id']} vs {gid}")
                return
            if ns['player_a'] != pa or ns['player_b'] != pb:
                fail(run_idx, f"ex {ex}: player IDs drifted")
                return

            if action == 'legal':
                # Apply legal move locally and compare.
                for p in applied_pins_knocked:
                    expected_pins[p] = False
                if all(not x for x in expected_pins):
                    expected_status = 3 if current_label == 'A' else 4
                else:
                    expected_status = 1 if status == 2 else 2
                last_mover_label = current_label
                moves_done += 1
            # For all non-legal actions, expected state equals previous state.

            if ns['status'] != expected_status:
                fail(run_idx,
                     f"ex {ex} action={action}: expected status "
                     f"{expected_status}, got {ns['status']}")
                return
            if ns['pins'] != expected_pins:
                fail(run_idx,
                     f"ex {ex} action={action}: bitmap changed unexpectedly")
                return

            pins = expected_pins
            status = expected_status

        # After the exchange loop, try a KEEP_ALIVE from A on this game —
        # state should be unchanged.
        sock.sendto(make_ka(pa, gid), addr)
        resp, _ = sock.recvfrom(4096)
        ns = parse_state(resp, max_pawn)
        if ns is None:
            fail(run_idx, f"post-loop KA: unparseable {resp.hex()}")
            return
        if ns['status'] != status or ns['pins'] != pins:
            fail(run_idx, f"post-loop KA: state changed")
            return

        print(f"  OK run {run_idx}: max_pawn={max_pawn} moves={moves_done} "
              f"final_status={status}")
    finally:
        try: proc.terminate(); proc.wait(timeout=2)
        except Exception:
            try: proc.kill()
            except Exception: pass
        if proc in _servers: _servers.remove(proc)

for i in range(N_RUNS):
    run_one(i)

if failures:
    print(f"\n{len(failures)} FAILURES")
    sys.exit(1)
print("\nAll mixed-moves fuzz runs OK.")
PYEOF

echo "All fuzz_mixed_moves tests passed."
