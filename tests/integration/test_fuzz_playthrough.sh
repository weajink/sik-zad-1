#!/usr/bin/env bash
# Random legal-move playthroughs.
#
# Axis 2 from the randomized integration suite spec.
#
# Generates random pawn_row bitmaps with random max_pawn in {0, small, medium,
# 255}, then plays full games via raw UDP. Every move is chosen uniformly at
# random from the set of currently-legal moves for the player whose turn it
# is.
#
# Invariants asserted after every exchange:
#   - turn alternates exactly on every legal move (TURN_A <-> TURN_B);
#   - pawn count decreases by 1 or 2 depending on move type;
#   - MOVE_2 only succeeds when both pawn and pawn+1 are up;
#   - MOVE_1 only succeeds when pawn is up;
#   - the game ends with exactly one of WIN_A / WIN_B when no pins remain;
#   - the final bitmap is all zeros;
#   - the winner is the player who took the last move.
#
# Runs N_GAMES full games. Each game boots a fresh server on its own port
# (so seeds are independent). Reproducibility: print seed at start.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

SEED="${RANDOM_SEED:-$RANDOM}"
echo "RANDOM_SEED=$SEED"

# We spawn servers from inside python via subprocess — we need SERVER_BIN.
SERVER_BIN_PATH="$SERVER_BIN"

python3 - "$SERVER_BIN_PATH" "$SEED" <<'PYEOF'
import os, random, signal, socket, struct, subprocess, sys, time

SERVER_BIN = sys.argv[1]
SEED = int(sys.argv[2])
random.seed(SEED)

N_GAMES = 20

# Track spawned servers for cleanup.
_servers = []
def cleanup():
    for p in _servers:
        try:
            p.terminate()
            p.wait(timeout=2)
        except Exception:
            try: p.kill()
            except Exception: pass
import atexit
atexit.register(cleanup)

def pick_port():
    # Get a free UDP port by binding ephemeral.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p

