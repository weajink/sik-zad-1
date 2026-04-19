// Unit tests for the new protocol API in src/kayles_protocol.h.
//
// Covers:
//   - ClientMessage serialize/deserialize round trips
//   - Byte-exact wire format (network byte order)
//   - Error paths (invalid length, invalid message type)
//   - GameState serialize/deserialize and pawn_row bitmap encoding
//   - MessageWrong serialize/deserialize
//   - Stream insertion operators (smoke tests)

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <variant>
#include <vector>

#include "kayles_protocol.h"

using namespace kayles::protocol;
using kayles::error::ErrorType;
using kayles::error::KaylesError;
using kayles::types::game_id_t;
using kayles::types::pawn_row_t;
using kayles::types::pawn_t;
using kayles::types::player_id_t;

// ===========================================================================
// Helpers
// ===========================================================================

// Read a 32-bit big-endian value from the given offset in a byte buffer.
static uint32_t read_u32_be(const std::vector<uint8_t> &buf, size_t offset) {
    EXPECT_GE(buf.size(), offset + 4);
    return (static_cast<uint32_t>(buf[offset]) << 24) |
           (static_cast<uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<uint32_t>(buf[offset + 2]) << 8) | static_cast<uint32_t>(buf[offset + 3]);
}

static pawn_row_t make_row(std::initializer_list<int> bits) {
    pawn_row_t row;
    row.reserve(bits.size());
    for (int b : bits)
        row.push_back(b != 0);
    return row;
}

static bool messages_equal(const ClientMessage &a, const ClientMessage &b) {
    if (a.msg_type != b.msg_type)
        return false;
    if (a.player_id != b.player_id)
        return false;
    switch (a.msg_type) {
        case ClientMessageType::MSG_JOIN:
            return true;
        case ClientMessageType::MSG_KEEP_ALIVE:
        case ClientMessageType::MSG_GIVE_UP:
            return a.game_id == b.game_id;
        case ClientMessageType::MSG_MOVE_1:
        case ClientMessageType::MSG_MOVE_2:
            return a.game_id == b.game_id && a.pawn == b.pawn;
    }
    return false;
}

static bool game_states_equal(const GameState &a, const GameState &b) {
    return a.game_id == b.game_id && a.player_a_id == b.player_a_id &&
           a.player_b_id == b.player_b_id && a.status == b.status && a.max_pawn == b.max_pawn &&
           a.pawn_row == b.pawn_row;
}

// ===========================================================================
// ClientMessageSerialize
// ===========================================================================

TEST(ClientMessageSerialize, JoinSizeIsFive) {
    ClientMessage m{ClientMessageType::MSG_JOIN, 42u, 0u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 5u);
}

TEST(ClientMessageSerialize, Move1SizeIsTen) {
    ClientMessage m{ClientMessageType::MSG_MOVE_1, 42u, 7u, 3u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 10u);
}

TEST(ClientMessageSerialize, Move2SizeIsTen) {
    ClientMessage m{ClientMessageType::MSG_MOVE_2, 42u, 7u, 3u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 10u);
}

TEST(ClientMessageSerialize, KeepAliveSizeIsNine) {
    ClientMessage m{ClientMessageType::MSG_KEEP_ALIVE, 42u, 7u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 9u);
}

TEST(ClientMessageSerialize, GiveUpSizeIsNine) {
    ClientMessage m{ClientMessageType::MSG_GIVE_UP, 42u, 7u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 9u);
}

TEST(ClientMessageSerialize, JoinMsgTypeByteIsZero) {
    ClientMessage m{ClientMessageType::MSG_JOIN, 1u, 0u, 0u};
    auto bytes = m.serialize();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes[0], 0u);
}

TEST(ClientMessageSerialize, Move1MsgTypeByteIsOne) {
    ClientMessage m{ClientMessageType::MSG_MOVE_1, 1u, 0u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes[0], 1u);
}

TEST(ClientMessageSerialize, Move2MsgTypeByteIsTwo) {
    ClientMessage m{ClientMessageType::MSG_MOVE_2, 1u, 0u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes[0], 2u);
}

TEST(ClientMessageSerialize, KeepAliveMsgTypeByteIsThree) {
    ClientMessage m{ClientMessageType::MSG_KEEP_ALIVE, 1u, 0u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes[0], 3u);
}

TEST(ClientMessageSerialize, GiveUpMsgTypeByteIsFour) {
    ClientMessage m{ClientMessageType::MSG_GIVE_UP, 1u, 0u, 0u};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes[0], 4u);
}

TEST(ClientMessageSerialize, PlayerIdIsBigEndian) {
    ClientMessage m{ClientMessageType::MSG_JOIN, 0x01020304u, 0u, 0u};
    auto bytes = m.serialize();
    ASSERT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[1], 0x01u);
    EXPECT_EQ(bytes[2], 0x02u);
    EXPECT_EQ(bytes[3], 0x03u);
    EXPECT_EQ(bytes[4], 0x04u);
}

