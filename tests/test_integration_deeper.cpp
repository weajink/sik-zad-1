// Deeper integration tests with an RAII server fixture that does NOT have the
// copy-constructor bug (c.f. test_integration.cpp's ServerFixture which, when
// returned by value without NRVO, destroys the original and kills the child).
//
// These tests boot a real KaylesServer on 127.0.0.1 with an ephemeral port and
// hammer it with nasty input to verify spec-level behavior end-to-end.
//
// Coverage focus (gaps vs. test_integration.cpp):
//   - player_id = 0 on every message type (JOIN/MOVE_1/MOVE_2/KEEP_ALIVE/GIVE_UP).
//   - MSG_WRONG_MSG error_index exactness for every error category.
//   - Server doesn't resurrect finished games on JOIN.
//   - Game_id=MAX-valued requests return MSG_WRONG_MSG (INVALID_GAME_ID).
//   - Overlong datagrams (>12 bytes) still get a proper WRONG_MSG reply.
//   - Exactly 11-byte MOVE_1/MOVE_2 overrun detection.
//   - Truncated KEEP_ALIVE and GIVE_UP.
//   - Bitmap after a sequence of moves preserves MSB ordering for every
//     single pawn knock across the whole range.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

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

namespace {

// RAII-safe server fixture: move-only, default destruction only happens once.
// Returning by value uses an explicit std::move; any copy attempt is a compile
// error, which makes it IMPOSSIBLE for a copy/destroy dance to kill the server
// (the bug in the original test_integration.cpp fixture).
struct ServerHandle {
    pid_t pid = -1;
    uint16_t port = 0;

    ServerHandle() = default;
    ServerHandle(const ServerHandle&) = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;
    ServerHandle(ServerHandle&& o) noexcept : pid(o.pid), port(o.port) { o.pid = -1; }
    ServerHandle& operator=(ServerHandle&& o) noexcept {
        if (this != &o) {
            shutdown();
            pid = o.pid;
            port = o.port;
            o.pid = -1;
        }
        return *this;
    }
    ~ServerHandle() { shutdown(); }

    void shutdown() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};

static uint16_t pick_ephemeral_port() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t l = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    ::close(fd);
    return ntohs(a.sin_port);
}

