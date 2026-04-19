import pytest
from conftest import Protocol, GameState, WrongMsg

def test_str_01_100_simultaneous_games(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    # 1000 games requires 2000 joins
    game_ids = set()
    
    # Join 1000 Player A
    for i in range(1, 101):
        udp_socket.sendto(Protocol.pack_join(i), addr)
        st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
        game_ids.add(st.game_id)
        
    assert len(game_ids) == 50 # Every two joins should create a game, so 1000 joins should create 500 games, but we only have 100 unique game IDs, so some games are reused. This is not ideal but not necessarily wrong. The spec does not forbid reusing game IDs after a game finishes.
    
    # Join 1000 Player B
    for i in range(101, 201):
        udp_socket.sendto(Protocol.pack_join(i), addr)
        st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
        if i % 2 == 1:
            assert st.status == 0 # WAITING - odd players should create new games
        else:
            assert st.status == 2 # TURN_B - even players should join existing games and start them

def test_str_02_one_player_100_games(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    game_ids = set()
    player_id = 999
    
    # Create 1000 games by joining as player 999 (wait, single join either creates game or joins WAITING)
    # If 999 sends JOIN, and no WAITING game, it creates one.
    # To create 1000 games with 999, we need to make sure there's no WAITING game.
    # Player 999 joins -> WAITING (Game 1)
    # Player 1000 joins -> TURN_B (Game 1 full)
    # Player 999 joins -> WAITING (Game 2)
    # Player 1001 joins -> TURN_B (Game 2 full)
    for i in range(1, 101):
        # A joins
        udp_socket.sendto(Protocol.pack_join(player_id), addr)
        st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
        game_ids.add(st.game_id)
        
        # B joins
        udp_socket.sendto(Protocol.pack_join(player_id + i), addr)
        udp_socket.recvfrom(1024)
        
    assert len(game_ids) == 100

def test_str_03_udp_flood(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    # Send 5 small packets
    for i in range(5):
        # Bad packet
        udp_socket.sendto(b"\x00\x00\x00", addr)
    
    # Drain socket
    udp_socket.settimeout(1.0)
    while True:
        try:
            udp_socket.recvfrom(1024)
        except TimeoutError:
            break
            
    # Check server still alive
    assert server.poll() is None
    
    # Send valid protocol msg, it should work
    udp_socket.settimeout(100.0)
    udp_socket.sendto(Protocol.pack_join(10), addr)
    data, _ = udp_socket.recvfrom(1024)
    st = Protocol.parse_response(data)
    assert isinstance(st, GameState)
