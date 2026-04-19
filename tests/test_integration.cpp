// Integration tests — real UDP sockets, real server loop.
//
// Strategy: fork() a child process that runs KaylesServer::run(). The parent
// sends UDP datagrams at the child and verifies byte-for-byte responses.
// Communication uses an ephemeral port obtained by binding a throw-away socket
// (SO_REUSEPORT etc. are not needed because we pass the port to the server
// before it binds).
//
// Spec tested end-to-end:
//   - 3.2 client→server message encoding (byte layouts)
//   - 3.3 MSG_GAME_STATE response on valid messages
//   - 3.3 MSG_WRONG_MSG (14 bytes: 12 client-echo + status=255 + error_index)
//        for invalid messages: unknown msg_type, truncated, overlong,
//        player_id=0, unknown game_id, non-member player.
//   - 3.3 "server sends to the (address, port) it received from".
//   - 5.1 at-most-one WAITING game; second JOIN pairs up.
//   - 5.2 illegal moves still yield MSG_GAME_STATE with unchanged state.
//   - Bitmap ordering (MSB first, excess bits zero) observed on the wire.
//
// IMPORTANT: we never modify production code. If the code has a bug, the test
// will fail and the summary will flag it as a real production bug.

#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
#include "kayles_protocol.h"
#include "kayles_server.h"
// clang-format on

