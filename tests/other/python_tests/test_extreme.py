import pytest
from conftest import Protocol, GameState, WrongMsg

def test_wrd_01_max_value_ids(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    max_id = 4294967295 # 0xFFFFFFFF
    udp_socket.sendto(Protocol.pack_join(max_id), addr)
    data, _ = udp_socket.recvfrom(1024)
    st = Protocol.parse_response(data)
    
    assert st.player_a_id == max_id

def test_wrd_02_pawn_index_wrap_around(server_factory, udp_socket):
    board = "1" * 256
    server, port, host, ret = server_factory(board=board)
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    udp_socket.sendto(Protocol.pack_join(20), addr)
    starting_board = Protocol.parse_response(udp_socket.recvfrom(1024)[0])

    # Send MSG_MOVE_2 targeting pawn 255
    # If it unwraps, it might hit 0, or crash. It should reject it.
    udp_socket.sendto(Protocol.pack_move_2(10, st.game_id, 255), addr)
    dt = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    # Either accepted (if logic strikes 255 and out of bounds doesn't crash) or naturally rejected.
    # Spec: "It should reject the move as out of bounds"
    print(dt)
    assert dt.status == starting_board.status # It should not change the game state

def test_wrd_03_give_up_while_waiting(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 0 # WAITING
    
    udp_socket.sendto(Protocol.pack_give_up(10, st.game_id), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st2.status == 0 # WAITING - giving up while waiting should not change the state

def test_wrd_04_keep_alive_while_waiting(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    udp_socket.sendto(Protocol.pack_keep_alive(10, st.game_id), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st2.status == 0 # WAITING

def test_wrd_05_dual_identity_single_client(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    g1 = st.game_id
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    assert st2.game_id == g1
    assert st2.player_a_id == 10
    assert st2.player_b_id == 10
    assert st2.status == 2 # TURN_B