static ServerHandle spawn_server(const std::string& row_str,
                                 timeout_t timeout = std::chrono::seconds(60)) {
    uint16_t port = pick_ephemeral_port();
    pawn_row_t row;
    for (char c : row_str) row.push_back(c == '1');
    address_t addr{};
    addr.s_addr = htonl(INADDR_LOOPBACK);
    pawn_t max_pawn = static_cast<pawn_t>(row.size() - 1);

    pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork()");
    if (pid == 0) {
        int dn = ::open("/dev/null", O_WRONLY);
        if (dn >= 0) { ::dup2(dn, 2); ::close(dn); }
        try {
            KaylesServer server(addr, port, timeout, max_pawn, std::move(row));
            server.start();
            server.run();
        } catch (...) { ::_exit(1); }
        ::_exit(0);
    }

    // Poll until server accepts a probe.
    for (int i = 0; i < 80; ++i) {
        int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
        timeval tv{0, 100000};
        ::setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(port);
        uint8_t probe_byte = 0xFEu;
        ::sendto(probe, &probe_byte, 1, 0, reinterpret_cast<sockaddr*>(&target),
                 sizeof(target));
        uint8_t buf[32];
        ssize_t n = ::recvfrom(probe, buf, sizeof(buf), 0, nullptr, nullptr);
        ::close(probe);
        if (n > 0) {
            ServerHandle h;
            h.pid = pid;
            h.port = port;
            return h;
        }
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            ADD_FAILURE() << "server child exited during spawn";
            ServerHandle h;
            h.pid = -1;
            return h;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
    ADD_FAILURE() << "server did not come up on port " << port;
    ServerHandle h;
    return h;
}

// Send one datagram, receive one reply. Empty vector on timeout.
static std::vector<uint8_t>
udp_exchange(uint16_t port, std::span<const uint8_t> payload,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(600)) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT_GE(fd, 0);
    timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(port);
    ::sendto(fd, payload.data(), payload.size(), 0,
             reinterpret_cast<sockaddr*>(&target), sizeof(target));
    std::vector<uint8_t> out(1024);
    ssize_t n = ::recvfrom(fd, out.data(), out.size(), 0, nullptr, nullptr);
    ::close(fd);
    if (n < 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

static std::vector<uint8_t> wire_join(uint32_t pid) {
    std::vector<uint8_t> v;
    v.push_back(0);
    uint32_t n = htonl(pid);
    auto* p = reinterpret_cast<const uint8_t*>(&n);
    v.insert(v.end(), p, p + 4);
    return v;
}

static std::vector<uint8_t> wire_move(uint8_t mt, uint32_t pid, uint32_t gid,
                                      uint8_t pawn) {
    std::vector<uint8_t> v;
    v.push_back(mt);
    uint32_t np = htonl(pid), ng = htonl(gid);
    auto* p1 = reinterpret_cast<const uint8_t*>(&np);
    v.insert(v.end(), p1, p1 + 4);
    auto* p2 = reinterpret_cast<const uint8_t*>(&ng);
    v.insert(v.end(), p2, p2 + 4);
    v.push_back(pawn);
    return v;
}

static std::vector<uint8_t> wire_ka_giveup(uint8_t mt, uint32_t pid, uint32_t gid) {
    std::vector<uint8_t> v;
    v.push_back(mt);
    uint32_t np = htonl(pid), ng = htonl(gid);
    auto* p1 = reinterpret_cast<const uint8_t*>(&np);
    v.insert(v.end(), p1, p1 + 4);
    auto* p2 = reinterpret_cast<const uint8_t*>(&ng);
    v.insert(v.end(), p2, p2 + 4);
    return v;
}

static bool is_wrong(const std::vector<uint8_t>& v) {
    return v.size() == 14 && v[12] == MSG_WRONG_STATUS;
}

static bool is_game_state(const std::vector<uint8_t>& v) {
    return v.size() >= 15 && v[12] != MSG_WRONG_STATUS;
}

}  // namespace

// ===========================================================================
// Test: server is actually alive (sanity)
// ===========================================================================

TEST(IntegrationDeeper, ServerBootsAndRespondsToJoin) {
    auto srv = spawn_server("111");
    ASSERT_GT(srv.pid, 0);
    auto resp = udp_exchange(srv.port, wire_join(1));
    ASSERT_TRUE(is_game_state(resp)) << "join must yield game state";
}

// ===========================================================================
// Player_id = 0 on every message type — all must be rejected with WRONG_MSG
// whose error_index points at the player_id field (byte 1 of wire).
// ===========================================================================

TEST(IntegrationDeeper, JoinWithPlayerIdZeroYieldsWrongMsg) {
    auto srv = spawn_server("111");
    auto p = wire_join(0);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    // error_index should point at start of player_id (byte 1).
    EXPECT_EQ(resp[13], 1u) << "error_index must be start of player_id field";
    // First 5 bytes echoed, 5..11 zero.
    for (size_t i = 0; i < 5; ++i) EXPECT_EQ(resp[i], p[i]);
    for (size_t i = 5; i < 12; ++i) EXPECT_EQ(resp[i], 0u);
}

TEST(IntegrationDeeper, Move1WithPlayerIdZeroYieldsWrongMsg) {
    auto srv = spawn_server("111");
    auto p = wire_move(1, 0, 0, 0);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, Move2WithPlayerIdZeroYieldsWrongMsg) {
    auto srv = spawn_server("111");
    auto p = wire_move(2, 0, 0, 0);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, KeepAliveWithPlayerIdZeroYieldsWrongMsg) {
    auto srv = spawn_server("111");
    auto p = wire_ka_giveup(3, 0, 0);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, GiveUpWithPlayerIdZeroYieldsWrongMsg) {
    auto srv = spawn_server("111");
    auto p = wire_ka_giveup(4, 0, 0);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

// ===========================================================================
// Unknown game_id via MOVE/KEEP_ALIVE/GIVE_UP gets WRONG_MSG
// ===========================================================================

TEST(IntegrationDeeper, KeepAliveUnknownGameIdGetsWrongMsgWithGameIdErrorIndex) {
    auto srv = spawn_server("111");
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    // Use a nonexistent game_id.
    auto p = wire_ka_giveup(3, /*pid=*/1, /*gid=*/0xDEADBEEF);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
    // error_index should be byte 5 (start of game_id).
    EXPECT_EQ(resp[13], 5u);
    for (size_t i = 0; i < 9; ++i) EXPECT_EQ(resp[i], p[i]);
}

TEST(IntegrationDeeper, GiveUpUnknownGameIdByMemberOfOtherGameGetsWrongMsg) {
    auto srv = spawn_server("111");
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto p = wire_ka_giveup(4, /*pid=*/1, /*gid=*/999u);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
    EXPECT_EQ(resp[13], 5u);
}

TEST(IntegrationDeeper, Move1ByNonMemberOfExistingGameGetsWrongMsg) {
    auto srv = spawn_server("111");
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto p = wire_move(1, /*pid=*/99, /*gid=*/0, 0);
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
    // error_index should be byte 1 (start of player_id).
    EXPECT_EQ(resp[13], 1u);
}

// ===========================================================================
// Truncated messages — each truncation must yield WRONG_MSG
// ===========================================================================

TEST(IntegrationDeeper, TruncatedKeepAliveGetsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(8, 0);  // 8 bytes, KA expects 9
    p[0] = 3;
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, TruncatedGiveUpGetsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(8, 0);
    p[0] = 4;
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, OverlongKeepAliveGetsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(10, 0);
    p[0] = 3;
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, OverlongGiveUpGetsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(10, 0);
    p[0] = 4;
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, OverlongMove1ElevenBytesGetsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(11, 0);
    p[0] = 1;
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

TEST(IntegrationDeeper, OverlongMove2ElevenBytesGetsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(11, 0);
    p[0] = 2;
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

// ===========================================================================
// Oversized datagrams (> 12 bytes) still yield WRONG_MSG (never silently drop)
// ===========================================================================

TEST(IntegrationDeeper, VeryLargeDatagramStillYieldsWrongMsg) {
    auto srv = spawn_server("111");
    std::vector<uint8_t> p(100, 0xCCu);
    p[0] = 1;  // MOVE_1 but 100 bytes
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
}

// ===========================================================================
// Finished games are NOT resurrected on JOIN
// ===========================================================================

TEST(IntegrationDeeper, JoinAfterGiveUpCreatesNewGameIdNotReopening) {
    auto srv = spawn_server("111");
    (void)udp_exchange(srv.port, wire_join(1));
    auto r2 = udp_exchange(srv.port, wire_join(2));
    ASSERT_TRUE(is_game_state(r2));
    // B gives up → WIN_A.
    auto gu = udp_exchange(srv.port, wire_ka_giveup(4, 2, 0));
    ASSERT_TRUE(is_game_state(gu));
    auto parsed_gu = deserialize_game_state(gu);
    ASSERT_TRUE(parsed_gu.has_value());
    EXPECT_EQ(parsed_gu->status, GameStatus::WIN_A);
    // Now a new JOIN: must NOT become B of game 0; must create game 1.
    auto j3 = udp_exchange(srv.port, wire_join(3));
    ASSERT_TRUE(is_game_state(j3));
    auto parsed = deserialize_game_state(j3);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->game_id, 1u);
    EXPECT_EQ(parsed->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(parsed->player_a_id, 3u);
    EXPECT_EQ(parsed->player_b_id, 0u);
}

// ===========================================================================
// Bitmap ordering — every single-pawn knock preserves MSB-first layout
// ===========================================================================

TEST(IntegrationDeeper, EveryPawnKnockAffectsOnlyItsBit) {
    // For a new server per pawn value k: max_pawn = 15 (2 bitmap bytes),
    // B knocks pawn k, verify that exactly bit k went to 0.
    for (int k = 0; k <= 15; ++k) {
        auto srv = spawn_server("1111111111111111");  // 16 pins
        (void)udp_exchange(srv.port, wire_join(1));
        (void)udp_exchange(srv.port, wire_join(2));  // TURN_B
        auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, static_cast<uint8_t>(k)));
        ASSERT_EQ(resp.size(), 14u + 2u) << "k=" << k;
        auto p = deserialize_game_state(resp);
        ASSERT_TRUE(p.has_value()) << "k=" << k;
        for (int i = 0; i <= 15; ++i) {
            bool expected = (i != k);
            ASSERT_EQ(p->pawn_row[i], expected)
                << "after knock of " << k << ", pin " << i
                << " must be " << expected;
        }
        // Byte-exact check.
        uint8_t expected_byte0 = 0xFFu, expected_byte1 = 0xFFu;
        if (k < 8) expected_byte0 &= static_cast<uint8_t>(~(1u << (7 - k)));
        else       expected_byte1 &= static_cast<uint8_t>(~(1u << (7 - (k - 8))));
        EXPECT_EQ(resp[14], expected_byte0) << "k=" << k;
        EXPECT_EQ(resp[15], expected_byte1) << "k=" << k;
    }
}

// ===========================================================================
// Server reply port == server listening port (spec 3.3 last sentence).
// ===========================================================================

TEST(IntegrationDeeper, ReplySourcePortEqualsServerListenPort) {
    auto srv = spawn_server("111");
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    src.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src));
    timeval tv{0, 500000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(srv.port);
    auto p = wire_join(42);
    ::sendto(fd, p.data(), p.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    uint8_t buf[128];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    ::close(fd);
    ASSERT_GT(n, 0);
    EXPECT_EQ(ntohs(from.sin_port), srv.port);
    EXPECT_EQ(from.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
}

// ===========================================================================
// End-to-end: full 4-pin game, alternating moves, must end in correct winner.
// ===========================================================================

TEST(IntegrationDeeper, FourPinAlternatingMove1EndsInWinA) {
    auto srv = spawn_server("1111");  // max_pawn=3
    auto j1 = udp_exchange(srv.port, wire_join(1));
    ASSERT_TRUE(is_game_state(j1));
    auto j2 = udp_exchange(srv.port, wire_join(2));
    ASSERT_TRUE(is_game_state(j2));
    // B, A, B, A — even count, A wins.
    auto r1 = udp_exchange(srv.port, wire_move(1, 2, 0, 0));  // B knocks 0
    ASSERT_TRUE(is_game_state(r1));
    auto r2 = udp_exchange(srv.port, wire_move(1, 1, 0, 1));  // A knocks 1
    ASSERT_TRUE(is_game_state(r2));
    auto r3 = udp_exchange(srv.port, wire_move(1, 2, 0, 2));  // B knocks 2
    ASSERT_TRUE(is_game_state(r3));
    auto r4 = udp_exchange(srv.port, wire_move(1, 1, 0, 3));  // A knocks 3
    ASSERT_TRUE(is_game_state(r4));
    auto parsed = deserialize_game_state(r4);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->status, GameStatus::WIN_A);
    // All pins knocked.
    EXPECT_EQ(r4[14], 0x00u);
}

// ===========================================================================
// Move by A on B's turn is VALID (returns MSG_GAME_STATE) but state unchanged.
// ===========================================================================

TEST(IntegrationDeeper, MoveByAOnBTurnReturnsGameStateUnchanged) {
    auto srv = spawn_server("1111");
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));  // TURN_B
    // A tries to move — not their turn.
    auto resp = udp_exchange(srv.port, wire_move(1, 1, 0, 0));
    ASSERT_TRUE(is_game_state(resp));
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_B);
    // Row must be unchanged (all pins up).
    for (int i = 0; i <= 3; ++i) EXPECT_TRUE(p->pawn_row[i]);
}

