import pytest
import subprocess
from conftest import SERVER_BIN, CLIENT_BIN, get_free_port
import time

def test_srv_cli_01_valid_args():
    port = get_free_port()
    cmd = [SERVER_BIN, "-r", "1110111", "-a", "127.0.0.1", "-p", str(port), "-t", "30"]
    p = subprocess.Popen(cmd)
    time.sleep(0.5)
    assert p.poll() is None
    p.terminate()
    p.wait()

def test_srv_cli_02_missing_param():
    for args in [
        ["-r", "1110111", "-a", "127.0.0.1", "-p", "8080"],
        ["-r", "1110111", "-a", "127.0.0.1", "-t", "30"],
        ["-r", "1110111", "-p", "8080", "-t", "30"],
        ["-a", "127.0.0.1", "-p", "8080", "-t", "30"]
    ]:
        try:
            p = subprocess.run([SERVER_BIN] + args, capture_output=True, timeout=1.0)
            assert p.returncode == 1
            assert len(p.stderr) > 0 or len(p.stdout) > 0
        except subprocess.TimeoutExpired:
            pytest.fail("Server did not exit on missing parameter")

def test_srv_cli_03_duplicated_params():
    port = get_free_port()
    cmd = [SERVER_BIN, "-r", "1110111", "-a", "127.0.0.1", "-p", str(port), "-p", str(port), "-t", "30"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.5)
    ret = p.poll()
    if ret is None:
        p.terminate()
        p.wait()
    else:
        assert ret == 1 # Depending on implementation either fails or ignores

def test_srv_cli_04_port_zero():
    cmd = [SERVER_BIN, "-r", "1110111", "-a", "127.0.0.1", "-p", "0", "-t", "30"]
    p = subprocess.Popen(cmd)
    time.sleep(0.5)
    assert p.poll() is None
    p.terminate()
    p.wait()

def test_srv_cli_05_port_out_of_bounds():
    for port in ["-1", "65536"]:
        try:
            p = subprocess.run([SERVER_BIN, "-r", "1110111", "-a", "127.0.0.1", "-p", port, "-t", "30"], capture_output=True, timeout=1.0)
            assert p.returncode == 1
        except subprocess.TimeoutExpired:
            pytest.fail("Server did not exit on out-of-bounds port")

def test_srv_cli_06_invalid_initial_board():
    for board in ["", "1010a", "01110", "1110", "0111"]:
        try:
            p = subprocess.run([SERVER_BIN, "-r", board, "-a", "127.0.0.1", "-p", "0", "-t", "30"], capture_output=True, timeout=1.0)
            assert p.returncode == 1
        except subprocess.TimeoutExpired:
            pytest.fail(f"Server did not exit on invalid board: {board}")
    
    too_long = "1" * 300
    try:
        p = subprocess.run([SERVER_BIN, "-r", too_long, "-a", "127.0.0.1", "-p", "0", "-t", "30"], capture_output=True, timeout=1.0)
        assert p.returncode == 1
    except subprocess.TimeoutExpired:
        pytest.fail("Server did not exit on too long board")

def test_srv_cli_07_timeout_out_of_bounds():
    for t in ["0", "-1", "100"]:
        try:
            p = subprocess.run([SERVER_BIN, "-r", "111", "-a", "127.0.0.1", "-p", "0", "-t", t], capture_output=True, timeout=1.0)
            assert p.returncode == 1
        except subprocess.TimeoutExpired:
            pytest.fail(f"Server did not exit on invalid timeout: {t}")

def test_srv_cli_08_multiple_ip_addresses():
    try:
        p = subprocess.run([SERVER_BIN, "-r", "111", "-a", "127.0.0.1,192.168.0.1", "-p", "0", "-t", "30"], capture_output=True, timeout=1.0)
        assert p.returncode == 1
    except subprocess.TimeoutExpired:
        pytest.fail("Server did not exit on invalid address format")

def test_cli_cli_01_valid_args(server_factory, client_factory):
    server, port, host, ret = server_factory()
    assert ret is None
    
    # Send MSG_JOIN (0/10)
    res = client_factory(host, port, "0/10")
    assert res.returncode == 0
    assert len(res.stdout) > 0 # Should print output

def test_cli_cli_02_invalid_message_format():
    for msg in ["0", "0/10/20", "a/b", "1//2", "",
                "0/abc", "abc/10", "0/-1", "-1/10",
                "/", "0/", "/10"]:
        try:
            p = subprocess.run([CLIENT_BIN, "-a", "127.0.0.1", "-p", "8080", "-m", msg, "-t", "30"], capture_output=True, timeout=1.0)
            assert p.returncode == 1
        except subprocess.TimeoutExpired:
            pytest.fail(f"Client did not exit on invalid msg: {msg}")

def test_cli_cli_03_client_timeout():
    port = get_free_port() # A port that nobody is listening on
    start_time = time.time()
    p = subprocess.run([CLIENT_BIN, "-a", "127.0.0.1", "-p", str(port), "-m", "0/10", "-t", "2"], capture_output=True)
    elapsed = time.time() - start_time
    assert elapsed >= 1.9 # Should wait ~2 secs
    assert p.returncode == 0 # "wypisuje... stosowny komunikat i kończy się kodem 0"
