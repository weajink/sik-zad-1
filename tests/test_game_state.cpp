// Include the server source directly, renaming main to avoid conflicts.
#define main kayles_server_main
#include "../src/kayles_server.cpp"
#undef main

#include <gtest/gtest.h>

using namespace kayles_game;

// ============================================================
// Tests for get_game_state() serialization.
// Verifies network byte order, bitmap encoding, correct sizes.
// ============================================================

static size_t wire_size(const game_state_t &buf) {
    return sizeof(game_state_t) - kayles_common::MAX_BITMAP_SIZE + (buf.max_pawn / 8 + 1);
}

// --- Buffer size ---

TEST(GameState, BufferSizeMaxPawn0) {
    // max_pawn=0: bitmap = 0/8+1 = 1 byte
    // Total: 4 + 4 + 4 + 1 + 1 + 1 = 15
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(wire_size(buf), 15u);
}

TEST(GameState, BufferSizeMaxPawn7) {
    // max_pawn=7: bitmap = 7/8+1 = 1 byte
    // Total: 4 + 4 + 4 + 1 + 1 + 1 = 15
    pawn_row_t row = {1, 1, 1, 1, 1, 1, 1, 1};
    KaylesGame g(0, 1, 7, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(wire_size(buf), 15u);
}

TEST(GameState, BufferSizeMaxPawn8) {
    // max_pawn=8: bitmap = 8/8+1 = 2 bytes
    // Total: 4 + 4 + 4 + 1 + 1 + 2 = 16
    pawn_row_t row = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    KaylesGame g(0, 1, 8, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(wire_size(buf), 16u);
}

TEST(GameState, BufferSizeMaxPawn15) {
    // max_pawn=15: bitmap = 15/8+1 = 2 bytes
    // Total: 4 + 4 + 4 + 1 + 1 + 2 = 16
    pawn_row_t row(16, true);
    KaylesGame g(0, 1, 15, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(wire_size(buf), 16u);
}

// --- Network byte order for IDs ---

TEST(GameState, GameIdNetworkOrder) {
    pawn_row_t row = {1};
    KaylesGame g(0x01020304, 1, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(ntohl(buf.game_id), 0x01020304u);
}

TEST(GameState, PlayerAIdNetworkOrder) {
    pawn_row_t row = {1};
    KaylesGame g(0, 0xAABBCCDD, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(ntohl(buf.player_a_id), 0xAABBCCDDu);
}

TEST(GameState, PlayerBIdNetworkOrder) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(0x11223344);
    auto buf = g.get_game_state();
    EXPECT_EQ(ntohl(buf.player_b_id), 0x11223344u);
}

TEST(GameState, PlayerBIdZeroBeforeJoin) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(ntohl(buf.player_b_id), 0u);
}

// --- Status byte ---

TEST(GameState, StatusWaiting) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.status, 0);  // WAITING_FOR_OPPONENT
}

TEST(GameState, StatusTurnB) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.status, 2);  // TURN_B
}

TEST(GameState, StatusTurnA) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // B moves, now TURN_A
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.status, 1);  // TURN_A
}

TEST(GameState, StatusWinA) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.give_up(2);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.status, 3);  // WIN_A
}

TEST(GameState, StatusWinB) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // B takes last pawn
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.status, 4);  // WIN_B
}

// --- max_pawn byte ---

TEST(GameState, MaxPawnZero) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.max_pawn, 0);
}

TEST(GameState, MaxPawn255) {
    pawn_row_t row(256, true);
    KaylesGame g(0, 1, 255, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.max_pawn, 255);
}

// --- Bitmap encoding (MSB first) ---

TEST(GameState, BitmapSinglePawnStanding) {
    // max_pawn=0, pawn 0 standing -> bitmap byte: bit 7 set = 0x80
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0x80);  // pawn 0 = MSB of byte 0
}

TEST(GameState, BitmapAllEightPawnsStanding) {
    // max_pawn=7, all 8 pawns standing -> bitmap byte: 0xFF
    pawn_row_t row = {1, 1, 1, 1, 1, 1, 1, 1};
    KaylesGame g(0, 1, 7, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0xFF);
}

TEST(GameState, BitmapMixed) {
    // max_pawn=7, row = 1 0 1 0 1 0 1 0
    // Bitmap: bit7=1, bit6=0, bit5=1, bit4=0, bit3=1, bit2=0, bit1=1, bit0=0 = 0xAA
    pawn_row_t row = {1, 0, 1, 0, 1, 0, 1, 0};
    KaylesGame g(0, 1, 7, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0xAA);
}

TEST(GameState, BitmapExcessBitsZeroed) {
    // max_pawn=2, row = 1 0 1 -> 3 bits used in byte 0
    // Pawn 0 = bit 7, pawn 1 = bit 6, pawn 2 = bit 5
    // Expected: 1 0 1 0 0 0 0 0 = 0xA0
    pawn_row_t row = {1, 0, 1};
    KaylesGame g(0, 1, 2, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0xA0);
}

TEST(GameState, BitmapTwoBytes) {
    // max_pawn=8, 9 pawns: first 8 standing (0xFF), pawn 8 standing (bit 7 of byte 1 = 0x80)
    pawn_row_t row = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    KaylesGame g(0, 1, 8, row);
    auto buf = g.get_game_state();
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0xFF);
    EXPECT_EQ(buf.pawn_row_bitmap[1], 0x80);
}

TEST(GameState, BitmapAfterMove) {
    // Verify bitmap updates after a pawn is knocked down.
    // max_pawn=3, row = 1 1 1 1 -> bitmap byte = 0xF0
    pawn_row_t row = {1, 1, 1, 1};
    KaylesGame g(0, 1, 3, row);
    g.join_player_b(2);
    // B takes pawn 0
    g.move(2, 0, 1);
    auto buf = g.get_game_state();
    // After removing pawn 0: 0 1 1 1 -> 0x70
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0x70);
}

TEST(GameState, BitmapAfterTwoConsecutiveMove) {
    // max_pawn=3, row = 1 1 1 1 -> bitmap byte = 0xF0
    pawn_row_t row = {1, 1, 1, 1};
    KaylesGame g(0, 1, 3, row);
    g.join_player_b(2);
    // B takes pawns 1 and 2
    g.move(2, 1, 2);
    auto buf = g.get_game_state();
    // After removing pawns 1,2: 1 0 0 1 -> bit7=1, bit6=0, bit5=0, bit4=1 = 0x90
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0x90);
}

// --- Full round-trip: game_id, player ids, status, max_pawn, bitmap all correct ---

TEST(GameState, FullRoundTrip) {
    pawn_row_t row = {1, 0, 1, 1};
    KaylesGame g(42, 100, 3, row);
    g.join_player_b(200);

    auto buf = g.get_game_state();

    // game_id
    EXPECT_EQ(ntohl(buf.game_id), 42u);
    // player_a_id
    EXPECT_EQ(ntohl(buf.player_a_id), 100u);
    // player_b_id
    EXPECT_EQ(ntohl(buf.player_b_id), 200u);
    // status = TURN_B (2)
    EXPECT_EQ(buf.status, 2);
    // max_pawn = 3
    EXPECT_EQ(buf.max_pawn, 3);
    // bitmap: 1 0 1 1 -> bit7=1, bit6=0, bit5=1, bit4=1 = 0xB0
    EXPECT_EQ(buf.pawn_row_bitmap[0], 0xB0);
    // Wire size: 4+4+4+1+1+1 = 15
    EXPECT_EQ(wire_size(buf), 15u);
}
