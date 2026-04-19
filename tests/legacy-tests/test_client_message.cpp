#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <kayles_server.h>

#include <cstring>

using namespace kayles_common;
using namespace kayles_server;

// Helper: build a valid MSG_JOIN buffer (msg_type=0, player_id in network order).
// Total size: 1 (type) + 4 (player_id) = 5 bytes.
static std::vector<char> make_join(uint32_t player_id) {
    std::vector<char> buf(5);
    buf[0] = 0;  // MSG_JOIN
    uint32_t pid_n = htonl(player_id);
    std::memcpy(buf.data() + 1, &pid_n, 4);
    return buf;
}

// Helper: build a valid MSG_MOVE_1 buffer.
// Size: 1 (type) + 4 (player_id) + 4 (game_id) + 1 (pawn) = 10 bytes.
static std::vector<char> make_move1(uint32_t player_id, uint32_t game_id, uint8_t pawn) {
    std::vector<char> buf(10);
    buf[0] = 1;  // MSG_MOVE_1
    uint32_t pid_n = htonl(player_id);
    std::memcpy(buf.data() + 1, &pid_n, 4);
    uint32_t gid_n = htonl(game_id);
    std::memcpy(buf.data() + 5, &gid_n, 4);
    buf[9] = static_cast<char>(pawn);
    return buf;
}

// Helper: build a valid MSG_MOVE_2 buffer.
static std::vector<char> make_move2(uint32_t player_id, uint32_t game_id, uint8_t pawn) {
    std::vector<char> buf(10);
    buf[0] = 2;  // MSG_MOVE_2
    uint32_t pid_n = htonl(player_id);
    std::memcpy(buf.data() + 1, &pid_n, 4);
    uint32_t gid_n = htonl(game_id);
    std::memcpy(buf.data() + 5, &gid_n, 4);
    buf[9] = static_cast<char>(pawn);
    return buf;
}

// Helper: build a valid MSG_KEEP_ALIVE buffer.
// Size: 1 (type) + 4 (player_id) + 4 (game_id) = 9 bytes.
static std::vector<char> make_keep_alive(uint32_t player_id, uint32_t game_id) {
    std::vector<char> buf(9);
    buf[0] = 3;  // MSG_KEEP_ALIVE
    uint32_t pid_n = htonl(player_id);
    std::memcpy(buf.data() + 1, &pid_n, 4);
    uint32_t gid_n = htonl(game_id);
    std::memcpy(buf.data() + 5, &gid_n, 4);
    return buf;
}

// Helper: build a valid MSG_GIVE_UP buffer.
static std::vector<char> make_give_up(uint32_t player_id, uint32_t game_id) {
    std::vector<char> buf(9);
    buf[0] = 4;  // MSG_GIVE_UP
    uint32_t pid_n = htonl(player_id);
    std::memcpy(buf.data() + 1, &pid_n, 4);
    uint32_t gid_n = htonl(game_id);
    std::memcpy(buf.data() + 5, &gid_n, 4);
    return buf;
}

// ==================== Valid messages ====================

TEST(ClientMessage, ValidJoin) {
    auto buf = make_join(42);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_JOIN);
    EXPECT_EQ(result->player_id, 42u);
}

TEST(ClientMessage, ValidJoinLargePlayerId) {
    auto buf = make_join(0xDEADBEEF);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_JOIN);
    EXPECT_EQ(result->player_id, 0xDEADBEEFu);
}

TEST(ClientMessage, ValidJoinPlayerId1) {
    auto buf = make_join(1);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->player_id, 1u);
}

TEST(ClientMessage, ValidMove1) {
    auto buf = make_move1(10, 20, 5);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_MOVE_1);
    EXPECT_EQ(result->player_id, 10u);
    EXPECT_EQ(result->game_id, 20u);
    EXPECT_EQ(result->pawn, 5);
}

TEST(ClientMessage, ValidMove2) {
    auto buf = make_move2(100, 200, 255);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_MOVE_2);
    EXPECT_EQ(result->player_id, 100u);
    EXPECT_EQ(result->game_id, 200u);
    EXPECT_EQ(result->pawn, 255);
}

TEST(ClientMessage, ValidKeepAlive) {
    auto buf = make_keep_alive(7, 99);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_KEEP_ALIVE);
    EXPECT_EQ(result->player_id, 7u);
    EXPECT_EQ(result->game_id, 99u);
}