// ===========================================================================
// Give-up by opposite-turn player — valid message, no state change.
// ===========================================================================

TEST(IntegrationDeeper, GiveUpByAOnBTurnReturnsGameStateUnchanged) {
    auto srv = spawn_server("1111");
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_ka_giveup(4, 1, 0));
    ASSERT_TRUE(is_game_state(resp));
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::TURN_B)
        << "give-up by non-on-turn player must leave game unchanged";
}

// ===========================================================================
// Echo correctness: the first 12 client bytes must be verbatim in the reply.
// ===========================================================================

TEST(IntegrationDeeper, WrongMsgEchoesClientBytesVerbatim12Bytes) {
    auto srv = spawn_server("111");
    // Send a full 12-byte datagram with distinct bytes.
    std::vector<uint8_t> p(12, 0);
    for (size_t i = 0; i < 12; ++i) p[i] = static_cast<uint8_t>(0x10u + i);
    // msg_type = 0x10 is invalid → error_index 0.
    auto resp = udp_exchange(srv.port, p);
    ASSERT_TRUE(is_wrong(resp));
    for (size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(resp[i], static_cast<uint8_t>(0x10u + i))
            << "echo byte " << i;
    }
    EXPECT_EQ(resp[12], MSG_WRONG_STATUS);
    EXPECT_EQ(resp[13], 0u);
}