TEST(ClientMessageSerialize, Move1GameIdIsBigEndian) {
    ClientMessage m{ClientMessageType::MSG_MOVE_1, 0x11223344u, 0xAABBCCDDu, 0u};
    auto bytes = m.serialize();
    ASSERT_EQ(bytes.size(), 10u);
    EXPECT_EQ(read_u32_be(bytes, 1), 0x11223344u);
    EXPECT_EQ(read_u32_be(bytes, 5), 0xAABBCCDDu);
}

TEST(ClientMessageSerialize, Move1PawnByteAppearsLast) {
    ClientMessage m{ClientMessageType::MSG_MOVE_1, 1u, 0u, 0xABu};
    auto bytes = m.serialize();
    ASSERT_EQ(bytes.size(), 10u);
    EXPECT_EQ(bytes[9], 0xABu);
}

TEST(ClientMessageSerialize, Move2PawnByteAppearsLast) {
    ClientMessage m{ClientMessageType::MSG_MOVE_2, 1u, 0u, 0xFEu};
    auto bytes = m.serialize();
    ASSERT_EQ(bytes.size(), 10u);
    EXPECT_EQ(bytes[9], 0xFEu);
}

TEST(ClientMessageSerialize, KeepAliveWireFormat) {
    ClientMessage m{ClientMessageType::MSG_KEEP_ALIVE, 0xDEADBEEFu, 0xCAFEBABEu, 0u};
    auto bytes = m.serialize();
    ASSERT_EQ(bytes.size(), 9u);
    EXPECT_EQ(bytes[0], 3u);
    EXPECT_EQ(read_u32_be(bytes, 1), 0xDEADBEEFu);
    EXPECT_EQ(read_u32_be(bytes, 5), 0xCAFEBABEu);
}

TEST(ClientMessageSerialize, JoinDoesNotEmitGameIdOrPawn) {
    // For MSG_JOIN serialization must stop after player_id (5 bytes total).
    ClientMessage m{ClientMessageType::MSG_JOIN, 1u, 0x11223344u, 0xFFu};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 5u);
}

TEST(ClientMessageSerialize, KeepAliveDoesNotEmitPawn) {
    ClientMessage m{ClientMessageType::MSG_KEEP_ALIVE, 1u, 2u, 0xFFu};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 9u);
}

TEST(ClientMessageSerialize, GiveUpDoesNotEmitPawn) {
    ClientMessage m{ClientMessageType::MSG_GIVE_UP, 1u, 2u, 0xFFu};
    auto bytes = m.serialize();
    EXPECT_EQ(bytes.size(), 9u);
}

// ===========================================================================
// ClientMessageDeserialize
// ===========================================================================

TEST(ClientMessageDeserialize, EmptyBufferFailsWithInvalidLengthZero) {
    std::vector<uint8_t> buf;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 0u);
}

TEST(ClientMessageDeserialize, InvalidMsgTypeFiveFails) {
    std::vector<uint8_t> buf{5, 0, 0, 0, 1};  // msg_type=5 is invalid
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_ARGUMENT);
    EXPECT_EQ(r.error().error_index(), 0u);
}

TEST(ClientMessageDeserialize, InvalidMsgType255Fails) {
    std::vector<uint8_t> buf{0xFF, 0, 0, 0, 1};
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_ARGUMENT);
}

TEST(ClientMessageDeserialize, InvalidMsgType100Fails) {
    std::vector<uint8_t> buf{100, 0, 0, 0, 1};
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_ARGUMENT);
}