TEST(ClientMessage, ValidGiveUp) {
    auto buf = make_give_up(3, 0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_GIVE_UP);
    EXPECT_EQ(result->player_id, 3u);
    EXPECT_EQ(result->game_id, 0u);
}

// ==================== Network byte order ====================

TEST(ClientMessage, NetworkByteOrderPlayerIdJoin) {
    // Verify that multi-byte fields are correctly parsed from network byte order.
    // player_id = 0x01020304, which in network order is bytes 01 02 03 04.
    auto buf = make_join(0x01020304);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->player_id, 0x01020304u);
}

TEST(ClientMessage, NetworkByteOrderGameIdMove) {
    auto buf = make_move1(1, 0x0A0B0C0D, 0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->game_id, 0x0A0B0C0Du);
}

// ==================== Invalid messages ====================

TEST(ClientMessage, EmptyBuffer) {
    auto result = get_message_from_buffer(nullptr, 0);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, InvalidMsgType) {
    char buf[10] = {};
    buf[0] = 5;  // Invalid: max valid is 4
    auto result = get_message_from_buffer(buf, 10);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 0);  // error at offset 0 (msg_type)
}

TEST(ClientMessage, InvalidMsgType255) {
    char buf[10] = {};
    buf[0] = static_cast<char>(255);
    auto result = get_message_from_buffer(buf, 10);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, TruncatedJoinNoPlayerId) {
    // MSG_JOIN with only the type byte, no player_id
    char buf[1] = {0};
    auto result = get_message_from_buffer(buf, 1);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, TruncatedMoveNoGameId) {
    // MSG_MOVE_1 with type + player_id but no game_id or pawn
    char buf[5] = {};
    buf[0] = 1;  // MSG_MOVE_1
    auto result = get_message_from_buffer(buf, 5);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, TruncatedMoveNoPawn) {
    // MSG_MOVE_1 with type + player_id + game_id but no pawn byte
    char buf[9] = {};
    buf[0] = 1;  // MSG_MOVE_1
    auto result = get_message_from_buffer(buf, 9);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, JoinWithExtraBytes) {
    // MSG_JOIN is 5 bytes. Sending 6 should be rejected (offset != len).
    auto buf = make_join(1);
    buf.push_back(0);  // extra byte
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, KeepAliveWithExtraBytes) {
    auto buf = make_keep_alive(1, 1);
    buf.push_back(0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, MoveWithExtraBytes) {
    auto buf = make_move1(1, 1, 0);
    buf.push_back(0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, GiveUpTruncated) {
    // MSG_GIVE_UP needs 9 bytes. Send only 7.
    char buf[7] = {};
    buf[0] = 4;  // MSG_GIVE_UP
    auto result = get_message_from_buffer(buf, 7);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, SingleByteMsgType0) {
    // Just msg_type=0 (JOIN) with no player_id -> truncated
    char buf[1] = {0};
    auto result = get_message_from_buffer(buf, 1);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, ZeroPlayerIdJoinRejected) {
    auto buf = make_join(0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 1);  // error at player_id field offset
}

// ==================== Exact wire bytes verification ====================

TEST(ClientMessage, WireBytesJoin) {
    // MSG_JOIN: type=0, player_id=1 in network order
    // Expected bytes: 00 00 00 00 01
    char buf[] = {0x00, 0x00, 0x00, 0x00, 0x01};
    auto result = get_message_from_buffer(buf, 5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_JOIN);
    EXPECT_EQ(result->player_id, 1u);
}

TEST(ClientMessage, WireBytesMove1) {
    // MSG_MOVE_1: type=1, player_id=256 (0x00000100), game_id=0, pawn=7
    // Bytes: 01  00 00 01 00  00 00 00 00  07
    char buf[] = {0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};
    auto result = get_message_from_buffer(buf, 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_MOVE_1);
    EXPECT_EQ(result->player_id, 256u);
    EXPECT_EQ(result->game_id, 0u);
    EXPECT_EQ(result->pawn, 7);
}

TEST(ClientMessage, WireBytesMove2) {
    // MSG_MOVE_2: type=2, player_id=0xFFFFFFFF, game_id=0x12345678, pawn=0
    char buf[10];
    buf[0] = 0x02;
    uint32_t pid = htonl(0xFFFFFFFF);
    std::memcpy(buf + 1, &pid, 4);
    uint32_t gid = htonl(0x12345678);
    std::memcpy(buf + 5, &gid, 4);
    buf[9] = 0x00;
    auto result = get_message_from_buffer(buf, 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_MOVE_2);
    EXPECT_EQ(result->player_id, 0xFFFFFFFFu);
    EXPECT_EQ(result->game_id, 0x12345678u);
    EXPECT_EQ(result->pawn, 0);
}

TEST(ClientMessage, WireBytesKeepAlive) {
    // MSG_KEEP_ALIVE: type=3, player_id=42, game_id=100
    char buf[9];
    buf[0] = 0x03;
    uint32_t pid = htonl(42);
    std::memcpy(buf + 1, &pid, 4);
    uint32_t gid = htonl(100);
    std::memcpy(buf + 5, &gid, 4);
    auto result = get_message_from_buffer(buf, 9);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_KEEP_ALIVE);
    EXPECT_EQ(result->player_id, 42u);
    EXPECT_EQ(result->game_id, 100u);
}

TEST(ClientMessage, WireBytesGiveUp) {
    // MSG_GIVE_UP: type=4, player_id=1000, game_id=999
    char buf[9];
    buf[0] = 0x04;
    uint32_t pid = htonl(1000);
    std::memcpy(buf + 1, &pid, 4);
    uint32_t gid = htonl(999);
    std::memcpy(buf + 5, &gid, 4);
    auto result = get_message_from_buffer(buf, 9);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ClientMessageType::MSG_GIVE_UP);
    EXPECT_EQ(result->player_id, 1000u);
    EXPECT_EQ(result->game_id, 999u);
}