using namespace kayles::protocol;
using namespace kayles::server;
using namespace kayles::error;
using kayles::types::address_t;
using kayles::types::pawn_row_t;
using kayles::types::pawn_t;
using kayles::types::player_id_t;
using kayles::types::timeout_t;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

    // Tear-down registry so that one failing test cannot leak a server child.
    struct ServerFixture {
        pid_t pid = -1;
        uint16_t port = 0;

        ServerFixture() = default;
        ServerFixture(const ServerFixture&) = delete;
        ServerFixture& operator=(const ServerFixture&) = delete;
        ServerFixture(ServerFixture&& other) noexcept : pid(other.pid), port(other.port) {
            other.pid = -1;
            other.port = 0;
        }
        ServerFixture& operator=(ServerFixture&& other) noexcept {
            if (this != &other) {
                if (pid > 0) {
                    kill(pid, SIGKILL);
                    int st = 0;
                    waitpid(pid, &st, 0);
                }
                pid = other.pid;
                port = other.port;
                other.pid = -1;
                other.port = 0;
            }
            return *this;
        }
        ~ServerFixture() {
            if (pid > 0) {
                kill(pid, SIGKILL);
                int status = 0;
                waitpid(pid, &status, 0);
            }
        }
    };

    // Bind a UDP socket to 127.0.0.1:0 to get a kernel-assigned port, read the
    // port back, then close the socket. Port is racy but acceptable for tests.
    uint16_t pick_ephemeral_port() {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            throw std::runtime_error("socket()");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // kernel-assign
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("bind()");
        }
        socklen_t l = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &l) < 0) {
            ::close(fd);
            throw std::runtime_error("getsockname()");
        }
        ::close(fd);
        return ntohs(addr.sin_port);
    }

    // fork() a server child running on the specified port with the given row.
    // Returns fixture with pid; kill & wait on destruction.
    ServerFixture spawn_server(const std::string& row_str, uint16_t port,
                               timeout_t timeout = std::chrono::seconds(60)) {
        pawn_row_t row;
        row.reserve(row_str.size());
        for (char c : row_str)
            row.push_back(c == '1');
        EXPECT_FALSE(row.empty());
        EXPECT_TRUE(row.front());
        EXPECT_TRUE(row.back());

        address_t addr{};
        addr.s_addr = htonl(INADDR_LOOPBACK);
        pawn_t max_pawn = static_cast<pawn_t>(row.size() - 1);

        pid_t pid = ::fork();
        if (pid < 0)
            throw std::runtime_error("fork()");

        if (pid == 0) {
            // Child: run the server forever.
            // Redirect stderr to /dev/null so the test log stays clean.
            int dn = ::open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                ::dup2(dn, 2);
                ::close(dn);
            }
            try {
                KaylesServer server(addr, port, timeout, max_pawn, std::move(row));
                server.start();
                server.run();
            } catch (...) {
                ::_exit(1);
            }
            ::_exit(0);
        }

        // Parent: wait briefly for the server to bind. The server binds after
        // fork, so we must poll. The receive timeout is per-attempt, not total.
        for (int i = 0; i < 50; ++i) {
            int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (probe < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            // Set recv timeout BEFORE sendto so it is in effect.
            timeval tv{0, 100000};  // 100 ms
            ::setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            target.sin_port = htons(port);
            uint8_t probe_byte = 0xFEu;  // unknown msg_type — triggers WRONG_MSG
            ssize_t sent = ::sendto(probe, &probe_byte, 1, 0, reinterpret_cast<sockaddr*>(&target),
                                    sizeof(target));
            if (sent < 0) {
                ::close(probe);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            uint8_t buf[32];
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            ssize_t n =
                ::recvfrom(probe, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            ::close(probe);
            if (n > 0 && from.sin_port == htons(port)) {
                ServerFixture f;
                f.pid = pid;
                f.port = port;
                return f;
            }
            // Check child process still alive.
            int status = 0;
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                // child exited
                ADD_FAILURE() << "Server child exited prematurely, status=" << status;
                ServerFixture f;
                return f;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        // Server didn't come up — tear down and fail.
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        ADD_FAILURE() << "Server did not come up on port " << port;
        ServerFixture f;
        return f;
    }

    // UDP client — send `payload` and recv up to 256 bytes with a 500ms timeout.
    // Returns the received bytes; empty vector on timeout.
    std::vector<uint8_t> udp_exchange(
        uint16_t port, std::span<const uint8_t> payload,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        EXPECT_GE(fd, 0);
        timeval tv{};
        tv.tv_sec = timeout.count() / 1000;
        tv.tv_usec = (timeout.count() % 1000) * 1000;
        EXPECT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(port);

        ssize_t sent = ::sendto(fd, payload.data(), payload.size(), 0,
                                reinterpret_cast<sockaddr*>(&target), sizeof(target));
        EXPECT_EQ(sent, static_cast<ssize_t>(payload.size()));

        std::vector<uint8_t> out(1024);
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t n =
            ::recvfrom(fd, out.data(), out.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        ::close(fd);
        if (n < 0)
            return {};
        out.resize(static_cast<size_t>(n));
        return out;
    }

    // Build a raw client message on the wire.
    std::vector<uint8_t> wire_join(uint32_t player_id) {
        std::vector<uint8_t> v;
        v.push_back(0);
        uint32_t n = htonl(player_id);
        auto* p = reinterpret_cast<const uint8_t*>(&n);
        v.insert(v.end(), p, p + 4);
        return v;
    }

    std::vector<uint8_t> wire_move(uint8_t msg_type, uint32_t player_id, uint32_t game_id,
                                   uint8_t pawn) {
        std::vector<uint8_t> v;
        v.push_back(msg_type);
        uint32_t np = htonl(player_id);
        auto* p1 = reinterpret_cast<const uint8_t*>(&np);
        v.insert(v.end(), p1, p1 + 4);
        uint32_t ng = htonl(game_id);
        auto* p2 = reinterpret_cast<const uint8_t*>(&ng);
        v.insert(v.end(), p2, p2 + 4);
        v.push_back(pawn);
        return v;
    }

    std::vector<uint8_t> wire_keep_give(uint8_t msg_type, uint32_t player_id, uint32_t game_id) {
        std::vector<uint8_t> v;
        v.push_back(msg_type);
        uint32_t np = htonl(player_id);
        auto* p1 = reinterpret_cast<const uint8_t*>(&np);
        v.insert(v.end(), p1, p1 + 4);
        uint32_t ng = htonl(game_id);
        auto* p2 = reinterpret_cast<const uint8_t*>(&ng);
        v.insert(v.end(), p2, p2 + 4);
        return v;
    }

    // Helper to decode MSG_GAME_STATE/MSG_WRONG_MSG discrimination by status byte.
    bool is_wrong(const std::vector<uint8_t>& v) {
        return v.size() == 14 && v[12] == MSG_WRONG_STATUS;
    }

}  // namespace

// ===========================================================================
// BringUp / basic lifecycle
// ===========================================================================

TEST(Integration, ServerBringUpAndUnknownTypeGetsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // msg_type 0xAB is unknown → MSG_WRONG_MSG with error_index 0, client_bytes[0]=0xAB.
    // Send also 13 more bytes so we reach 14 bytes; server truncates to 14 for the reply.
    std::vector<uint8_t> payload{0xAB, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u) << "MSG_WRONG_MSG must be exactly 14 bytes";
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    EXPECT_EQ(resp[13], 0u) << "error_index must point to the bad msg_type byte";
    // First 12 bytes must echo the client message.
    for (size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(resp[i], payload[i]) << "byte " << i << " must echo client bytes";
    }
}

TEST(Integration, JoinReturnsGameStateWithZeroIds) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    auto resp = udp_exchange(srv.port, wire_join(42));
    // MSG_GAME_STATE: 4 + 4 + 4 + 1 + 1 + 1 bitmap byte = 15 bytes for max_pawn=2.
    ASSERT_EQ(resp.size(), 15u);
    auto parsed = deserialize_game_state(resp);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().what();
    EXPECT_EQ(parsed->game_id, 0u);
    EXPECT_EQ(parsed->player_a_id, 42u);
    EXPECT_EQ(parsed->player_b_id, 0u);
    EXPECT_EQ(parsed->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(parsed->max_pawn, 2u);
    // bitmap: 0b11100000 = 0xE0 (3 pins, bits 0..2 set, excess zeroed).
    EXPECT_EQ(resp[14], 0xE0u);
}

TEST(Integration, JoinResponseNetworkByteOrderOnPlayerId) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // 0x11223344 must appear as 0x11,0x22,0x33,0x44 in bytes 4..7.
    auto resp = udp_exchange(srv.port, wire_join(0x11223344u));
    ASSERT_EQ(resp.size(), 15u);
    EXPECT_EQ(resp[4], 0x11u);
    EXPECT_EQ(resp[5], 0x22u);
    EXPECT_EQ(resp[6], 0x33u);
    EXPECT_EQ(resp[7], 0x44u);
}

TEST(Integration, SecondJoinPairsUp) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    (void)udp_exchange(srv.port, wire_join(7));
    auto resp = udp_exchange(srv.port, wire_join(8));
    ASSERT_EQ(resp.size(), 15u);
    auto parsed = deserialize_game_state(resp);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->player_a_id, 7u);
    EXPECT_EQ(parsed->player_b_id, 8u);
    EXPECT_EQ(parsed->status, GameStatus::TURN_B);
}

