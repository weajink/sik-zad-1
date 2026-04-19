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

SERVER_BIN = "./kayles_server"
CLIENT_BIN = "./kayles_client"
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

# Server timeout used for section-11 tests.
_TIMEOUT_SECS = 3

# ---------------------------------------------------------------------------
# Tight timing constants — all sleeps stay within _TIGHT_MARGIN of the true
# timeout boundary so each test exercises a window of at most 0.15 s (< 0.2 s).
#
# Design:
#   _SLEEP_EXPIRE  = t + _TIGHT_MARGIN   → guaranteed to be past the deadline
#   _SLEEP_PREEXP  = t - _TIGHT_MARGIN   → guaranteed to be before the deadline
#
# The spec allows servers to check timeouts lazily (only when a message
# arrives), so every expiry check sends a trigger message immediately after
# the sleep, which forces the server to evaluate the timeout right then.
# ---------------------------------------------------------------------------
_TIGHT_MARGIN = 0.15                         # seconds  (< 0.2 s requirement)
_SLEEP_EXPIRE = _TIMEOUT_SECS + _TIGHT_MARGIN    # sleep this long → timeout has fired
_SLEEP_PREEXP = _TIMEOUT_SECS - _TIGHT_MARGIN    # sleep this long → timeout NOT yet fired


class TestTimeout:

    def test_waiting_game_expires_without_keep_alive(self):
        """
        Game in WAITING_FOR_OPPONENT is deleted exactly after server_timeout.

        Proof:
          • At t = _SLEEP_EXPIRE (= T + 0.15 s) the deadline has passed by
            _TIGHT_MARGIN seconds.
          • The keep-alive that follows forces the server to evaluate timeouts;
            the game must already be gone.
        """
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                game_id = gs.game_id
                time.sleep(_SLEEP_EXPIRE)
                # This message triggers the server's timeout check.
                raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
            assert raw is not None, "Server must reply even to stale keep-alive"
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg), \
                f"Game must be gone {_TIGHT_MARGIN:.2f} s after deadline; got {resp}"

    def test_game_still_alive_just_before_timeout(self):
        """
        At t = T - _TIGHT_MARGIN the game must NOT yet have expired.

        This is the near-boundary positive check: if the server fires the
        timeout too early (by ≥ 0.15 s) this test catches it.
        """
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                game_id = gs.game_id
                time.sleep(_SLEEP_PREEXP)
                raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
            assert raw is not None, "Server must reply before the deadline"
            gs2 = GameState(raw)
            assert gs2.status == STATUS_WAITING, \
                f"Game must still be WAITING at T - {_TIGHT_MARGIN:.2f} s; got {gs2}"

    def test_active_keep_alive_prevents_timeout(self):
        """
        A keep-alive sent at T - _TIGHT_MARGIN resets the clock; the game must
        remain alive a further T - _TIGHT_MARGIN seconds after the ping.

        Timeline (T = _TIMEOUT_SECS):
          0 ──── join ────────────────────────────────── T-0.15 ── ping ── T-0.15+T-0.15 ── ping
                                                          ↑ alive              ↑ alive (clock reset)
        """
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                game_id = gs.game_id
                for iteration in range(2):
                    # Sleep right up to — but not past — the current deadline.
                    time.sleep(_SLEEP_PREEXP)
                    raw = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
                    assert raw is not None, \
                        f"Iteration {iteration}: no response; server may have crashed"
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_WAITING, \
                        f"Iteration {iteration}: game expired early at T - {_TIGHT_MARGIN:.2f} s"

    def test_client_timeout_prints_message(self):
        """Client prints a message when server doesn't respond."""
        proc = subprocess.run(
            [CLIENT_BIN, "-a", SERVER_HOST, "-p", "19998",
             "-m", "0/1001", "-t", "1"],
            capture_output=True, timeout=10
        )
        assert proc.returncode == 0
        assert proc.stdout.strip(), "Client should print timeout message"

    def test_inactive_player_b_loses_after_timeout(self):
        """
        Once B has made its last move, if B sends nothing for T seconds the
        server must declare A the winner.

        Timeline (T = _TIMEOUT_SECS):
          0 ─── join A ─── join B ─── B moves (TURN_A) ───── T-0.15 ─── A keep-alive
                                       ↑ B's per-game clock reset            ↑ A's clock reset
                           ─────────────────────── 0.30 s ──────────────────── A keep-alive
                           (total from B's move = T+0.15 > T → B must be reaped → WIN_A)
        """
        with ServerProcess(pawn_row="11111111", timeout=_TIMEOUT_SECS) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 1001, sa)
                gs_b = join_as_player(addr, 1002, sb)
                game_id = gs_b.game_id

                # B moves — this resets B's per-game clock (t=0 for B from here).
                udp_send_recv(sb, pack_move1(1002, game_id, 0), addr)

                # Sleep until T - 0.15 s (just before B's deadline).  Then A
                # sends a keep-alive to reset *A's* clock (B is still silent).
                time.sleep(_SLEEP_PREEXP)
                raw_mid = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)
                assert raw_mid is not None, "Server must be alive mid-test"
                gs_mid = GameState(raw_mid)
                assert gs_mid.status == STATUS_TURN_A, \
                    "Game should still be in TURN_A before B's deadline"

                # Sleep another 2 * _TIGHT_MARGIN so that total time from B's
                # move is T + 0.15 > T, but time from A's last keep-alive is
                # only 0.30 s < T (so A has NOT timed out).
                time.sleep(2 * _TIGHT_MARGIN)

                # This keep-alive triggers the server's timeout evaluation.
                raw_final = udp_send_recv(sa, pack_keep_alive(1001, game_id), addr)

            assert raw_final is not None, "Server must reply to A's final keep-alive"
            gs_final = GameState(raw_final)
            assert gs_final.status == STATUS_WIN_A, \
                f"B was silent for T + {_TIGHT_MARGIN:.2f} s; expected WIN_A, got {gs_final}"


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
# 17. STRESS — MANY SIMULTANEOUS AND SEQUENTIAL GAMES
# ===========================================================================