// ===========================================================================
// Bitmap carrying pin 255 (the very last bit of the bitmap) in max_pawn=255
// ===========================================================================

TEST(IntegrationDeeper, BitmapPin255KnockZeroLastBitOfLastByte) {
    std::string row(256, '1');
    auto srv = spawn_server(row);
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto resp = udp_exchange(srv.port, wire_move(1, 2, 0, 255));  // knock 255
    ASSERT_EQ(resp.size(), 14u + 32u);
    // Byte 14..44 all 0xFF except last: bit 0 (LSB) of byte 45 cleared.
    for (size_t i = 14; i < 14 + 31; ++i) EXPECT_EQ(resp[i], 0xFFu);
    EXPECT_EQ(resp[14 + 31], 0xFEu) << "last bit of last byte is pin 255";
}

// ===========================================================================
// The second-game waiting invariant: third JOIN yields game 1 waiting,
// fourth JOIN pairs up, fifth yields game 2 waiting, etc.
// ===========================================================================

TEST(IntegrationDeeper, WaitingInvariantAcrossFiveJoins) {
    auto srv = spawn_server("111");
    // 5 joins → games 0 (paired), 1 (paired), 2 (waiting).
    auto r1 = udp_exchange(srv.port, wire_join(1));
    auto r2 = udp_exchange(srv.port, wire_join(2));
    auto r3 = udp_exchange(srv.port, wire_join(3));
    auto r4 = udp_exchange(srv.port, wire_join(4));
    auto r5 = udp_exchange(srv.port, wire_join(5));
    for (auto& r : {std::ref(r1), std::ref(r2), std::ref(r3), std::ref(r4),
                    std::ref(r5)}) {
        ASSERT_TRUE(is_game_state(r.get()));
    }
    auto p1 = deserialize_game_state(r1); ASSERT_TRUE(p1.has_value());
    auto p2 = deserialize_game_state(r2); ASSERT_TRUE(p2.has_value());
    auto p3 = deserialize_game_state(r3); ASSERT_TRUE(p3.has_value());
    auto p4 = deserialize_game_state(r4); ASSERT_TRUE(p4.has_value());
    auto p5 = deserialize_game_state(r5); ASSERT_TRUE(p5.has_value());

    EXPECT_EQ(p1->game_id, 0u); EXPECT_EQ(p1->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(p2->game_id, 0u); EXPECT_EQ(p2->status, GameStatus::TURN_B);
    EXPECT_EQ(p3->game_id, 1u); EXPECT_EQ(p3->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(p4->game_id, 1u); EXPECT_EQ(p4->status, GameStatus::TURN_B);
    EXPECT_EQ(p5->game_id, 2u); EXPECT_EQ(p5->status, GameStatus::WAITING_FOR_OPPONENT);
}

// ===========================================================================
// After a won game, MOVE by either player still yields MSG_GAME_STATE with
// the WIN status; state must be unchanged.
// ===========================================================================

TEST(IntegrationDeeper, MoveOnFinishedGameReturnsGameStateUnchanged) {
    auto srv = spawn_server("1");  // 1 pin, one move wins
    (void)udp_exchange(srv.port, wire_join(1));
    (void)udp_exchange(srv.port, wire_join(2));
    auto win = udp_exchange(srv.port, wire_move(1, 2, 0, 0));
    auto parsed = deserialize_game_state(win);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->status, GameStatus::WIN_B);
    auto resp = udp_exchange(srv.port, wire_move(1, 1, 0, 0));
    ASSERT_TRUE(is_game_state(resp));
    auto p = deserialize_game_state(resp);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, GameStatus::WIN_B) << "finished game must stay finished";
    EXPECT_EQ(resp[14], 0x00u) << "row stays empty";
}