TEST(ClientMessageDeserialize, JoinTooShortFails) {
    // MSG_JOIN expects 5 bytes. Give only 4.
    std::vector<uint8_t> buf{0, 0, 0, 1};
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, JoinTooLongFails) {
    // MSG_JOIN expects 5 bytes. Give 6.
    std::vector<uint8_t> buf{0, 0, 0, 0, 1, 99};
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, Move1TooShortFails) {
    // MSG_MOVE_1 expects 10 bytes. Give 9.
    std::vector<uint8_t> buf(9, 0);
    buf[0] = 1;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, Move1TooLongFails) {
    std::vector<uint8_t> buf(11, 0);
    buf[0] = 1;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, Move2TooShortFails) {
    std::vector<uint8_t> buf(9, 0);
    buf[0] = 2;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, Move2TooLongFails) {
    std::vector<uint8_t> buf(11, 0);
    buf[0] = 2;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, KeepAliveTooShortFails) {
    // MSG_KEEP_ALIVE expects 9 bytes.
    std::vector<uint8_t> buf(8, 0);
    buf[0] = 3;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, KeepAliveTooLongFails) {
    std::vector<uint8_t> buf(10, 0);
    buf[0] = 3;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, GiveUpTooShortFails) {
    std::vector<uint8_t> buf(8, 0);
    buf[0] = 4;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ClientMessageDeserialize, GiveUpTooLongFails) {
    std::vector<uint8_t> buf(10, 0);
    buf[0] = 4;
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

// ===========================================================================
// ProtocolRoundTrip (serialize → deserialize)
// ===========================================================================

TEST(ProtocolRoundTrip, JoinTypical) {
    ClientMessage original{ClientMessageType::MSG_JOIN, 42u, 0u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value()) << r.error().what();
    EXPECT_TRUE(messages_equal(original, *r));
}

TEST(ProtocolRoundTrip, JoinPlayerIdOne) {
    ClientMessage original{ClientMessageType::MSG_JOIN, 1u, 0u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->player_id, 1u);
}

TEST(ProtocolRoundTrip, JoinPlayerIdMax) {
    ClientMessage original{ClientMessageType::MSG_JOIN, UINT32_MAX, 0u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->player_id, UINT32_MAX);
}

TEST(ProtocolRoundTrip, Move1Typical) {
    ClientMessage original{ClientMessageType::MSG_MOVE_1, 7u, 11u, 3u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(messages_equal(original, *r));
}

TEST(ProtocolRoundTrip, Move1PawnZero) {
    ClientMessage original{ClientMessageType::MSG_MOVE_1, 7u, 11u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->pawn, 0u);
}

TEST(ProtocolRoundTrip, Move1PawnMax) {
    ClientMessage original{ClientMessageType::MSG_MOVE_1, 7u, 11u, 255u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->pawn, 255u);
}

TEST(ProtocolRoundTrip, Move1GameIdZero) {
    ClientMessage original{ClientMessageType::MSG_MOVE_1, 7u, 0u, 5u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, 0u);
}

TEST(ProtocolRoundTrip, Move1GameIdMax) {
    ClientMessage original{ClientMessageType::MSG_MOVE_1, 7u, UINT32_MAX, 5u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, UINT32_MAX);
}

TEST(ProtocolRoundTrip, Move2Typical) {
    ClientMessage original{ClientMessageType::MSG_MOVE_2, 7u, 11u, 3u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(messages_equal(original, *r));
}

TEST(ProtocolRoundTrip, Move2Boundaries) {
    ClientMessage original{ClientMessageType::MSG_MOVE_2, UINT32_MAX, UINT32_MAX, 255u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(messages_equal(original, *r));
}

TEST(ProtocolRoundTrip, KeepAliveTypical) {
    ClientMessage original{ClientMessageType::MSG_KEEP_ALIVE, 7u, 11u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(messages_equal(original, *r));
}

TEST(ProtocolRoundTrip, KeepAliveBoundaries) {
    ClientMessage original{ClientMessageType::MSG_KEEP_ALIVE, 1u, 0u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->player_id, 1u);
    EXPECT_EQ(r->game_id, 0u);
}

TEST(ProtocolRoundTrip, GiveUpTypical) {
    ClientMessage original{ClientMessageType::MSG_GIVE_UP, 7u, 11u, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(messages_equal(original, *r));
}

TEST(ProtocolRoundTrip, GiveUpBoundaries) {
    ClientMessage original{ClientMessageType::MSG_GIVE_UP, UINT32_MAX, UINT32_MAX, 0u};
    auto bytes = original.serialize();
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(messages_equal(original, *r));
}

// ===========================================================================
// GameStateSerialize
// ===========================================================================

TEST(GameStateSerialize, HeaderSizePlusOneBitmapForMaxPawn0) {
    // 4+4+4+1+1 = 14 header bytes, + 1 bitmap byte = 15 total.
    GameState s{0u, 1u, 2u, GameStatus::WAITING_FOR_OPPONENT, 0u, make_row({0})};
    auto bytes = s.serialize();
    EXPECT_EQ(bytes.size(), 15u);
}

TEST(GameStateSerialize, HeaderSizePlusOneBitmapForMaxPawn7) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 7u, pawn_row_t(8, false)};
    auto bytes = s.serialize();
    EXPECT_EQ(bytes.size(), 15u);
}

TEST(GameStateSerialize, HeaderSizePlusTwoBitmapForMaxPawn8) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 8u, pawn_row_t(9, false)};
    auto bytes = s.serialize();
    EXPECT_EQ(bytes.size(), 16u);
}

TEST(GameStateSerialize, HeaderSizePlusTwoBitmapForMaxPawn15) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_B, 15u, pawn_row_t(16, false)};
    auto bytes = s.serialize();
    EXPECT_EQ(bytes.size(), 16u);
}

TEST(GameStateSerialize, HeaderSizePlusThreeBitmapForMaxPawn16) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_B, 16u, pawn_row_t(17, false)};
    auto bytes = s.serialize();
    EXPECT_EQ(bytes.size(), 17u);
}

TEST(GameStateSerialize, BitmapSizeForMaxPawn127) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_B, 127u, pawn_row_t(128, false)};
    auto bytes = s.serialize();
    // 127/8 + 1 = 16 bitmap bytes; header=14; total=30.
    EXPECT_EQ(bytes.size(), 30u);
}

TEST(GameStateSerialize, BitmapSizeForMaxPawn255) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 255u, pawn_row_t(256, false)};
    auto bytes = s.serialize();
    // 255/8 + 1 = 32 bitmap bytes; header=14; total=46.
    EXPECT_EQ(bytes.size(), 46u);
}

TEST(GameStateSerialize, HeaderFieldsAreBigEndian) {
    GameState s{0x01020304u, 0x0A0B0C0Du, 0x11223344u, GameStatus::TURN_A, 0u, make_row({0})};
    auto bytes = s.serialize();
    ASSERT_GE(bytes.size(), 14u);
    EXPECT_EQ(read_u32_be(bytes, 0), 0x01020304u);
    EXPECT_EQ(read_u32_be(bytes, 4), 0x0A0B0C0Du);
    EXPECT_EQ(read_u32_be(bytes, 8), 0x11223344u);
    EXPECT_EQ(bytes[12], 1u);  // TURN_A = 1
    EXPECT_EQ(bytes[13], 0u);  // max_pawn
}

