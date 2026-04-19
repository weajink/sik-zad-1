import pytest
import subprocess
import socket
from conftest import SERVER_BIN, Protocol, GameState, WrongMsg, get_free_port
import time

def test_sys_01_port_already_in_use():
    port = get_free_port()
    # Start first instance
    p1 = subprocess.Popen([SERVER_BIN, "-r", "1", "-a", "127.0.0.1", "-p", str(port), "-t", "30"])
    time.sleep(0.5)
    assert p1.poll() is None
    
    # Start second instance on same port
    p2 = subprocess.run([SERVER_BIN, "-r", "1", "-a", "127.0.0.1", "-p", str(port), "-t", "30"], capture_output=True)
    assert p2.returncode == 1 # Second instance should fail gracefully
    
    p1.terminate()
    p1.wait()

def test_net_01_zero_byte_udp(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(b"", addr)
    # Server should ignore or WRONG_MSG, but not crash
    try:
        udp_socket.recvfrom(1024)
    except TimeoutError:
        pass
    assert server.poll() is None

def test_net_02_maximum_size_udp(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    # 65507 is max UDP payload size for IPv4
    huge_payload = b"\x00" * 65507
    udp_socket.sendto(huge_payload, addr)
    
    try:
        data, _ = udp_socket.recvfrom(1024)
        # Assuming WRONG_MSG or ignore
    except TimeoutError:
        pass
    assert server.poll() is None

def test_net_03_endianness(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    import struct
    
    # Protocol expects Big Endian: !BI (type 0, player_id 1) -> 0x00, 0x00, 0x00, 0x00, 0x01
    # We send Little Endian: <BI (type 0, player_id 1) -> 0x00, 0x01, 0x00, 0x00, 0x00
    le_msg = struct.pack("<BI", 0, 1)
    udp_socket.sendto(le_msg, addr)
    
    data, _ = udp_socket.recvfrom(1024)
    st = Protocol.parse_response(data)
    assert isinstance(st, GameState)
    # 0x01 00 00 00 interpreted as Big Endian is 16777216
    assert st.player_a_id == 16777216

def test_net_04_network_roaming(server_factory, udp_socket):
    server, port, host, ret = server_factory(board="111")
    addr = (host, port)
    
    udp_socket.sendto(Protocol.pack_join(10), addr)
    st = Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    udp_socket.sendto(Protocol.pack_join(20), addr)
    Protocol.parse_response(udp_socket.recvfrom(1024)[0])
    
    # Simulate roaming by using a new UDP socket (new ephemeral port)
    new_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    new_socket.settimeout(1.0)
    
    new_socket.sendto(Protocol.pack_move_1(20, st.game_id, 0), addr)
    data, recv_addr = new_socket.recvfrom(1024)
    st2 = Protocol.parse_response(data)
    
    assert st2.status == 1 # Move accepted, now TURN_A
    assert recv_addr == addr
    new_socket.close()
