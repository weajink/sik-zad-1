#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <kayles_client.h>

#include <cstdint>
#include <cstring>

using namespace kayles_client;

// ---------------------------------------------------------------------------
// Helper: extract a big-endian uint32_t from a byte buffer at the given offset.
// ---------------------------------------------------------------------------
static uint32_t read_u32_be(const uint8_t *buf, size_t offset) {
    uint32_t val;
    std::memcpy(&val, buf + offset, 4);
    return ntohl(val);
}

// ===========================================================================
// 1. Valid message types with normal values
// ===========================================================================

TEST(ClientParsing, ValidJoin) {
    auto result = parse_client_message("0/42");
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 5u);
    EXPECT_EQ(w.data[0], 0);
    EXPECT_EQ(read_u32_be(w.data, 1), 42u);
}

TEST(ClientParsing, ValidMove1) {
    auto result = parse_client_message("1/10/20/30");
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 10u);
    EXPECT_EQ(w.data[0], 1);
    EXPECT_EQ(read_u32_be(w.data, 1), 10u);
    EXPECT_EQ(read_u32_be(w.data, 5), 20u);
    EXPECT_EQ(w.data[9], 30);
}

TEST(ClientParsing, ValidMove2) {
    auto result = parse_client_message("2/100/200/55");
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 10u);
    EXPECT_EQ(w.data[0], 2);
    EXPECT_EQ(read_u32_be(w.data, 1), 100u);
    EXPECT_EQ(read_u32_be(w.data, 5), 200u);
    EXPECT_EQ(w.data[9], 55);
}

TEST(ClientParsing, ValidKeepAlive) {
    auto result = parse_client_message("3/7/99");
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 9u);
    EXPECT_EQ(w.data[0], 3);
    EXPECT_EQ(read_u32_be(w.data, 1), 7u);
    EXPECT_EQ(read_u32_be(w.data, 5), 99u);
}

TEST(ClientParsing, ValidGiveUp) {
    auto result = parse_client_message("4/1000/2000");
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 9u);
    EXPECT_EQ(w.data[0], 4);
    EXPECT_EQ(read_u32_be(w.data, 1), 1000u);
    EXPECT_EQ(read_u32_be(w.data, 5), 2000u);
}

// ===========================================================================
// 2. Big-endian byte order verification
// ===========================================================================

TEST(ClientParsing, BigEndianPlayerIdBytes) {
    auto result = parse_client_message("0/16909060");  // 0x01020304
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 5u);
    EXPECT_EQ(w.data[1], 0x01);
    EXPECT_EQ(w.data[2], 0x02);
    EXPECT_EQ(w.data[3], 0x03);
    EXPECT_EQ(w.data[4], 0x04);
}

TEST(ClientParsing, BigEndianGameIdBytes) {
    auto result = parse_client_message("3/1/2864434397");  // 0xAABBCCDD
    ASSERT_TRUE(result.has_value());
    auto &w = *result;
    EXPECT_EQ(w.len, 9u);
    EXPECT_EQ(w.data[5], static_cast<uint8_t>(0xAA));
    EXPECT_EQ(w.data[6], static_cast<uint8_t>(0xBB));
    EXPECT_EQ(w.data[7], static_cast<uint8_t>(0xCC));
    EXPECT_EQ(w.data[8], static_cast<uint8_t>(0xDD));
}

// ===========================================================================
// 3. Boundary values
// ===========================================================================

TEST(ClientParsing, PlayerIdMin) {
    auto result = parse_client_message("0/1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(read_u32_be(result->data, 1), 1u);
}

TEST(ClientParsing, PlayerIdMax) {
    auto result = parse_client_message("0/4294967295");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(read_u32_be(result->data, 1), UINT32_MAX);
}

TEST(ClientParsing, GameIdZero) {
    auto result = parse_client_message("3/1/0");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(read_u32_be(result->data, 5), 0u);
}

TEST(ClientParsing, GameIdMax) {
    auto result = parse_client_message("3/1/4294967295");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(read_u32_be(result->data, 5), UINT32_MAX);
}

TEST(ClientParsing, PawnZero) {
    auto result = parse_client_message("1/1/0/0");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->data[9], 0);
}

TEST(ClientParsing, PawnMax) {
    auto result = parse_client_message("1/1/0/255");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->data[9], 255);
}

// ===========================================================================
// 4. Wrong field count
// ===========================================================================

TEST(ClientParsing, JoinTooFewFields) {
    EXPECT_FALSE(parse_client_message("0").has_value());
}

TEST(ClientParsing, JoinTooManyFields) {
    EXPECT_FALSE(parse_client_message("0/1/2").has_value());
}

TEST(ClientParsing, Move1TooFewFields) {
    EXPECT_FALSE(parse_client_message("1/1/2").has_value());
}