TEST(GameStateSerialize, StatusByteEncoding) {
    auto check = [](GameStatus st, uint8_t expected) {
        GameState s{0u, 1u, 2u, st, 0u, make_row({0})};
        auto bytes = s.serialize();
        ASSERT_GE(bytes.size(), 13u);
        EXPECT_EQ(bytes[12], expected);
    };
    check(GameStatus::WAITING_FOR_OPPONENT, 0);
    check(GameStatus::TURN_A, 1);
    check(GameStatus::TURN_B, 2);
    check(GameStatus::WIN_A, 3);
    check(GameStatus::WIN_B, 4);
}

TEST(GameStateSerialize, BitmapPawn0IsMSBOfByte0) {
    // max_pawn=0, pawn 0 set → bitmap byte = 0b10000000 = 0x80.
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 0u, make_row({1})};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    EXPECT_EQ(bytes[14], 0x80u);
}

TEST(GameStateSerialize, BitmapPawn7IsLSBOfByte0) {
    // max_pawn=7, only pawn 7 set → bitmap byte = 0b00000001 = 0x01.
    pawn_row_t row(8, false);
    row[7] = true;
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 7u, row};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    EXPECT_EQ(bytes[14], 0x01u);
}

TEST(GameStateSerialize, BitmapPawn8IsMSBOfByte1) {
    // max_pawn=8, only pawn 8 set → bytes [0x00, 0x80].
    pawn_row_t row(9, false);
    row[8] = true;
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 8u, row};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(bytes[14], 0x00u);
    EXPECT_EQ(bytes[15], 0x80u);
}

TEST(GameStateSerialize, BitmapAllOnesMaxPawn7) {
    // max_pawn=7, all 8 pins set → 0xFF.
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 7u, pawn_row_t(8, true)};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    EXPECT_EQ(bytes[14], 0xFFu);
}

TEST(GameStateSerialize, BitmapAllZerosMaxPawn7) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 7u, pawn_row_t(8, false)};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    EXPECT_EQ(bytes[14], 0x00u);
}

TEST(GameStateSerialize, BitmapAlternatingMaxPawn7) {
    // Pattern: 1 0 1 0 1 0 1 0 → bits msb..lsb = 10101010 = 0xAA.
    pawn_row_t row = make_row({1, 0, 1, 0, 1, 0, 1, 0});
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 7u, row};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    EXPECT_EQ(bytes[14], 0xAAu);
}

TEST(GameStateSerialize, BitmapExcessBitsZeroedMaxPawn2) {
    // max_pawn=2 → 3 pins at positions 0,1,2 (MSB-side of the byte).
    // All three set → bits 0..2 = 111, bits 3..7 must be 0 → 0b11100000 = 0xE0.
    pawn_row_t row = make_row({1, 1, 1});
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 2u, row};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    EXPECT_EQ(bytes[14], 0xE0u);
}

TEST(GameStateSerialize, BitmapExcessBitsZeroedMaxPawn9) {
    // max_pawn=9 → bitmap size 2 bytes. Pins 0..9 all set.
    // First byte all ones (0xFF), second byte bits 0..1 set (0b11000000 = 0xC0).
    pawn_row_t row(10, true);
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 9u, row};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(bytes[14], 0xFFu);
    EXPECT_EQ(bytes[15], 0xC0u);
}

TEST(GameStateSerialize, BitmapSinglePinAtBoundary15) {
    // Only pin 15 is set → bytes [0x00, 0x01].
    pawn_row_t row(16, false);
    row[15] = true;
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 15u, row};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(bytes[14], 0x00u);
    EXPECT_EQ(bytes[15], 0x01u);
}

// ===========================================================================
// GameStateDeserialize
// ===========================================================================