TEST(Integration, ThirdJoinCreatesSecondGame) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_join(3));
    ASSERT_EQ(resp.size(), 15u);
    auto parsed = deserialize_game_state(resp);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->game_id, 1u);
    EXPECT_EQ(parsed->player_a_id, 3u);
    EXPECT_EQ(parsed->player_b_id, 0u);
    EXPECT_EQ(parsed->status, GameStatus::WAITING_FOR_OPPONENT);
}

TEST(Integration, Move1LegalFlipsTurnToA) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("1111", port);  // max_pawn=3
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, 0));  // B knocks pawn 0
    // Header=14 + 1 bitmap byte (max_pawn=3 → 1 byte) = 15.
    ASSERT_EQ(resp.size(), 15u);
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_A);
    // bitmap: pawns 1,2,3 present → 0b01110000 = 0x70.
    EXPECT_EQ(resp[14], 0x70u);
}

TEST(Integration, Move2LegalKnocksTwoAdjacentPins) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("1111", port);
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // B move_2 at pawn=1 → knocks pawns 1 and 2.
    auto resp = udp_exchange(srv.port, wire_move(2, 2, 0, 1));
    ASSERT_EQ(resp.size(), 15u);
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_A);
    // pawns 0,3 remain → 0b10010000 = 0x90.
    EXPECT_EQ(resp[14], 0x90u);
}

TEST(Integration, WinningMoveProducesWinStatus) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("1", port);  // max_pawn=0, only pawn 0
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, 0));  // B knocks last pawn
    ASSERT_EQ(resp.size(), 15u);
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::WIN_B) << "Last pin knocked by B must yield WIN_B.";
    // bitmap 0x00 (pawn 0 cleared).
    EXPECT_EQ(resp[14], 0x00u);
}

// ===========================================================================
// Illegal moves still receive MSG_GAME_STATE (spec 3.3)
// ===========================================================================

TEST(Integration, IllegalMovePawnOutOfRangeStillGetsGameState) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);  // max_pawn=2
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // pawn=42 is out of range; message is VALID (3.3 para 2), move is illegal.
    auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, 42));
    ASSERT_EQ(resp.size(), 15u) << "Illegal move must still be MSG_GAME_STATE";
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_B);  // unchanged
    EXPECT_EQ(resp[14], 0xE0u);                // unchanged bitmap
}

TEST(Integration, IllegalMoveWrongTurnStillGetsGameState) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // B's turn — A moves → illegal, no state change.
    auto resp = udp_exchange(srv.port, wire_move(1, 1, 0, 0));
    ASSERT_EQ(resp.size(), 15u);
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_B);
    EXPECT_EQ(resp[14], 0xE0u);
}

TEST(Integration, GiveUpByNonMemberReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // Player 99 is not in game 0.
    auto payload = wire_keep_give(4, 99, 0);
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    // Updated spec: a nonzero player_id is never "invalid" — a player-in-game
    // mismatch is attributed to the game_id field (byte 5).
    EXPECT_EQ(resp[13], 5u);
    // Echo of 9-byte payload; bytes 9..11 must be zero-padded.
    for (size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(resp[i], payload[i]);
    }
    for (size_t i = payload.size(); i < 12; ++i) {
        EXPECT_EQ(resp[i], 0u) << "tail must be zero-padded at " << i;
    }
}

TEST(Integration, MoveOnMissingGameIdReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // No JOINs first → game_id=0 doesn't exist.
    auto payload = wire_move(1, 1, 999, 0);
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    // error_index should point at the game_id field (byte 5).
    EXPECT_EQ(resp[13], 5u);
    for (size_t i = 0; i < payload.size(); ++i)
        EXPECT_EQ(resp[i], payload[i]);
}

// ===========================================================================
// Malformed inputs
// ===========================================================================

TEST(Integration, EmptyDatagramReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    std::vector<uint8_t> payload;
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    // All 12 echo bytes should be zero.
    for (size_t i = 0; i < 12; ++i)
        EXPECT_EQ(resp[i], 0u);
    EXPECT_EQ(resp[13], 0u);
}

TEST(Integration, TruncatedJoinReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // MSG_JOIN expects 5 bytes; send only 4.
    std::vector<uint8_t> payload{0, 1, 2, 3};
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    for (size_t i = 0; i < 4; ++i)
        EXPECT_EQ(resp[i], payload[i]);
    for (size_t i = 4; i < 12; ++i)
        EXPECT_EQ(resp[i], 0u);
}

TEST(Integration, TruncatedMove1ReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // MSG_MOVE_1 expects 10 bytes; send 9.
    std::vector<uint8_t> payload(9, 0);
    payload[0] = 1;  // msg_type
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
}

TEST(Integration, OverlongMove1ReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // MSG_MOVE_1 expects 10 bytes. Send 11. Server reads up to
    // CLIENT_MESSAGE_SIZE_WITH_BUF=12, enough to detect the overlong case.
    std::vector<uint8_t> payload(11, 0);
    payload[0] = 1;
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
}

TEST(Integration, UnknownMsgType5ReturnsWrongMsgErrorIndex0) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    std::vector<uint8_t> payload(5, 0);
    payload[0] = 5;  // unknown msg_type
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    EXPECT_EQ(resp[13], 0u);
    EXPECT_EQ(resp[0], 5u);
}

TEST(Integration, UnknownMsgType255ReturnsWrongMsg) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    std::vector<uint8_t> payload(5, 0);
    payload[0] = 0xFF;
    auto resp = udp_exchange(srv.port, payload);
    ASSERT_EQ(resp.size(), 14u);
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    EXPECT_EQ(resp[13], 0u);
    EXPECT_EQ(resp[0], 0xFFu);
}

// ===========================================================================
// Bitmap layout on the wire
// ===========================================================================

TEST(Integration, BitmapMaxPawn0IsSingleByte0x80) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("1", port);  // max_pawn=0
    auto resp = udp_exchange(srv.port, wire_join(1));
    ASSERT_EQ(resp.size(), 15u);
    EXPECT_EQ(resp[14], 0x80u) << "pawn 0 = MSB of byte 0";
}

TEST(Integration, BitmapMaxPawn7AllOnesIs0xFF) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("11111111", port);  // max_pawn=7
    auto resp = udp_exchange(srv.port, wire_join(1));
    // Header 14 + 1 byte.
    ASSERT_EQ(resp.size(), 15u);
    EXPECT_EQ(resp[14], 0xFFu);
}

TEST(Integration, BitmapMaxPawn8TwoBytesExcessZeroed) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111111111", port);  // 9 pawns → max_pawn=8
    auto resp = udp_exchange(srv.port, wire_join(1));
    // Header 14 + 2 bytes = 16.
    ASSERT_EQ(resp.size(), 16u);
    // byte14 = 0xFF, byte15 has only pawn 8 set (MSB) with excess bits zero = 0x80.
    EXPECT_EQ(resp[14], 0xFFu);
    EXPECT_EQ(resp[15], 0x80u);
}

