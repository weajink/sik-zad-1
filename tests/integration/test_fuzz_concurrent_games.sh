#!/usr/bin/env bash
# Many parallel games at scale.
#
# Axis 4 from the randomized integration suite spec.
#
# Part 1: Create N_PARALLEL games in an arbitrary interleaved order, play
# each to completion in an interleaved schedule, and verify every response
# contains the correct (game_id, player_a, player_b, bitmap) — no crosstalk.
#
# Part 2: Spam N_SEQUENTIAL JOINs one after the other (using odd-indexed
# joins to transition WAITING -> ACTIVE each time) and after every step
# verify: at-most-one WAITING game exists (by probing with a fresh JOIN
# and observing which games pair up vs create new).
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

N_PARALLEL = 60        # concurrent games in part 1
N_SEQUENTIAL = 200     # JOINs in part 2

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

failures = []

# ============================================================================
# Part 1: 60 concurrent games, played in interleaved random order.
# Board: 4 pins (max_pawn=3, pawn_row="1111"). Small so we can finish games
# quickly. Long server_timeout so nothing gets reaped.
# ============================================================================
print(f"Part 1: {N_PARALLEL} concurrent games (board '1111').")
proc, port = start_server('1111', timeout_s=60)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    # Pick distinct player IDs. Use 1000+i for A, 2000+i for B to spot
    # crosstalk visually when a failure prints.
    games = []  # list of dicts
    # Create all the games in arbitrary order.
    for i in range(N_PARALLEL):
        pa = 10000 + 2 * i
        pb = 10001 + 2 * i
        sock.sendto(make_join(pa), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if not s or s['status'] != 0 or s['player_a'] != pa:
            failures.append(f"create game {i} (A): {resp.hex()}")
            continue
        sock.sendto(make_join(pb), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if not s or s['status'] != 2 or s['player_a'] != pa or s['player_b'] != pb:
            failures.append(f"create game {i} (B): {resp.hex()}")
            continue
        games.append({'gid': s['game_id'], 'pa': pa, 'pb': pb,
                      'pins': [True, True, True, True], 'status': 2,
                      'last_mover': None})

    # Check all game_ids distinct.
    gids = [g['gid'] for g in games]
    if len(set(gids)) != len(gids):
        failures.append(f"duplicate game_ids: {gids}")

    # Interleave moves across all active games.
    # At any moment: pick a random not-yet-finished game, make a random legal
    # move for the player whose turn it is. Repeat until all games finished.
    active = [i for i in range(len(games))]
    moves = 0
    while active:
        pick = random.choice(active)
        g = games[pick]
        me = g['pa'] if g['status'] == 1 else g['pb']
        me_label = 'A' if g['status'] == 1 else 'B'
        move1s = [i for i in range(4) if g['pins'][i]]
        move2s = [i for i in range(3) if g['pins'][i] and g['pins'][i + 1]]
        if move1s and move2s:
            which = random.choice(['m1', 'm2'])
        elif move1s:
            which = 'm1'
        else:
            which = 'm2'
        if which == 'm1':
            p = random.choice(move1s)
            data = make_move1(me, g['gid'], p)
            g['pins'][p] = False
        else:
            p = random.choice(move2s)
            data = make_move2(me, g['gid'], p)
            g['pins'][p] = False
            g['pins'][p + 1] = False
        g['last_mover'] = me_label
        sock.sendto(data, addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if s is None:
            failures.append(f"game {pick} move {moves}: {resp.hex()}")
            active.remove(pick); continue
        # Cross-contamination check: returned game_id/player_a/player_b must
        # match the game we targeted — NOT some other game in the system.
        if s['game_id'] != g['gid']:
            failures.append(
                f"game {pick} move {moves}: game_id mismatch "
                f"{s['game_id']} (expected {g['gid']})")
            active.remove(pick); continue
        if s['player_a'] != g['pa']:
            failures.append(
                f"game {pick} move {moves}: player_a {s['player_a']} "
                f"(expected {g['pa']})")
            active.remove(pick); continue
        if s['player_b'] != g['pb']:
            failures.append(
                f"game {pick} move {moves}: player_b {s['player_b']} "
                f"(expected {g['pb']})")
            active.remove(pick); continue

        # Update local state to match the server's reported status.
        if all(not x for x in g['pins']):
            expected = 3 if me_label == 'A' else 4
        else:
            expected = 1 if g['status'] == 2 else 2
        if s['status'] != expected:
            failures.append(
                f"game {pick} move {moves}: status {s['status']} "
                f"(expected {expected})")
            active.remove(pick); continue
        if s['pins'] != g['pins']:
            failures.append(f"game {pick}: bitmap mismatch")
            active.remove(pick); continue
        g['status'] = s['status']
        if g['status'] in (3, 4):
            active.remove(pick)
        moves += 1

    # After all games finished, verify each one reports the correct winner via
    # KEEP_ALIVE from player A.
    for i, g in enumerate(games):
        sock.sendto(make_ka(g['pa'], g['gid']), addr)
        resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if s is None:
            failures.append(f"game {i} post-KA: {resp.hex()}"); continue
        if s['game_id'] != g['gid'] or s['player_a'] != g['pa'] \
                or s['player_b'] != g['pb']:
            failures.append(f"game {i} post-KA: id drift")
            continue
        expected = 3 if g['last_mover'] == 'A' else 4
        if s['status'] != expected:
            failures.append(
                f"game {i} post-KA: status {s['status']} "
                f"(expected {expected})")

    print(f"  Part 1: completed {moves} moves across {len(games)} games.")
finally:
    try: proc.terminate(); proc.wait(timeout=3)
    except Exception:
        try: proc.kill()
        except Exception: pass
    if proc in _servers: _servers.remove(proc)

# ============================================================================
# Part 2: 200 sequential JOINs on a fresh server. The at-most-one-WAITING
# invariant says:
#   After 1st JOIN: 1 WAITING.
#   After 2nd JOIN: 0 WAITING (paired).
#   After 3rd JOIN: 1 WAITING.
#   ...
# We assert this by inspecting the 'status' field of each response: odd
# JOINs -> WAITING_FOR_OPPONENT (0), even JOINs -> TURN_B (2).
# Also verify player IDs and game IDs are stable and uniquely assigned.
# ============================================================================
print(f"\nPart 2: {N_SEQUENTIAL} sequential JOINs, at-most-one-WAITING invariant.")
proc, port = start_server('11111', timeout_s=60)  # 5-pin board
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    seen_gids = set()
    waiting_pa = None  # player_a of the currently-WAITING game, if any

    for i in range(N_SEQUENTIAL):
        pid = 100_000 + i
        sock.sendto(make_join(pid), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 4)
        if s is None:
            failures.append(f"seq JOIN {i}: unparseable {resp.hex()}")
            continue

        if i % 2 == 0:
            # Odd-count JOIN — this one should CREATE a new WAITING game.
            if s['status'] != 0:
                failures.append(
                    f"seq JOIN {i}: expected WAITING (0), got {s['status']}")
            if s['player_a'] != pid:
                failures.append(
                    f"seq JOIN {i}: player_a {s['player_a']} (expected {pid})")
            if s['player_b'] != 0:
                failures.append(
                    f"seq JOIN {i}: player_b should be 0, got {s['player_b']}")
            if s['game_id'] in seen_gids:
                failures.append(f"seq JOIN {i}: reused game_id {s['game_id']}")
            seen_gids.add(s['game_id'])
            waiting_pa = pid
        else:
            # Even-count JOIN — pair up with the WAITING game.
            if s['status'] != 2:
                failures.append(
                    f"seq JOIN {i}: expected TURN_B (2), got {s['status']}")
            if s['player_a'] != waiting_pa:
                failures.append(
                    f"seq JOIN {i}: paired with wrong A "
                    f"{s['player_a']} (expected {waiting_pa})")
            if s['player_b'] != pid:
                failures.append(
                    f"seq JOIN {i}: player_b {s['player_b']} (expected {pid})")
            if s['game_id'] not in seen_gids:
                failures.append(
                    f"seq JOIN {i}: unknown game_id {s['game_id']}")
            waiting_pa = None

    # After even number of JOINs, there should be NO waiting game.
    # Another JOIN must create a new game.
    if N_SEQUENTIAL % 2 == 0:
        probe_pid = 10 ** 9
        sock.sendto(make_join(probe_pid), addr); resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 4)
        if s is None or s['status'] != 0 or s['player_a'] != probe_pid:
            failures.append(
                f"after {N_SEQUENTIAL} JOINs: probe did not create new "
                f"WAITING game: resp={resp.hex()}")

    print(f"  Part 2: saw {len(seen_gids)} distinct game_ids.")
finally:
    try: proc.terminate(); proc.wait(timeout=3)
    except Exception:
        try: proc.kill()
        except Exception: pass
    if proc in _servers: _servers.remove(proc)

if failures:
    print(f"\n{len(failures)} FAILURES (showing first 20)")
    for f in failures[:20]:
        print("  " + f)
    sys.exit(1)

print("\nAll concurrent/scale fuzz tests OK.")
PYEOF

echo "All fuzz_concurrent_games tests passed."
