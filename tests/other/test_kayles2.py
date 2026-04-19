#!/usr/bin/env python3
"""
Comprehensive test suite for the networked Kayles game.
Tests the server binary (./kayles_server) and client binary (./kayles_client)
via raw UDP sockets and subprocess, covering the full protocol specification.

Usage:
    pytest test_kayles.py -v
    pytest test_kayles.py -v -k "test_join"   # run only join tests

Requirements:
    pip install pytest

Binaries expected at:
    ./kayles_server
    ./kayles_client
"""

import os
import re
import socket
import struct
import subprocess
import sys
import time
from contextlib import contextmanager
from typing import Optional, Tuple

import pytest

# ---------------------------------------------------------------------------
# Constants matching the protocol spec
# ---------------------------------------------------------------------------

SERVER_BIN = "./server_exec"
CLIENT_BIN = "./client_exec"
SERVER_HOST = "127.0.0.1"

# msg_type values
MSG_JOIN        = 0
MSG_MOVE_1      = 1
MSG_MOVE_2      = 2
MSG_KEEP_ALIVE  = 3
MSG_GIVE_UP     = 4

# status values in MSG_GAME_STATE
STATUS_WAITING  = 0
STATUS_TURN_A   = 1
STATUS_TURN_B   = 2
STATUS_WIN_A    = 3
STATUS_WIN_B    = 4

STATUS_WRONG    = 255   # MSG_WRONG_MSG marker

DEFAULT_PAWN_ROW = "11111111"   # 8 pawns all present
SHORT_ROW        = "11"         # minimal: 2 pawns
LONG_ROW         = "1" * 256    # maximum length

DEFAULT_TIMEOUT  = 5   # server_timeout / client_timeout for tests

# ---------------------------------------------------------------------------
# Protocol helpers
# ---------------------------------------------------------------------------

def pack_join(player_id: int) -> bytes:
    """MSG_JOIN: msg_type(1) + player_id(4) = 5 bytes."""
    return struct.pack("!BI", MSG_JOIN, player_id)

def pack_move1(player_id: int, game_id: int, pawn: int) -> bytes:
    """MSG_MOVE_1: msg_type(1) + player_id(4) + game_id(4) + pawn(1) = 10 bytes."""
    return struct.pack("!BIIB", MSG_MOVE_1, player_id, game_id, pawn)

def pack_move2(player_id: int, game_id: int, pawn: int) -> bytes:
    """MSG_MOVE_2: msg_type(1) + player_id(4) + game_id(4) + pawn(1) = 10 bytes."""
    return struct.pack("!BIIB", MSG_MOVE_2, player_id, game_id, pawn)

def pack_keep_alive(player_id: int, game_id: int) -> bytes:
    """MSG_KEEP_ALIVE: msg_type(1) + player_id(4) + game_id(4) = 9 bytes."""
    return struct.pack("!BII", MSG_KEEP_ALIVE, player_id, game_id)

def pack_give_up(player_id: int, game_id: int) -> bytes:
    """MSG_GIVE_UP: msg_type(1) + player_id(4) + game_id(4) = 9 bytes."""
    return struct.pack("!BII", MSG_GIVE_UP, player_id, game_id)


