import pytest
from conftest import Protocol, GameState, WrongMsg
import struct

def setup_game(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    g1 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    udp_socket.sendto(Protocol.pack_join(20), addr)
    g2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    return addr, g2.game_id

def test_err_move_01_out_of_turn(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    # It is TURN_B right now. Player A tries to move.
    udp_socket.sendto(Protocol.pack_move_1(10, g_id, 0), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    # Move rejected, no change
    assert st.status == 2 # Still TURN_B
    assert st.pawn_row[0] & 0b10000000 != 0 # pawn 0 is still 1

def test_err_move_02_empty_space(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    # Player B takes pawn 0
    udp_socket.sendto(Protocol.pack_move_1(20, g_id, 0), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 1 # TURN_A
    
    # Player A tries to take pawn 0 again
    udp_socket.sendto(Protocol.pack_move_1(10, g_id, 0), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st2.status == 1 # Move rejected, still TURN_A

def test_err_move_03_msg_move_2_empty_spaces(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    # B takes pawn 0
    udp_socket.sendto(Protocol.pack_move_1(20, g_id, 0), addr)
    udp_socket.recvfrom(1024)
    
    # A tries to take (0, 1) spanning empty 0
    udp_socket.sendto(Protocol.pack_move_2(10, g_id, 0), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 1 # Move rejected

def test_err_move_04_out_of_bounds(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    # max_pawn is 2. B tries to strike pawn 5
    udp_socket.sendto(Protocol.pack_move_1(20, g_id, 5), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 2 # Rejected, still B's turn

def test_err_move_05_msg_move_2_edge_of_board(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    # max_pawn is 2. B tries to strike 2 and 3.
    udp_socket.sendto(Protocol.pack_move_2(20, g_id, 2), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 2 # Rejected

def test_err_move_06_action_in_finished_game(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    udp_socket.sendto(Protocol.pack_give_up(20, g_id), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 3 # WIN_A
    
    # Send move in finished game
    udp_socket.sendto(Protocol.pack_move_1(10, g_id, 0), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st2.status == 3 # Unchanged

def test_err_msg_01_nonexistent_game(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_move_1(10, 9999, 0), addr)
    try:
        data, _ = udp_socket.recvfrom(1024)
        msg = Protocol.parse_response(data)
        assert isinstance(msg, WrongMsg)
    except TimeoutError:
        pass # "ignores or replies WRONG_MSG"

def test_err_msg_02_game_not_containing_player(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    udp_socket.sendto(Protocol.pack_move_1(30, g_id, 0), addr) # Player 30 is not in game
    try:
        data, _ = udp_socket.recvfrom(1024)
        msg = Protocol.parse_response(data)
        assert isinstance(msg, WrongMsg)
    except TimeoutError:
        pass

def test_err_msg_03_invalid_msg_type(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    import struct
    bad_msg = struct.pack("!BI", 5, 10)
    udp_socket.sendto(bad_msg, addr)
    data, _ = udp_socket.recvfrom(1024)
    msg = Protocol.parse_response(data)
    assert isinstance(msg, WrongMsg)
    assert msg.error_index == 0 # First byte is msg_type

def test_err_msg_04_truncated_msg(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    bad_msg = struct.pack("!BI", 1, 10) # MSG_MOVE_1 but missing game_id and pawn
    udp_socket.sendto(bad_msg, addr)
    data, _ = udp_socket.recvfrom(1024)
    msg = Protocol.parse_response(data)
    assert isinstance(msg, WrongMsg)

def test_err_msg_05_appended_garbage(server_factory, udp_socket):
    addr, g_id = setup_game(server_factory, udp_socket)
    
    bad_msg = Protocol.pack_join(30) + (b"\x00" * 50)
    udp_socket.sendto(bad_msg, addr)
    data, _ = udp_socket.recvfrom(1024)
    msg = Protocol.parse_response(data)
    assert isinstance(msg, WrongMsg)

def test_err_msg_06_join_player_zero(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(0), addr)
    data, _ = udp_socket.recvfrom(1024)
    msg = Protocol.parse_response(data)
    assert isinstance(msg, WrongMsg)
