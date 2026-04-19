#!/usr/bin/env bash
# Wire-level fuzz / malformed packet sweep.
#
# Axis 1 from the randomized integration suite spec.
#
# This test:
#   (a) Blasts the server with thousands of random-length / random-byte
#       UDP datagrams and verifies each one elicits a MSG_WRONG_MSG
#       response of exactly 14 bytes with status byte 0xFF at offset 12
#       and a plausible error_index at offset 13.
#   (b) For each *valid* msg_type with a *wrong* tail, verifies that
#       error_index matches the spec's per-type offsets.
#   (c) Bit-flip mutates a valid captured JOIN/MOVE_1/KA/GIVE_UP message
#       and verifies the server never crashes and always replies either
#       a valid GAME_STATE or MSG_WRONG_MSG.
#   (d) After all that abuse, the server must still be responsive:
#       a clean MSG_JOIN must elicit a clean WAITING GAME_STATE.
#
# Reproducibility: print the seed at the top. Failures can be reproduced
# with  RANDOM_SEED=<seed> bash tests/integration/test_fuzz_wire.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/run_tests.sh"

SEED="${RANDOM_SEED:-$RANDOM}"
echo "RANDOM_SEED=$SEED"

trap stop_server EXIT
PORT=$(get_random_port)
# pawn_row "111" — small board. Plenty of room for a real JOIN to come
# through after the fuzz campaign.
start_server "111" "$PORT" 30

# -----------------------------------------------------------------------------
# Python driver. We spawn a single python child that does all the fuzzing so
# that RNG state is kept in one place (seeded deterministically from $SEED)
# and so that we don't pay python-startup overhead thousands of times.
# -----------------------------------------------------------------------------
python3 - "$PORT" "$SEED" <<'PYEOF'
import os, random, socket, struct, sys

port = int(sys.argv[1])
seed = int(sys.argv[2])
random.seed(seed)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(2.0)
addr = ('127.0.0.1', port)

# Server's buffer is CLIENT_MESSAGE_SIZE_WITH_BUF = 12 bytes
# (1 type + 4 player + 4 game + 1 pawn + 2 buf). Any larger datagram is
# truncated server-side, so the server can only see up to 12 bytes.
CLIENT_BUF_SIZE = 12
SERVER_WRONG_MSG_LEN = 14  # 12 echo + status + error_index

# expected message sizes per valid type (bytes the full wire message takes).
EXPECTED_SIZE = {0: 5, 1: 10, 2: 10, 3: 9, 4: 9}

failures = []

def recv_one(label):
    try:
        return sock.recvfrom(4096)[0]
    except socket.timeout:
        failures.append(f"{label}: server did not respond")
        return None

def check_wrong_msg(resp, label, sent_bytes, expected_idx=None):
    """Verify resp is a well-formed MSG_WRONG_MSG. If expected_idx is not None,
    check that error_index matches."""
    if resp is None:
        return
    if len(resp) != SERVER_WRONG_MSG_LEN:
        failures.append(
            f"{label}: expected {SERVER_WRONG_MSG_LEN}-byte WRONG_MSG, got {len(resp)}: {resp.hex()}")
        return
    if resp[12] != 0xFF:
        failures.append(f"{label}: status byte at offset 12 should be 0xFF, got 0x{resp[12]:02x}")
        return
    # First 12 bytes must be the echo of what the server saw (i.e.,
    # first min(14, len(sent_bytes)) bytes, truncated further to 12).
    server_saw = sent_bytes[:CLIENT_BUF_SIZE]
    expected_echo = server_saw[:12] + b'\x00' * (12 - min(12, len(server_saw)))
    if resp[:12] != expected_echo:
        failures.append(
            f"{label}: echo mismatch. expected {expected_echo.hex()} got {resp[:12].hex()}")
        return
    if expected_idx is not None and resp[13] != expected_idx:
        failures.append(
            f"{label}: expected error_index={expected_idx}, got {resp[13]}")
        return