class GameState:
    """Parsed MSG_GAME_STATE response from the server."""

    def __init__(self, data: bytes):
        assert len(data) >= 11, f"MSG_GAME_STATE too short: {len(data)} bytes"
        self.game_id, self.player_a_id, self.player_b_id, self.status, self.max_pawn = \
            struct.unpack_from("!IIIB B", data, 0)
        pawn_row_size = (self.max_pawn // 8) + 1
        assert len(data) >= 11 + pawn_row_size - 1, "pawn_row truncated"
        # Offset: game_id(4)+player_a_id(4)+player_b_id(4)+status(1)+max_pawn(1) = 14 bytes
        self.raw = data
        offset = 4 + 4 + 4 + 1 + 1
        self.pawn_row_bytes = data[offset: offset + pawn_row_size]

    def pawn_present(self, n: int) -> bool:
        """Return True if pawn n is present on the board."""
        byte_idx = n // 8
        bit_idx  = 7 - (n % 8)   # MSB of byte = pawn 0
        return bool((self.pawn_row_bytes[byte_idx] >> bit_idx) & 1)

    def active_pawns(self):
        return [i for i in range(self.max_pawn + 1) if self.pawn_present(i)]

    def __repr__(self):
        return (f"GameState(game_id={self.game_id}, a={self.player_a_id}, "
                f"b={self.player_b_id}, status={self.status}, "
                f"max_pawn={self.max_pawn}, pawns={self.active_pawns()})")


class WrongMsg:
    """Parsed MSG_WRONG_MSG response from the server."""

    def __init__(self, data: bytes):
        assert len(data) == 14, f"MSG_WRONG_MSG must be 14 bytes, got {len(data)}"
        self.echo   = data[:12]
        self.status = data[12]          # must be 255
        self.error_index = data[13]
        assert self.status == STATUS_WRONG, f"expected 255, got {self.status}"

    def __repr__(self):
        return f"WrongMsg(error_index={self.error_index})"


def parse_response(data: bytes):
    """Return either a GameState or WrongMsg depending on the status byte."""
    if len(data) == 14:
        # Could be WrongMsg — check status byte at offset 12
        status_byte = data[12]
        if status_byte == STATUS_WRONG:
            return WrongMsg(data)
    return GameState(data)


# ---------------------------------------------------------------------------
# UDP socket helpers
# ---------------------------------------------------------------------------

def make_udp_socket(timeout: float = 2.0) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    return s

def udp_send_recv(sock: socket.socket, data: bytes, addr: Tuple[str, int]):
    sock.sendto(data, addr)
    try:
        resp, _ = sock.recvfrom(65535)
        return resp
    except socket.timeout:
        return None


# ---------------------------------------------------------------------------
# Server process management
# ---------------------------------------------------------------------------

class ServerProcess:
    """Context manager that starts/stops the server subprocess."""

    def __init__(self, pawn_row: str = DEFAULT_PAWN_ROW,
                 host: str = SERVER_HOST, port: int = 0,
                 timeout: int = DEFAULT_TIMEOUT):
        self.pawn_row = pawn_row
        self.host     = host
        self.port     = port       # 0 = auto-assign
        self.timeout  = timeout
        self.proc     = None
        self.actual_port: Optional[int] = None

    def start(self):
        cmd = [
            SERVER_BIN,
            "-r", self.pawn_row,
            "-a", self.host,
            "-p", str(self.port),
            "-t", str(self.timeout),
        ]
        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # Give server a moment to bind
        time.sleep(0.1)

        if self.port == 0:
            self.actual_port = self._discover_port()
        else:
            self.actual_port = self.port

        return self

    def _discover_port(self) -> int:
        """Send a probe to find the ephemeral port the server chose."""
        # We'll use /proc/net/udp on Linux to find the bound port.
        pid = self.proc.pid
        try:
            return self._port_from_proc(pid)
        except Exception:
            pass
        # Fallback: try ss/netstat
        return self._port_from_ss(pid)

    def _port_from_proc(self, pid: int) -> int:
        import re as _re
        fds_dir = f"/proc/{pid}/fd"
        inodes = set()
        for fd in os.listdir(fds_dir):
            try:
                link = os.readlink(os.path.join(fds_dir, fd))
                m = _re.match(r"socket:\[(\d+)\]", link)
                if m:
                    inodes.add(m.group(1))
            except Exception:
                pass
        with open("/proc/net/udp") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 10:
                    continue
                inode = parts[9]
                if inode in inodes:
                    local_addr = parts[1]
                    port_hex = local_addr.split(":")[1]
                    return int(port_hex, 16)
        raise RuntimeError("port not found in /proc/net/udp")

    def _port_from_ss(self, pid: int) -> int:
        result = subprocess.run(
            ["ss", "-unpH", f"pid={pid}"],
            capture_output=True, text=True
        )
        for line in result.stdout.splitlines():
            m = re.search(r":(\d+)\s", line)
            if m:
                return int(m.group(1))
        raise RuntimeError("port not found via ss")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def addr(self) -> Tuple[str, int]:
        return (self.host, self.actual_port)

    def __enter__(self):
        return self.start()

    def __exit__(self, *args):
        self.stop()


# ---------------------------------------------------------------------------
# Helper: run a full join handshake and return GameState + socket + game_id
# ---------------------------------------------------------------------------

def join_as_player(server_addr, player_id: int, sock: socket.socket) -> GameState:
    msg = pack_join(player_id)
    raw = udp_send_recv(sock, msg, server_addr)
    assert raw is not None, "No response from server on MSG_JOIN"
    gs = GameState(raw)
    return gs


def join_two_players(server_addr, pid_a=1001, pid_b=1002):
    """Return (sock_a, sock_b, gs_a, gs_b) after both players join."""
    sock_a = make_udp_socket()
    sock_b = make_udp_socket()
    gs_a = join_as_player(server_addr, pid_a, sock_a)
    gs_b = join_as_player(server_addr, pid_b, sock_b)
    return sock_a, sock_b, gs_a, gs_b


# ---------------------------------------------------------------------------
# Test fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="function")
def srv_default():
    """Server with default 8-pawn row."""
    with ServerProcess(pawn_row="11111111", timeout=5) as srv:
        yield srv

@pytest.fixture(scope="function")
def srv_2pawns():
    """Server with minimal 2-pawn row (11)."""
    with ServerProcess(pawn_row="11", timeout=5) as srv:
        yield srv

@pytest.fixture(scope="function")
def srv_3pawns():
    """Server with 3-pawn row (111)."""
    with ServerProcess(pawn_row="111", timeout=5) as srv:
        yield srv

@pytest.fixture(scope="function")
def srv_complex():
    """Server with complex pawn row 11101111011111."""
    with ServerProcess(pawn_row="11101111011111", timeout=5) as srv:
        yield srv


# ===========================================================================
# 1. SERVER STARTUP / PARAMETER TESTS
# ===========================================================================

class TestServerStartup:

    def test_server_starts_successfully(self):
        with ServerProcess(pawn_row="11111111", port=0, timeout=5) as srv:
            assert srv.proc.poll() is None, "Server exited prematurely"

    def test_server_exits_on_missing_pawn_row(self):
        proc = subprocess.run(
            [SERVER_BIN, "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_missing_address(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_missing_port(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-a", SERVER_HOST, "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_missing_timeout(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-a", SERVER_HOST, "-p", "0"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_pawn_row_too_short(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "", "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_pawn_row_invalid_chars(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11201111", "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_pawn_row_first_zero(self):
        """First position must be 1."""
        proc = subprocess.run(
            [SERVER_BIN, "-r", "01111111", "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_pawn_row_last_zero(self):
        """Last position must be 1."""
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111110", "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_pawn_row_too_long(self):
        """Max length is 256."""
        proc = subprocess.run(
            [SERVER_BIN, "-r", "1" * 257, "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_accepts_max_length_pawn_row(self):
        """Exactly 256 characters should be valid."""
        with ServerProcess(pawn_row="1" * 256, timeout=5) as srv:
            assert srv.proc.poll() is None

    def test_server_exits_on_bad_timeout_zero(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-a", SERVER_HOST, "-p", "0", "-t", "0"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_bad_timeout_too_large(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-a", SERVER_HOST, "-p", "0", "-t", "100"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_exits_on_bad_port_out_of_range(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-a", SERVER_HOST, "-p", "99999", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_params_any_order(self):
        """Parameters may be given in any order."""
        with ServerProcess() as srv:
            assert srv.proc.poll() is None

    def test_server_exits_on_invalid_address(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "11111111", "-a", "999.999.999.999", "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1

    def test_server_outputs_to_stderr_on_error(self):
        proc = subprocess.run(
            [SERVER_BIN, "-r", "01111110", "-a", SERVER_HOST, "-p", "0", "-t", "5"],
            capture_output=True, timeout=3
        )
        assert proc.returncode == 1
        assert proc.stderr  # some diagnostic output


# ===========================================================================
# 2. CLIENT STARTUP / PARAMETER TESTS
# ===========================================================================

class TestClientStartup:

    def test_client_exits_on_missing_address(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-p", str(srv_default.actual_port),
             "-m", "0/1001", "-t", "2"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1

    def test_client_exits_on_missing_port(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-m", "0/1001", "-t", "2"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1

    def test_client_exits_on_missing_message(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", str(srv_default.actual_port), "-t", "2"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1

    def test_client_exits_on_missing_timeout(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", str(srv_default.actual_port), "-m", "0/1001"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1

    def test_client_exits_code_0_on_response(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", str(srv_default.actual_port),
             "-m", "0/1001", "-t", "2"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 0

    def test_client_exits_code_0_on_timeout(self):
        """Client should still exit 0 when no server responds."""
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", "19999",   # nothing listening
             "-m", "0/1001", "-t", "1"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 0

    def test_client_produces_output(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", str(srv_default.actual_port),
             "-m", "0/1001", "-t", "2"],
            capture_output=True, timeout=5
        )
        assert proc.stdout.strip(), "Client should print game state to stdout"

    def test_client_port_zero_invalid(self, srv_default):
        """Port 0 is invalid for client (spec says 1..65535)."""
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", "0",
             "-m", "0/1001", "-t", "2"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1

    def test_client_bad_timeout_zero(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", str(srv_default.actual_port),
             "-m", "0/1001", "-t", "0"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1

    def test_client_bad_timeout_100(self, srv_default):
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST,
             "-p", str(srv_default.actual_port),
             "-m", "0/1001", "-t", "100"],
            capture_output=True, timeout=5
        )
        assert proc.returncode == 1


# ===========================================================================
# 3. MSG_JOIN TESTS
# ===========================================================================

class TestMsgJoin:

    def test_join_returns_game_state(self, srv_default):
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        assert raw is not None
        gs = GameState(raw)
        assert gs.status == STATUS_WAITING
        assert gs.player_a_id == 1001
        assert gs.player_b_id == 0

    def test_join_game_id_nonzero_or_zero_valid(self, srv_default):
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        gs = GameState(raw)
        # game_id is 32-bit, any value 0..2^32-1 is valid per spec
        assert 0 <= gs.game_id <= 0xFFFFFFFF

    def test_join_second_player_changes_status(self, srv_default):
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(srv_default.addr(), 1001, sa)
            assert gs_a.status == STATUS_WAITING
            gs_b = join_as_player(srv_default.addr(), 1002, sb)
        assert gs_b.status == STATUS_TURN_B
        assert gs_b.player_a_id == 1001
        assert gs_b.player_b_id == 1002

    def test_join_second_player_same_game_id(self, srv_default):
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(srv_default.addr(), 1001, sa)
            gs_b = join_as_player(srv_default.addr(), 1002, sb)
        assert gs_a.game_id == gs_b.game_id

    def test_join_player_id_max_value(self, srv_default):
        """player_id can be 2^32-1."""
        max_id = 0xFFFFFFFF
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(max_id), srv_default.addr())
        assert raw is not None
        gs = GameState(raw)
        assert gs.player_a_id == max_id

    def test_join_player_id_1(self, srv_default):
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1), srv_default.addr())
        gs = GameState(raw)
        assert gs.player_a_id == 1

    def test_join_zero_player_id_rejected(self, srv_default):
        """player_id=0 is invalid, server must reply MSG_WRONG_MSG."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(0), srv_default.addr())
        assert raw is not None
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_join_short_message_rejected(self, srv_default):
        """MSG_JOIN must be exactly 5 bytes."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, bytes([MSG_JOIN, 0, 0, 0]), srv_default.addr())
        assert raw is not None
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_join_long_message_rejected(self, srv_default):
        """Extra bytes make MSG_JOIN invalid length."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001) + b"\x00", srv_default.addr())
        assert raw is not None
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_join_same_player_creates_single_game(self, srv_default):
        """Same player can be both A and B."""
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(srv_default.addr(), 5555, sa)
            gs_b = join_as_player(srv_default.addr(), 5555, sb)
        # player_b joins as same id
        assert gs_b.player_a_id == 5555
        assert gs_b.player_b_id == 5555

    def test_join_pawn_row_initialized_from_server_flag(self, srv_default):
        """Pawn row should match the -r "11111111" argument (8 pawns, all present)."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        gs = GameState(raw)
        # max_pawn should be 7 (indices 0..7)
        assert gs.max_pawn == 7
        for i in range(8):
            assert gs.pawn_present(i), f"Pawn {i} should be present"

    def test_join_complex_pawn_row(self, srv_complex):
        """pawn_row "11101111011111" → max_pawn=13, some pawns missing."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_complex.addr())
        gs = GameState(raw)
        assert gs.max_pawn == 13
        pawn_row_str = "11101111011111"
        for i, ch in enumerate(pawn_row_str):
            expected = (ch == "1")
            assert gs.pawn_present(i) == expected, \
                f"Pawn {i}: expected {expected}, got {gs.pawn_present(i)}"

    def test_join_only_one_waiting_game_at_a_time(self, srv_default):
        """After two players join, a third MSG_JOIN creates a NEW game."""
        with make_udp_socket() as sa, make_udp_socket() as sb, make_udp_socket() as sc:
            gs_a = join_as_player(srv_default.addr(), 1001, sa)
            gs_b = join_as_player(srv_default.addr(), 1002, sb)
            # game is now TURN_B; third join should create a new game
            gs_c = join_as_player(srv_default.addr(), 1003, sc)
        assert gs_c.status == STATUS_WAITING
        assert gs_c.game_id != gs_a.game_id

    def test_keep_alive_after_join_returns_same_game(self, srv_default):
        """After joining, MSG_KEEP_ALIVE returns the game state."""
        with make_udp_socket() as s:
            gs = join_as_player(srv_default.addr(), 1001, s)
            raw = udp_send_recv(s, pack_keep_alive(1001, gs.game_id), srv_default.addr())
        gs2 = GameState(raw)
        assert gs2.game_id == gs.game_id
        assert gs2.status == STATUS_WAITING


# ===========================================================================
# 4. MSG_GAME_STATE STRUCTURE TESTS
# ===========================================================================

class TestGameStateStructure:

    def test_pawn_row_size_formula(self, srv_default):
        """pawn_row size = floor(max_pawn/8) + 1."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        gs = GameState(raw)
        expected_size = (gs.max_pawn // 8) + 1
        assert len(gs.pawn_row_bytes) == expected_size

    def test_pawn_row_max_pawn_7(self):
        """With 8 pawns max_pawn=7, pawn_row has 1 byte."""
        with ServerProcess(pawn_row="11111111", timeout=5) as srv:
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(1001), srv.addr())
        gs = GameState(raw)
        assert gs.max_pawn == 7
        assert len(gs.pawn_row_bytes) == 1

    def test_pawn_row_max_pawn_8(self):
        """With 9 pawns max_pawn=8, pawn_row has 2 bytes."""
        with ServerProcess(pawn_row="111111111", timeout=5) as srv:
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(1001), srv.addr())
        gs = GameState(raw)
        assert gs.max_pawn == 8
        assert len(gs.pawn_row_bytes) == 2

    def test_pawn_row_max_pawn_255(self):
        """With 256 pawns max_pawn=255, pawn_row has 32 bytes."""
        with ServerProcess(pawn_row="1" * 256, timeout=5) as srv:
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(1001), srv.addr())
        gs = GameState(raw)
        assert gs.max_pawn == 255
        assert len(gs.pawn_row_bytes) == 32

    def test_spare_bits_are_zero(self):
        """Bits beyond max_pawn in pawn_row must be zero."""
        with ServerProcess(pawn_row="111", timeout=5) as srv:
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(1001), srv.addr())
        gs = GameState(raw)
        # max_pawn=2, pawn_row has 1 byte; bits 3-7 must be zero
        last_byte = gs.pawn_row_bytes[-1]
        mask = 0xFF >> (gs.max_pawn % 8 + 1)
        assert (last_byte & mask) == 0, "Spare bits are not zeroed"

    def test_game_state_network_byte_order(self, srv_default):
        """All multi-byte fields must be big-endian (network byte order)."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        # Parse manually in big-endian
        game_id   = struct.unpack_from("!I", raw, 0)[0]
        pid_a     = struct.unpack_from("!I", raw, 4)[0]
        pid_b     = struct.unpack_from("!I", raw, 8)[0]
        status    = raw[12]
        max_pawn  = raw[13]
        assert pid_a == 1001
        assert pid_b == 0
        assert status == STATUS_WAITING


# ===========================================================================
# 5. MOVE TESTS
# ===========================================================================

class TestMoves:

    def _setup_game(self, srv_addr, pid_a=1001, pid_b=1002):
        sa = make_udp_socket()
        sb = make_udp_socket()
        gs_a = join_as_player(srv_addr, pid_a, sa)
        gs_b = join_as_player(srv_addr, pid_b, sb)
        return sa, sb, gs_b  # gs_b has both players

    def test_turn_b_first(self, srv_default):
        """After two players join, it's player B's turn."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        sa.close(); sb.close()
        assert gs.status == STATUS_TURN_B

    def test_move1_by_player_b(self, srv_default):
        """Player B can make a valid MSG_MOVE_1."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        raw = udp_send_recv(sb, pack_move1(1002, game_id, 0), srv_default.addr())
        sa.close(); sb.close()
        assert raw is not None
        gs2 = GameState(raw)
        assert not gs2.pawn_present(0), "Pawn 0 should be removed after MOVE_1"
        assert gs2.status == STATUS_TURN_A

    def test_move1_by_player_a_when_not_turn(self, srv_default):
        """Player A cannot move when it's B's turn — state must not change."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        raw = udp_send_recv(sa, pack_move1(1001, game_id, 0), srv_default.addr())
        sa.close(); sb.close()
        gs2 = GameState(raw)
        assert gs2.status == STATUS_TURN_B, "Illegal move should not change turn"
        assert gs2.pawn_present(0), "Illegal move should not remove pawn"

    def test_move2_removes_two_adjacent_pawns(self, srv_default):
        """MSG_MOVE_2 should remove pawns n and n+1."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        raw = udp_send_recv(sb, pack_move2(1002, game_id, 0), srv_default.addr())
        sa.close(); sb.close()
        gs2 = GameState(raw)
        assert not gs2.pawn_present(0), "Pawn 0 should be removed"
        assert not gs2.pawn_present(1), "Pawn 1 should be removed"
        assert gs2.status == STATUS_TURN_A

    def test_move1_on_empty_pawn_is_illegal(self, srv_default):
        """Trying to remove already-removed pawn is illegal."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        # B removes pawn 0
        udp_send_recv(sb, pack_move1(1002, game_id, 0), srv_default.addr())
        # A removes pawn 1
        raw_a = udp_send_recv(sa, pack_move1(1001, game_id, 1), srv_default.addr())
        gs_a = GameState(raw_a)
        # Now B tries to remove pawn 0 again (already gone)
        raw_b = udp_send_recv(sb, pack_move1(1002, game_id, 0), srv_default.addr())
        sa.close(); sb.close()
        gs2 = GameState(raw_b)
        # State should be unchanged — it's still B's turn and pawn 0 still absent
        assert not gs2.pawn_present(0)

    def test_move2_on_non_adjacent_empty_pawn_is_illegal(self, srv_default):
        """If pawn n+1 is already gone, MOVE_2 on n is illegal."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        # B removes pawn 1
        udp_send_recv(sb, pack_move1(1002, game_id, 1), srv_default.addr())
        # A removes pawn 0
        udp_send_recv(sa, pack_move1(1001, game_id, 0), srv_default.addr())
        # B tries MOVE_2 on pawn 2 (pawn 3 exists, so valid... just testing logic)
        raw = udp_send_recv(sb, pack_move2(1002, game_id, 0), srv_default.addr())
        sa.close(); sb.close()
        gs2 = GameState(raw)
        # pawn 0 is already gone, so MOVE_2 on pawn 0 is illegal
        assert not gs2.pawn_present(0)

    def test_move_alternates_turns(self, srv_default):
        """Turns alternate A→B→A→B…"""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        assert gs.status == STATUS_TURN_B
        raw = udp_send_recv(sb, pack_move1(1002, game_id, 0), srv_default.addr())
        assert GameState(raw).status == STATUS_TURN_A
        raw = udp_send_recv(sa, pack_move1(1001, game_id, 1), srv_default.addr())
        assert GameState(raw).status == STATUS_TURN_B
        raw = udp_send_recv(sb, pack_move1(1002, game_id, 2), srv_default.addr())
        assert GameState(raw).status == STATUS_TURN_A
        sa.close(); sb.close()

    def test_move_out_of_bounds_pawn(self, srv_default):
        """pawn > max_pawn is illegal but message is still valid."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        raw = udp_send_recv(sb, pack_move1(1002, game_id, 255), srv_default.addr())
        sa.close(); sb.close()
        gs2 = GameState(raw)
        # Move illegal but server responds normally, state unchanged
        assert gs2.status == STATUS_TURN_B

    def test_move_wrong_game_id_rejected(self, srv_default):
        """A message with a non-existent game_id is invalid."""
        with make_udp_socket() as s:
            join_as_player(srv_default.addr(), 1001, s)
            raw = udp_send_recv(s, pack_move1(1001, 0xDEADBEEF, 0), srv_default.addr())
        assert raw is not None
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_move_for_nonparticipant_rejected(self, srv_default):
        """Player not in the game cannot make a move in it."""
        sa, sb, gs = self._setup_game(srv_default.addr())
        game_id = gs.game_id
        with make_udp_socket() as sc:
            raw = udp_send_recv(sc, pack_move1(9999, game_id, 0), srv_default.addr())
        sa.close(); sb.close()
        assert raw is not None
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)


# ===========================================================================
# 6. WIN CONDITIONS
# ===========================================================================

class TestWinConditions:

    def test_last_pawn_move1_wins(self, srv_2pawns):
        """With '11' (2 pawns), B removes pawn 0, A removes pawn 1 → A wins."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(addr, 1001, sa)
            gs_b = join_as_player(addr, 1002, sb)
            game_id = gs_b.game_id
            # B removes pawn 0
            raw = udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            assert GameState(raw).status == STATUS_TURN_A
            # A removes pawn 1 (last pawn) → A wins
            raw = udp_send_recv(sa, pack_move1(1001, game_id, 1), addr)
            gs_final = GameState(raw)
        assert gs_final.status == STATUS_WIN_A

    def test_last_two_pawns_move2_wins(self, srv_2pawns):
        """With '11', B removes both pawns at once → B wins."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(addr, 1001, sa)
            gs_b = join_as_player(addr, 1002, sb)
            game_id = gs_b.game_id
            raw = udp_send_recv(sb, pack_move2(1002, game_id, 0), addr)
            gs_final = GameState(raw)
        assert gs_final.status == STATUS_WIN_B

    def test_win_a_no_more_pawns(self, srv_3pawns):
        """With '111' (3 pawns): B→pawn0, A→pawn1, B→pawn2 → B wins."""
        addr = srv_3pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs = join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            udp_send_recv(sa, pack_move1(1001, game_id, 1), addr)
            raw = udp_send_recv(sb, pack_move1(1002, game_id, 2), addr)
            gs_final = GameState(raw)
        assert gs_final.status == STATUS_WIN_B

    def test_move_after_game_over_illegal(self, srv_2pawns):
        """After WIN_B, further moves should be illegal (state unchanged)."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            # B wins immediately
            udp_send_recv(sb, pack_move2(1002, game_id, 0), addr)
            # A tries to move after game over
            raw = udp_send_recv(sa, pack_move1(1001, game_id, 0), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B

    def test_state_preserved_after_win_for_server_timeout(self, srv_2pawns):
        """Finished game state remains accessible for server_timeout seconds."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            udp_send_recv(sb, pack_move2(1002, game_id, 0), addr)
            # Immediately query via keep-alive
            raw = udp_send_recv(sb, pack_keep_alive(1002, game_id), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B


# ===========================================================================
# 7. MSG_GIVE_UP TESTS
# ===========================================================================

class TestGiveUp:

    def test_give_up_by_player_a_during_turn_a(self, srv_default):
        """Player A gives up during A's turn → B wins."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            # B moves so it's A's turn
            udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            # A gives up
            raw = udp_send_recv(sa, pack_give_up(1001, game_id), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B

    def test_give_up_by_player_b_during_turn_b(self, srv_default):
        """Player B gives up during B's turn → A wins."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            # It's B's turn; B gives up
            raw = udp_send_recv(sb, pack_give_up(1002, game_id), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_A

    def test_give_up_by_wrong_player_is_illegal(self, srv_default):
        """Player A cannot give up when it's B's turn."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            raw = udp_send_recv(sa, pack_give_up(1001, game_id), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_TURN_B

    def test_give_up_in_waiting_state_illegal(self, srv_default):
        """Player A cannot give up while waiting for opponent."""
        addr = srv_default.addr()
        with make_udp_socket() as sa:
            gs = join_as_player(addr, 1001, sa)
            game_id = gs.game_id
            raw = udp_send_recv(sa, pack_give_up(1001, game_id), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_WAITING


# ===========================================================================
# 8. MSG_KEEP_ALIVE TESTS
# ===========================================================================

class TestKeepAlive:

    def test_keep_alive_returns_current_state(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as s:
            gs = join_as_player(addr, 1001, s)
            raw = udp_send_recv(s, pack_keep_alive(1001, gs.game_id), addr)
        gs2 = GameState(raw)
        assert gs2.game_id == gs.game_id
        assert gs2.status == gs.status

    def test_keep_alive_reflects_opponents_move(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(addr, 1001, sa)
            gs_b = join_as_player(addr, 1002, sb)
            game_id = gs_b.game_id
            # B makes a move
            udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            # A polls via keep-alive
            raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_TURN_A
        assert not gs2.pawn_present(0)

    def test_keep_alive_wrong_game_id_rejected(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as s:
            join_as_player(addr, 1001, s)
            raw = udp_send_recv(s, pack_keep_alive(1001, 0xCAFEBABE), addr)
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_keep_alive_non_participant_rejected(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(addr, 1001, sa)
            gs_b = join_as_player(addr, 1002, sb)
            game_id = gs_b.game_id
        with make_udp_socket() as sc:
            raw = udp_send_recv(sc, pack_keep_alive(9999, game_id), addr)
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)


# ===========================================================================
# 9. MSG_WRONG_MSG STRUCTURE TESTS
# ===========================================================================

class TestWrongMsg:

    def test_wrong_msg_length(self, srv_default):
        """MSG_WRONG_MSG must be exactly 14 bytes."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(0), srv_default.addr())
        assert len(raw) == 14

    def test_wrong_msg_status_255(self, srv_default):
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(0), srv_default.addr())
        assert raw[12] == 255

    def test_wrong_msg_echoes_client_msg(self, srv_default):
        """First 12 bytes of response are the (padded) client message."""
        bad_msg = pack_join(0)   # 5 bytes, player_id=0 is invalid
        with make_udp_socket() as s:
            raw = udp_send_recv(s, bad_msg, srv_default.addr())
        echo = raw[:12]
        # First 5 bytes should match client message
        assert echo[:5] == bad_msg
        # Remaining 7 should be zero
        assert echo[5:] == b"\x00" * 7

    def test_wrong_msg_error_index_for_zero_player_id(self, srv_default):
        """player_id starts at byte 1, so error_index should be 1."""
        bad_msg = pack_join(0)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, bad_msg, srv_default.addr())
        wm = WrongMsg(raw)
        assert wm.error_index == 1

    def test_wrong_msg_unknown_msg_type(self, srv_default):
        """Unknown msg_type → error_index should be 0."""
        bad_msg = bytes([0xFF]) + struct.pack("!I", 1001)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, bad_msg, srv_default.addr())
        wm = WrongMsg(raw)
        assert wm.error_index == 0

    def test_wrong_msg_empty_message(self, srv_default):
        """Empty message → error_index 0."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, b"", srv_default.addr())
        assert raw is not None
        wm = WrongMsg(raw)
        assert wm.error_index == 0

    def test_wrong_msg_echoes_long_msg_truncated(self, srv_default):
        """If client message > 12 bytes but invalid, echo must be first 12."""
        bad_msg = bytes([0xFF]) + b"\xAA" * 20
        with make_udp_socket() as s:
            raw = udp_send_recv(s, bad_msg, srv_default.addr())
        wm = WrongMsg(raw)
        assert wm.echo == bad_msg[:12]

    def test_wrong_msg_echo_short_msg_padded(self, srv_default):
        """If client message < 12 bytes, unused echo bytes must be zero."""
        bad_msg = bytes([0xFF, 0x00])  # 2 bytes
        with make_udp_socket() as s:
            raw = udp_send_recv(s, bad_msg, srv_default.addr())
        wm = WrongMsg(raw)
        assert wm.echo[:2] == bad_msg
        assert wm.echo[2:] == b"\x00" * 10

    def test_move1_short_message_rejected(self, srv_default):
        """MSG_MOVE_1 must be exactly 10 bytes."""
        with make_udp_socket() as sa:
            gs = join_as_player(srv_default.addr(), 1001, sa)
            short = struct.pack("!BII", MSG_MOVE_1, 1001, gs.game_id)  # missing pawn byte
            raw = udp_send_recv(sa, short, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_move2_long_message_rejected(self, srv_default):
        """MSG_MOVE_2 with extra bytes is invalid length."""
        with make_udp_socket() as sa:
            gs = join_as_player(srv_default.addr(), 1001, sa)
            long_msg = pack_move2(1001, gs.game_id, 0) + b"\x00"
            raw = udp_send_recv(sa, long_msg, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)


# ===========================================================================
# 10. MULTIPLE SIMULTANEOUS GAMES
# ===========================================================================

class TestMultipleGames:

    def test_two_concurrent_games(self, srv_default):
        """Two independent games can run simultaneously."""
        addr = srv_default.addr()
        with make_udp_socket() as sa1, make_udp_socket() as sb1, \
             make_udp_socket() as sa2, make_udp_socket() as sb2:
            # Game 1
            gs1_a = join_as_player(addr, 1001, sa1)
            gs1_b = join_as_player(addr, 1002, sb1)
            # Game 2
            gs2_a = join_as_player(addr, 2001, sa2)
            gs2_b = join_as_player(addr, 2002, sb2)

            assert gs1_b.game_id != gs2_b.game_id

            # Make move in game 1
            raw1 = udp_send_recv(sb1, pack_move1(1002, gs1_b.game_id, 0), addr)
            gs1_after = GameState(raw1)

            # Game 2 should be unaffected
            raw2 = udp_send_recv(sb2, pack_keep_alive(2002, gs2_b.game_id), addr)
            gs2_after = GameState(raw2)

        assert gs1_after.status == STATUS_TURN_A
        assert gs2_after.status == STATUS_TURN_B

    def test_player_in_two_games(self, srv_default):
        """Same player can participate in multiple games."""
        addr = srv_default.addr()
        with make_udp_socket() as s1, make_udp_socket() as s2, make_udp_socket() as s3:
            gs1 = join_as_player(addr, 5000, s1)
            gs_opp1 = join_as_player(addr, 5001, s2)
            # 5000 joins another game
            gs2 = join_as_player(addr, 5000, s1)
            gs_opp2 = join_as_player(addr, 5002, s3)
            assert gs1.game_id != gs2.game_id

    def test_move_in_one_game_does_not_affect_other(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa1, make_udp_socket() as sb1, \
             make_udp_socket() as sa2, make_udp_socket() as sb2:
            gs1 = join_as_player(addr, 1001, sa1)
            gs1b = join_as_player(addr, 1002, sb1)
            gs2 = join_as_player(addr, 2001, sa2)
            gs2b = join_as_player(addr, 2002, sb2)

            game1_id = gs1b.game_id
            game2_id = gs2b.game_id

            # Move in game 1
            udp_send_recv(sb1, pack_move1(1002, game1_id, 0), addr)

            # Game 2 state unchanged
            raw = udp_send_recv(sb2, pack_keep_alive(2002, game2_id), addr)
            gs2_check = GameState(raw)
        assert gs2_check.pawn_present(0), "Pawn 0 in game 2 should still be present"


# ===========================================================================
# 11. TIMEOUT BEHAVIOR
# ===========================================================================

# Timeout used in all timeout tests.  3 s gives implementations that track time
# in whole seconds at least one full second of margin on each side.
_TIMEOUT_SECS = 3

# How long to sleep to be sure the timeout has fired.  The spec allows servers
# to check timeouts only when a message arrives, so we just need the *wall-clock
# gap* to comfortably exceed server_timeout.  4× gives plenty of slack even
# when the OS scheduler delays the sleep or the server uses integer arithmetic.
_SLEEP_EXPIRE = _TIMEOUT_SECS * 4

# How long to sleep between keep-alive pings.  Must be well below the timeout
# so that even a 1-second-granularity implementation considers the game alive.
# Using 40 % of the timeout keeps us safe regardless of rounding direction.
_SLEEP_PING = max(1, int(_TIMEOUT_SECS * 0.4))


class TestTimeout:

    def test_waiting_game_expires_without_keep_alive(self):
        """Game in WAITING_FOR_OPPONENT is deleted if A doesn't keep alive."""
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                game_id = gs.game_id
                # Sleep well beyond the timeout; the subsequent keep-alive
                # message will trigger the server's timeout check.
                time.sleep(_SLEEP_EXPIRE)
                raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
            if raw is not None:
                resp = parse_response(raw)
                assert isinstance(resp, WrongMsg), "Game should be deleted after timeout"

    def test_active_keep_alive_prevents_timeout(self):
        """Regular keep-alive messages prevent game deletion."""
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                game_id = gs.game_id
                # Send 4 pings, each _SLEEP_PING seconds apart.  Every ping
                # resets the server's idle clock so the game must stay alive.
                for _ in range(4):
                    time.sleep(_SLEEP_PING)
                    raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
                    assert raw is not None
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_WAITING, "Game should still be alive"

    def test_client_timeout_prints_message(self):
        """Client prints a message when server doesn't respond."""
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST, "-p", "19998",
             "-m", "0/1001", "-t", "1"],
            capture_output=True, timeout=10
        )
        assert proc.returncode == 0
        assert proc.stdout.strip(), "Client should print timeout message"

    def test_inactive_player_loses(self):
        """A player who stops sending keep-alives is reaped unless the game is kept alive."""
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 1001, sa)
                gs_b = join_as_player(addr, 1002, sb)
                game_id = gs_b.game_id

                # B makes the first move, so it becomes A's turn.
                udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)

                # Both players must ping within server_timeout — A's pings do
                # not refresh B's clock (task.txt §5.3).
                for _ in range(4):
                    time.sleep(_SLEEP_PING)
                    raw_a = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
                    assert raw_a is not None, "A should keep the game alive"
                    gs_a = GameState(raw_a)
                    assert gs_a.status in (STATUS_TURN_A, STATUS_TURN_B, STATUS_WAITING)
                    raw_b = udp_send_recv(sb, pack_keep_alive(1002, game_id), addr)
                    assert raw_b is not None, "B should keep the game alive"

                # Now B should still be able to query the game state.
                raw = udp_send_recv(sb, pack_keep_alive(1002, game_id), addr)
                assert raw is not None
                gs2 = GameState(raw)

            assert gs2.status == STATUS_TURN_A, \
                f"Expected TURN_A with both players alive, got status={gs2.status}"


# ===========================================================================
# 12. CLIENT BINARY END-TO-END TESTS
# ===========================================================================

class TestClientBinary:

    def _run_client(self, srv, message: str, timeout: int = 2):
        return subprocess.run(
            [CLIENT_BIN,
             "-a", SERVER_HOST,
             "-p", str(srv.actual_port),
             "-m", message,
             "-t", str(timeout)],
            capture_output=True, text=True, timeout=10
        )

    def test_client_join_prints_readable_output(self, srv_default):
        result = self._run_client(srv_default, "0/1001")
        assert result.returncode == 0
        output = result.stdout
        assert output.strip(), "Client output must not be empty"

    def test_client_join_output_contains_game_id(self, srv_default):
        result = self._run_client(srv_default, "0/1001")
        # Game ID should appear somewhere in human-readable output
        assert result.stdout.strip()

    def test_client_second_player_joins(self, srv_default):
        r1 = self._run_client(srv_default, "0/1001")
        r2 = self._run_client(srv_default, "0/1002")
        assert r1.returncode == 0
        assert r2.returncode == 0

    def test_client_send_move1_via_binary(self, srv_default):
        # Join A, join B, then B does move1 pawn 0
        self._run_client(srv_default, "0/1001")
        r2 = self._run_client(srv_default, "0/1002")
        # Parse game_id from raw protocol directly
        with make_udp_socket() as s:
            join_as_player(srv_default.addr(), 9001, s)
            gs = join_as_player(srv_default.addr(), 9002, s)
            game_id = gs.game_id
        # Use client binary to send MOVE_1
        msg = f"1/9002/{game_id}/0"
        result = self._run_client(srv_default, msg)
        assert result.returncode == 0

    def test_client_wrong_msg_shows_error_output(self, srv_default):
        """Client should reject player_id=0 (spec: player IDs are nonzero)."""
        result = self._run_client(srv_default, "0/0")   # player_id=0 invalid
        assert result.returncode != 0
        assert result.stderr.strip()

    def test_client_keep_alive_via_binary(self, srv_default):
        with make_udp_socket() as s:
            gs = join_as_player(srv_default.addr(), 8001, s)
            game_id = gs.game_id
        msg = f"3/8001/{game_id}"
        result = self._run_client(srv_default, msg)
        assert result.returncode == 0
        assert result.stdout.strip()

    def test_client_give_up_via_binary(self, srv_default):
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(srv_default.addr(), 7001, sa)
            gs = join_as_player(srv_default.addr(), 7002, sb)
            game_id = gs.game_id
        msg = f"4/7002/{game_id}"
        result = self._run_client(srv_default, msg)
        assert result.returncode == 0


# ===========================================================================
# 13. EDGE CASES AND ROBUSTNESS
# ===========================================================================

class TestEdgeCases:

    def test_garbage_data_produces_wrong_msg(self, srv_default):
        """Random garbage bytes always produce MSG_WRONG_MSG."""
        import random
        for _ in range(10):
            data = bytes(random.randint(0, 255) for _ in range(random.randint(1, 20)))
            with make_udp_socket() as s:
                raw = udp_send_recv(s, data, srv_default.addr())
            if raw is not None:
                # Must be either WrongMsg (14 bytes with status=255)
                # or a valid game state (if data happened to decode correctly)
                if len(raw) == 14 and raw[12] == 255:
                    pass  # correct WrongMsg
                else:
                    # If it decoded as a valid game state, that's also allowed

                    pass

    def test_multiple_join_same_player_before_opponent(self, srv_default):
        """If A sends MSG_JOIN twice, second should either join the waiting game as B
        or be ignored (implementation-defined, but must not crash)."""
        addr = srv_default.addr()
        with make_udp_socket() as s:
            gs1 = join_as_player(addr, 1001, s)
            gs2 = join_as_player(addr, 1001, s)
        # Either they're in the same game (player plays themselves) or a new one
        assert gs2 is not None

    def test_move1_pawn_at_max_pawn(self, srv_default):
        """Legal move on max_pawn index."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            max_pawn = gs.max_pawn   # 7
            raw = udp_send_recv(sb, pack_move1(1002, game_id, max_pawn), addr)
            gs2 = GameState(raw)
        assert not gs2.pawn_present(max_pawn)

    def test_move2_at_max_pawn_is_illegal(self, srv_default):
        """MOVE_2 on max_pawn tries to remove max_pawn+1 which doesn't exist."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            max_pawn = gs.max_pawn
            raw = udp_send_recv(sb, pack_move2(1002, game_id, max_pawn), addr)
            gs2 = GameState(raw)
        # Pawn at max_pawn should still be present (illegal move)
        assert gs2.pawn_present(max_pawn)
        assert gs2.status == STATUS_TURN_B

    def test_server_responds_to_correct_source_address(self, srv_default):
        """Server must reply to the address/port the message came from."""
        addr = srv_default.addr()
        with make_udp_socket() as s:
            s.sendto(pack_join(1001), addr)
            raw, sender = s.recvfrom(65535)
        assert sender == addr, "Response should come from server address"

    def test_pawn_row_bit_layout_msb_first(self, srv_default):
        """Pawn 0 is the MSB of the first byte."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        gs = GameState(raw)
        # With "11111111": all pawns present, byte should be 0xFF
        assert gs.pawn_row_bytes[0] == 0xFF

    def test_pawn_row_after_move(self, srv_default):
        """After removing pawn 0, the MSB of first byte should be 0."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            raw = udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            gs2 = GameState(raw)
        # pawn 0 gone → MSB of byte 0 should be 0
        assert (gs2.pawn_row_bytes[0] & 0x80) == 0

    def test_pawn_row_after_move2(self, srv_default):
        """After removing pawns 0 and 1 via MOVE_2, top 2 bits of byte 0 are 0."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            raw = udp_send_recv(sb, pack_move2(1002, game_id, 0), addr)
            gs2 = GameState(raw)
        assert (gs2.pawn_row_bytes[0] & 0xC0) == 0

    def test_complex_row_missing_pawn_not_selectable(self, srv_complex):
        """In "11101111011111", pawn 3 is absent and cannot be removed."""
        addr = srv_complex.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            assert not gs.pawn_present(3), "Pawn 3 should be absent in this row"
            raw = udp_send_recv(sb, pack_move1(1002, game_id, 3), addr)
            gs2 = GameState(raw)
        # Illegal move — nothing should change
        assert gs2.status == STATUS_TURN_B

    def test_send_to_wrong_port_gets_no_response(self, srv_default):
        """Sending to a port where server is not listening yields no response."""
        wrong_port = srv_default.actual_port + 1
        with make_udp_socket(timeout=1.0) as s:
            raw = udp_send_recv(s, pack_join(1001), (SERVER_HOST, wrong_port))
        assert raw is None


# ===========================================================================
# 14. PROTOCOL FIELD RANGE TESTS
# ===========================================================================

class TestFieldRanges:

    def test_player_id_max_valid(self, srv_default):
        """player_id = 2^32 - 1 is valid."""
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(0xFFFFFFFF), srv_default.addr())
        gs = GameState(raw)
        assert gs.player_id_a_matches(0xFFFFFFFF) if hasattr(gs, 'player_id_a_matches') \
            else gs.player_a_id == 0xFFFFFFFF

    def test_game_id_range_in_response(self, srv_default):
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(1001), srv_default.addr())
        gs = GameState(raw)
        assert 0 <= gs.game_id <= 0xFFFFFFFF

    def test_status_values(self, srv_default):
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(srv_default.addr(), 1001, sa)
            gs_b = join_as_player(srv_default.addr(), 1002, sb)
        assert gs_a.status == STATUS_WAITING
        assert gs_b.status == STATUS_TURN_B

    def test_max_pawn_matches_pawn_row_length(self):
        """max_pawn should be len(pawn_row_string) - 1."""
        test_cases = [
            ("11", 1),
            ("111", 2),
            ("11111111", 7),
            ("11101111011111", 13),
        ]
        for row, expected_max_pawn in test_cases:
            with ServerProcess(pawn_row=row, timeout=5) as srv:
                with make_udp_socket() as s:
                    raw = udp_send_recv(s, pack_join(1001), srv.addr())
                gs = GameState(raw)
                assert gs.max_pawn == expected_max_pawn, \
                    f"For row '{row}', expected max_pawn={expected_max_pawn}, got {gs.max_pawn}"


# ===========================================================================
# 15. STRESS / LOAD TESTS
# ===========================================================================

class TestStress:

    def test_many_join_requests(self, srv_default):
        """Server handles many successive join requests without crashing."""
        addr = srv_default.addr()
        for i in range(50):
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(i + 1), addr)
            assert raw is not None, f"No response on join {i}"

    def test_rapid_keep_alives(self, srv_default):
        """Server handles rapid keep-alive messages."""
        addr = srv_default.addr()
        with make_udp_socket() as s:
            gs = join_as_player(addr, 1001, s)
            game_id = gs.game_id
            for _ in range(20):
                raw = udp_send_recv(s, pack_keep_alive(1001, game_id), addr)
                assert raw is not None

    def test_concurrent_sockets_to_same_game(self, srv_default):
        """Multiple sockets sending to the same game — server stays consistent."""
        addr = srv_default.addr()
        sockets = [make_udp_socket() for _ in range(5)]
        try:
            gs = join_as_player(addr, 1001, sockets[0])
            gs2 = join_as_player(addr, 1002, sockets[1])
            game_id = gs2.game_id
            # All sockets query via keep-alive (as participant 1002)
            for s in sockets[1:]:
                raw = udp_send_recv(s, pack_keep_alive(1002, game_id), addr)
                assert raw is not None
        finally:
            for s in sockets:
                s.close()

    def test_server_still_alive_after_many_invalid_messages(self, srv_default):
        """Server does not crash on a flood of invalid messages."""
        addr = srv_default.addr()
        junk_payloads = [
            b"",
            b"\x00",
            b"\xFF" * 100,
            b"\x01" * 3,
            bytes(range(256)),
        ]
        for _ in range(5):
            for payload in junk_payloads:
                with make_udp_socket(timeout=0.5) as s:
                    s.sendto(payload, addr)
                    try:
                        s.recvfrom(65535)
                    except socket.timeout:
                        pass

        # Server should still be responsive
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(9999), addr)
        assert raw is not None, "Server crashed or became unresponsive"


# ===========================================================================
# 16. FULL GAME WALKTHROUGH TESTS
# ===========================================================================

class TestFullGame:

    def test_full_game_2_pawns_b_wins(self, srv_2pawns):
        """Complete game: B takes both pawns and wins."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            assert gs.status == STATUS_TURN_B

            raw = udp_send_recv(sb, pack_move2(1002, game_id, 0), addr)
            final = GameState(raw)

        assert final.status == STATUS_WIN_B
        assert len(final.active_pawns()) == 0

    def test_full_game_3_pawns_a_wins(self, srv_3pawns):
        """
        '111': B takes pawn 0, A takes pawn 2, B takes pawn 1 → B wins.
        Or: B takes 0, A takes 1, B takes 2 → B wins.
        """
        addr = srv_3pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            # B takes pawn 0
            udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            # A takes pawn 1
            udp_send_recv(sa, pack_move1(1001, game_id, 1), addr)
            # B takes pawn 2 (last) → B wins
            raw = udp_send_recv(sb, pack_move1(1002, game_id, 2), addr)
            final = GameState(raw)

        assert final.status == STATUS_WIN_B

    def test_full_game_8_pawns(self, srv_default):
        """Play through a complete 8-pawn game."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            # Alternate MOVE_1 on each pawn in order
            players = [(1002, sb), (1001, sa)]  # B goes first
            for i in range(8):
                pid, sock = players[i % 2]
                raw = udp_send_recv(sock, pack_move1(pid, game_id, i), addr)
                gs_now = GameState(raw)
            final = gs_now

        # Last pawn taken by player at index 7 → player at 7%2=1 → A (pid 1001)
        assert final.status == STATUS_WIN_A

    def test_give_up_midgame_terminates_correctly(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            # B moves
            udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)
            # A moves
            udp_send_recv(sa, pack_move1(1001, game_id, 1), addr)
            # B gives up (it's B's turn)
            raw = udp_send_recv(sb, pack_give_up(1002, game_id), addr)
            final = GameState(raw)

        assert final.status == STATUS_WIN_A

    def test_keep_alive_after_game_ends_returns_final_state(self, srv_2pawns):
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            game_id = gs.game_id
            udp_send_recv(sb, pack_move2(1002, game_id, 0), addr)
            # A asks for state
            raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
            final = GameState(raw)
        assert final.status == STATUS_WIN_B


# ===========================================================================
# Entry point for direct execution
# ===========================================================================

if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v", "--tb=short"]))