TEST(GameStateDeserialize, TooShortForHeaderFails) {
    std::vector<uint8_t> buf(13, 0);  // header needs 14
    auto r = deserialize_game_state(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(GameStateDeserialize, ExactHeaderOnlyFails) {
    // 14 bytes: header only, no bitmap → still needs 1 bitmap byte even at max_pawn=0.
    std::vector<uint8_t> buf(14, 0);
    auto r = deserialize_game_state(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(GameStateDeserialize, WrongBitmapSizeTooShort) {
    // max_pawn=8 → expects 2 bitmap bytes. Provide only 1.
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 8u, pawn_row_t(9, false)};
    auto bytes = s.serialize();
    ASSERT_EQ(bytes.size(), 16u);
    bytes.pop_back();  // drop one bitmap byte
    auto r = deserialize_game_state(bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(GameStateDeserialize, WrongBitmapSizeTooLong) {
    GameState s{0u, 1u, 2u, GameStatus::TURN_A, 0u, make_row({1})};
    auto bytes = s.serialize();
    bytes.push_back(0);  // extra byte
    auto r = deserialize_game_state(bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

// ===========================================================================
// GameState round trip
// ===========================================================================

TEST(ProtocolRoundTrip, GameStateMaxPawn0) {
    GameState original{0u, 1u, 2u, GameStatus::WAITING_FOR_OPPONENT, 0u, make_row({1})};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateMaxPawn7) {
    GameState original{5u, 10u, 20u, GameStatus::TURN_A, 7u, make_row({1, 0, 1, 1, 0, 0, 1, 0})};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateMaxPawn8) {
    pawn_row_t row(9, false);
    row[0] = row[4] = row[8] = true;
    GameState original{100u, 1u, 2u, GameStatus::TURN_B, 8u, row};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateMaxPawn15) {
    pawn_row_t row(16, true);
    GameState original{1u, 2u, 3u, GameStatus::WIN_A, 15u, row};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateMaxPawn16) {
    pawn_row_t row(17, false);
    row[0] = row[16] = true;
    GameState original{1u, 2u, 3u, GameStatus::WIN_B, 16u, row};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateMaxPawn127) {
    pawn_row_t row(128, false);
    for (size_t i = 0; i < 128; i += 2)
        row[i] = true;
    GameState original{0xDEADBEEFu, 0xAABBCCDDu, 0x11223344u, GameStatus::TURN_B, 127u, row};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateMaxPawn255AllOnes) {
    pawn_row_t row(256, true);
    GameState original{UINT32_MAX, UINT32_MAX, UINT32_MAX, GameStatus::TURN_A, 255u, row};
    auto r = deserialize_game_state(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(game_states_equal(original, *r));
}

TEST(ProtocolRoundTrip, GameStateAllStatusesSurviveRoundTrip) {
    for (auto st : {GameStatus::WAITING_FOR_OPPONENT, GameStatus::TURN_A, GameStatus::TURN_B,
                    GameStatus::WIN_A, GameStatus::WIN_B}) {
        GameState original{0u, 1u, 2u, st, 3u, make_row({1, 0, 1, 0})};
        auto r = deserialize_game_state(original.serialize());
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->status, st);
    }
}

TEST(ProtocolRoundTrip, GameStateSinglePinSetAtEachBoundary) {
    for (pawn_t mp : {pawn_t{0}, pawn_t{1}, pawn_t{7}, pawn_t{8}, pawn_t{15}, pawn_t{16},
                      pawn_t{31}, pawn_t{63}, pawn_t{127}, pawn_t{255}}) {
        pawn_row_t row(static_cast<size_t>(mp) + 1, false);
        row[mp] = true;  // last pin set
        GameState original{0u, 1u, 2u, GameStatus::TURN_A, mp, row};
        auto r = deserialize_game_state(original.serialize());
        ASSERT_TRUE(r.has_value()) << "failed at max_pawn=" << static_cast<int>(mp);
        EXPECT_TRUE(game_states_equal(original, *r))
            << "round trip mismatch at max_pawn=" << static_cast<int>(mp);
    }
}

// ===========================================================================
// MessageWrongSerialize
// ===========================================================================

TEST(MessageWrongSerialize, SizeIsFourteen) {
    MessageWrong w{};
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i)
        w.client_bytes[i] = 0;
    w.error_index = 0;
    auto bytes = w.serialize();
    EXPECT_EQ(bytes.size(), 14u);
}

TEST(MessageWrongSerialize, ClientBytesCopiedVerbatim) {
    MessageWrong w{};
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        w.client_bytes[i] = static_cast<uint8_t>(i + 1);
    }
    w.error_index = 0;
    auto bytes = w.serialize();
    ASSERT_EQ(bytes.size(), 14u);
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        EXPECT_EQ(bytes[i], static_cast<uint8_t>(i + 1));
    }
}

TEST(MessageWrongSerialize, StatusByteFollowsClientBytes) {
    MessageWrong w{};
    for (auto &b : w.client_bytes)
        b = 0;
    w.status = MSG_WRONG_STATUS;
    w.error_index = 0;
    auto bytes = w.serialize();
    ASSERT_EQ(bytes.size(), 14u);
    EXPECT_EQ(bytes[12], 255u);
}

TEST(MessageWrongSerialize, ErrorIndexIsLastByte) {
    MessageWrong w{};
    for (auto &b : w.client_bytes)
        b = 0;
    w.error_index = 7;
    auto bytes = w.serialize();
    ASSERT_EQ(bytes.size(), 14u);
    EXPECT_EQ(bytes[13], 7u);
}

TEST(MessageWrongSerialize, VariousErrorIndexValues) {
    for (uint8_t idx : {uint8_t{0}, uint8_t{1}, uint8_t{9}, uint8_t{128}, uint8_t{255}}) {
        MessageWrong w{};
        for (auto &b : w.client_bytes)
            b = 0xAAu;
        w.error_index = idx;
        auto bytes = w.serialize();
        ASSERT_EQ(bytes.size(), 14u);
        EXPECT_EQ(bytes[13], idx);
    }
}

// ===========================================================================
// MessageWrongDeserialize
// ===========================================================================

TEST(MessageWrongDeserialize, CorrectSizeSucceeds) {
    std::vector<uint8_t> buf(14);
    for (size_t i = 0; i < 12; ++i)
        buf[i] = static_cast<uint8_t>(0xF0u | i);
    buf[12] = MSG_WRONG_STATUS;
    buf[13] = 3;
    auto r = deserialize_message_wrong(buf);
    ASSERT_TRUE(r.has_value());
    for (size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(r->client_bytes[i], static_cast<uint8_t>(0xF0u | i));
    }
    EXPECT_EQ(r->status, MSG_WRONG_STATUS);
    EXPECT_EQ(r->error_index, 3u);
}

TEST(MessageWrongDeserialize, SizeThirteenFails) {
    std::vector<uint8_t> buf(13, 0);
    auto r = deserialize_message_wrong(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(MessageWrongDeserialize, SizeFifteenFails) {
    std::vector<uint8_t> buf(15, 0);
    auto r = deserialize_message_wrong(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(MessageWrongDeserialize, EmptyFails) {
    std::vector<uint8_t> buf;
    auto r = deserialize_message_wrong(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

// ===========================================================================
// MessageWrong round trip
// ===========================================================================

TEST(ProtocolRoundTrip, MessageWrongTypical) {
    MessageWrong original{};
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        original.client_bytes[i] = static_cast<uint8_t>(i * 17);
    }
    original.status = MSG_WRONG_STATUS;
    original.error_index = 9;
    auto r = deserialize_message_wrong(original.serialize());
    ASSERT_TRUE(r.has_value());
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        EXPECT_EQ(r->client_bytes[i], original.client_bytes[i]);
    }
    EXPECT_EQ(r->status, original.status);
    EXPECT_EQ(r->error_index, original.error_index);
}

TEST(ProtocolRoundTrip, MessageWrongErrorIndexBoundary) {
    MessageWrong original{};
    for (auto &b : original.client_bytes)
        b = 0;
    original.error_index = 255;
    auto r = deserialize_message_wrong(original.serialize());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->error_index, 255u);
}

// ===========================================================================
// ProtocolPrint — smoke tests for operator<<
// ===========================================================================

TEST(ProtocolPrint, ClientMessageTypeNonEmpty) {
    for (auto t :
         {ClientMessageType::MSG_JOIN, ClientMessageType::MSG_MOVE_1, ClientMessageType::MSG_MOVE_2,
          ClientMessageType::MSG_KEEP_ALIVE, ClientMessageType::MSG_GIVE_UP}) {
        std::ostringstream os;
        os << t;
        EXPECT_FALSE(os.str().empty());
    }
}

TEST(ProtocolPrint, GameStatusNonEmpty) {
    for (auto s : {GameStatus::WAITING_FOR_OPPONENT, GameStatus::TURN_A, GameStatus::TURN_B,
                   GameStatus::WIN_A, GameStatus::WIN_B}) {
        std::ostringstream os;
        os << s;
        EXPECT_FALSE(os.str().empty());
    }
}

TEST(ProtocolPrint, ClientMessageJoinNonEmpty) {
    ClientMessage m{ClientMessageType::MSG_JOIN, 42u, 0u, 0u};
    std::ostringstream os;
    os << m;
    EXPECT_FALSE(os.str().empty());
}

TEST(ProtocolPrint, ClientMessageMove1NonEmpty) {
    ClientMessage m{ClientMessageType::MSG_MOVE_1, 42u, 7u, 3u};
    std::ostringstream os;
    os << m;
    EXPECT_FALSE(os.str().empty());
}

TEST(ProtocolPrint, ClientMessageKeepAliveNonEmpty) {
    ClientMessage m{ClientMessageType::MSG_KEEP_ALIVE, 42u, 7u, 0u};
    std::ostringstream os;
    os << m;
    EXPECT_FALSE(os.str().empty());
}

TEST(ProtocolPrint, GameStateNonEmpty) {
    GameState s{1u, 2u, 3u, GameStatus::TURN_A, 7u, pawn_row_t(8, true)};
    std::ostringstream os;
    os << s;
    EXPECT_FALSE(os.str().empty());
}

TEST(ProtocolPrint, MessageWrongNonEmpty) {
    MessageWrong w{};
    for (auto &b : w.client_bytes)
        b = 0xAAu;
    w.error_index = 5;
    std::ostringstream os;
    os << w;
    EXPECT_FALSE(os.str().empty());
}

// ===========================================================================
// ServerMessageDeserialize — unified dispatcher that discriminates on the
// status byte at offset 12 and delegates to the right inner deserializer.
// ===========================================================================

// Build wire bytes that structurally look like a GameState but let the caller
// override the status byte at offset 12. `max_pawn` and `pawn_row` are used
// to emit a valid-size bitmap (so the inner deserializer won't reject on
// length) unless the caller wants otherwise.
static std::vector<uint8_t> make_game_state_bytes(game_id_t game_id, player_id_t a, player_id_t b,
                                                  uint8_t status_byte, pawn_t max_pawn,
                                                  const pawn_row_t &row) {
    std::vector<uint8_t> v;
    append_u32(v, game_id);
    append_u32(v, a);
    append_u32(v, b);
    append_u8(v, status_byte);
    append_u8(v, max_pawn);
    append_bitmap(v, row, max_pawn);
    return v;
}

// ---- Discrimination ----

TEST(ServerMessageDeserialize, ValidGameStateBytesYieldGameStateVariant) {
    GameState gs{7u, 11u, 13u, GameStatus::TURN_A, 7u, make_row({1, 0, 1, 0, 1, 0, 1, 0})};
    auto bytes = gs.serialize();
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value()) << r.error().what();
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_FALSE(std::holds_alternative<MessageWrong>(*r));
    const auto &got = std::get<GameState>(*r);
    EXPECT_TRUE(game_states_equal(gs, got));
}

TEST(ServerMessageDeserialize, ValidMessageWrongBytesYieldMessageWrongVariant) {
    MessageWrong w{};
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        w.client_bytes[i] = static_cast<uint8_t>(i * 3 + 1);
    }
    w.status = MSG_WRONG_STATUS;
    w.error_index = 9;
    auto bytes = w.serialize();
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value()) << r.error().what();
    ASSERT_TRUE(std::holds_alternative<MessageWrong>(*r));
    EXPECT_FALSE(std::holds_alternative<GameState>(*r));
    const auto &got = std::get<MessageWrong>(*r);
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        EXPECT_EQ(got.client_bytes[i], w.client_bytes[i]);
    }
    EXPECT_EQ(got.status, MSG_WRONG_STATUS);
    EXPECT_EQ(got.error_index, 9u);
}

// ---- Boundary on status byte at offset 12 ----

TEST(ServerMessageDeserialize, StatusByteZeroRoutesToGameState) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/0u, /*max_pawn=*/0u, make_row({1}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(std::get<GameState>(*r).status, GameStatus::WAITING_FOR_OPPONENT);
}

TEST(ServerMessageDeserialize, StatusByteOneRoutesToGameState) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/1u, /*max_pawn=*/0u, make_row({0}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(std::get<GameState>(*r).status, GameStatus::TURN_A);
}

TEST(ServerMessageDeserialize, StatusByteTwoRoutesToGameState) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/2u, /*max_pawn=*/0u, make_row({0}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(std::get<GameState>(*r).status, GameStatus::TURN_B);
}

TEST(ServerMessageDeserialize, StatusByteThreeRoutesToGameState) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/3u, /*max_pawn=*/0u, make_row({1}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(std::get<GameState>(*r).status, GameStatus::WIN_A);
}

TEST(ServerMessageDeserialize, StatusByteFourRoutesToGameState) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/4u, /*max_pawn=*/0u, make_row({1}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(std::get<GameState>(*r).status, GameStatus::WIN_B);
}

// status bytes 5..254 are not valid GameStatus enum values, but the dispatcher
// still routes them to deserialize_game_state (which does NOT validate status).
// The inner deserializer must accept such bytes when the size is correct.
TEST(ServerMessageDeserialize, StatusByteFiveRoutesToGameStateAndAccepts) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/5u, /*max_pawn=*/0u, make_row({0}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    // The underlying byte was 5; it's been cast to GameStatus. We don't care
    // what enum name it takes — just that routing picked GameState, not wrong.
    EXPECT_EQ(static_cast<uint8_t>(std::get<GameState>(*r).status), 5u);
}

TEST(ServerMessageDeserialize, StatusByte100RoutesToGameStateAndAccepts) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/100u, /*max_pawn=*/0u, make_row({0}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(static_cast<uint8_t>(std::get<GameState>(*r).status), 100u);
}

TEST(ServerMessageDeserialize, StatusByte254RoutesToGameStateAndAccepts) {
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/254u, /*max_pawn=*/0u, make_row({0}));
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(static_cast<uint8_t>(std::get<GameState>(*r).status), 254u);
}

TEST(ServerMessageDeserialize, StatusByte255RoutesToMessageWrong) {
    // Bytes shaped exactly like a MessageWrong (14 bytes, status=255).
    std::vector<uint8_t> bytes(14, 0);
    for (size_t i = 0; i < 12; ++i)
        bytes[i] = static_cast<uint8_t>(i);
    bytes[12] = 255u;
    bytes[13] = 7u;
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<MessageWrong>(*r));
    EXPECT_EQ(std::get<MessageWrong>(*r).error_index, 7u);
}

// ---- Length failures from outer dispatcher ----

TEST(ServerMessageDeserialize, EmptyBufferFailsInvalidLengthZero) {
    std::vector<uint8_t> buf;
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 0u);
}

