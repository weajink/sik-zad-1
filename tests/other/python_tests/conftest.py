import pytest
import subprocess
import socket
import time
import struct
import math
import select
from dataclasses import dataclass
from typing import Optional, Tuple

SERVER_BIN = "./kayles_server"
CLIENT_BIN = "./kayles_client"

def get_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port

@pytest.fixture
def server_factory():
    processes = []
    def _start_server(board="1110111", host="127.0.0.1", port=0, timeout=30):
        if port == 0:
            port = get_free_port()
        cmd = [SERVER_BIN, "-r", board, "-a", host, "-p", str(port), "-t", str(timeout)]
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.1) # Wait for server to start and bind
        
        # Check if process died immediately (e.g. invalid args)
        retcode = p.poll()
        processes.append(p)
        return p, port, host, retcode
    
    yield _start_server
    
    for p in processes:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=1)
            except subprocess.TimeoutExpired:
                p.kill()

@pytest.fixture
def client_factory():
    def _run_client(host, port, msg, timeout=5):
        cmd = [CLIENT_BIN, "-a", host, "-p", str(port), "-m", msg, "-t", str(timeout)]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return p
    return _run_client

@pytest.fixture
def udp_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(100.0)
    yield s
    s.close()

# Protocol definitions
MSG_JOIN = 0
MSG_MOVE_1 = 1
MSG_MOVE_2 = 2
MSG_KEEP_ALIVE = 3
MSG_GIVE_UP = 4

@dataclass
class GameState:
    game_id: int
    player_a_id: int
    player_b_id: int
    status: int
    max_pawn: int
    pawn_row: bytes

@dataclass
class WrongMsg:
    original_msg: bytes
    status: int
    error_index: int

class Protocol:
    @staticmethod
    def pack_join(player_id: int) -> bytes:
        return struct.pack("!BI", MSG_JOIN, player_id)

    @staticmethod
    def pack_move_1(player_id: int, game_id: int, pawn: int) -> bytes:
        return struct.pack("!BIIB", MSG_MOVE_1, player_id, game_id, pawn)

    @staticmethod
    def pack_move_2(player_id: int, game_id: int, pawn: int) -> bytes:
        return struct.pack("!BIIB", MSG_MOVE_2, player_id, game_id, pawn)

    @staticmethod
    def pack_keep_alive(player_id: int, game_id: int) -> bytes:
        return struct.pack("!BII", MSG_KEEP_ALIVE, player_id, game_id)

    @staticmethod
    def pack_give_up(player_id: int, game_id: int) -> bytes:
        return struct.pack("!BII", MSG_GIVE_UP, player_id, game_id)

    @staticmethod
    def parse_response(data: bytes) -> tuple:
        if len(data) == 14 and data[12] == 255:
            # Maybe MSG_WRONG_MSG
            orig, status, err_idx = struct.unpack("!12sBB", data)
            if status == 255:
                return WrongMsg(orig, status, err_idx)
        
        # Parse Game state
        if len(data) >= 14: # min size: 4+4+4+1+1 = 14 bytes, + bitmap
            game_id, p_a, p_b, status, max_pawn = struct.unpack("!IIIBB", data[:14])
            expected_bitmap_len = (max_pawn // 8) + 1
            if len(data) == 14 + expected_bitmap_len:
                pawn_row = data[14:]
                return GameState(game_id, p_a, p_b, status, max_pawn, pawn_row)
            
        return None # Unknown
