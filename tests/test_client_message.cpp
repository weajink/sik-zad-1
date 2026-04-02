#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <kayles_common.h>

#include <cstring>

using namespace kayles_common;

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
