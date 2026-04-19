// Brutal, spec-driven tests for kayles_protocol.h edge cases not covered by
// test_protocol.cpp.
//
// Focus areas:
//   - Every byte offset in every wire-format message is verified at byte level.
//   - Bitmap MSB-first ordering at EVERY pawn index across byte boundaries
//     (including exhaustive coverage at max_pawn=255).
//   - Excess-bit zeroing at every (max_pawn, byte_index) boundary.
//   - MessageWrong echo padding — tail bytes after the source must be zero.
//   - deserialize_server_message boundary behavior (exact SERVER_MESSAGE_STATUS_OFFSET).
//   - Round-trip for every status byte value from 0..4, 100, 254, 255.
//   - Every client message type at the spec's boundary sizes.

#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "kayles_protocol.h"

using namespace kayles::protocol;
using kayles::error::ErrorType;
using kayles::types::game_id_t;
using kayles::types::pawn_row_t;
using kayles::types::pawn_t;
using kayles::types::player_id_t;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t read_u32_be(const std::vector<uint8_t>& buf, size_t offset) {
    return (static_cast<uint32_t>(buf[offset]) << 24) |
           (static_cast<uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<uint32_t>(buf[offset + 2]) << 8) |
           static_cast<uint32_t>(buf[offset + 3]);
}

static pawn_row_t solid_row(size_t n) { return pawn_row_t(n, true); }

// ===========================================================================
// Network byte order: every multi-byte field must be big-endian.
// The tests use values with distinct bytes so that endian-swap bugs are
// immediately visible.
// ===========================================================================

TEST(ProtocolByteOrder, ClientMessageJoinPlayerIdEveryByteIsMSBFirst) {
    for (uint32_t pid : {0x01000000u, 0x00010000u, 0x00000100u, 0x00000001u,
                         0xFF000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu}) {
        ClientMessage m{ClientMessageType::MSG_JOIN, pid, 0u, 0u};
        auto bytes = m.serialize();
        ASSERT_EQ(bytes.size(), 5u);
        EXPECT_EQ(bytes[1], static_cast<uint8_t>((pid >> 24) & 0xFFu)) << "pid=" << pid;
        EXPECT_EQ(bytes[2], static_cast<uint8_t>((pid >> 16) & 0xFFu)) << "pid=" << pid;
        EXPECT_EQ(bytes[3], static_cast<uint8_t>((pid >> 8) & 0xFFu)) << "pid=" << pid;
        EXPECT_EQ(bytes[4], static_cast<uint8_t>(pid & 0xFFu)) << "pid=" << pid;
    }
}

TEST(ProtocolByteOrder, GameStateAllThreeU32FieldsAreBigEndian) {
    // Distinct byte patterns at each field so any cross-field swap shows up.
    GameState gs{0xA0B1C2D3u, 0x01234567u, 0x89ABCDEFu,
                 GameStatus::TURN_A, 0u, {true}};
    auto bytes = gs.serialize();
    ASSERT_GE(bytes.size(), 14u);
    EXPECT_EQ(read_u32_be(bytes, 0), 0xA0B1C2D3u);
    EXPECT_EQ(read_u32_be(bytes, 4), 0x01234567u);
    EXPECT_EQ(read_u32_be(bytes, 8), 0x89ABCDEFu);
    // and exact byte triples
    EXPECT_EQ(bytes[0], 0xA0u);
    EXPECT_EQ(bytes[3], 0xD3u);
    EXPECT_EQ(bytes[4], 0x01u);
    EXPECT_EQ(bytes[7], 0x67u);
    EXPECT_EQ(bytes[8], 0x89u);
    EXPECT_EQ(bytes[11], 0xEFu);
}

TEST(ProtocolByteOrder, DeserializeDoesNotInterpretLittleEndian) {
    // Hand-craft bytes for pid=0x01020304 big-endian.
    std::vector<uint8_t> bytes = {
        /*msg_type=JOIN*/ 0,
        0x01, 0x02, 0x03, 0x04,
    };
    auto r = deserialize_client_message(bytes);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->player_id, 0x01020304u) << "deserialize must read big-endian";
}

// ===========================================================================
// Bitmap MSB-first ordering — exhaustive at every pawn index.
// pawn k lives at byte (k/8), bit (7 - k%8).
// ===========================================================================