TEST(ServerMessageDeserialize, SizeOneFailsInvalidLength) {
    std::vector<uint8_t> buf(1, 0);
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 1u);
}

TEST(ServerMessageDeserialize, SizeFiveFailsInvalidLength) {
    std::vector<uint8_t> buf(5, 0);
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 5u);
}

TEST(ServerMessageDeserialize, SizeElevenFailsInvalidLength) {
    std::vector<uint8_t> buf(11, 0);
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 11u);
}

TEST(ServerMessageDeserialize, SizeExactlyTwelveFailsInvalidLength) {
    // Check is `<=`, so size==12 must fail at the outer guard.
    std::vector<uint8_t> buf(12, 0);
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 12u);
}

TEST(ServerMessageDeserialize, SizeThirteenWithStatus255ForwardsAndFailsInner) {
    // 13 bytes passes the outer guard (`<= 12` is false for 13). Status=255
    // routes to deserialize_message_wrong, which requires exactly 14 bytes and
    // returns invalid_length(13).
    std::vector<uint8_t> buf(13, 0);
    buf[12] = 255u;
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 13u);
}

// ---- Error propagation from inner deserializers ----

TEST(ServerMessageDeserialize, GameStateHeaderOkButBitmapTooShortPropagates) {
    // max_pawn=8 requires a 2-byte bitmap. Drop one bitmap byte; the inner
    // deserialize_game_state should fail with invalid_length.
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, 8u, pawn_row_t(9, false)};
    auto bytes = gs.serialize();
    ASSERT_EQ(bytes.size(), 16u);
    bytes.pop_back();  // now 15 bytes, still > 12, status byte unchanged
    auto r = deserialize_server_message(bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    // deserialize_game_state on a mismatched bitmap returns invalid_length
    // with GAME_STATE_HEADER_SIZE (14) as error_index.
    EXPECT_EQ(r.error().error_index(), 14u);
}

