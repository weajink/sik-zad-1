#!/usr/bin/env bash
# task.txt §3.3 + §5.3 bullet 3:
#
#   §3.3:  "Inne komunikaty klienta są poprawne, jeśli mają poprawną długość
#           i zawierają poprawne wartości w polach msg_type, player_id i
#           game_id (istnieje podana gra i uczestniczy w niej podany gracz).
#           Komunikat zawierający niepoprawną wartość pola pawn uznaje się
#           za poprawny, ale taki ruch jest nielegalny. Ruch jest nielegalny
#           również wtedy, gdy nie może być wykonany w aktualnym stanie gry
#           lub gdy próbuje go wykonać gracz, którego nie jest kolej (dotyczy
#           to także komunikatu MSG_GIVE_UP)."
#
#   §5.3:  "któryś z graczy nie przysłał żadnego poprawnego komunikatu z
#           identyfikatorem tej rozgrywki przez server_timeout – ten gracz
#           przegrywa."
#
# In words: a client message that is "valid" (well-formed, correct msg_type,
# non-zero player_id, existing game_id, player is a participant) resets the
# sender's server_timeout clock — *even* if the move it encodes is illegal
# (out-of-range pawn, wrong turn, GIVE_UP on the other player's turn, etc.).
# "Valid" (poprawny) and "legal" (legalny) are distinct in the spec.
#
# This test pins that rule down with two sub-tests in which a non-turn
# player spams illegal-but-valid messages while the turn player stays
# silent. After > server_timeout, the silent turn player must be the one
# who loses.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

SERVER_BIN_PATH="$SERVER_BIN"

python3 - "$SERVER_BIN_PATH" <<'PYEOF'
import atexit, socket, struct, subprocess, sys, time

SERVER_BIN = sys.argv[1]

_servers = []
def cleanup():
    for p in _servers:
        try:
            p.terminate(); p.wait(timeout=3)
        except Exception:
            try: p.kill()
            except Exception: pass
atexit.register(cleanup)

def pick_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p