TEST(ProtocolBitmap, EverySinglePawnLitAtCorrectBit) {
    // Test max_pawn=31: 4 bitmap bytes. For each pawn position k, set exactly
    // that bit and assert the bitmap matches the spec formula.
    const pawn_t mp = 31;
    for (pawn_t k = 0; k <= mp; ++k) {
        pawn_row_t row(static_cast<size_t>(mp) + 1, false);
        row[k] = true;
        GameState gs{0u, 1u, 2u, GameStatus::TURN_A, mp, row};
        auto bytes = gs.serialize();
        ASSERT_EQ(bytes.size(), 14u + 4u);
        for (size_t b = 0; b < 4; ++b) {
            uint8_t expected = 0;
            if (k / 8 == b) expected = static_cast<uint8_t>(1u << (7 - k % 8));
            EXPECT_EQ(bytes[14 + b], expected)
                << "pawn=" << int(k) << " byte=" << b;
        }
    }
}

TEST(ProtocolBitmap, ExhaustiveSinglePawnLitMaxPawn255) {
    // max_pawn=255 → 32 bitmap bytes. Exhaust every pawn index.
    const pawn_t mp = 255;
    for (int k = 0; k <= 255; ++k) {
        pawn_row_t row(256, false);
        row[k] = true;
        GameState gs{0u, 1u, 2u, GameStatus::TURN_A, mp, row};
        auto bytes = gs.serialize();
        ASSERT_EQ(bytes.size(), 14u + 32u);
        for (size_t b = 0; b < 32; ++b) {
            uint8_t expected = 0;
            if (static_cast<size_t>(k / 8) == b) {
                expected = static_cast<uint8_t>(1u << (7 - k % 8));
            }
            ASSERT_EQ(bytes[14 + b], expected)
                << "pawn=" << k << " byte=" << b;
        }
    }
}

TEST(ProtocolBitmap, ExcessBitsAreZeroed) {
    // For every max_pawn value, if all pins are up, the bitmap must have
    // exactly max_pawn+1 ones and all excess bits zero.
    for (pawn_t mp : {pawn_t{0}, pawn_t{1}, pawn_t{6}, pawn_t{7},
                      pawn_t{8}, pawn_t{9}, pawn_t{15}, pawn_t{16}, pawn_t{23},
                      pawn_t{63}, pawn_t{64}, pawn_t{127}, pawn_t{128},
                      pawn_t{240}, pawn_t{254}, pawn_t{255}}) {
        pawn_row_t row(static_cast<size_t>(mp) + 1, true);
        GameState gs{0u, 1u, 2u, GameStatus::TURN_A, mp, row};
        auto bytes = gs.serialize();
        const size_t bm_size = static_cast<size_t>(mp) / 8 + 1;
        ASSERT_EQ(bytes.size(), 14u + bm_size) << "mp=" << int(mp);

        // Verify every bit position.
        const size_t total_bits = bm_size * 8;
        for (size_t bit = 0; bit < total_bits; ++bit) {
            size_t byte_idx = 14u + bit / 8;
            uint8_t mask = static_cast<uint8_t>(1u << (7 - bit % 8));
            bool actual = (bytes[byte_idx] & mask) != 0;
            bool expected = (bit <= static_cast<size_t>(mp));
            ASSERT_EQ(actual, expected)
                << "mp=" << int(mp) << " bit=" << bit;
        }
    }
}

TEST(ProtocolBitmap, EvenMaxPawnAllZerosMustProduceAllZeroBytes) {
    for (pawn_t mp : {pawn_t{0}, pawn_t{7}, pawn_t{8}, pawn_t{63}, pawn_t{255}}) {
        pawn_row_t row(static_cast<size_t>(mp) + 1, false);
        GameState gs{0u, 1u, 2u, GameStatus::TURN_A, mp, row};
        auto bytes = gs.serialize();
        const size_t bm_size = static_cast<size_t>(mp) / 8 + 1;
        for (size_t b = 0; b < bm_size; ++b) {
            EXPECT_EQ(bytes[14 + b], 0u) << "mp=" << int(mp) << " byte=" << b;
        }
    }
}

// ===========================================================================
// Adjacent-pawn patterns: pins 7 and 8 straddle a byte boundary.
// ===========================================================================

TEST(ProtocolBitmap, Pawn7And8StraddleByteBoundary) {
    // max_pawn=15 → 2 bytes. Only 7 and 8 lit.
    pawn_row_t row(16, false);
    row[7] = row[8] = true;
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, 15u, row};
    auto bytes = gs.serialize();
    ASSERT_EQ(bytes.size(), 14u + 2u);
    // byte 0: only bit for pawn 7 set = 0b00000001 = 0x01.
    EXPECT_EQ(bytes[14], 0x01u);
    // byte 1: only bit for pawn 8 set = 0b10000000 = 0x80.
    EXPECT_EQ(bytes[15], 0x80u);
}