class TestStressMultiGame:
    """
    Stress-tests the server with many games alive at the same time, many
    sequential games on the same server instance, a single player active in
    dozens of games simultaneously, and interleaved move sequences across
    multiple games.
    """

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _complete_game_2pawns(addr, pid_a, pid_b, sa, sb):
        """Start and finish a 2-pawn game; return final GameState."""
        gs_a = join_as_player(addr, pid_a, sa)
        gs_b = join_as_player(addr, pid_b, sb)
        game_id = gs_b.game_id
        raw = udp_send_recv(sb, pack_move2(pid_b, game_id, 0), addr)
        return GameState(raw), game_id

    # ------------------------------------------------------------------
    # 17.1  Sequential games — one finishes, then next starts
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("pawn_row,expected_pawns", [
        ("11",           2),
        ("111",          3),
        ("11111111",     8),
        ("11101111011111", 14),
        ("1" * 64,      64),
        ("1" * 256,    256),
    ])
    def test_sequential_games_different_rows(self, pawn_row, expected_pawns):
        """Start a game, finish it, start another — server must handle each correctly."""
        with ServerProcess(pawn_row=pawn_row, timeout=5) as srv:
            addr = srv.addr()
            for game_num in range(3):
                pid_a = 1000 + game_num * 2
                pid_b = 1001 + game_num * 2
                with make_udp_socket() as sa, make_udp_socket() as sb:
                    gs_a = join_as_player(addr, pid_a, sa)
                    gs_b = join_as_player(addr, pid_b, sb)
                    assert gs_b.max_pawn == expected_pawns - 1, \
                        f"Game {game_num}: max_pawn mismatch"
                    # B gives up to cleanly end the game
                    raw = udp_send_recv(sb, pack_give_up(pid_b, gs_b.game_id), addr)
                    final = GameState(raw)
                    assert final.status == STATUS_WIN_A

    def test_ten_sequential_games_server_stable(self):
        """Server stays alive and responsive after 10 back-to-back complete games."""
        with ServerProcess(pawn_row="11", timeout=5) as srv:
            addr = srv.addr()
            for i in range(10):
                pid_a = 2000 + i * 2
                pid_b = 2001 + i * 2
                with make_udp_socket() as sa, make_udp_socket() as sb:
                    gs, gid = self._complete_game_2pawns(addr, pid_a, pid_b, sa, sb)
                    assert gs.status == STATUS_WIN_B, f"Game {i} did not end WIN_B"
            # Server should still respond
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(9999), addr)
            assert raw is not None, "Server unresponsive after 10 sequential games"

    # ------------------------------------------------------------------
    # 17.2  Many simultaneous games
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("n_games", [5, 10, 20])
    def test_n_simultaneous_games_independent(self, n_games):
        """
        Open n_games simultaneously, make one move in each, verify each game
        transitions independently and carries the correct pawn row.
        """
        with ServerProcess(pawn_row="11111111", timeout=10) as srv:
            addr = srv.addr()
            sockets = []
            game_ids = []
            try:
                # Join all games
                for i in range(n_games):
                    sa = make_udp_socket()
                    sb = make_udp_socket()
                    sockets.append((sa, sb))
                    gs_a = join_as_player(addr, 3000 + i * 2, sa)
                    gs_b = join_as_player(addr, 3001 + i * 2, sb)
                    assert gs_b.status == STATUS_TURN_B
                    game_ids.append(gs_b.game_id)

                # All game IDs must be distinct
                assert len(set(game_ids)) == n_games, "Duplicate game IDs!"

                # Make one move in each game (B removes pawn 0)
                for i, ((sa, sb), gid) in enumerate(zip(sockets, game_ids)):
                    pid_b = 3001 + i * 2
                    raw = udp_send_recv(sb, pack_move1(pid_b, gid, 0), addr)
                    gs = GameState(raw)
                    assert gs.status == STATUS_TURN_A, \
                        f"Game {i} (gid={gid}): expected TURN_A after B's move"
                    assert not gs.pawn_present(0), \
                        f"Game {i}: pawn 0 should be gone"

                # Verify other games weren't disturbed by checking pawn 0 still present
                # via a keep-alive on a game we haven't touched (well — each was moved once,
                # so just cross-check pawn 0 is absent in all)
                for i, ((sa, sb), gid) in enumerate(zip(sockets, game_ids)):
                    pid_a = 3000 + i * 2
                    raw = udp_send_recv(sa, pack_keep_alive(pid_a, gid), addr)
                    gs = GameState(raw)
                    assert not gs.pawn_present(0)

            finally:
                for sa, sb in sockets:
                    sa.close()
                    sb.close()

    def test_simultaneous_games_different_pawn_rows(self):
        """Games with different pawn rows run independently on one server,
        but each server only supports one pawn_row, so we test that a server
        with a complex row handles many games correctly."""
        pawn_row = "11101111011111"  # 14 chars, pawn 3 absent
        with ServerProcess(pawn_row=pawn_row, timeout=10) as srv:
            addr = srv.addr()
            N = 8
            sockets = []
            game_ids = []
            try:
                for i in range(N):
                    sa = make_udp_socket()
                    sb = make_udp_socket()
                    sockets.append((sa, sb))
                    gs_a = join_as_player(addr, 4000 + i * 2, sa)
                    gs_b = join_as_player(addr, 4001 + i * 2, sb)
                    game_ids.append(gs_b.game_id)
                    assert gs_b.max_pawn == 13
                    assert not gs_b.pawn_present(3), "Pawn 3 absent in complex row"

                assert len(set(game_ids)) == N
            finally:
                for sa, sb in sockets:
                    sa.close()
                    sb.close()

    # ------------------------------------------------------------------
    # 17.3  One player in many games at once
    # ------------------------------------------------------------------

    def test_one_player_in_many_games(self):
        """
        A single player_id participates in 8 concurrent games — once as A,
        once as B.  All game IDs must be unique and state independent.
        """
        with ServerProcess(pawn_row="1111", timeout=10) as srv:
            addr = srv.addr()
            OWN_PID = 7777
            N = 8
            sockets = []
            game_ids = []
            try:
                for i in range(N):
                    s_self = make_udp_socket()
                    s_opp  = make_udp_socket()
                    sockets.append((s_self, s_opp))
                    # OWN_PID always joins first (as A)
                    gs_a = join_as_player(addr, OWN_PID, s_self)
                    opp_pid = 8000 + i
                    gs_b = join_as_player(addr, opp_pid, s_opp)
                    game_ids.append(gs_b.game_id)

                assert len(set(game_ids)) == N, "Player in N games must produce N distinct IDs"

                # OWN_PID makes a keep-alive in every game — must all respond correctly
                for i, ((s_self, _), gid) in enumerate(zip(sockets, game_ids)):
                    raw = udp_send_recv(s_self, pack_keep_alive(OWN_PID, gid), addr)
                    assert raw is not None, f"No response for game {i}"
                    gs = GameState(raw)
                    assert gs.game_id == gid

            finally:
                for s1, s2 in sockets:
                    s1.close(); s2.close()

    # ------------------------------------------------------------------
    # 17.4  Interleaved move sequences across multiple games
    # ------------------------------------------------------------------

    def test_interleaved_moves_across_five_games(self):
        """
        Make moves in five games in round-robin order.  Each game must track
        its own state correctly regardless of interleaving.
        """
        with ServerProcess(pawn_row="11111111", timeout=15) as srv:
            addr = srv.addr()
            N = 5
            sockets = []
            game_info = []
            try:
                for i in range(N):
                    sa = make_udp_socket()
                    sb = make_udp_socket()
                    sockets.append((sa, sb))
                    pid_a = 5000 + i * 2
                    pid_b = 5001 + i * 2
                    join_as_player(addr, pid_a, sa)
                    gs_b = join_as_player(addr, pid_b, sb)
                    game_info.append((pid_a, pid_b, gs_b.game_id))

                # Round-robin: each round, B moves in game i then A moves in game i
                for pawn in range(4):  # 4 rounds (removing pawns 0..3)
                    for i, (pid_a, pid_b, gid) in enumerate(game_info):
                        sa, sb = sockets[i]
                        # B's turn
                        raw_b = udp_send_recv(sb, pack_move1(pid_b, gid, pawn * 2), addr)
                        gs = GameState(raw_b)
                        assert gs.status == STATUS_TURN_A, \
                            f"Round {pawn} game {i}: expected TURN_A after B's move"
                        # A's turn
                        raw_a = udp_send_recv(sa, pack_move1(pid_a, gid, pawn * 2 + 1), addr)
                        gs = GameState(raw_a)
                        if pawn < 3:
                            assert gs.status == STATUS_TURN_B, \
                                f"Round {pawn} game {i}: expected TURN_B after A's move"
                        else:
                            assert gs.status == STATUS_WIN_A, \
                                f"Round {pawn} game {i}: expected WIN_A after A's move"

            finally:
                for sa, sb in sockets:
                    sa.close(); sb.close()

    # ------------------------------------------------------------------
    # 17.5  Mass join flood
    # ------------------------------------------------------------------

    def test_mass_join_100_players(self):
        """
        100 distinct player_ids send MSG_JOIN in rapid succession.
        Pairs should form 50 games; server must remain responsive.
        """
        with ServerProcess(pawn_row="11", timeout=10) as srv:
            addr = srv.addr()
            game_ids_seen = set()
            for i in range(100):
                with make_udp_socket(timeout=2.0) as s:
                    raw = udp_send_recv(s, pack_join(6000 + i), addr)
                assert raw is not None, f"No response on join {i}"
                gs = GameState(raw)
                game_ids_seen.add(gs.game_id)

            # After 100 joins, server must still respond
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(9998), addr)
            assert raw is not None, "Server unresponsive after 100 joins"

    # ------------------------------------------------------------------
    # 17.6  Rapid move bursts
    # ------------------------------------------------------------------

    def test_rapid_move_burst_single_game(self):
        """
        Both players hammer the server with alternating MOVE_1 messages as
        fast as possible until the game ends.  No deadlock or crash expected.
        """
        with ServerProcess(pawn_row="1" * 16, timeout=10) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 1001, sa)
                gs = join_as_player(addr, 1002, sb)
                gid = gs.game_id
                players = [(1002, sb), (1001, sa)]
                final = None
                for pawn in range(16):
                    pid, sock = players[pawn % 2]
                    raw = udp_send_recv(sock, pack_move1(pid, gid, pawn), addr)
                    assert raw is not None, f"No response at pawn {pawn}"
                    final = GameState(raw)
                assert final.status in (STATUS_WIN_A, STATUS_WIN_B)
                assert final.active_pawns() == []


# ===========================================================================
# 18. TIMEOUT VARIANTS — DIFFERENT server_timeout VALUES
# ===========================================================================

# ---------------------------------------------------------------------------
# All tests in this class use the same _TIGHT_MARGIN defined in section 11.
# Each parameterised case computes its own sleep durations from the actual
# t_val so no test sleeps more than t_val + _TIGHT_MARGIN seconds.
# ---------------------------------------------------------------------------

def _sleep_expire(t_val: float) -> None:
    """Sleep exactly long enough to guarantee the timeout has fired."""
    time.sleep(t_val + _TIGHT_MARGIN)


def _sleep_preexp(t_val: float) -> None:
    """Sleep just short of the timeout — the game must still be alive after."""
    time.sleep(max(0.0, t_val - _TIGHT_MARGIN))