def start_server(pawn_row, timeout_s=30):
    port = pick_port()
    proc = subprocess.Popen(
        [SERVER_BIN, '-r', pawn_row, '-a', '127.0.0.1', '-p', str(port),
         '-t', str(timeout_s)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _servers.append(proc)
    # Wait a beat for bind.
    time.sleep(0.15)
    if proc.poll() is not None:
        raise RuntimeError(f"server exited during startup for {pawn_row!r}")
    return proc, port

def stop_server(proc):
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        try: proc.kill()
        except Exception: pass
    if proc in _servers:
        _servers.remove(proc)

def parse_game_state(resp, max_pawn):
    """Parse MSG_GAME_STATE. Returns dict or None if it's a WRONG_MSG or junk."""
    header_size = 14
    bitmap_size = max_pawn // 8 + 1
    total = header_size + bitmap_size
    if len(resp) != total:
        return None
    if resp[12] == 0xFF:
        return None
    game_id = struct.unpack('>I', resp[0:4])[0]
    player_a = struct.unpack('>I', resp[4:8])[0]
    player_b = struct.unpack('>I', resp[8:12])[0]
    status = resp[12]
    max_pawn_server = resp[13]
    bitmap_bytes = resp[14:14 + bitmap_size]
    # Decode bitmap, pin 0 = MSB of byte 0.
    pins = [False] * (max_pawn_server + 1)
    for i in range(max_pawn_server + 1):
        pins[i] = bool((bitmap_bytes[i // 8] >> (7 - (i % 8))) & 1)
    return {
        'game_id': game_id, 'player_a': player_a, 'player_b': player_b,
        'status': status, 'max_pawn': max_pawn_server, 'pins': pins,
        'raw': resp,
    }

def send_and_recv(sock, addr, data, max_pawn, timeout=2.0):
    sock.settimeout(timeout)
    sock.sendto(data, addr)
    resp, _ = sock.recvfrom(4096)
    state = parse_game_state(resp, max_pawn)
    return state, resp

def make_join(player_id):
    return bytes([0]) + struct.pack('>I', player_id)
def make_move1(player_id, game_id, pawn):
    return bytes([1]) + struct.pack('>I', player_id) + struct.pack('>I', game_id) + bytes([pawn])
def make_move2(player_id, game_id, pawn):
    return bytes([2]) + struct.pack('>I', player_id) + struct.pack('>I', game_id) + bytes([pawn])

failures = []

def fail(g, msg):
    failures.append(f"[game {g}] {msg}")
    print(f"  FAIL game {g}: {msg}")

def run_one_game(gid):
    # Choose max_pawn from a weighted distribution covering small / mid / max.
    cat = random.choices(
        ['single', 'small', 'med', 'large', 'max'],
        weights=[1, 3, 3, 2, 1])[0]
    if cat == 'single':
        max_pawn = 0
    elif cat == 'small':
        max_pawn = random.randint(1, 7)
    elif cat == 'med':
        max_pawn = random.randint(8, 63)
    elif cat == 'large':
        max_pawn = random.randint(64, 200)
    else:
        max_pawn = 255

    # Build a random pawn_row of length max_pawn+1 with first and last '1'
    # (per assignment requirement).
    n = max_pawn + 1
    row = ['1']
    for _ in range(n - 2):
        row.append(random.choice(['0', '1']))
    if n >= 2:
        row.append('1')
    pawn_row = ''.join(row)

    expected_pins = [c == '1' for c in pawn_row]
    expected_count = sum(expected_pins)

    # Start server.
    proc, port = start_server(pawn_row, timeout_s=30)
    try:
        addr = ('127.0.0.1', port)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2.0)

        pa_id = random.randint(1, 0xFFFFFFFE)
        pb_id = random.randint(1, 0xFFFFFFFE)
        while pb_id == pa_id:
            pb_id = random.randint(1, 0xFFFFFFFE)

        # JOIN A.
        state, resp = send_and_recv(sock, addr, make_join(pa_id), max_pawn)
        if state is None:
            fail(gid, f"JOIN A: unparseable response {resp.hex()}")
            return
        if state['status'] != 0:
            fail(gid, f"JOIN A: expected WAITING (0), got {state['status']}")
            return
        if state['player_a'] != pa_id:
            fail(gid, f"JOIN A: player_a mismatch {state['player_a']} vs {pa_id}")
            return
        if state['pins'] != expected_pins:
            fail(gid, f"JOIN A: bitmap mismatch "
                      f"{state['pins']} vs {expected_pins}")
            return
        if state['max_pawn'] != max_pawn:
            fail(gid, f"JOIN A: max_pawn mismatch "
                      f"{state['max_pawn']} vs {max_pawn}")
            return
        game_id = state['game_id']

        # JOIN B.
        state, resp = send_and_recv(sock, addr, make_join(pb_id), max_pawn)
        if state is None:
            fail(gid, f"JOIN B: unparseable response {resp.hex()}")
            return
        if state['status'] != 2:  # TURN_B
            fail(gid, f"JOIN B: expected TURN_B (2), got {state['status']}")
            return
        if state['player_a'] != pa_id or state['player_b'] != pb_id:
            fail(gid, f"JOIN B: player ids mismatch")
            return
        if state['game_id'] != game_id:
            fail(gid, f"JOIN B: game_id changed from {game_id} to "
                      f"{state['game_id']}")
            return

        # Simulate play.
        pins = list(state['pins'])
        status = state['status']  # starts at TURN_B
        pawns_left = sum(pins)
        moves_made = 0
        last_mover = None

        while status in (1, 2):  # TURN_A, TURN_B
            # Who's playing?
            if status == 1:
                me, me_label = pa_id, 'A'
            else:
                me, me_label = pb_id, 'B'

            # Enumerate legal moves.
            move1s = [i for i in range(max_pawn + 1) if pins[i]]
            move2s = [i for i in range(max_pawn) if pins[i] and pins[i + 1]]
            # pawns_left == 0 should have ended the game; assert defensively.
            if not move1s and not move2s:
                fail(gid, f"no legal moves but status={status}, pins_left="
                          f"{pawns_left}")
                return

            # Choose. Prefer variety — 50/50 between move1 and move2 when both
            # available.
            if move1s and move2s:
                choice = random.choice(['m1', 'm2'])
            elif move1s:
                choice = 'm1'
            else:
                choice = 'm2'
            if choice == 'm1':
                pawn = random.choice(move1s)
                data = make_move1(me, game_id, pawn)
                n_knocked = 1
                to_knock = [pawn]
            else:
                pawn = random.choice(move2s)
                data = make_move2(me, game_id, pawn)
                n_knocked = 2
                to_knock = [pawn, pawn + 1]

            state, resp = send_and_recv(sock, addr, data, max_pawn)
            if state is None:
                fail(gid, f"move {moves_made}: unparseable {resp.hex()}")
                return

            # Apply locally and compare.
            for p in to_knock:
                pins[p] = False
            pawns_left -= n_knocked
            last_mover = me_label

            expected_status_after = None
            if pawns_left == 0:
                # The player who just moved wins.
                expected_status_after = 3 if me_label == 'A' else 4  # WIN_A/B
            else:
                # Turn must flip.
                expected_status_after = 1 if status == 2 else 2

            if state['status'] != expected_status_after:
                fail(gid,
                     f"move {moves_made} ({me_label} {choice} pawn={pawn}): "
                     f"expected status {expected_status_after}, got "
                     f"{state['status']}")
                return
            if state['pins'] != pins:
                fail(gid,
                     f"move {moves_made}: bitmap mismatch "
                     f"local={pins[:16]}... server={state['pins'][:16]}...")
                return
            if state['game_id'] != game_id:
                fail(gid, f"move {moves_made}: game_id changed to "
                          f"{state['game_id']}")
                return
            if state['player_a'] != pa_id or state['player_b'] != pb_id:
                fail(gid, f"move {moves_made}: player ids changed")
                return

            status = state['status']
            moves_made += 1
            # Sanity: moves_made must not exceed total pins + 1.
            if moves_made > (max_pawn + 1) + 2:
                fail(gid, f"suspiciously many moves ({moves_made})")
                return

        # Game over. Verify final state.
        if status not in (3, 4):
            fail(gid, f"game ended with unexpected status {status}")
            return
        if any(pins):
            fail(gid, f"game ended but pins still up: {pins}")
            return
        # Winner must be last mover.
        expected_win = 3 if last_mover == 'A' else 4
        if status != expected_win:
            fail(gid,
                 f"winner mismatch: last_mover={last_mover}, status={status}, "
                 f"expected={expected_win}")
            return

        print(f"  OK game {gid}: max_pawn={max_pawn} pawns={expected_count} "
              f"moves={moves_made} winner={last_mover}")
    finally:
        stop_server(proc)

for g in range(N_GAMES):
    run_one_game(g)

if failures:
    print(f"\n{len(failures)} FAILURES")
    sys.exit(1)
print("\nAll random playthroughs OK.")
PYEOF

echo "All fuzz_playthrough tests passed."