TEST(ProtocolBitmap, BitmapDeserializeRecoversEveryBit) {
    // Deterministic pseudo-random pattern: every 3rd bit set.
    const pawn_t mp = 63;
    pawn_row_t row(64, false);
    for (size_t i = 0; i < 64; ++i) row[i] = (i % 3 == 0);
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, mp, row};
    auto r = deserialize_game_state(gs.serialize());
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->pawn_row.size(), 64u);
    for (size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(r->pawn_row[i], (i % 3 == 0)) << "bit " << i;
    }
}

// ===========================================================================
// Client message size sanity (spec 3.2 sizes)
// ===========================================================================

TEST(ProtocolSize, JoinExactlyFiveBytes) {
    EXPECT_EQ(get_client_message_size(ClientMessageType::MSG_JOIN), 5u);
}

TEST(ProtocolSize, Move1ExactlyTenBytes) {
    EXPECT_EQ(get_client_message_size(ClientMessageType::MSG_MOVE_1), 10u);
}

TEST(ProtocolSize, Move2ExactlyTenBytes) {
    EXPECT_EQ(get_client_message_size(ClientMessageType::MSG_MOVE_2), 10u);
}

TEST(ProtocolSize, KeepAliveExactlyNineBytes) {
    EXPECT_EQ(get_client_message_size(ClientMessageType::MSG_KEEP_ALIVE), 9u);
}

TEST(ProtocolSize, GiveUpExactlyNineBytes) {
    EXPECT_EQ(get_client_message_size(ClientMessageType::MSG_GIVE_UP), 9u);
}

// ===========================================================================
// deserialize_client_message: sweep every length 0..15 for each type.
// Only the exact expected size should succeed.
// ===========================================================================

TEST(ProtocolDeserialize, SweepAllLengthsForJoin) {
    for (size_t n = 0; n < 16; ++n) {
        std::vector<uint8_t> buf(n, 0);
        // msg_type = 0 (JOIN) iff we put 0 at byte 0; but empty buffer fails before.
        if (n > 0) buf[0] = 0;
        auto r = deserialize_client_message(buf);
        if (n == 5) {
            EXPECT_TRUE(r.has_value()) << "length 5 must succeed for JOIN";
        } else {
            EXPECT_FALSE(r.has_value())
                << "length " << n << " must fail for JOIN";
        }
    }
}

TEST(ProtocolDeserialize, SweepAllLengthsForMove1) {
    for (size_t n = 0; n < 16; ++n) {
        std::vector<uint8_t> buf(n, 0);
        if (n > 0) buf[0] = 1;
        auto r = deserialize_client_message(buf);
        if (n == 10) {
            EXPECT_TRUE(r.has_value()) << "length 10 must succeed for MOVE_1";
        } else {
            EXPECT_FALSE(r.has_value())
                << "length " << n << " must fail for MOVE_1";
        }
    }
}

TEST(ProtocolDeserialize, SweepAllLengthsForMove2) {
    for (size_t n = 0; n < 16; ++n) {
        std::vector<uint8_t> buf(n, 0);
        if (n > 0) buf[0] = 2;
        auto r = deserialize_client_message(buf);
        if (n == 10) {
            EXPECT_TRUE(r.has_value()) << "length 10 must succeed for MOVE_2";
        } else {
            EXPECT_FALSE(r.has_value())
                << "length " << n << " must fail for MOVE_2";
        }
    }
}

TEST(ProtocolDeserialize, SweepAllLengthsForKeepAlive) {
    for (size_t n = 0; n < 16; ++n) {
        std::vector<uint8_t> buf(n, 0);
        if (n > 0) buf[0] = 3;
        auto r = deserialize_client_message(buf);
        if (n == 9) {
            EXPECT_TRUE(r.has_value()) << "length 9 must succeed for KEEP_ALIVE";
        } else {
            EXPECT_FALSE(r.has_value())
                << "length " << n << " must fail for KEEP_ALIVE";
        }
    }
}

TEST(ProtocolDeserialize, SweepAllLengthsForGiveUp) {
    for (size_t n = 0; n < 16; ++n) {
        std::vector<uint8_t> buf(n, 0);
        if (n > 0) buf[0] = 4;
        auto r = deserialize_client_message(buf);
        if (n == 9) {
            EXPECT_TRUE(r.has_value()) << "length 9 must succeed for GIVE_UP";
        } else {
            EXPECT_FALSE(r.has_value())
                << "length " << n << " must fail for GIVE_UP";
        }
    }
}