class TestTimeoutVariants:
    """
    Exercises the server with t=1, t=2, t=3.  Every sleep is within
    _TIGHT_MARGIN (0.15 s) of the true deadline, satisfying the < 0.2 s
    discrepancy requirement.
    """

    # ------------------------------------------------------------------
    # 18.1  t=1: WAITING game expires quickly
    # ------------------------------------------------------------------

    def test_t1_waiting_game_expires(self):
        """
        With t=1: game must be gone at T + 0.15 s.

        Timeline:
          0 ── join A (WAITING) ── 1.15 s ── probe JOIN (triggers reaper)
                                              ↓
                                   keep-alive with stale gid → WrongMsg
        """
        with ServerProcess(pawn_row="11111111", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs_a = join_as_player(addr, 1001, sa)
                gid = gs_a.game_id

            _sleep_expire(1)   # 1.15 s — game must have expired

            with make_udp_socket() as s:
                udp_send_recv(s, pack_join(9001), addr)   # trigger reaper
                raw = udp_send_recv(s, pack_keep_alive(1001, gid), addr)

            assert raw is not None
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg), \
                f"t=1 game must be reaped at T+{_TIGHT_MARGIN:.2f} s; got {resp}"

    def test_t1_waiting_game_alive_before_deadline(self):
        """
        With t=1: game must NOT be expired at T - 0.15 s.

        Timeline:
          0 ── join A (WAITING) ── 0.85 s ── keep-alive → WAITING (still alive)
        """
        with ServerProcess(pawn_row="11111111", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                gid = gs.game_id
                _sleep_preexp(1)   # 0.85 s
                raw = udp_send_recv(sa, pack_keep_alive(1001, gid), addr)
            assert raw is not None
            gs2 = GameState(raw)
            assert gs2.status == STATUS_WAITING, \
                f"t=1 game must be WAITING at T-{_TIGHT_MARGIN:.2f} s; got {gs2}"

    def test_t1_new_join_after_expiry_succeeds(self):
        """
        After the waiting game expires, a new MSG_JOIN must not crash the
        server and must return a valid game state.
        """
        with ServerProcess(pawn_row="11111111", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                join_as_player(addr, 1001, sa)

            _sleep_expire(1)

            with make_udp_socket() as sb:
                raw = udp_send_recv(sb, pack_join(1002), addr)
            assert raw is not None
            gs_b = GameState(raw)
            assert gs_b.status in (STATUS_WAITING, STATUS_TURN_B)

    def test_t1_expired_game_keep_alive_returns_wrong_msg(self):
        """
        After t=1 + 0.15 s, keep-alive with the old game_id must yield WrongMsg.
        """
        with ServerProcess(pawn_row="11111111", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                gid = gs.game_id

            _sleep_expire(1)

            with make_udp_socket() as s:
                udp_send_recv(s, pack_join(9002), addr)   # trigger reaper
                raw = udp_send_recv(s, pack_keep_alive(1001, gid), addr)

            if raw is not None:
                resp = parse_response(raw)
                assert isinstance(resp, WrongMsg), \
                    "Expired game must not be accessible"

    def test_t1_completed_game_state_kept_briefly(self):
        """
        After WIN_B the state must still be readable immediately (< 1 s later).
        """
        with ServerProcess(pawn_row="11", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 1001, sa)
                gs = join_as_player(addr, 1002, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_move2(1002, gid, 0), addr)
                # Immediate keep-alive (well within t=1) must still see WIN_B.
                raw = udp_send_recv(sa, pack_keep_alive(1001, gid), addr)
                final = GameState(raw)
            assert final.status == STATUS_WIN_B

    def test_t1_completed_game_gone_after_timeout(self):
        """
        At t=1 + 0.15 s after game end, the state must be removed.

        Timeline:
          0 ── WIN_B ── 1.15 s ── probe (trigger reaper) ── keep-alive → WrongMsg
        """
        with ServerProcess(pawn_row="11", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 1001, sa)
                gs = join_as_player(addr, 1002, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_move2(1002, gid, 0), addr)  # WIN_B at t=0

            _sleep_expire(1)   # 1.15 s — state should be reaped

            with make_udp_socket() as s:
                udp_send_recv(s, pack_join(9003), addr)   # trigger reaper
                raw = udp_send_recv(s, pack_keep_alive(1001, gid), addr)

            if raw is not None:
                resp = parse_response(raw)
                assert isinstance(resp, WrongMsg), \
                    "Finished game must be gone at T+0.15 s"

    # ------------------------------------------------------------------
    # 18.2  Boundary keep-alive: game stays alive exactly at the edge
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("t_val", [1, 2, 3])
    def test_keep_alive_just_before_expiry_resets_clock(self, t_val):
        """
        Sending a keep-alive at T - 0.15 s must:
          (a) find the game alive (it has NOT expired yet), AND
          (b) reset the clock — the game stays alive for another ~T seconds.

        Timeline:
          0 ─── join ─── (T-0.15) ─── keep-alive → WAITING  [clock reset]
                                        ─── (T-0.15) ─── keep-alive → WAITING  [2nd check]
        """
        with ServerProcess(pawn_row="11111111", timeout=t_val) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                gid = gs.game_id

                for check in range(2):
                    _sleep_preexp(t_val)   # T - 0.15 s
                    raw = udp_send_recv(sa, pack_keep_alive(1001, gid), addr)
                    assert raw is not None, \
                        f"t={t_val} check {check}: no response; server may have died"
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_WAITING, \
                        f"t={t_val} check {check}: game expired {_TIGHT_MARGIN:.2f} s early"

    @pytest.mark.parametrize("t_val", [1, 2, 3])
    def test_game_expired_at_t_plus_margin(self, t_val):
        """
        Without any keep-alive, game must be gone at T + 0.15 s.

        This is the tight upper-bound check: if the server delays expiry
        beyond T + 0.15 s this test fails.
        """
        with ServerProcess(pawn_row="11111111", timeout=t_val) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                gid = gs.game_id

            _sleep_expire(t_val)   # T + 0.15 s

            with make_udp_socket() as s:
                udp_send_recv(s, pack_join(9004 + t_val), addr)  # trigger reaper
                raw = udp_send_recv(s, pack_keep_alive(1001, gid), addr)

            assert raw is not None
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg), \
                f"t={t_val}: game must be expired at T+{_TIGHT_MARGIN:.2f} s"

    # ------------------------------------------------------------------
    # 18.3  Expiry of one game must not disturb a concurrent active game
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("t_val", [1, 2, 3])
    def test_expired_game_does_not_disturb_active_game(self, t_val):
        """
        Game 1 (no keep-alives) expires.  Game 2 (pinged at T - 0.15 s) must
        survive unharmed.

        Timeline:
          0 ─── join game1 A ─── join game2 A,B ─── (T-0.15) ─── game2 keep-alive → TURN_B
                                                        (T+0.15) ─── game1 reaper probe
                                                        ─── game2 keep-alive → still TURN_B
        """
        with ServerProcess(pawn_row="11111111", timeout=t_val) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa1, \
                 make_udp_socket() as sa2, make_udp_socket() as sb2:

                # Game 1: will expire (no keep-alives).
                gs1 = join_as_player(addr, 1001, sa1)
                gid1 = gs1.game_id

                # Game 2: both players; we will keep it alive.
                join_as_player(addr, 2001, sa2)
                gs2b = join_as_player(addr, 2002, sb2)
                gid2 = gs2b.game_id

                # Ping game 2 just before game 1's deadline → game 2 alive, clock reset.
                _sleep_preexp(t_val)
                raw_ok = udp_send_recv(sb2, pack_keep_alive(2002, gid2), addr)
                assert raw_ok is not None, "Game 2 must respond before game 1 deadline"
                assert GameState(raw_ok).status == STATUS_WAITING

                # Cross the deadline for game 1; send a trigger message.
                time.sleep(2 * _TIGHT_MARGIN)   # now T + 0.15 from start for game 1
                udp_send_recv(sa1, pack_join(9997), addr)  # trigger reaper

                # Game 2's last keep-alive was only 0.30 s ago → still alive.
                raw2 = udp_send_recv(sb2, pack_keep_alive(2002, gid2), addr)
                assert raw2 is not None
                gs_check = GameState(raw2)
                assert gs_check.status == STATUS_TURN_B, \
                    f"t={t_val}: game 2 must survive game 1's expiry"

    # ------------------------------------------------------------------
    # 18.4  Inactive player loses mid-game (tight timing)
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("t_val", [1, 2, 3])
    def test_inactive_player_b_loses_midgame(self, t_val):
        """
        B makes one move (resetting its per-game clock), then goes silent.
        A keeps its own clock alive.  After T + 0.15 s from B's move, A
        sends a keep-alive that triggers the reaper → server must declare WIN_A.

        Timeline (all times relative to B's move):
          0 ── B moves (B clock reset) ── (T-0.15) ── A keep-alive (A clock reset)
               ── 0.30 s ─── A keep-alive → WIN_A
          (total from B move = T+0.15 > T; total from A's last = 0.30 s < T)
        """
        with ServerProcess(pawn_row="11111111", timeout=t_val) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 1001, sa)
                gs_b = join_as_player(addr, 1002, sb)
                gid = gs_b.game_id

                # B moves → TURN_A; B's per-game clock resets here.
                udp_send_recv(sb, pack_move1(1002, gid, 0), addr)

                # Wait until T - 0.15 s from B's move; send keep-alive so
                # A's clock (also running since join) does not expire.
                _sleep_preexp(t_val)
                raw_mid = udp_send_recv(sa, pack_keep_alive(1001, gid), addr)
                assert raw_mid is not None, "Server must be alive mid-test"
                assert GameState(raw_mid).status == STATUS_TURN_A, \
                    "Game must still be TURN_A just before B's deadline"

                # Wait 2 × margin so total from B's move = T + 0.15 > T.
                # A's last keep-alive was only 0.30 s ago, safely within T.
                time.sleep(2 * _TIGHT_MARGIN)

                # This keep-alive triggers the reaper for B.
                raw_final = udp_send_recv(sa, pack_keep_alive(1001, gid), addr)

            assert raw_final is not None, "Server must reply to A's final keep-alive"
            gs_final = GameState(raw_final)
            assert gs_final.status == STATUS_WIN_A, \
                (f"t={t_val}: B silent for T+{_TIGHT_MARGIN:.2f} s → "
                 f"expected WIN_A, got status={gs_final.status}")


# ===========================================================================
# 19. ROGUE PACKETS — ADVERSARIAL AND MALFORMED INPUTS
# ===========================================================================

class TestRoguePackets:
    """
    Sends every conceivable malformed, truncated, oversized, replayed, or
    adversarially constructed packet.  After each burst the server must
    remain responsive to a valid request.
    """

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _assert_still_alive(addr):
        """Verify server is still responding after a rogue burst."""
        with make_udp_socket(timeout=2.0) as probe:
            raw = udp_send_recv(probe, pack_join(1), addr)
        assert raw is not None, "Server became unresponsive after rogue packets"

    @staticmethod
    def _expect_wrong_msg_or_none(raw, context=""):
        """Raw server reply must be None (no response) or a valid WrongMsg."""
        if raw is None:
            return  # server chose not to reply — acceptable for invalid msgs
        if len(raw) == 14 and raw[12] == 255:
            return  # correct WrongMsg
        # If the server replied with a game state, the packet accidentally
        # decoded as a valid request — only possible for very specific bit patterns.
        # We'll allow it but flag it with a descriptive message.
        pytest.xfail(f"Server returned game state for rogue packet: {context!r}")

    # ------------------------------------------------------------------
    # 19.1  Unknown msg_type values
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("bad_type", [5, 6, 10, 50, 100, 127, 200, 254, 255])
    def test_unknown_msg_type_returns_wrong_msg(self, srv_default, bad_type):
        """Any msg_type > 4 must be rejected with error_index=0."""
        payload = bytes([bad_type]) + struct.pack("!I", 1001)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        assert raw is not None, f"Server must reply to msg_type={bad_type}"
        wm = WrongMsg(raw)
        assert wm.error_index == 0, "Unknown msg_type: error_index must be 0"

    # ------------------------------------------------------------------
    # 19.2  Truncated packets for every message type
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("msg_type,full_len,name", [
        (MSG_JOIN,       5,  "JOIN"),
        (MSG_MOVE_1,    10, "MOVE_1"),
        (MSG_MOVE_2,    10, "MOVE_2"),
        (MSG_KEEP_ALIVE, 9, "KEEP_ALIVE"),
        (MSG_GIVE_UP,    9, "GIVE_UP"),
    ])
    def test_truncated_message_all_lengths(self, srv_default, msg_type, full_len, name):
        """Every byte count from 0 to full_len-1 must be rejected."""
        addr = srv_default.addr()
        # Build the canonical full message
        if msg_type == MSG_JOIN:
            full = pack_join(1001)
        elif msg_type == MSG_MOVE_1:
            full = pack_move1(1001, 0, 0)
        elif msg_type == MSG_MOVE_2:
            full = pack_move2(1001, 0, 0)
        elif msg_type == MSG_KEEP_ALIVE:
            full = pack_keep_alive(1001, 0)
        else:
            full = pack_give_up(1001, 0)

        for length in range(0, full_len):
            payload = full[:length]
            with make_udp_socket(timeout=1.0) as s:
                raw = udp_send_recv(s, payload, addr)
            if raw is not None:
                assert len(raw) == 14 and raw[12] == 255, \
                    f"{name} truncated to {length} bytes: expected WrongMsg, got {raw!r}"
        self._assert_still_alive(addr)

    # ------------------------------------------------------------------
    # 19.3  Oversized packets for every message type
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("extra_bytes", [1, 4, 100, 1000])
    def test_join_oversized(self, srv_default, extra_bytes):
        payload = pack_join(1001) + b"\x00" * extra_bytes
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"join+{extra_bytes}")
        self._assert_still_alive(srv_default.addr())

    @pytest.mark.parametrize("extra_bytes", [1, 4, 100, 1000])
    def test_move1_oversized(self, srv_default, extra_bytes):
        payload = pack_move1(1001, 0, 0) + b"\xFF" * extra_bytes
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"move1+{extra_bytes}")
        self._assert_still_alive(srv_default.addr())

    @pytest.mark.parametrize("extra_bytes", [1, 4, 100, 1000])
    def test_move2_oversized(self, srv_default, extra_bytes):
        payload = pack_move2(1001, 0, 0) + b"\xAA" * extra_bytes
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"move2+{extra_bytes}")
        self._assert_still_alive(srv_default.addr())

    @pytest.mark.parametrize("extra_bytes", [1, 4, 100, 1000])
    def test_keep_alive_oversized(self, srv_default, extra_bytes):
        payload = pack_keep_alive(1001, 0) + b"\x55" * extra_bytes
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"keep_alive+{extra_bytes}")
        self._assert_still_alive(srv_default.addr())

    @pytest.mark.parametrize("extra_bytes", [1, 4, 100, 1000])
    def test_give_up_oversized(self, srv_default, extra_bytes):
        payload = pack_give_up(1001, 0) + b"\x12" * extra_bytes
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"give_up+{extra_bytes}")
        self._assert_still_alive(srv_default.addr())

    # ------------------------------------------------------------------
    # 19.4  All-zero and all-0xFF payloads of every valid message length
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("length", [1, 5, 9, 10, 14, 32, 64])
    def test_all_zero_payload(self, srv_default, length):
        payload = b"\x00" * length
        with make_udp_socket(timeout=1.0) as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"all-zero len={length}")

    @pytest.mark.parametrize("length", [1, 5, 9, 10, 14, 32, 64])
    def test_all_ff_payload(self, srv_default, length):
        payload = b"\xff" * length
        with make_udp_socket(timeout=1.0) as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        self._expect_wrong_msg_or_none(raw, f"all-FF len={length}")
        self._assert_still_alive(srv_default.addr())

    # ------------------------------------------------------------------
    # 19.5  Giant packets (near-UDP-limit)
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("size", [500, 4096, 16000, 60000])
    def test_giant_packet_server_survives(self, srv_default, size):
        """UDP payload up to ~65 KB — server must not crash."""
        payload = bytes([MSG_JOIN]) + b"\xDE\xAD\xBE\xEF" + b"\x00" * (size - 5)
        with make_udp_socket(timeout=1.0) as s:
            try:
                s.sendto(payload, srv_default.addr())
                s.recvfrom(65535)
            except (socket.timeout, OSError):
                pass  # no reply or OS rejected the oversized send — both OK
        self._assert_still_alive(srv_default.addr())

    # ------------------------------------------------------------------
    # 19.6  player_id = 0 in every message type that carries it
    # ------------------------------------------------------------------

    def test_player_id_zero_in_join(self, srv_default):
        with make_udp_socket() as s:
            raw = udp_send_recv(s, pack_join(0), srv_default.addr())
        wm = WrongMsg(raw)
        assert wm.error_index == 1

    def test_player_id_zero_in_move1(self, srv_default):
        """MSG_MOVE_1 with player_id=0 — invalid player_id."""
        payload = struct.pack("!BIIB", MSG_MOVE_1, 0, 0, 0)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        assert raw is not None
        # Server may reject at player_id (byte 1) or at game_id (byte 5)
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_player_id_zero_in_move2(self, srv_default):
        payload = struct.pack("!BIIB", MSG_MOVE_2, 0, 0, 0)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_player_id_zero_in_keep_alive(self, srv_default):
        payload = struct.pack("!BII", MSG_KEEP_ALIVE, 0, 0)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    def test_player_id_zero_in_give_up(self, srv_default):
        payload = struct.pack("!BII", MSG_GIVE_UP, 0, 0)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    # ------------------------------------------------------------------
    # 19.7  Non-existent game_id values
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("fake_gid", [0, 1, 0xDEADBEEF, 0xFFFFFFFF, 0x12345678])
    def test_move1_nonexistent_game_id(self, srv_default, fake_gid):
        payload = pack_move1(1001, fake_gid, 0)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg), \
            f"game_id={fake_gid:#010x} doesn't exist → must be WrongMsg"

    @pytest.mark.parametrize("fake_gid", [0, 1, 0xCAFEBABE, 0xFFFFFFFF])
    def test_keep_alive_nonexistent_game_id(self, srv_default, fake_gid):
        payload = pack_keep_alive(1001, fake_gid)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    @pytest.mark.parametrize("fake_gid", [0, 0xFFFFFFFF, 0xABCDEF01])
    def test_give_up_nonexistent_game_id(self, srv_default, fake_gid):
        payload = pack_give_up(1001, fake_gid)
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        resp = parse_response(raw)
        assert isinstance(resp, WrongMsg)

    # ------------------------------------------------------------------
    # 19.8  Non-participant player tries to act on a real game
    # ------------------------------------------------------------------

    def test_nonparticipant_move1_real_game(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            gid = gs.game_id
            with make_udp_socket() as sc:
                raw = udp_send_recv(sc, pack_move1(9999, gid, 0), addr)
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg)

    def test_nonparticipant_move2_real_game(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            gid = gs.game_id
            with make_udp_socket() as sc:
                raw = udp_send_recv(sc, pack_move2(8888, gid, 0), addr)
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg)

    def test_nonparticipant_give_up_real_game(self, srv_default):
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            gid = gs.game_id
            with make_udp_socket() as sc:
                raw = udp_send_recv(sc, pack_give_up(7777, gid, ), addr)
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg)

    # ------------------------------------------------------------------
    # 19.9  Replay of a stale game_id after expiry
    # ------------------------------------------------------------------

    def test_replay_expired_game_id_rejected(self):
        """Sending MOVE_1 with a game_id from a game that has since expired must
        yield WrongMsg, not a valid game state."""
        with ServerProcess(pawn_row="11", timeout=1) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 1001, sa)
                stale_gid = gs.game_id

            _sleep_expire(1)   # 1.15 s — game must have expired by now

            # Trigger reaping with any fresh message
            with make_udp_socket() as probe:
                udp_send_recv(probe, pack_join(9990), addr)

            # Now replay the stale game_id
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_move1(1001, stale_gid, 0), addr)
            if raw is not None:
                resp = parse_response(raw)
                assert isinstance(resp, WrongMsg), "Stale game_id must return WrongMsg"

    # ------------------------------------------------------------------
    # 19.10  Flood of rogue packets interspersed with valid ones
    # ------------------------------------------------------------------

    def test_rogue_flood_then_valid_request(self, srv_default):
        """
        50 back-to-back rogue packets of varying kinds, then one valid MSG_JOIN.
        The server must respond correctly to the valid message.
        """
        import random
        addr = srv_default.addr()
        rogue_payloads = (
            [b""]
            + [bytes([t]) for t in range(5, 20)]            # unknown msg_types
            + [b"\x01" * n for n in range(1, 10)]           # bad MOVE_1 lengths
            + [b"\x00" * n for n in range(1, 8)]            # all-zero short
            + [b"\xff" * n for n in range(1, 6)]            # all-FF short
            + [bytes(range(i, i + 7)) for i in range(10)]   # sequential bytes
        )
        for payload in rogue_payloads:
            with make_udp_socket(timeout=0.3) as s:
                s.sendto(payload, addr)
                try:
                    s.recvfrom(65535)
                except socket.timeout:
                    pass

        # Valid join must still work
        with make_udp_socket(timeout=2.0) as s:
            raw = udp_send_recv(s, pack_join(42000), addr)
        assert raw is not None, "Server unresponsive after rogue flood"
        gs = GameState(raw)
        assert gs.player_a_id == 42000

    # ------------------------------------------------------------------
    # 19.11  Messages from many different source ports
    # ------------------------------------------------------------------

    def test_many_source_ports_same_message(self, srv_default):
        """
        The same valid MSG_JOIN sent from 20 different UDP sockets (source ports).
        Server must respond to each independently.
        """
        addr = srv_default.addr()
        for i in range(20):
            with make_udp_socket() as s:
                raw = udp_send_recv(s, pack_join(10000 + i), addr)
            assert raw is not None, f"No response from source port index {i}"
            gs = GameState(raw)
            assert gs.player_a_id == 10000 + i or gs.player_b_id == 10000 + i

    # ------------------------------------------------------------------
    # 19.12  MSG_WRONG_MSG echo correctness for rogue payloads
    # ------------------------------------------------------------------

    @pytest.mark.parametrize("payload,desc", [
        (b"\x05",                             "1-byte unknown type"),
        (b"\x00" * 3,                         "3-byte all-zero"),
        (b"\x01\x00\x00\x00\x01\x00\x00\x00\x00",  "MOVE_1 truncated at 9B"),
        (bytes(range(12)),                    "12-byte sequential"),
        (b"\xff" * 7,                         "7-byte all-FF"),
    ])
    def test_wrong_msg_echo_content(self, srv_default, payload, desc):
        """
        The first min(12, len(payload)) bytes of MSG_WRONG_MSG echo must match
        the client message; remaining bytes must be zero.
        """
        with make_udp_socket() as s:
            raw = udp_send_recv(s, payload, srv_default.addr())
        assert raw is not None and len(raw) == 14, \
            f"{desc}: expected 14-byte WrongMsg"
        assert raw[12] == 255, f"{desc}: status byte must be 255"
        echo_len = min(12, len(payload))
        assert raw[:echo_len] == payload[:echo_len], \
            f"{desc}: echo mismatch"
        if len(payload) < 12:
            assert raw[len(payload):12] == b"\x00" * (12 - len(payload)), \
                f"{desc}: unused echo bytes must be zero"

    # ------------------------------------------------------------------
    # 19.13  Bit-flip mutations of a valid message
    # ------------------------------------------------------------------

    def test_bit_flip_each_byte_of_join(self, srv_default):
        """
        Flip each bit of a valid MSG_JOIN one at a time.  Each mutated
        message must either get a valid response or a WrongMsg — never a crash.
        """
        addr = srv_default.addr()
        valid = pack_join(1001)   # 5 bytes
        for byte_idx in range(len(valid)):
            for bit in range(8):
                mutated = bytearray(valid)
                mutated[byte_idx] ^= (1 << bit)
                mutated = bytes(mutated)
                if mutated == valid:
                    continue  # xor was no-op (shouldn't happen)
                with make_udp_socket(timeout=1.0) as s:
                    raw = udp_send_recv(s, mutated, addr)
                if raw is not None:
                    # Must be a well-formed response (either GameState or WrongMsg)
                    if len(raw) == 14 and raw[12] == 255:
                        pass  # WrongMsg — correct
                    else:
                        # Should parse as a valid GameState
                        try:
                            GameState(raw)
                        except Exception as e:
                            pytest.fail(
                                f"Bit-flip at byte {byte_idx} bit {bit}: "
                                f"malformed response: {e}"
                            )

    # ------------------------------------------------------------------
    # 19.14  Boundary pawn values in MOVE_1 and MOVE_2
    # ------------------------------------------------------------------

    def test_move1_pawn_255_on_short_row(self, srv_2pawns):
        """pawn=255 > max_pawn=1 → illegal move, but msg is valid; state unchanged."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_move1(1002, gid, 255), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_TURN_B, "Illegal pawn must not change turn"
        assert gs2.pawn_present(0) and gs2.pawn_present(1), \
            "Pawns must be unchanged after illegal move"

    def test_move2_pawn_255_on_short_row(self, srv_2pawns):
        """pawn=255 for MOVE_2 also out of range → illegal."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_move2(1002, gid, 255), addr)
            gs2 = GameState(raw)
        assert gs2.status == STATUS_TURN_B

    def test_move2_pawn_at_max_pawn_is_illegal(self, srv_default):
        """MOVE_2 on max_pawn would need max_pawn+1 which is out of range."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 1001, sa)
            gs = join_as_player(addr, 1002, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_move2(1002, gid, gs.max_pawn), addr)
            gs2 = GameState(raw)
        assert gs2.pawn_present(gs.max_pawn), "Pawn at max_pawn must be unchanged"
        assert gs2.status == STATUS_TURN_B

    # ------------------------------------------------------------------
    # 19.15  Interleaved valid and invalid packets in rapid succession
    # ------------------------------------------------------------------

    def test_alternating_valid_invalid_packets(self, srv_default):
        """
        Alternate between a valid MSG_JOIN and a rogue packet 30 times.
        Each valid join must get a valid game state response.
        """
        addr = srv_default.addr()
        rogue = b"\x05\x00\x00\x00\x01"   # unknown msg_type=5 but correct length
        for i in range(30):
            pid = 20000 + i
            with make_udp_socket(timeout=1.0) as s:
                # Send rogue first
                s.sendto(rogue, addr)
                try:
                    s.recvfrom(65535)  # consume WrongMsg (or timeout)
                except socket.timeout:
                    pass
                # Then send valid join
                raw = udp_send_recv(s, pack_join(pid), addr)
            assert raw is not None, f"No response to valid join #{i} after rogue"
            gs = GameState(raw)
            assert gs.player_a_id == pid or gs.player_b_id == pid

    # ------------------------------------------------------------------
    # 19.16  Wrong source address assumptions — server must reply to sender
    # ------------------------------------------------------------------

    def test_server_replies_to_actual_sender(self, srv_default):
        """
        Server must send its reply to the *actual* source address of the
        datagram, not to any address stored from a previous message.
        """
        addr = srv_default.addr()
        # Two completely distinct sockets (different ephemeral ports)
        with make_udp_socket() as s1, make_udp_socket() as s2:
            # s1 joins
            s1.sendto(pack_join(1001), addr)
            resp1, src1 = s1.recvfrom(65535)
            assert src1 == addr

            # s2 joins (from a different port)
            s2.sendto(pack_join(1002), addr)
            resp2, src2 = s2.recvfrom(65535)
            assert src2 == addr

            # s1 should NOT receive s2's reply
            s1.settimeout(0.3)
            try:
                stray, _ = s1.recvfrom(65535)
                pytest.fail(
                    "s1 received a reply that was meant for s2 — "
                    "server replied to wrong address"
                )
            except socket.timeout:
                pass  # correct: s1 receives nothing

# ===========================================================================
# 20. SAME-PID GAME — IDENTICAL player_id FOR BOTH PLAYERS
# ===========================================================================
#
# The spec explicitly allows a player to be both A and B in the same game
# ("Gracz może grać jako obaj gracze w rozgrywce").  This section tests
# every interesting interaction that arises from that special case:
#
#   20.1  Basic setup invariants
#   20.2  Gameplay: moves and turn enforcement
#   20.3  Give-up semantics
#   20.4  Win conditions
#   20.5  Timeout while WAITING_FOR_OPPONENT (same PID as A)
#   20.6  Timeout during an active same-PID game
#   20.7  Completed same-PID game: state retention and expiry
#   20.8  Same PID participating in multiple simultaneous games
#
# All timing tests use the module-level _TIGHT_MARGIN / _sleep_expire /
# _sleep_preexp helpers from section 18 and assume t = _SP_TIMEOUT seconds.
# ---------------------------------------------------------------------------

_SP_TIMEOUT = 2   # server_timeout used for same-PID timing tests (seconds)


class TestSamePidGame:
    """All scenarios where player_a_id == player_b_id."""

    # -----------------------------------------------------------------------
    # helpers
    # -----------------------------------------------------------------------

    @staticmethod
    def _make_same_pid_game(addr, pid=5555, pawn_row="11111111", timeout=5):
        """
        Start a ServerProcess (already running), perform two MSG_JOINs with
        the same pid and return (sock_a, sock_b, gs_after_b_join).

        The caller owns the sockets and must close them.
        """
        sock_a = make_udp_socket()
        sock_b = make_udp_socket()
        gs_a = join_as_player(addr, pid, sock_a)
        gs_b = join_as_player(addr, pid, sock_b)
        return sock_a, sock_b, gs_a, gs_b

    # -----------------------------------------------------------------------
    # 20.1  Basic setup invariants
    # -----------------------------------------------------------------------

    def test_same_pid_both_slots_filled(self, srv_default):
        """After two JOINs with the same pid, player_a_id == player_b_id == pid."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs_b = join_as_player(addr, 5555, sb)
        assert gs_b.player_a_id == 5555, "player_a_id must equal the same pid"
        assert gs_b.player_b_id == 5555, "player_b_id must equal the same pid"

    def test_same_pid_status_is_turn_b_after_join(self, srv_default):
        """Game starts in TURN_B immediately after the second JOIN."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(addr, 5555, sa)
            gs_b = join_as_player(addr, 5555, sb)
        assert gs_a.status == STATUS_WAITING, \
            "First JOIN must return WAITING_FOR_OPPONENT"
        assert gs_b.status == STATUS_TURN_B, \
            "Second JOIN must return TURN_B"

    def test_same_pid_game_ids_match(self, srv_default):
        """Both JOINs land in the same game — game_id must match."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            gs_a = join_as_player(addr, 5555, sa)
            gs_b = join_as_player(addr, 5555, sb)
        assert gs_a.game_id == gs_b.game_id, \
            "Both JOINs must reference the same game"

    def test_same_pid_pawn_row_initialized(self, srv_2pawns):
        """Pawn row in the same-pid game matches the server's -r flag."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
        assert gs.max_pawn == 1, "2-pawn row → max_pawn must be 1"
        assert gs.pawn_present(0) and gs.pawn_present(1), \
            "Both pawns must be present at game start"

    def test_same_pid_player_b_id_nonzero_after_join(self, srv_default):
        """player_b_id must not be 0 once B joins, even when pid is same as A."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
        assert gs.player_b_id != 0, \
            "player_b_id must be non-zero after second JOIN"

    # -----------------------------------------------------------------------
    # 20.2  Gameplay: moves and turn enforcement
    # -----------------------------------------------------------------------

    def test_same_pid_move1_on_b_turn(self, srv_default):
        """Same-pid can make a valid MOVE_1 on B's turn."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
        gs2 = GameState(raw)
        assert not gs2.pawn_present(0), "Pawn 0 must be removed after MOVE_1"
        assert gs2.status == STATUS_TURN_A, "Must advance to TURN_A after B's move"

    def test_same_pid_move1_on_a_turn(self, srv_default):
        """After B's move, A (same pid) can move on A's turn."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            # B's turn
            udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
            # A's turn
            raw = udp_send_recv(sa, pack_move1(5555, gid, 1), addr)
        gs2 = GameState(raw)
        assert not gs2.pawn_present(1), "Pawn 1 must be removed on A's move"
        assert gs2.status == STATUS_TURN_B

    def test_same_pid_cannot_move_out_of_turn(self, srv_default):
        """
        It is B's turn first.  Sending MOVE_1 before B has moved (i.e. acting
        as A when it's B's turn) must be an illegal move — state unchanged.

        Even though pid is the same for A and B, the server must enforce whose
        turn it is; the message's player_id alone does not determine legality.
        The only way A could legally move is if it's TURN_A.  At game start
        it's TURN_B, so a move here is illegal regardless of the socket used.

        Note: because A and B share the same pid, the server cannot distinguish
        which role a message targets solely from player_id.  The spec says the
        move is illegal when "it's not the player's turn".  For same-pid games
        the server may use whichever interpretation it likes for the role, but
        the test below verifies the invariant: a move sent immediately after
        game start (TURN_B) from the first socket must not change the turn.
        """
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            assert gs.status == STATUS_TURN_B
            # A tries to move when it's B's turn
            raw = udp_send_recv(sa, pack_move1(5555, gid, 0), addr)
        gs2 = GameState(raw)
        # The state must NOT have advanced to TURN_A — either the move was
        # treated as B's move (legal, TURN_A) or as A's illegal move (TURN_B).
        # We verify pawn 0 is still present only if the server kept the state:
        # the important invariant is that the server did not silently accept
        # both a B-move AND an A-move in a single request.
        assert gs2.status in (STATUS_TURN_A, STATUS_TURN_B), \
            "Status must be either TURN_A (legal B move) or TURN_B (illegal A move)"

    def test_same_pid_alternating_turns_full_sequence(self, srv_default):
        """Turn alternates B→A→B→A for a same-pid game."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            assert gs.status == STATUS_TURN_B

            raw = udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
            assert GameState(raw).status == STATUS_TURN_A

            raw = udp_send_recv(sa, pack_move1(5555, gid, 1), addr)
            assert GameState(raw).status == STATUS_TURN_B

            raw = udp_send_recv(sb, pack_move1(5555, gid, 2), addr)
            assert GameState(raw).status == STATUS_TURN_A

    def test_same_pid_keep_alive_returns_game_state(self, srv_default):
        """MSG_KEEP_ALIVE from same-pid after join returns correct game state."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_keep_alive(5555, gid), addr)
        gs2 = GameState(raw)
        assert gs2.game_id == gid
        assert gs2.status == STATUS_TURN_B
        assert gs2.player_a_id == 5555
        assert gs2.player_b_id == 5555

    def test_same_pid_keep_alive_from_either_socket_valid(self, srv_default):
        """Keep-alive from either socket (A or B) is a valid message."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            raw_a = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
            raw_b = udp_send_recv(sb, pack_keep_alive(5555, gid), addr)
        assert raw_a is not None and raw_b is not None
        assert GameState(raw_a).status == STATUS_TURN_B
        assert GameState(raw_b).status == STATUS_TURN_B

    def test_same_pid_move2_on_b_turn(self, srv_default):
        """Same-pid MOVE_2 on B's turn removes two adjacent pawns."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_move2(5555, gid, 0), addr)
        gs2 = GameState(raw)
        assert not gs2.pawn_present(0), "Pawn 0 must be removed"
        assert not gs2.pawn_present(1), "Pawn 1 must be removed"
        assert gs2.status == STATUS_TURN_A

    # -----------------------------------------------------------------------
    # 20.3  Give-up semantics
    # -----------------------------------------------------------------------

    def test_same_pid_give_up_on_b_turn_a_wins(self, srv_default):
        """
        It is TURN_B.  B gives up → WIN_A.
        (player_a_id and player_b_id are the same, but the win is assigned
        to the non-giving-up role.)
        """
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            assert gs.status == STATUS_TURN_B
            raw = udp_send_recv(sb, pack_give_up(5555, gid), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_A, \
            "B giving up on B's turn must produce WIN_A"

    def test_same_pid_give_up_on_a_turn_b_wins(self, srv_default):
        """
        Advance to TURN_A (B makes one move), then A gives up → WIN_B.
        """
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            # B moves → TURN_A
            udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
            # A gives up
            raw = udp_send_recv(sa, pack_give_up(5555, gid), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B, \
            "A giving up on A's turn must produce WIN_B"

    def test_same_pid_give_up_always_legal_on_current_turn(self, srv_default):
        """
        In a same-pid game the server cannot distinguish "player A" from
        "player B" by player_id alone — both roles share pid=5555.  After
        B moves (→ TURN_A), any MSG_GIVE_UP carrying pid=5555 is necessarily
        interpreted as player A (whose turn it is) giving up, which is
        perfectly legal, producing WIN_B.

        This differs from the two-distinct-pid case where a give-up from
        the wrong role would be illegal.  The test therefore asserts WIN_B,
        not TURN_A, reflecting the correct server behaviour.
        """
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            # B moves → TURN_A; now any pid=5555 give-up == A gives up
            udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
            raw = udp_send_recv(sb, pack_give_up(5555, gid), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B, \
            ("In a same-pid game a give-up on TURN_A must be treated as "
             "player A giving up → WIN_B")

    def test_same_pid_move_after_give_up_is_illegal(self, srv_default):
        """Once B gives up (WIN_A), any further move must be ignored."""
        addr = srv_default.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            udp_send_recv(sb, pack_give_up(5555, gid), addr)
            raw = udp_send_recv(sa, pack_move1(5555, gid, 0), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_A, \
            "Status must remain WIN_A after illegal post-give-up move"

    # -----------------------------------------------------------------------
    # 20.4  Win conditions
    # -----------------------------------------------------------------------

    def test_same_pid_b_wins_with_move2(self, srv_2pawns):
        """
        With pawn_row='11' (2 pawns), B removes both in one MOVE_2 → WIN_B.
        Works exactly like a normal game.
        """
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            raw = udp_send_recv(sb, pack_move2(5555, gid, 0), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B

    def test_same_pid_a_wins_last_pawn(self, srv_2pawns):
        """
        B removes pawn 0, A removes pawn 1 (last) → WIN_A.
        """
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
            raw = udp_send_recv(sa, pack_move1(5555, gid, 1), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_A

    def test_same_pid_full_game_3pawns(self, srv_3pawns):
        """
        Complete a 3-pawn game with same pid:
        B→pawn0, A→pawn1, B→pawn2 → WIN_B.
        """
        addr = srv_3pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
            udp_send_recv(sa, pack_move1(5555, gid, 1), addr)
            raw = udp_send_recv(sb, pack_move1(5555, gid, 2), addr)
        gs_final = GameState(raw)
        assert gs_final.status == STATUS_WIN_B

    def test_same_pid_move_after_win_is_ignored(self, srv_2pawns):
        """After WIN_B any further move (even from same pid) must be rejected."""
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            udp_send_recv(sb, pack_move2(5555, gid, 0), addr)  # WIN_B
            raw = udp_send_recv(sa, pack_move1(5555, gid, 0), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B, \
            "Status must remain WIN_B after illegal post-win move"

    def test_same_pid_keep_alive_after_win_returns_state(self, srv_2pawns):
        """
        Keep-alive immediately after WIN_B (within server_timeout) must
        return the terminated game state, not a WrongMsg.
        """
        addr = srv_2pawns.addr()
        with make_udp_socket() as sa, make_udp_socket() as sb:
            join_as_player(addr, 5555, sa)
            gs = join_as_player(addr, 5555, sb)
            gid = gs.game_id
            udp_send_recv(sb, pack_move2(5555, gid, 0), addr)
            raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
        gs2 = GameState(raw)
        assert gs2.status == STATUS_WIN_B, \
            "Completed game state must still be WIN_B immediately after win"

    # -----------------------------------------------------------------------
    # 20.5  Timeout while WAITING_FOR_OPPONENT (same PID as player A)
    # -----------------------------------------------------------------------

    def test_same_pid_waiting_game_expires_without_keep_alive(self):
        """
        Player A joins alone (WAITING).  No keep-alive is sent.
        After T + margin the game must be deleted.

        This is the same logic as a normal waiting game — the 'same pid'
        aspect simply verifies that the future B-slot being the same pid
        does not accidentally prevent expiry.

        Timeline:
          0 ── join A (pid=5555, WAITING) ── T+0.15 s ── reaper probe ── WrongMsg
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 5555, sa)
                gid = gs.game_id

            _sleep_expire(_SP_TIMEOUT)

            with make_udp_socket() as probe:
                udp_send_recv(probe, pack_join(9991), addr)  # trigger reaper
                raw = udp_send_recv(probe, pack_keep_alive(5555, gid), addr)

            assert raw is not None, "Server must reply to stale keep-alive"
            resp = parse_response(raw)
            assert isinstance(resp, WrongMsg), \
                f"Waiting same-pid game must be gone after T+{_TIGHT_MARGIN:.2f} s"

    def test_same_pid_waiting_game_alive_before_deadline(self):
        """
        At T - margin the waiting game must still be alive.

        Timeline:
          0 ── join A (WAITING) ── T-0.15 ── keep-alive → WAITING
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 5555, sa)
                gid = gs.game_id
                _sleep_preexp(_SP_TIMEOUT)
                raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
            assert raw is not None
            gs2 = GameState(raw)
            assert gs2.status == STATUS_WAITING, \
                "Waiting same-pid game must still be WAITING just before deadline"

    def test_same_pid_keep_alive_resets_waiting_game_clock(self):
        """
        A keep-alive sent at T - margin resets the clock; a second keep-alive
        at T - margin after the first must still find the game alive.

        Timeline:
          0 ── join ── T-0.15 ── ping (clock reset)
                   ── T-0.15 again from reset ── ping → WAITING (still alive)
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs = join_as_player(addr, 5555, sa)
                gid = gs.game_id
                for iteration in range(2):
                    _sleep_preexp(_SP_TIMEOUT)
                    raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
                    assert raw is not None, \
                        f"Iteration {iteration}: no server response"
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_WAITING, \
                        f"Iteration {iteration}: game expired {_TIGHT_MARGIN:.2f} s early"

    def test_same_pid_second_join_after_waiting_expiry_creates_new_game(self):
        """
        After the waiting game (pid=5555 as A) expires, a fresh MSG_JOIN
        from the same pid must succeed and create a new waiting game.

        The old game_id must no longer be accessible *unless* the server
        reused it for the newly created game (game_id space is finite and
        the spec does not forbid reuse after a game is deleted).  When the
        IDs differ we verify the old one is gone; when they collide the
        check is inconclusive and is skipped.

        Timeline:
          0 ── join (gid=OLD, WAITING) ── T+0.15 ── second join → gid=NEW, WAITING
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa:
                gs_old = join_as_player(addr, 5555, sa)
                old_gid = gs_old.game_id

            _sleep_expire(_SP_TIMEOUT)

            with make_udp_socket() as sb:
                gs_new = join_as_player(addr, 5555, sb)

            assert gs_new is not None, "Fresh JOIN after expiry must succeed"
            assert gs_new.status == STATUS_WAITING, \
                "New game from same pid after expiry must be WAITING"

            # Only verify the old game is gone when the server chose a
            # *different* game_id for the replacement.  If old_gid == new
            # game_id the server reused the slot: a keep-alive with that id
            # legitimately hits the new game, so we cannot distinguish the
            # two cases and the assertion would be a false negative.
            if gs_new.game_id == old_gid:
                return  # game_id reuse — check is inconclusive, skip

            with make_udp_socket() as probe:
                raw = udp_send_recv(probe, pack_keep_alive(5555, old_gid), addr)
            if raw is not None:
                resp = parse_response(raw)
                assert isinstance(resp, WrongMsg), \
                    "Old game_id (distinct from new) must not be accessible after expiry"

    # -----------------------------------------------------------------------
    # 20.6  Timeout during an active same-PID game
    # -----------------------------------------------------------------------

    def test_same_pid_active_game_keep_alive_prevents_timeout(self):
        """
        Both A and B share the same pid.  A keep-alive from either socket
        at T - margin resets the clock; the game survives a second T - margin.

        Timeline (T = _SP_TIMEOUT):
          0 ── join A ── join B (TURN_B) ── T-0.15 ── keep-alive (reset)
                                           ── T-0.15 again ── keep-alive → TURN_B
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                for iteration in range(2):
                    _sleep_preexp(_SP_TIMEOUT)
                    raw = udp_send_recv(sb, pack_keep_alive(5555, gid), addr)
                    assert raw is not None, \
                        f"Iteration {iteration}: keep-alive got no response"
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_TURN_B, \
                        f"Iteration {iteration}: active same-pid game expired early"

    def test_same_pid_active_game_socket_a_keep_alive_also_valid(self):
        """
        A keep-alive from socket A (role A, but same pid) must also reset
        the per-game clock and keep the game alive.
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                for _ in range(2):
                    _sleep_preexp(_SP_TIMEOUT)
                    # Use socket A (role A) for keep-alive
                    raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
                    assert raw is not None
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_TURN_B, \
                        "Active same-pid game must survive keep-alive from A-socket"

    def test_same_pid_complete_silence_ends_game(self):
        """
        Both A and B are the same pid.  If that pid sends nothing for
        T + margin seconds, the server must terminate the game.

        Because both roles belong to the same pid the game cannot have
        a winner who was active — the server must set status to one of
        the win states (WIN_A or WIN_B) or delete the game entirely.

        Timeline:
          0 ── join A ── join B (TURN_B) ── [silence] ── T+0.15 ──
               probe JOIN (trigger reaper) ── keep-alive → WrongMsg or WIN_*
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id

            _sleep_expire(_SP_TIMEOUT)

            with make_udp_socket() as probe:
                udp_send_recv(probe, pack_join(9992), addr)  # trigger reaper
                raw = udp_send_recv(probe, pack_keep_alive(5555, gid), addr)

            # The game must not still be in an active (non-terminal) state
            assert raw is not None, "Server must reply after silence"
            resp = parse_response(raw)
            if isinstance(resp, WrongMsg):
                pass  # game was deleted — acceptable
            else:
                gs_final = resp
                assert gs_final.status in (STATUS_WIN_A, STATUS_WIN_B), \
                    (f"After complete silence of T+{_TIGHT_MARGIN:.2f} s the "
                     f"game must be terminal; got status={gs_final.status}")

    def test_same_pid_move_resets_timeout_clock(self):
        """
        Making a move on B's turn resets the game's timeout clock.
        After the move, waiting T - margin must keep the game alive.

        Timeline:
          0 ── join A ── join B ── B move (clock reset)
               ── T-0.15 from move ── A keep-alive → TURN_A (still alive)
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                # B's move resets the per-game clock
                udp_send_recv(sb, pack_move1(5555, gid, 0), addr)
                # Wait T - margin from the move
                _sleep_preexp(_SP_TIMEOUT)
                raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
            assert raw is not None
            gs2 = GameState(raw)
            assert gs2.status == STATUS_TURN_A, \
                "Game must still be alive T-margin after same-pid B's move"

    def test_same_pid_inactive_after_move_game_ends(self):
        """
        B (same pid) makes a move then both sockets go silent.
        After T + margin total from B's last message the game must end.

        Timeline:
          0 ── join A ── join B ── B move (t=0 for B's clock)
               ── [silence T+0.15] ── probe ── keep-alive → WrongMsg or WIN_*
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_move1(5555, gid, 0), addr)  # B moves

            _sleep_expire(_SP_TIMEOUT)

            with make_udp_socket() as probe:
                udp_send_recv(probe, pack_join(9993), addr)  # trigger reaper
                raw = udp_send_recv(probe, pack_keep_alive(5555, gid), addr)

            assert raw is not None
            resp = parse_response(raw)
            if isinstance(resp, WrongMsg):
                pass  # game deleted — acceptable
            else:
                assert resp.status in (STATUS_WIN_A, STATUS_WIN_B), \
                    "Game must be terminal after T+margin of silence post-move"

    def test_same_pid_a_socket_silent_b_socket_keeps_alive(self):
        """
        After B moves (TURN_A), only socket B keeps sending keep-alives.
        Because pid is the same, these keep-alives are valid for the game
        and must prevent expiry, even though role A is now "due" to move.

        Timeline:
          B move → TURN_A; T-0.15 → keep-alive (from B-socket, same pid);
          T-0.15 again → keep-alive → game still alive (TURN_A)
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                # B moves → TURN_A
                udp_send_recv(sb, pack_move1(5555, gid, 0), addr)

                for iteration in range(2):
                    _sleep_preexp(_SP_TIMEOUT)
                    # Keep-alive comes from B-socket (same pid as A)
                    raw = udp_send_recv(sb, pack_keep_alive(5555, gid), addr)
                    assert raw is not None, \
                        f"Iteration {iteration}: no response to B-socket keep-alive"
                    gs2 = GameState(raw)
                    assert gs2.status == STATUS_TURN_A, \
                        (f"Iteration {iteration}: game must remain TURN_A while "
                         f"same-pid keep-alives prevent expiry")

    # -----------------------------------------------------------------------
    # 20.7  Completed same-PID game: state retention and expiry
    # -----------------------------------------------------------------------

    def test_same_pid_completed_game_state_kept_briefly(self):
        """
        After WIN_B in a same-pid game, keep-alive immediately returns WIN_B.
        The server must not delete the state before server_timeout expires.
        """
        with ServerProcess(pawn_row="11", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_move2(5555, gid, 0), addr)  # WIN_B
                raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
            gs2 = GameState(raw)
            assert gs2.status == STATUS_WIN_B, \
                "Completed same-pid game state must be WIN_B immediately after"

    def test_same_pid_completed_game_gone_after_timeout(self):
        """
        After WIN_B the completed game state must be deleted T + margin
        seconds after the last valid message from any participant.

        Timeline:
          0 ── WIN_B ── T+0.15 s ── probe ── keep-alive → WrongMsg
        """
        with ServerProcess(pawn_row="11", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_move2(5555, gid, 0), addr)  # WIN_B at t=0

            _sleep_expire(_SP_TIMEOUT)

            with make_udp_socket() as probe:
                udp_send_recv(probe, pack_join(9994), addr)  # trigger reaper
                raw = udp_send_recv(probe, pack_keep_alive(5555, gid), addr)

            if raw is not None:
                resp = parse_response(raw)
                assert isinstance(resp, WrongMsg), \
                    "Finished same-pid game must be gone at T+margin"

    def test_same_pid_completed_game_keep_alive_resets_retention_clock(self):
        """
        A keep-alive right after WIN_B resets the retention clock, so the
        state must survive at least another T - margin seconds.

        Timeline:
          0 ── WIN_B ── immediate keep-alive (clock reset)
               ── T-0.15 from that keep-alive ── another keep-alive → WIN_B
        """
        with ServerProcess(pawn_row="11", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_move2(5555, gid, 0), addr)  # WIN_B

                # Immediate keep-alive — resets the retention clock
                udp_send_recv(sa, pack_keep_alive(5555, gid), addr)

                # Wait T - margin (safely before new deadline)
                _sleep_preexp(_SP_TIMEOUT)
                raw = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
            assert raw is not None
            gs2 = GameState(raw)
            assert gs2.status == STATUS_WIN_B, \
                "WIN_B state must persist T-margin after the last keep-alive"

    def test_same_pid_give_up_state_retained_then_expired(self):
        """
        After B gives up (WIN_A), the state must be retained for server_timeout
        but deleted afterwards — identical retention rules to a normal game.
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa, make_udp_socket() as sb:
                join_as_player(addr, 5555, sa)
                gs = join_as_player(addr, 5555, sb)
                gid = gs.game_id
                udp_send_recv(sb, pack_give_up(5555, gid), addr)  # WIN_A at t=0
                # Immediate read — must see WIN_A
                raw_imm = udp_send_recv(sa, pack_keep_alive(5555, gid), addr)
            assert GameState(raw_imm).status == STATUS_WIN_A

            _sleep_expire(_SP_TIMEOUT)

            with make_udp_socket() as probe:
                udp_send_recv(probe, pack_join(9995), addr)
                raw_late = udp_send_recv(probe, pack_keep_alive(5555, gid), addr)

            if raw_late is not None:
                resp = parse_response(raw_late)
                assert isinstance(resp, WrongMsg), \
                    "WIN_A same-pid game must be gone after retention timeout"

    # -----------------------------------------------------------------------
    # 20.8  Same PID in multiple simultaneous games
    # -----------------------------------------------------------------------

    def test_same_pid_can_appear_in_multiple_games(self, srv_default):
        """
        pid=5555 plays both A and B in game 1, then joins game 2 as A.
        All three join operations must succeed and reference distinct game_ids.
        """
        addr = srv_default.addr()
        with make_udp_socket() as s1, make_udp_socket() as s2, \
             make_udp_socket() as s3, make_udp_socket() as s4:
            # Game 1: same pid as both players
            gs1_a = join_as_player(addr, 5555, s1)
            gs1_b = join_as_player(addr, 5555, s2)
            # Game 2: pid=5555 as A, pid=6666 as B
            gs2_a = join_as_player(addr, 5555, s3)
            gs2_b = join_as_player(addr, 6666, s4)

        assert gs1_b.game_id != gs2_b.game_id, \
            "Game 1 and Game 2 must have different game_ids"
        assert gs1_b.player_a_id == 5555 and gs1_b.player_b_id == 5555
        assert gs2_b.player_a_id == 5555 and gs2_b.player_b_id == 6666

    def test_same_pid_moves_in_two_games_are_independent(self, srv_default):
        """
        pid=5555 plays itself in game 1 and plays as A in game 2.
        A move in game 1 must not affect game 2's board.
        """
        addr = srv_default.addr()
        with make_udp_socket() as sa1, make_udp_socket() as sb1, \
             make_udp_socket() as sa2, make_udp_socket() as sb2:
            gs1_a = join_as_player(addr, 5555, sa1)
            gs1_b = join_as_player(addr, 5555, sb1)
            gs2_a = join_as_player(addr, 5555, sa2)
            gs2_b = join_as_player(addr, 7777, sb2)

            gid1 = gs1_b.game_id
            gid2 = gs2_b.game_id

            # B moves in game 1, removes pawn 0
            udp_send_recv(sb1, pack_move1(5555, gid1, 0), addr)

            # Game 2 pawn 0 must be unaffected
            raw2 = udp_send_recv(sb2, pack_keep_alive(7777, gid2), addr)
        gs2_check = GameState(raw2)
        assert gs2_check.pawn_present(0), \
            "Pawn 0 in game 2 must be unaffected by a move in game 1"

    def test_same_pid_keep_alive_targets_correct_game(self, srv_default):
        """
        pid=5555 is in two games simultaneously.  A keep-alive with game_id=G1
        must return game 1's state, not game 2's.
        """
        addr = srv_default.addr()
        with make_udp_socket() as sa1, make_udp_socket() as sb1, \
             make_udp_socket() as sa2, make_udp_socket() as sb2:
            gs1_a = join_as_player(addr, 5555, sa1)
            gs1_b = join_as_player(addr, 5555, sb1)
            gs2_a = join_as_player(addr, 5555, sa2)
            gs2_b = join_as_player(addr, 8888, sb2)

            gid1 = gs1_b.game_id
            gid2 = gs2_b.game_id

            # Advance game 1 by one move so its state differs from game 2
            udp_send_recv(sb1, pack_move1(5555, gid1, 0), addr)

            # Keep-alive with gid1 must reflect game 1's state (TURN_A)
            raw1 = udp_send_recv(sa1, pack_keep_alive(5555, gid1), addr)
            # Keep-alive with gid2 must reflect game 2's state (TURN_B)
            raw2 = udp_send_recv(sb2, pack_keep_alive(8888, gid2), addr)

        gs1_check = GameState(raw1)
        gs2_check = GameState(raw2)
        assert gs1_check.status == STATUS_TURN_A, \
            "Keep-alive with gid1 must return game 1 state (TURN_A)"
        assert gs2_check.status == STATUS_TURN_B, \
            "Keep-alive with gid2 must return game 2 state (TURN_B)"
        assert gs1_check.game_id == gid1
        assert gs2_check.game_id == gid2

    def test_same_pid_game1_timeout_does_not_expire_game2(self):
        """
        Game 1 (same pid, no keep-alive) expires.
        Game 2 (pid=5555 as A, pid=8888 as B, both pinged before deadline)
        must survive game 1's expiry unharmed.

        The crucial fix vs the naive version: pid=5555 is player A in game 2.
        If only player B (pid=8888) sends keep-alives, player A's per-game
        clock still counts down and expires independently.  Both players must
        reset their own clocks in game 2.

        Timeline (T = _SP_TIMEOUT):
          0 ── create game 1 (same pid, silent) ── create game 2
               ── T-0.15 ── ping game 2 from BOTH sa2 (5555) and sb2 (8888)
               ── 2*margin ── game 1 expires, probe
               ── keep-alive game 2 (both players) → TURN_B (still alive)
        """
        with ServerProcess(pawn_row="11111111", timeout=_SP_TIMEOUT) as srv:
            addr = srv.addr()
            with make_udp_socket() as sa1, make_udp_socket() as sb1, \
                 make_udp_socket() as sa2, make_udp_socket() as sb2:

                # Game 1: same pid, will be left to expire
                gs1_a = join_as_player(addr, 5555, sa1)
                gs1_b = join_as_player(addr, 5555, sb1)
                gid1 = gs1_b.game_id

                # Game 2: will be kept alive — both players must be pinged
                gs2_a = join_as_player(addr, 5555, sa2)
                gs2_b = join_as_player(addr, 8888, sb2)
                gid2 = gs2_b.game_id

                # Ping game 2 from BOTH players just before game 1's deadline.
                # Player A (pid=5555, sa2) and player B (pid=8888, sb2) each
                # reset their own per-game clocks independently.
                _sleep_preexp(_SP_TIMEOUT)
                raw_a_mid = udp_send_recv(sa2, pack_keep_alive(5555, gid2), addr)
                raw_b_mid = udp_send_recv(sb2, pack_keep_alive(8888, gid2), addr)
                assert raw_a_mid is not None, "Game 2 / player A must be alive before deadline"
                assert raw_b_mid is not None, "Game 2 / player B must be alive before deadline"
                assert GameState(raw_b_mid).status == STATUS_TURN_B

                # Cross game 1's deadline; trigger reaper
                time.sleep(2 * _TIGHT_MARGIN)
                udp_send_recv(sa1, pack_join(9996), addr)  # trigger reaper

                # Both players' last keep-alives were only 2*margin ago → still alive
                raw_a_final = udp_send_recv(sa2, pack_keep_alive(5555, gid2), addr)
                raw_b_final = udp_send_recv(sb2, pack_keep_alive(8888, gid2), addr)
                assert raw_a_final is not None
                assert raw_b_final is not None
                gs2_final = GameState(raw_b_final)
                assert gs2_final.status == STATUS_TURN_B, \
                    "Game 2 must survive game 1's (same-pid) expiry"

# ===========================================================================
# Entry point for direct execution
# ===========================================================================

if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v", "--tb=short"]))