TEST(Integration, BitmapAfterMove2HasBothBitsClearedInSameByte) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("11111111", port);  // max_pawn=7
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // B move_2 at pawn=3 knocks pawns 3 and 4. Remaining: 0,1,2,5,6,7
    // bits:   1 1 1 0 0 1 1 1 = 0xE7.
    auto resp = udp_exchange(srv.port, wire_move(2, 2, 0, 3));
    ASSERT_EQ(resp.size(), 15u);
    EXPECT_EQ(resp[14], 0xE7u);
}

TEST(Integration, BitmapMaxPawn255AllOnesCompletedLayout) {
    auto port = pick_ephemeral_port();
    std::string row(256, '1');
    auto srv = spawn_server(row, port);  // max_pawn=255
    auto resp = udp_exchange(srv.port, wire_join(1));
    // 14 header + 32 bitmap bytes.
    ASSERT_EQ(resp.size(), 46u);
    for (size_t i = 14; i < 46; ++i) {
        EXPECT_EQ(resp[i], 0xFFu) << "byte " << i;
    }
}

// ===========================================================================
// Reply destination: server must reply to (sender_ip, sender_port)
// ===========================================================================

TEST(Integration, ServerRepliesToSenderNotElsewhere) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    // Bind an explicit source port so we can verify the destination.
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    src.sin_port = 0;
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)), 0);
    socklen_t sl = sizeof(src);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&src), &sl);
    uint16_t my_port = ntohs(src.sin_port);
    timeval tv{0, 500000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(srv.port);

    auto payload = wire_join(7);
    ::sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&target),
             sizeof(target));
    uint8_t buf[64];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    ASSERT_GT(n, 0);
    EXPECT_EQ(ntohs(from.sin_port), srv.port) << "Reply must come from the server's listening port";
    ::close(fd);
    (void)my_port;  // only used for introspection
}

// ===========================================================================
// KeepAlive path
// ===========================================================================

TEST(Integration, KeepAliveReturnsCurrentState) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_keep_give(3, 1, 0));
    ASSERT_EQ(resp.size(), 15u);
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_B);
    EXPECT_EQ(p->player_a_id, 1u);
    EXPECT_EQ(p->player_b_id, 2u);
}

TEST(Integration, KeepAliveByPlayerZeroIsNotValid) {
    // Spec: player_id must be nonzero. player_id=0 on the wire is an invalid
    // message and must receive MSG_WRONG_MSG, not a MSG_GAME_STATE.
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    (void)udp_exchange(srv.port, wire_join(1));
    auto payload = wire_keep_give(3, 0, 0);  // player_id=0
    auto resp = udp_exchange(srv.port, payload);
    // This is the brutal part: if the server accepts player_id=0 and returns a
    // MSG_GAME_STATE (because player_b_id=0 in the waiting game), that is a bug.
    ASSERT_FALSE(resp.empty()) << "Server must reply something";
    EXPECT_TRUE(is_wrong(resp))
        << "player_id=0 must be rejected per spec 3.2 — got MSG_GAME_STATE instead";
}

TEST(Integration, JoinByPlayerZeroIsRejected) {
    // Spec 3.3: "Komunikat MSG_JOIN jest poprawny, jeśli (...) zawiera
    // niezerową wartość w polu player_id." Zero player_id must get WRONG_MSG.
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111", port);
    auto payload = wire_join(0);
    auto resp = udp_exchange(srv.port, payload);
    EXPECT_TRUE(is_wrong(resp)) << "MSG_JOIN with player_id=0 must yield MSG_WRONG_MSG";
}

// ===========================================================================
// GameState pawn_row ordering vs bitmap — round-trip bit positions
// ===========================================================================

TEST(Integration, MoveAtPawn8TouchesCorrectBit) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111111111", port);  // max_pawn=8
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // B knocks pawn 8 (MSB of byte1). Remaining: pawns 0..7.
    auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, 8));
    ASSERT_EQ(resp.size(), 16u);
    // byte14 = 0xFF (all of 0..7 present), byte15 = 0x00 (pawn 8 gone, excess 0).
    EXPECT_EQ(resp[14], 0xFFu);
    EXPECT_EQ(resp[15], 0x00u);
}

TEST(Integration, MoveAtPawn7TouchesLSBOfByte0) {
    auto port = pick_ephemeral_port();
    auto srv = spawn_server("111111111", port);  // max_pawn=8
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, 7));
    ASSERT_EQ(resp.size(), 16u);
    // pawns 0..6 and 8 present.
    // byte14 = 0b11111110 = 0xFE. byte15 = 0x80 (only pawn 8).
    EXPECT_EQ(resp[14], 0xFEu);
    EXPECT_EQ(resp[15], 0x80u);
}