TEST(ProtocolDeserialize, SweepMsgType5To255AllReject) {
    // msg_type >= 5 must be rejected regardless of length.
    for (int t = 5; t <= 255; ++t) {
        for (size_t n : {size_t{1}, size_t{5}, size_t{9}, size_t{10}, size_t{12}}) {
            std::vector<uint8_t> buf(n, 0);
            buf[0] = static_cast<uint8_t>(t);
            auto r = deserialize_client_message(buf);
            ASSERT_FALSE(r.has_value())
                << "msg_type=" << t << " len=" << n;
            EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_ARGUMENT)
                << "msg_type=" << t;
            EXPECT_EQ(r.error().error_index(), 0u);
        }
    }
}

// ===========================================================================
// deserialize_server_message discriminator at exactly offset 12
// ===========================================================================

TEST(ProtocolDispatch, BoundaryLengthExactly13Fails) {
    // 13 bytes: length > offset (12), but neither deserialize_game_state nor
    // deserialize_message_wrong accepts a size-13 buffer.
    // status byte = 0 (routes to game_state) → needs ≥14 → fail.
    std::vector<uint8_t> buf(13, 0);
    auto r = deserialize_server_message(buf);
    ASSERT_FALSE(r.has_value());
}

TEST(ProtocolDispatch, Status255ExactSize14Succeeds) {
    // status=255 at offset 12, total size 14 → valid MessageWrong.
    std::vector<uint8_t> buf(14, 0);
    buf[12] = MSG_WRONG_STATUS;
    buf[13] = 99;
    auto r = deserialize_server_message(buf);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<MessageWrong>(*r));
    EXPECT_EQ(std::get<MessageWrong>(*r).error_index, 99u);
}

// ===========================================================================
// MessageWrong echo — simulate the server's behavior of zero-padding short
// client messages into the 12-byte client_bytes field.
// ===========================================================================

TEST(MessageWrongEchoPadding, ShortClientDataAndZeroTail) {
    // If client sent only 5 bytes (e.g., a malformed JOIN), bytes 5..11 must be zero.
    MessageWrong w{};
    uint8_t src[5] = {0xFE, 0x01, 0x02, 0x03, 0x04};
    std::memset(w.client_bytes, 0, sizeof(w.client_bytes));
    std::memcpy(w.client_bytes, src, 5);
    w.status = MSG_WRONG_STATUS;
    w.error_index = 0;

    auto bytes = w.serialize();
    ASSERT_EQ(bytes.size(), 14u);
    for (size_t i = 0; i < 5; ++i) EXPECT_EQ(bytes[i], src[i]);
    for (size_t i = 5; i < 12; ++i) EXPECT_EQ(bytes[i], 0u)
        << "tail byte " << i << " must be zero";
    EXPECT_EQ(bytes[12], 255u);
    EXPECT_EQ(bytes[13], 0u);
}

// ===========================================================================
// Exhaustive round-trip for every enum status (including invalid bytes).
// GameStatus enum values 0..4; deserialize_game_state does NOT validate the
// status byte, so values 5..254 survive the round trip.
// ===========================================================================

TEST(ProtocolStatusRoundTrip, Values0Through254RoundTrip) {
    for (int s = 0; s <= 254; ++s) {
        // Build a raw 15-byte buffer: game_id=0, player_a=0, player_b=0,
        // status=s, max_pawn=0, 1 byte bitmap=0x00.
        std::vector<uint8_t> buf(15, 0);
        buf[12] = static_cast<uint8_t>(s);
        // (max_pawn stays 0; bitmap byte stays 0.)
        auto r = deserialize_game_state(buf);
        ASSERT_TRUE(r.has_value()) << "status=" << s;
        EXPECT_EQ(static_cast<uint8_t>(r->status), s);
    }
}

// ===========================================================================
// All-ones max_pawn=255 GameState bytes exactly — bitmap is 32×0xFF.
// ===========================================================================

TEST(ProtocolGameStateWire, MaxPawn255AllOnesBitmap) {
    pawn_row_t row(256, true);
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, 255u, row};
    auto bytes = gs.serialize();
    ASSERT_EQ(bytes.size(), 14u + 32u);
    for (size_t b = 0; b < 32; ++b) {
        ASSERT_EQ(bytes[14 + b], 0xFFu) << "byte " << b;
    }
}

TEST(ProtocolGameStateWire, MaxPawn255AlternatingBitmap) {
    pawn_row_t row(256, false);
    for (size_t i = 0; i < 256; i += 2) row[i] = true;
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, 255u, row};
    auto bytes = gs.serialize();
    ASSERT_EQ(bytes.size(), 14u + 32u);
    // Each byte encodes 8 bits starting at bit 0 (pawn 8b, bit 7) ... bit 7 (pawn 8b+7, bit 0).
    // Even pawns are 1, odd are 0. Within each byte, bits for even pawns at bit
    // positions 7, 5, 3, 1 (from MSB): so pattern 0b10101010 = 0xAA.
    for (size_t b = 0; b < 32; ++b) {
        ASSERT_EQ(bytes[14 + b], 0xAAu) << "byte " << b;
    }
}