# ---------------------------------------------------------------------------
# 1a. Random garbage: 2000 random byte-strings of random length in [0, 50].
#     Every one must elicit a 14-byte WRONG_MSG (server must never crash,
#     never hang, never return any other length). We don't check error_index
#     values exhaustively here — the per-type checks below handle that.
# ---------------------------------------------------------------------------
N_RANDOM = 2000
for i in range(N_RANDOM):
    n = random.randint(0, 50)
    data = bytes(random.randint(0, 255) for _ in range(n))

    # Skip if by sheer chance we produced a VALID message (rare but possible
    # for short messages like JOIN with nonzero player). We still want the
    # fuzz campaign to exercise the rejection path.
    #
    # Accept: type in {0..4} AND server-visible length matches that type AND
    # player_id nonzero (JOIN) / ignore for others.
    server_visible = data[:CLIENT_BUF_SIZE]
    is_valid = False
    if len(server_visible) >= 1 and server_visible[0] <= 4:
        t = server_visible[0]
        if len(server_visible) == EXPECTED_SIZE[t]:
            # Parse player_id.
            if len(server_visible) >= 5:
                pid = struct.unpack('>I', server_visible[1:5])[0]
                if pid != 0:
                    is_valid = True

    sock.sendto(data, addr)
    resp = recv_one(f"random[{i}] len={n}")
    if resp is None:
        continue

    if is_valid:
        # Server will respond with GAME_STATE or WRONG_MSG (e.g. unknown
        # game_id). Both are acceptable here — we only assert non-crash.
        continue

    check_wrong_msg(resp, f"random[{i}]", data)

# ---------------------------------------------------------------------------
# 1b. Valid msg_type but random remaining bytes of random length.
#     Length-mismatch → error_index == min(expected_size, actual length).
#       - Too short: first missing byte is at index L (received length).
#       - Too long:  first unparseable byte is at index EXPECTED_SIZE[t].
#     Length-match but type==JOIN with player=0 → error_index == 1
#       (MSG_TYPE_SIZE; KaylesError::player_id_zero).
# ---------------------------------------------------------------------------
for i in range(500):
    t = random.randint(0, 4)
    tail_len = random.randint(0, 30)
    tail = bytes(random.randint(0, 255) for _ in range(tail_len))
    data = bytes([t]) + tail
    server_visible = data[:CLIENT_BUF_SIZE]
    L = len(server_visible)

    sock.sendto(data, addr)
    resp = recv_one(f"typed[{i}] t={t} L={L}")
    if resp is None:
        continue

    if L != EXPECTED_SIZE[t]:
        expected_idx = min(L, EXPECTED_SIZE[t])
        check_wrong_msg(resp, f"typed[{i}] t={t} L={L}", data,
                        expected_idx=expected_idx)
    else:
        # Length matches expected. Now player_id might be zero or nonzero,
        # game_id may be unknown, etc. If player_id is zero for any type,
        # error_index should be 1 (MSG_TYPE_SIZE). For JOIN with nonzero
        # player, the server will happily create/join a game — skip.
        pid = struct.unpack('>I', server_visible[1:5])[0]
        if pid == 0:
            check_wrong_msg(resp, f"typed[{i}] t={t} pid=0", data, expected_idx=1)
        elif t == 0:
            # Valid JOIN — server returns GAME_STATE. Skip (not our concern
            # in this fuzz test; other tests cover JOIN semantics).
            pass
        else:
            # MOVE / KA / GU with nonzero player but (most likely) unknown
            # game_id → WRONG_MSG with error_index == 5 (MSG_TYPE_SIZE +
            # PLAYER_ID_SIZE). If the random game_id happens to be 0 and
            # matches the first game the server created during this run,
            # we might get a GAME_STATE — that's OK too.
            if resp[12] == 0xFF:
                check_wrong_msg(resp, f"typed[{i}] t={t} unknown_gid", data,
                                expected_idx=5)

