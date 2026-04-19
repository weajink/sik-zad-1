import pytest
import time
from conftest import Protocol, GameState, WrongMsg

def test_time_01_timeout_destroys_unstarted_game(server_factory, udp_socket):
    # Server timeout is 2 seconds
    server, port, host, ret = server_factory(board="111", timeout=2)
    addr = (host, port)
    
    # Player A joins
    udp_socket.sendto(Protocol.pack_join(10), addr)
    g1 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    # Wait for timeout
    time.sleep(5)
    
    # Try to send keep_alive to the game
    udp_socket.sendto(Protocol.pack_keep_alive(10, g1.game_id), addr)
    data, _ = udp_socket.recvfrom(1024)
    msg = Protocol.parse_response(data)
    
    # Should get WRONG_MSG because game is deleted
    assert isinstance(msg, WrongMsg)

def test_time_02_timeout_during_active_game(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111", timeout=2)
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    g1 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    udp_socket.sendto(Protocol.pack_join(20), addr)
    Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    # Wait 2.5s for timeout
    time.sleep(5)
    
    udp_socket.sendto(Protocol.pack_keep_alive(10, g1.game_id), addr)
    try:
        data, _ = udp_socket.recvfrom(1024)
        msg = Protocol.parse_response(data)
        if isinstance(msg, GameState):
            assert msg.status in (3, 4) # WIN_A or WIN_B
    except TimeoutError:
        pass # If it deleted it altogether without transition

def test_time_03_post_game_state_retention(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="1", timeout=2) # Only 1 pawn
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    g1 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    udp_socket.sendto(Protocol.pack_join(20), addr)
    Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    # Player A takes pawn 0 -> WIN_B
    udp_socket.sendto(Protocol.pack_move_1(20, g1.game_id, 0), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 4 # WIN_B
    
    # Send keep alive immediately
    udp_socket.sendto(Protocol.pack_keep_alive(10, g1.game_id), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st2.status == 4 # It should still be WIN_B immediately after game ends
    
    # Wait for post-time timeout
    time.sleep(5)
    
    udp_socket.sendto(Protocol.pack_keep_alive(10, g1.game_id), addr)
    try:
        data, _ = udp_socket.recvfrom(1024)
        msg = Protocol.parse_response(data)
        assert isinstance(msg, WrongMsg)
    except TimeoutError:
        pass