// ===========================================================================
// A 12-byte MessageWrong echo must preserve ANY byte value, including 0xFF.
// ===========================================================================

TEST(MessageWrongSerialize, AllBytes0xFFPreservedAndStatusStillAppended) {
    MessageWrong w{};
    for (auto& b : w.client_bytes) b = 0xFFu;
    w.status = 255u;
    w.error_index = 7u;
    auto bytes = w.serialize();
    ASSERT_EQ(bytes.size(), 14u);
    for (size_t i = 0; i < 12; ++i) EXPECT_EQ(bytes[i], 0xFFu);
    EXPECT_EQ(bytes[12], 255u);
    EXPECT_EQ(bytes[13], 7u);
}

// ===========================================================================
// Invalid msg_type via boundary values
// ===========================================================================

TEST(ProtocolDeserialize, MsgTypeExactly5Rejected) {
    std::vector<uint8_t> buf = {5, 0, 0, 0, 1};  // exact size for JOIN
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_ARGUMENT);
    EXPECT_EQ(r.error().error_index(), 0u);
}

// ===========================================================================
// A peculiar case: a client sends 10 bytes with msg_type=0.
// The parser checks "size vs expected" — must reject as invalid_length.
// ===========================================================================

TEST(ProtocolDeserialize, JoinMsgTypeButTenByteBodyRejected) {
    std::vector<uint8_t> buf(10, 0);
    buf[0] = 0;  // JOIN but oversized body → must be rejected as length mismatch
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ProtocolDeserialize, Move1MsgTypeButFiveByteBodyRejected) {
    std::vector<uint8_t> buf(5, 0);
    buf[0] = 1;  // MOVE_1 but only 5 bytes → rejected
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

TEST(ProtocolDeserialize, KeepAliveMsgTypeButTenByteBodyRejected) {
    std::vector<uint8_t> buf(10, 0);
    buf[0] = 3;  // KEEP_ALIVE but 10 bytes (MOVE size) → rejected
    auto r = deserialize_client_message(buf);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_MESSAGE_LENGTH);
}

// ===========================================================================
// Round-trip serialization through deserialize_server_message for every
// GameStatus value (sanity).
// ===========================================================================

TEST(ServerMessageDispatch, RoundTripEveryStatusEnum) {
    for (auto st : {GameStatus::WAITING_FOR_OPPONENT, GameStatus::TURN_A,
                    GameStatus::TURN_B, GameStatus::WIN_A, GameStatus::WIN_B}) {
        GameState gs{0u, 1u, 2u, st, 3u, {true, false, true, true}};
        auto bytes = gs.serialize();
        auto r = deserialize_server_message(bytes);
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(std::holds_alternative<GameState>(*r));
        EXPECT_EQ(std::get<GameState>(*r).status, st);
    }
}

// ===========================================================================
// "Aligned" assertion of SERVER_MESSAGE_STATUS_OFFSET
// ===========================================================================

TEST(ProtocolConstants, StatusOffsetIs12) {
    EXPECT_EQ(SERVER_MESSAGE_STATUS_OFFSET, 12u)
        << "Spec 3.3: the MSG_WRONG_MSG status byte is at offset 12 (after "
           "12 echo bytes).";
    EXPECT_EQ(CLIENT_MESSAGE_SIZE_WITH_BUF, 12u);
}

TEST(ProtocolConstants, MsgWrongStatusIs255) {
    EXPECT_EQ(MSG_WRONG_STATUS, 255u);
}

// ===========================================================================
// A MessageWrong whose status byte is NOT 255 is not a MessageWrong;
// the dispatcher must route based on byte 12.
// ===========================================================================

TEST(ServerMessageDispatch, StatusByte5InDeserializeDoesNotPanic) {
    // Build bytes of size > 14 so the dispatcher picks game_state; confirm no
    // crash, but the resulting game status is whatever byte 12 says.
    pawn_row_t row(16, true);
    GameState gs{0u, 1u, 2u, GameStatus::TURN_A, 15u, row};
    auto bytes = gs.serialize();
    bytes[12] = 5u;  // invalid GameStatus but still non-255
    auto r = deserialize_server_message(bytes);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<GameState>(*r));
    EXPECT_EQ(static_cast<uint8_t>(std::get<GameState>(*r).status), 5u);
}
