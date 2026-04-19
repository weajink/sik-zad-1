import pytest
from conftest import Protocol, GameState, WrongMsg

def get_game_state_from_output(stdout: str) -> bool:
    # We don't parse string output, for precise tests we use python raw UDP sockets!
    # But since the client is just a simple wrapper, we'll use raw sockets for everything gameplay.
    pass

def convert_bytes_to_01_string(pawn_row: bytes, max_pawn: int) -> str:
    # Convert the byte array representing the pawn row into a string of '0' and '1' for easier assertions.
    bits = []
    for byte in pawn_row:
        for i in range(8):
            if len(bits) >= max_pawn:
                break
            bits.append('1' if (byte & (1 << i)) else '0')
    return ''.join(bits)

def test_std_01_full_game(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    assert ret is None

    addr = (host, port)
    
    # Player A joins
    udp_socket.sendto(Protocol.pack_join(10), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    assert isinstance(state, GameState)
    assert state.player_a_id == 10
    assert state.player_b_id == 0
    assert state.status == 0 # WAITING_FOR_OPPONENT
    game_id = state.game_id

    # Player B joins
    udp_socket.sendto(Protocol.pack_join(20), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    assert isinstance(state, GameState)
    assert state.player_b_id == 20
    assert state.status == 2 # TURN_B
    
    # Player A sends keep alive to know the status
    udp_socket.sendto(Protocol.pack_keep_alive(10, game_id), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    assert state.status == 2 # TURN_B

    # Player B takes pawn 0 (leftmost)
    udp_socket.sendto(Protocol.pack_move_1(20, game_id, 0), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    assert state.status == 1 # TURN_A

    # Player A checks state internally it's already updated, takes pawn 1 and 2
    udp_socket.sendto(Protocol.pack_move_2(10, game_id, 1), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    readable_pawn_row = convert_bytes_to_01_string(state.pawn_row, state.max_pawn + 1)
    assert readable_pawn_row == "000" 
    assert state.status == 3 # WIN_A

def test_std_02_single_player_both_roles(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)

    # Join as A
    udp_socket.sendto(Protocol.pack_join(100), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    game_id = state.game_id

    # Join as B -> since player_a is already 100, if we join with 100 and WAITING_FOR_OPPONENT exists,
    # spec says "gracz może grać jako obaj gracze", so it should accept as player B in the same game.
    udp_socket.sendto(Protocol.pack_join(100), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    assert state.game_id == game_id
    assert state.player_b_id == 100
    assert state.status == 2 # TURN_B

def test_std_03_keep_alive_mechanism(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)

    udp_socket.sendto(Protocol.pack_join(10), addr)
    state = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    g_id = state.game_id

    udp_socket.sendto(Protocol.pack_join(20), addr)
    state = Protocol.parse_response(udp_socket.recvfrom(1024)[0])

    # Player B is to move (TURN_B). Player A sends keep alive.
    udp_socket.sendto(Protocol.pack_keep_alive(10, g_id), addr)
    data, _ = udp_socket.recvfrom(1024)
    state = Protocol.parse_response(data)
    assert state.status == 2 # Still TURN_B

def test_std_04_give_up_mechanism(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)

    udp_socket.sendto(Protocol.pack_join(10), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    g_id = st.game_id

    udp_socket.sendto(Protocol.pack_join(20), addr)
    Protocol.parse_response(udp_socket.recvfrom(1024)[0])

    udp_socket.sendto(Protocol.pack_give_up(10, g_id), addr)
    st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st2.status == 2 # TURN_B - giving up available only on your turn

    udp_socket.sendto(Protocol.pack_give_up(20, g_id), addr)
    st3 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st3.status == 3 # WIN_A - player B gave up, player A wins

def test_std_05_multiple_concurrent_games(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)

    games = []
    # Create 5 games
    for i in range(1, 6):
        udp_socket.sendto(Protocol.pack_join(i), addr)
        st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
        
        udp_socket.sendto(Protocol.pack_join(i+10), addr)
        st2 = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
        
        games.append(st2.game_id)
    
    assert len(set(games)) == 5 # Ensure 5 distinct game IDs
    
    # Play a move in game 1
    udp_socket.sendto(Protocol.pack_move_1(11, games[0], 0), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    assert st.status == 1 # TURN_A
    
    # Others should still be TURN_B
    for i in range(1, 5):
        udp_socket.sendto(Protocol.pack_keep_alive(i+1, games[i]), addr)
        st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
        assert st.status == 2 # TURN_B