# ---------------------------------------------------------------------------
# 1c. Bit-flip mutations of a valid MSG_JOIN.
#     Baseline: JOIN(player_id=42) = 00 00 00 00 2a (5 bytes).
#     For each of several hundred iterations, pick a random subset of bits
#     in a 5-byte buffer and flip them. Server must respond with either
#     GAME_STATE (if mutation accidentally produces a valid message) or
#     WRONG_MSG. Never crash, never hang.
# ---------------------------------------------------------------------------
BASE = bytes([0, 0, 0, 0, 42])
for i in range(300):
    buf = bytearray(BASE)
    # Flip between 1 and 5 random bits.
    n_flips = random.randint(1, 5)
    for _ in range(n_flips):
        bit_idx = random.randint(0, len(buf) * 8 - 1)
        buf[bit_idx // 8] ^= (1 << (bit_idx % 8))
    sock.sendto(bytes(buf), addr)
    resp = recv_one(f"bitflip_join[{i}]")
    if resp is None:
        continue
    # Any valid server response must be either GAME_STATE or WRONG_MSG.
    # Both carry the status byte at offset 12.
    if len(resp) < 14:
        failures.append(f"bitflip_join[{i}]: response too short ({len(resp)}): {resp.hex()}")
        continue
    status = resp[12]
    if len(resp) == 14:
        if status != 0xFF:
            failures.append(
                f"bitflip_join[{i}]: 14-byte response must be WRONG_MSG "
                f"(status=0xFF), got status=0x{status:02x}: {resp.hex()}")
    else:
        if status not in (0, 1, 2, 3, 4):
            failures.append(
                f"bitflip_join[{i}]: GAME_STATE has invalid status {status}: "
                f"{resp.hex()}")

# ---------------------------------------------------------------------------
# 1d. Bit-flip mutations of a valid MSG_MOVE_1.
#     Baseline: MOVE_1(player=42, game=0, pawn=0) = 01 00 00 00 2a 00 00 00 00 00
#     Same sanity checks as above.
# ---------------------------------------------------------------------------
BASE = bytes([1, 0, 0, 0, 42, 0, 0, 0, 0, 0])
for i in range(300):
    buf = bytearray(BASE)
    n_flips = random.randint(1, 8)
    for _ in range(n_flips):
        bit_idx = random.randint(0, len(buf) * 8 - 1)
        buf[bit_idx // 8] ^= (1 << (bit_idx % 8))
    sock.sendto(bytes(buf), addr)
    resp = recv_one(f"bitflip_move1[{i}]")
    if resp is None:
        continue
    if len(resp) < 14:
        failures.append(f"bitflip_move1[{i}]: response too short ({len(resp)}): {resp.hex()}")
        continue
    status = resp[12]
    if len(resp) == 14:
        if status != 0xFF:
            failures.append(
                f"bitflip_move1[{i}]: 14-byte response must be WRONG_MSG, "
                f"got status=0x{status:02x}: {resp.hex()}")
    else:
        if status not in (0, 1, 2, 3, 4):
            failures.append(
                f"bitflip_move1[{i}]: GAME_STATE has invalid status {status}: "
                f"{resp.hex()}")

# ---------------------------------------------------------------------------
# 1e. Bit-flip mutations of KEEP_ALIVE and GIVE_UP.
# ---------------------------------------------------------------------------
for base_type in (3, 4):
    BASE = bytes([base_type, 0, 0, 0, 42, 0, 0, 0, 0])
    for i in range(150):
        buf = bytearray(BASE)
        n_flips = random.randint(1, 7)
        for _ in range(n_flips):
            bit_idx = random.randint(0, len(buf) * 8 - 1)
            buf[bit_idx // 8] ^= (1 << (bit_idx % 8))
        sock.sendto(bytes(buf), addr)
        resp = recv_one(f"bitflip_t{base_type}[{i}]")
        if resp is None:
            continue
        if len(resp) < 14:
            failures.append(
                f"bitflip_t{base_type}[{i}]: response too short ({len(resp)}): "
                f"{resp.hex()}")
            continue
        status = resp[12]
        if len(resp) == 14:
            if status != 0xFF:
                failures.append(
                    f"bitflip_t{base_type}[{i}]: 14-byte response must be "
                    f"WRONG_MSG, got status=0x{status:02x}: {resp.hex()}")
        else:
            if status not in (0, 1, 2, 3, 4):
                failures.append(
                    f"bitflip_t{base_type}[{i}]: GAME_STATE has invalid "
                    f"status {status}: {resp.hex()}")

# ---------------------------------------------------------------------------
# 1f. Responsiveness probe — after all that abuse, send a clean JOIN with a
#     nonzero player_id and verify a WAITING GameState comes back.
# ---------------------------------------------------------------------------
probe = bytes([0, 0x12, 0x34, 0x56, 0x78])  # JOIN player=0x12345678
sock.sendto(probe, addr)
resp = recv_one("final_probe")
if resp is not None:
    # GAME_STATE for this small board is 14 (header) + 1 (bitmap) = 15 bytes.
    # Depending on prior JOINs during the fuzz campaign, the game might be
    # in WAITING or TURN_B. Either way, status must be in {0, 1, 2}.
    if len(resp) != 15:
        failures.append(
            f"final_probe: expected 15-byte GAME_STATE, got {len(resp)}: "
            f"{resp.hex()}")
    elif resp[12] not in (0, 1, 2):
        failures.append(
            f"final_probe: expected status in 0..2, got {resp[12]}: {resp.hex()}")
    else:
        # player_a or player_b must equal 0x12345678.
        pa = struct.unpack('>I', resp[4:8])[0]
        pb = struct.unpack('>I', resp[8:12])[0]
        if pa != 0x12345678 and pb != 0x12345678:
            failures.append(
                f"final_probe: player 0x12345678 not in state: pa={pa:x}, pb={pb:x}")

# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------
if failures:
    print(f"FUZZ FAILURES: {len(failures)} (showing first 20)")
    for f in failures[:20]:
        print("  " + f)
    sys.exit(1)

print("Wire fuzz OK — server survived all mutations and remained responsive.")
PYEOF

echo "All fuzz_wire tests passed."