TEST(ServerMessageDeserialize, GameStateHeaderOkButBitmapTooLongPropagates) {
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, 0u, make_row({1})};
    auto bytes = gs.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    bytes.push_back(0u);  // extra trailing byte
    auto r = deserialize_server_message(bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 14u);
}

TEST(ServerMessageDeserialize, MessageWrongSizeFifteenPropagates) {
    // 15 bytes, status byte 255 at offset 12 → routes to message_wrong, which
    // requires exactly 14 → propagate invalid_length(15).
    std::vector<uint8_t> buf(15, 0);
    buf[12] = 255u;
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 15u);
}

TEST(ServerMessageDeserialize, MessageWrongSizeLargePropagates) {
    std::vector<uint8_t> buf(100, 0);
    buf[12] = 255u;
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 100u);
}

// ---- Round trips through the unified dispatcher ----

TEST(ServerMessageDeserialize, RoundTripGameStateMaxPawn0) {
    GameState original{0u, 1u, 2u, GameStatus::WAITING_FOR_OPPONENT, 0u, make_row({1})};
    auto r = deserialize_server_message(original.serialize());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_TRUE(game_states_equal(original, std::get<GameState>(*r)));
}

TEST(ServerMessageDeserialize, RoundTripGameStateMaxPawn7) {
    GameState original{5u, 10u, 20u, GameStatus::TURN_A, 7u, make_row({1, 0, 1, 1, 0, 0, 1, 0})};
    auto r = deserialize_server_message(original.serialize());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_TRUE(game_states_equal(original, std::get<GameState>(*r)));
}