def start_server(pawn_row, timeout_s):
    port = pick_port()
    proc = subprocess.Popen(
        [SERVER_BIN, '-r', pawn_row, '-a', '127.0.0.1', '-p', str(port),
         '-t', str(timeout_s)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _servers.append(proc)
    time.sleep(0.2)
    if proc.poll() is not None:
        raise RuntimeError("server exited on startup")
    return proc, port

def stop_proc(proc):
    try:
        proc.terminate(); proc.wait(timeout=3)
    except Exception:
        try: proc.kill()
        except Exception: pass
    if proc in _servers:
        _servers.remove(proc)

def parse_state(resp, max_pawn):
    header_size = 14
    bitmap_size = max_pawn // 8 + 1
    if len(resp) != header_size + bitmap_size:
        return None
    if resp[12] == 0xFF:
        return None
    return {
        'game_id': struct.unpack('>I', resp[0:4])[0],
        'player_a': struct.unpack('>I', resp[4:8])[0],
        'player_b': struct.unpack('>I', resp[8:12])[0],
        'status': resp[12],
        'max_pawn': resp[13],
        'pins': [bool((resp[14 + i // 8] >> (7 - (i % 8))) & 1)
                 for i in range(resp[13] + 1)],
    }

def pins_to_str(pins):
    return ''.join('1' if p else '0' for p in pins)

def is_wrong(resp):
    return len(resp) == 14 and resp[12] == 0xFF

def make_join(pid):
    return bytes([0]) + struct.pack('>I', pid)

def make_move1(pid, gid, pawn):
    return bytes([1]) + struct.pack('>I', pid) + struct.pack('>I', gid) + bytes([pawn])

def make_ka(pid, gid):
    return bytes([3]) + struct.pack('>I', pid) + struct.pack('>I', gid)

def make_give_up(pid, gid):
    return bytes([4]) + struct.pack('>I', pid) + struct.pack('>I', gid)

# Drain any pending responses on the socket (without blocking).
def drain(sock):
    sock.setblocking(False)
    try:
        while True:
            try:
                sock.recvfrom(4096)
            except BlockingIOError:
                break
    finally:
        sock.setblocking(True)

failures = []

# ============================================================================
# Sub-test 1: Illegal out-of-range pawn (MOVE_1 with pawn=200 on a 4-pin board)
# from the NON-turn player, repeated for longer than server_timeout. The
# turn player (B) stays silent. Expectation: because every message A sends
# is still "poprawny" (valid) — correct length, msg_type=MOVE_1, non-zero
# player_id, existing game_id, A participates — A's timer is reset each
# time. B's timer is not. After > server_timeout, B is the one who times
# out and loses.
# ============================================================================
print("Test 1: out-of-range pawn from wrong-turn player keeps THEIR timer alive")

st = 2  # server_timeout seconds
proc, port = start_server('1111', timeout_s=st)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    pa = 42
    pb = 99

    # A joins.
    sock.sendto(make_join(pa), addr)
    resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 3)
    if not s or s['status'] != 0:
        failures.append(f"T1 JOIN A: expected WAITING_FOR_OPPONENT, got {resp.hex()}")
    else:
        gid = s['game_id']

        # B joins -> TURN_B (status 2).
        sock.sendto(make_join(pb), addr)
        resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if not s or s['status'] != 2:
            failures.append(f"T1 JOIN B: expected TURN_B, got {resp.hex()}")
        else:
            # Pawn row right after join must be all 1s (no moves yet).
            post_join_pins = pins_to_str(s['pins'])
            if post_join_pins != '1111':
                failures.append(f"T1 post-join pins: expected '1111', got {post_join_pins!r}")

            # Spam illegal MOVE_1 from A (not A's turn, and pawn=200 is out of
            # range for a max_pawn=3 board). Do this for longer than
            # server_timeout. B stays silent.
            #
            # Window = st + 1s to exceed server_timeout with jitter headroom.
            # Cadence = 0.5s as specified. We send 6 illegal messages over 3s
            # when st=2.
            window_s = st + 1.0
            end_time = time.monotonic() + window_s
            illegal_sent = 0
            last_pins = None
            while time.monotonic() < end_time:
                sock.sendto(make_move1(pa, gid, 200), addr)
                illegal_sent += 1
                try:
                    resp, _ = sock.recvfrom(4096)
                except socket.timeout:
                    failures.append(f"T1: no response to illegal MOVE_1 #{illegal_sent}")
                    break
                # Per §3.3: "Komunikat zawierający niepoprawną wartość pola pawn
                # uznaje się za poprawny" — illegal MOVE_1 must still yield a
                # GAME_STATE, not a MSG_WRONG_MSG.
                if is_wrong(resp):
                    failures.append(
                        f"T1: illegal-pawn MOVE_1 #{illegal_sent} returned "
                        f"WRONG_MSG — spec says pawn value errors are still "
                        f"'poprawny' so server must reply GAME_STATE "
                        f"(resp={resp.hex()})")
                    break
                s2 = parse_state(resp, 3)
                if s2 is None:
                    failures.append(f"T1: bad GAME_STATE response #{illegal_sent}: {resp.hex()}")
                    break
                # Illegal move must not mutate pins.
                cur_pins = pins_to_str(s2['pins'])
                if cur_pins != post_join_pins:
                    failures.append(
                        f"T1: illegal MOVE_1 mutated pins from "
                        f"{post_join_pins!r} to {cur_pins!r}")
                    break
                last_pins = cur_pins
                # Status must stay TURN_B (2) until B times out, at which
                # point the server legitimately flips to WIN_A (3) — that's
                # exactly the end-state we want and we stop the spam early.
                # WIN_B (4) would mean either A was wrongly timed out, or
                # (for Test 2) the wrong-turn GIVE_UP was accepted — both
                # are bugs.
                if s2['status'] == 3:
                    break
                if s2['status'] != 2:
                    failures.append(
                        f"T1: illegal MOVE_1 changed status from TURN_B (2) "
                        f"to {s2['status']} (expected 2 while B alive, or 3 "
                        f"once B times out)")
                    break
                time.sleep(0.5)

            if illegal_sent < 2:
                failures.append(
                    f"T1: only managed to send {illegal_sent} illegal moves; "
                    f"test windowing is broken")

            # Now trigger the timeout-check: A sends a KEEP_ALIVE. At this
            # point, from the server's point of view, A's last valid message
            # was moments ago (the last illegal MOVE_1), but B has been silent
            # for > server_timeout. So B must be the timed-out loser.
            # Expectation: status = WIN_A (3).
            sock.sendto(make_ka(pa, gid), addr)
            try:
                resp, _ = sock.recvfrom(4096)
            except socket.timeout:
                failures.append("T1: no response to A's post-window KEEP_ALIVE")
            else:
                s3 = parse_state(resp, 3)
                if s3 is None:
                    failures.append(
                        f"T1: expected GAME_STATE on post-window KA, got "
                        f"{resp.hex()} (is_wrong={is_wrong(resp)})")
                elif s3['status'] != 3:
                    failures.append(
                        f"T1: expected WIN_A (3) after {window_s:.1f}s of "
                        f"valid-but-illegal MOVE_1 spam from A with B silent; "
                        f"got status={s3['status']}. This means the server "
                        f"did NOT count A's illegal-but-valid messages as "
                        f"reseting A's timer (violates §3.3 + §5.3).")
                else:
                    # Post-win state must still have original pawn_row: no pin
                    # was ever legally knocked.
                    final_pins = pins_to_str(s3['pins'])
                    if final_pins != '1111':
                        failures.append(
                            f"T1: post-win pins changed from '1111' to "
                            f"{final_pins!r} — illegal moves must not mutate "
                            f"state (§5.2)")
                    if last_pins is not None and last_pins != '1111':
                        failures.append(
                            f"T1: mid-stream pins drifted from '1111' to "
                            f"{last_pins!r}")
    sock.close()
finally:
    stop_proc(proc)

# ============================================================================
# Sub-test 2: Illegal GIVE_UP from the NON-turn player.
#
# §3.3 last sentence: "Ruch jest nielegalny również (...) gdy próbuje go
# wykonać gracz, którego nie jest kolej (dotyczy to także komunikatu
# MSG_GIVE_UP)."
#
# So MSG_GIVE_UP from a wrong-turn player is illegal (does NOT end the
# game), but it is valid (well-formed, msg_type=4, non-zero pid, existing
# game, participant). Therefore it must reset the sender's timer.
#
# Setup: A joins, B joins -> TURN_B. A (wrong turn) spams GIVE_UP for
# > server_timeout with B silent. Then A sends KA. Expectation: WIN_A
# (B timed out), and the game never transitioned through a GIVE_UP
# success.
# ============================================================================
print("Test 2: illegal GIVE_UP from wrong-turn player keeps THEIR timer alive")

st = 2
proc, port = start_server('1111', timeout_s=st)
try:
    addr = ('127.0.0.1', port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3.0)

    pa = 1001
    pb = 1002

    sock.sendto(make_join(pa), addr)
    resp, _ = sock.recvfrom(4096)
    s = parse_state(resp, 3)
    if not s or s['status'] != 0:
        failures.append(f"T2 JOIN A: expected WAITING_FOR_OPPONENT, got {resp.hex()}")
    else:
        gid = s['game_id']
        sock.sendto(make_join(pb), addr)
        resp, _ = sock.recvfrom(4096)
        s = parse_state(resp, 3)
        if not s or s['status'] != 2:
            failures.append(f"T2 JOIN B: expected TURN_B, got {resp.hex()}")
        else:
            post_join_pins = pins_to_str(s['pins'])
            if post_join_pins != '1111':
                failures.append(f"T2 post-join pins: expected '1111', got {post_join_pins!r}")

            window_s = st + 1.0
            end_time = time.monotonic() + window_s
            sent = 0
            while time.monotonic() < end_time:
                # A sends GIVE_UP — but it's B's turn, so this is illegal
                # (wrong-turn GIVE_UP; §3.3 last sentence).
                sock.sendto(make_give_up(pa, gid), addr)
                sent += 1
                try:
                    resp, _ = sock.recvfrom(4096)
                except socket.timeout:
                    failures.append(f"T2: no response to illegal GIVE_UP #{sent}")
                    break
                if is_wrong(resp):
                    failures.append(
                        f"T2: wrong-turn GIVE_UP #{sent} returned WRONG_MSG — "
                        f"spec says this is valid-but-illegal so server must "
                        f"reply GAME_STATE (resp={resp.hex()})")
                    break
                s2 = parse_state(resp, 3)
                if s2 is None:
                    failures.append(f"T2: bad GAME_STATE response #{sent}: {resp.hex()}")
                    break
                cur_pins = pins_to_str(s2['pins'])
                if cur_pins != post_join_pins:
                    failures.append(
                        f"T2: wrong-turn GIVE_UP mutated pins from "
                        f"{post_join_pins!r} to {cur_pins!r}")
                    break
                # Status must stay TURN_B until B times out, at which
                # point the server correctly flips to WIN_A (3). WIN_B (4)
                # would mean the server accepted A's wrong-turn GIVE_UP —
                # the exact bug this test is hunting.
                if s2['status'] == 3:
                    break
                if s2['status'] != 2:
                    failures.append(
                        f"T2: wrong-turn GIVE_UP from A transitioned status "
                        f"from TURN_B (2) to {s2['status']} — spec says "
                        f"wrong-turn GIVE_UP is illegal and must not change "
                        f"state (§3.3); WIN_A (3) via B timeout would be OK")
                    break
                time.sleep(0.5)

            if sent < 2:
                failures.append(
                    f"T2: only managed to send {sent} illegal GIVE_UPs; "
                    f"test windowing is broken")

            # A sends KA to trigger the server's timer re-evaluation.
            # Expectation: WIN_A (B timed out because only A sent valid
            # messages during the window).
            sock.sendto(make_ka(pa, gid), addr)
            try:
                resp, _ = sock.recvfrom(4096)
            except socket.timeout:
                failures.append("T2: no response to A's post-window KEEP_ALIVE")
            else:
                s3 = parse_state(resp, 3)
                if s3 is None:
                    failures.append(
                        f"T2: expected GAME_STATE on post-window KA, got "
                        f"{resp.hex()} (is_wrong={is_wrong(resp)})")
                elif s3['status'] != 3:
                    failures.append(
                        f"T2: expected WIN_A (3) after {window_s:.1f}s of "
                        f"valid-but-illegal GIVE_UP spam from A with B "
                        f"silent; got status={s3['status']}. The server did "
                        f"NOT accept A's illegal GIVE_UPs as timer-resetting "
                        f"valid messages (violates §3.3 + §5.3), OR it "
                        f"erroneously ended the game via A's wrong-turn "
                        f"GIVE_UP.")
                else:
                    final_pins = pins_to_str(s3['pins'])
                    if final_pins != '1111':
                        failures.append(
                            f"T2: post-win pins changed from '1111' to "
                            f"{final_pins!r} — no legal move ever occurred")
    sock.close()
finally:
    stop_proc(proc)

if failures:
    print(f"\n{len(failures)} FAILURES")
    for f in failures:
        print("  " + f)
    sys.exit(1)

print("\nAll illegal_moves_extend_timer tests passed.")
PYEOF

echo "All illegal_moves_extend_timer tests passed."