// ==================== Exact length vs one byte short ====================

TEST(ClientMessage, JoinExactLengthOk) {
    auto buf = make_join(1);
    ASSERT_EQ(buf.size(), 5u);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
}

TEST(ClientMessage, JoinOneByteShort) {
    auto buf = make_join(1);
    // Send only 4 bytes instead of 5
    auto result = get_message_from_buffer(buf.data(), 4);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, Move1ExactLengthOk) {
    auto buf = make_move1(1, 0, 0);
    ASSERT_EQ(buf.size(), 10u);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
}

TEST(ClientMessage, Move1OneByteShort) {
    auto buf = make_move1(1, 0, 0);
    // Send only 9 bytes instead of 10 (missing pawn byte)
    auto result = get_message_from_buffer(buf.data(), 9);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, KeepAliveExactLengthOk) {
    auto buf = make_keep_alive(1, 0);
    ASSERT_EQ(buf.size(), 9u);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
}

TEST(ClientMessage, KeepAliveOneByteShort) {
    auto buf = make_keep_alive(1, 0);
    auto result = get_message_from_buffer(buf.data(), 8);
    ASSERT_FALSE(result.has_value());
}

TEST(ClientMessage, GiveUpExactLengthOk) {
    auto buf = make_give_up(1, 0);
    ASSERT_EQ(buf.size(), 9u);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());
}

TEST(ClientMessage, GiveUpOneByteShort) {
    auto buf = make_give_up(1, 0);
    auto result = get_message_from_buffer(buf.data(), 8);
    ASSERT_FALSE(result.has_value());
}

// ==================== MSG_JOIN with trailing bytes ====================

TEST(ClientMessage, JoinWithOneTrailingByte) {
    auto buf = make_join(1);
    buf.push_back(static_cast<char>(0xFF));  // one extra byte
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    // Error at offset 5 (where parsing stopped, but len=6 != offset=5)
    EXPECT_EQ(result.error(), 5u);
}

TEST(ClientMessage, JoinWithManyTrailingBytes) {
    auto buf = make_join(1);
    buf.push_back(0x01);
    buf.push_back(0x02);
    buf.push_back(0x03);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 5u);  // offset where JOIN parsing stopped
}

// ==================== Zero player_id in non-JOIN messages ====================

TEST(ClientMessage, ZeroPlayerIdMove1Rejected) {
    auto buf = make_move1(0, 0, 0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 1u);  // error at player_id offset
}

TEST(ClientMessage, ZeroPlayerIdKeepAliveRejected) {
    auto buf = make_keep_alive(0, 0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 1u);
}

TEST(ClientMessage, ZeroPlayerIdGiveUpRejected) {
    auto buf = make_give_up(0, 0);
    auto result = get_message_from_buffer(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 1u);
}