TEST(ServerMessageDeserialize, RoundTripGameStateMaxPawn255AllOnes) {
    pawn_row_t row(256, true);
    GameState original{UINT32_MAX, UINT32_MAX, UINT32_MAX, GameStatus::WIN_B, 255u, row};
    auto r = deserialize_server_message(original.serialize());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_TRUE(game_states_equal(original, std::get<GameState>(*r)));
}

TEST(ServerMessageDeserialize, RoundTripMessageWrongTypical) {
    MessageWrong original{};
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        original.client_bytes[i] = static_cast<uint8_t>(i * 11 + 2);
    }
    original.status = MSG_WRONG_STATUS;
    original.error_index = 42u;
    auto r = deserialize_server_message(original.serialize());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<MessageWrong>(*r));
    const auto &got = std::get<MessageWrong>(*r);
    for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
        EXPECT_EQ(got.client_bytes[i], original.client_bytes[i]);
    }
    EXPECT_EQ(got.status, original.status);
    EXPECT_EQ(got.error_index, original.error_index);
}

TEST(ServerMessageDeserialize, RoundTripMessageWrongErrorIndexBoundary) {
    MessageWrong original{};
    for (auto &b : original.client_bytes)
        b = 0;
    original.status = MSG_WRONG_STATUS;
    original.error_index = 255u;
    auto r = deserialize_server_message(original.serialize());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<MessageWrong>(*r));
    EXPECT_EQ(std::get<MessageWrong>(*r).error_index, 255u);
}

// ---- Subtle routing case: handcrafted GameState-sized bytes with status=255 ----

TEST(ServerMessageDeserialize, GameStateSizedBytesWithStatus255RouteToMessageWrongAndReject) {
    // 15 bytes laid out like a valid GameState with max_pawn=0, but we set the
    // status byte at offset 12 to 255. Dispatcher routes to
    // deserialize_message_wrong, which needs exactly 14, so it rejects.
    auto bytes = make_game_state_bytes(1u, 2u, 3u, /*status=*/255u, /*max_pawn=*/0u, make_row({1}));
    ASSERT_EQ(bytes.size(), 15u);
    auto r = deserialize_server_message(bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 15u);
}

TEST(ServerMessageDeserialize, LargeGameStateSizedBytesWithStatus255RouteToMessageWrongAndReject) {
    // max_pawn=15 → bitmap 2 bytes → total 16 bytes. Status forced to 255.
    pawn_row_t row(16, true);
    auto bytes = make_game_state_bytes(0u, 1u, 2u, /*status=*/255u, /*max_pawn=*/15u, row);
    ASSERT_EQ(bytes.size(), 16u);
    auto r = deserialize_server_message(bytes);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
    EXPECT_EQ(r.error().error_index(), 16u);
}