TEST(ClientParsing, Move1TooManyFields) {
    EXPECT_FALSE(parse_client_message("1/1/2/3/4").has_value());
}

TEST(ClientParsing, Move2TooFewFields) {
    EXPECT_FALSE(parse_client_message("2/1/2").has_value());
}

TEST(ClientParsing, Move2TooManyFields) {
    EXPECT_FALSE(parse_client_message("2/1/2/3/4").has_value());
}

TEST(ClientParsing, KeepAliveTooFewFields) {
    EXPECT_FALSE(parse_client_message("3/1").has_value());
}

TEST(ClientParsing, KeepAliveTooManyFields) {
    EXPECT_FALSE(parse_client_message("3/1/2/3").has_value());
}

TEST(ClientParsing, GiveUpTooFewFields) {
    EXPECT_FALSE(parse_client_message("4/1").has_value());
}

TEST(ClientParsing, GiveUpTooManyFields) {
    EXPECT_FALSE(parse_client_message("4/1/2/3").has_value());
}

// ===========================================================================
// 5. Invalid msg_type
// ===========================================================================

TEST(ClientParsing, InvalidMsgType5) {
    EXPECT_FALSE(parse_client_message("5/1/2/3").has_value());
}

TEST(ClientParsing, InvalidMsgType255) {
    EXPECT_FALSE(parse_client_message("255/1/2/3").has_value());
}

TEST(ClientParsing, InvalidMsgTypeLarge) {
    EXPECT_FALSE(parse_client_message("999/1").has_value());
}

// ===========================================================================
// 6. player_id = 0 (invalid)
// ===========================================================================

TEST(ClientParsing, PlayerIdZeroJoin) {
    EXPECT_FALSE(parse_client_message("0/0").has_value());
}

TEST(ClientParsing, PlayerIdZeroMove1) {
    EXPECT_FALSE(parse_client_message("1/0/1/1").has_value());
}

TEST(ClientParsing, PlayerIdZeroKeepAlive) {
    EXPECT_FALSE(parse_client_message("3/0/1").has_value());
}

TEST(ClientParsing, PlayerIdZeroGiveUp) {
    EXPECT_FALSE(parse_client_message("4/0/1").has_value());
}

// ===========================================================================
// 7. Non-numeric tokens
// ===========================================================================

TEST(ClientParsing, NonNumericToken) {
    EXPECT_FALSE(parse_client_message("abc").has_value());
}

TEST(ClientParsing, NonNumericInMiddle) {
    EXPECT_FALSE(parse_client_message("1/abc/2/3").has_value());
}

TEST(ClientParsing, NonNumericPlayerIdInJoin) {
    EXPECT_FALSE(parse_client_message("0/hello").has_value());
}

TEST(ClientParsing, MixedAlphaNumeric) {
    EXPECT_FALSE(parse_client_message("0/12abc").has_value());
}

// ===========================================================================
// 8. Empty string, just slashes, trailing slash
// ===========================================================================

TEST(ClientParsing, EmptyString) {
    EXPECT_FALSE(parse_client_message("").has_value());
}

TEST(ClientParsing, JustSlashes) {
    EXPECT_FALSE(parse_client_message("///").has_value());
}

TEST(ClientParsing, TrailingSlash) {
    EXPECT_FALSE(parse_client_message("0/42/").has_value());
}

TEST(ClientParsing, LeadingSlash) {
    EXPECT_FALSE(parse_client_message("/0/42").has_value());
}

TEST(ClientParsing, DoubleSlash) {
    EXPECT_FALSE(parse_client_message("0//42").has_value());
}

// ===========================================================================
// 9. Negative numbers
// ===========================================================================

TEST(ClientParsing, NegativePlayerIdJoin) {
    EXPECT_FALSE(parse_client_message("0/-1").has_value());
}

TEST(ClientParsing, NegativeGameId) {
    EXPECT_FALSE(parse_client_message("3/1/-1").has_value());
}

TEST(ClientParsing, NegativeMsgType) {
    EXPECT_FALSE(parse_client_message("-1/1").has_value());
}

TEST(ClientParsing, NegativePawn) {
    EXPECT_FALSE(parse_client_message("1/1/1/-1").has_value());
}

// ===========================================================================
// 10. Values exceeding type range
// ===========================================================================

TEST(ClientParsing, PlayerIdExceedsUint32) {
    EXPECT_FALSE(parse_client_message("0/4294967296").has_value());
}

TEST(ClientParsing, GameIdExceedsUint32) {
    EXPECT_FALSE(parse_client_message("3/1/4294967296").has_value());
}

TEST(ClientParsing, PawnExceedsUint8) {
    EXPECT_FALSE(parse_client_message("1/1/1/256").has_value());
}

TEST(ClientParsing, VeryLargeNumber) {
    EXPECT_FALSE(parse_client_message("0/99999999999999999999").has_value());
}
