// Additional unit tests for KaylesGame and KaylesGameMap.
//
// This file complements test_game.cpp: it targets nastier edge cases the first
// file did not cover — max_pawn boundaries (0 and 255), bitmap edges, full
// alternating play-throughs, two-pawn winning moves, give-up semantics in
// every state, finer timeout-flip rules (any-silent-loses), stale-removal
// guarantees, concurrent games in the Map, and a smoke round-trip through
// GameState::serialize / deserialize_game_state.
//
// None of these tests should have counterparts in test_game.cpp. If any do,
// it's a regression on the spec coverage and the suite names are deliberately
// disjoint ("2" suffix) so the binaries can coexist.
//
// Tests that currently fail are marked explicitly in the test body. We do NOT
// adjust assertions to paper over production bugs.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

// kayles_protocol.h must be included first so that its guard is active while
// kayles_error.h is parsed — otherwise the circular include exposes
// MSG_TYPE_SIZE before it is declared.
// clang-format off
#include "kayles_protocol.h"
#include "kayles_game.h"
#include "kayles_error.h"
// clang-format on

using namespace kayles::game;
using namespace kayles::protocol;
using namespace kayles::error;
using kayles::clock::Clock;
using kayles::clock::SystemClock;
using kayles::types::game_id_t;
using kayles::types::pawn_row_t;
using kayles::types::pawn_t;
using kayles::types::player_id_t;
using kayles::types::time_point_t;
using kayles::types::timeout_t;

// ===========================================================================
// Test helpers
// ===========================================================================

struct FakeClock : public Clock {
    time_point_t t{};
    time_point_t now() const override {
        return t;
    }
    void advance(std::chrono::seconds d) {
        t += d;
    }
    void set(std::chrono::seconds s) {
        t = time_point_t{} + s;
    }
};

static pawn_row_t make_row(std::string_view s) {
    pawn_row_t row;
    row.reserve(s.size());
    for (char c : s)
        row.push_back(c == '1');
    return row;
}

// Build a solid row of length `len` (all pins up).
static pawn_row_t solid_row(size_t len) {
    return pawn_row_t(len, true);
}

static std::shared_ptr<FakeClock> make_fake_clock() {
    return std::make_shared<FakeClock>();
}

// ===========================================================================
// KaylesGameEdge — boundary inputs (max_pawn=0, max_pawn=255, gaps)
// ===========================================================================

TEST(KaylesGameEdge, MaxPawn0SinglePinImmediateWinForB) {
    // max_pawn = 0 means one pin total. B takes it → WIN_B immediately.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/0u, make_row("1"), clk);
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
    EXPECT_FALSE(g.get_game_state().pawn_row[0]);
}

TEST(KaylesGameEdge, MaxPawn0Move2IsRejectedNoUnderflow) {
    // Single-pin row: move_2 at pawn 0 would need pawn 1, which is out of range.
    // Must not crash, must not mutate state.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/0u, make_row("1"), clk);
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_TRUE(g.get_game_state().pawn_row[0]);
}

TEST(KaylesGameEdge, MaxPawn255Move1AtLastPawn) {
    // 256 pins; B knocks pawn 255. Row still has 255 pins → TURN_A.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/255u, solid_row(256), clk);
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/255, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);
    auto row = g.get_game_state().pawn_row;
    ASSERT_EQ(row.size(), 256u);
    EXPECT_FALSE(row[255]);
    EXPECT_TRUE(row[254]);
    EXPECT_TRUE(row[0]);
}

TEST(KaylesGameEdge, MaxPawn255Move2AtPenultimateBoundary) {
    // first_pawn=254 consumes pawn 254 and 255 (the exact last edge).
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/255u, solid_row(256), clk);
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/254, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);
    auto row = g.get_game_state().pawn_row;
    EXPECT_FALSE(row[254]);
    EXPECT_FALSE(row[255]);
    EXPECT_TRUE(row[253]);
}

TEST(KaylesGameEdge, MaxPawn255Move2AtFirstPawnEqualsMaxPawnIsNoOp) {
    // first_pawn == max_pawn == 255: would need pawn 256 which is out of range.
    // Must return false with no underflow and no state change.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/255u, solid_row(256), clk);
    g.join_player_b(2u);
    auto before = g.get_game_state().pawn_row;
    g.move(2u, /*pawn=*/255, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, before);
}

TEST(KaylesGameEdge, PawnRowWithGapsMoveIntoGapIsNoOp) {
    // Row "10101" — pawns at 0, 2, 4 only. Trying to knock pawn 1 must fail.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/4u, make_row("10101"), clk);
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/1, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("10101"));
}

TEST(KaylesGameEdge, PawnRowWithGapsMove2BetweenNonAdjacentIsNoOp) {
    // Row "10101" — there is no pair of adjacent pins. Any move_2 must fail.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/4u, make_row("10101"), clk);
    g.join_player_b(2u);
    for (size_t p = 0; p <= 4; ++p) {
        g.move(2u, p, /*no_of_pawns=*/2);
        ASSERT_EQ(g.get_status(), GameStatus::TURN_B)
            << "pawn " << p << " unexpectedly succeeded as move_2";
        ASSERT_EQ(g.get_game_state().pawn_row, make_row("10101"))
            << "pawn " << p << " mutated the row";
    }
}

TEST(KaylesGameEdge, PawnRowSinglePinAtEndKnockWins) {
    // Row "0001" — only pawn 3 is present. B knocks it → WIN_B.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("0001"), clk);
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/3, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

// ===========================================================================
// KaylesGameMoves2 — multi-step sequences, move-not-by-player
// ===========================================================================

TEST(KaylesGameMoves2, AlternatingSinglePawnTurnsUntilEmpty) {
    // 5 pawns, B starts. Turn order: B, A, B, A, B. B takes the last → WIN_B.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/4u, make_row("11111"), clk);
    g.join_player_b(2u);

    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
    g.move(2u, 0, 1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);
    g.move(1u, 1, 1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    g.move(2u, 2, 1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);
    g.move(1u, 3, 1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    g.move(2u, 4, 1);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);

    // Row fully empty.
    auto row = g.get_game_state().pawn_row;
    for (size_t i = 0; i < row.size(); ++i)
        EXPECT_FALSE(row[i]) << "pawn " << i << " not knocked";
}

TEST(KaylesGameMoves2, AlternatingParityFavorsA) {
    // 4 pawns. Turn order: B, A, B, A. A takes last → WIN_A.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.move(2u, 0, 1);  // TURN_A
    g.move(1u, 1, 1);  // TURN_B
    g.move(2u, 2, 1);  // TURN_A
    g.move(1u, 3, 1);  // WIN_A
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameMoves2, TwoPawnMoveEmptiesRowWinsForMover) {
    // 2 pawns. B's turn. Move_2 at 0 empties it → WIN_B.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/1u, make_row("11"), clk);
    g.join_player_b(2u);
    g.move(2u, 0, 2);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
    auto row = g.get_game_state().pawn_row;
    EXPECT_FALSE(row[0]);
    EXPECT_FALSE(row[1]);
}

TEST(KaylesGameMoves2, TwoPawnMoveLastTwoWinsForA) {
    // 4 pawns. Sequence: B move_1 at 0 (TURN_A), A move_1 at 1 (TURN_B),
    // B move_1 at 2 (TURN_A), A move_2 at... no — only pawn 3 is left. Use a
    // cleaner path: 4 pawns, B move_2 at 0 (TURN_A, two pawns left),
    // A move_2 at 2 → WIN_A.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.move(2u, 0, 2);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
    g.move(1u, 2, 2);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameMoves2, IllegalMoveDoesNotAdvanceTurn) {
    // Each illegal move variant must leave status at TURN_B with the row
    // unchanged. Pawn 0 is already knocked; out-of-range; and move_2 at
    // the last slot which has no neighbor.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("0111"), clk);
    g.join_player_b(2u);

    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);  // pawn 0 already knocked
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("0111"));

    g.move(2u, /*pawn=*/99, /*no_of_pawns=*/1);  // out-of-range
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("0111"));

    g.move(2u, /*pawn=*/3, /*no_of_pawns=*/2);  // first_pawn == max_pawn, no neighbor
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("0111"));

    // Non-adjacent move_2: pawn 0 is knocked, so move_2 at pawn 0 needs pawn 0 & 1.
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("0111"));
}

TEST(KaylesGameMoves2, MoveByNonPlayerIgnoredAtGameLayer) {
    // KaylesGame::move itself only checks "is it my turn". A player who is
    // neither A nor B will never pass check_if_my_turn, so the move is a no-op.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.move(/*player_id=*/999u, 0, 1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1111"));
}

TEST(KaylesGameMoves2, MoveOnWaitingGameByPlayerAIsNoOp) {
    // Waiting: no player's turn. A sending a move is ignored.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.move(1u, 0, 1);
    EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1111"));
}

// ===========================================================================
// KaylesGameGiveUp2 — give-up in states not covered in test_game.cpp
// ===========================================================================

TEST(KaylesGameGiveUp2, GiveUpInWaitingStateIsNoOp) {
    // WAITING_FOR_OPPONENT: neither player is on-turn, so give_up does nothing.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.give_up(1u);
    EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT);
    // game should still be joinable as B.
    g.join_player_b(2u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
}

TEST(KaylesGameGiveUp2, GiveUpByNonPlayerIsNoOp) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.give_up(/*player_id=*/99u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
}

TEST(KaylesGameGiveUp2, GiveUpTwiceIsIdempotent) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.give_up(2u);
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    g.give_up(2u);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
    g.give_up(1u);  // finished game; A can't flip anything either
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameGiveUp2, GiveUpByAOnATurnFlipsToWinB) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.move(2u, 0, 1);  // TURN_A
    ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
    g.give_up(1u);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameGiveUp2, GiveUpByAOnBTurnIsNoOp) {
    // It's B's turn. A can't give up because it's not A's move.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.give_up(1u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
}

// ===========================================================================
// KaylesGameTimeouts2 — finer timeout-flip and is_stale rules
// ===========================================================================

TEST(KaylesGameTimeouts2, TurnA_ASilent_FlipsToWinB) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both clocks at 0
    g.move(2u, 0, 1);     // TURN_A, B-last bumped to 0
    // A never sends anything. Keep B alive right up to the check.
    clk->set(std::chrono::seconds(15));
    g.keep_alive(2u);  // B-last = 15
    clk->set(std::chrono::seconds(25));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameTimeouts2, TurnB_BSilent_FlipsToWinA) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B
    clk->set(std::chrono::seconds(15));
    g.keep_alive(1u);  // A-last = 15, B-last still 0
    clk->set(std::chrono::seconds(25));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameTimeouts2, TurnA_OnlyBSilent_FlipsToWinA) {
    // Spec 5.3: any silent player loses, regardless of whose turn it is.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.move(2u, 0, 1);  // TURN_A, B-last = 0
    clk->set(std::chrono::seconds(15));
    g.keep_alive(1u);  // A-last = 15 (fresh)
    clk->set(std::chrono::seconds(20));
    // A silent 5s (ok), B silent 20s → B loses → WIN_A.
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameTimeouts2, TurnB_OnlyASilent_FlipsToWinB) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both 0
    clk->set(std::chrono::seconds(5));
    g.keep_alive(2u);  // B-last = 5
    clk->set(std::chrono::seconds(20));
    // A silent 20s (stale), B silent 15s (also stale). A is more stale, so
    // even without the any-silent-loses tiebreak this should pick A-loses.
    // But more importantly: use a scenario where ONLY A is silent.
    // We need B fresh and A stale. Re-do:
    g.keep_alive(2u);  // B-last = 20
    clk->set(std::chrono::seconds(25));
    // A silent 25s, B silent 5s. Only A silent past timeout=10 → WIN_B.
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameTimeouts2, KeepAliveResetsClockPreventsTimeout) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both 0
    // Advance close to timeout, keep_alive, then check.
    clk->set(std::chrono::seconds(9));
    g.keep_alive(1u);
    g.keep_alive(2u);
    clk->set(std::chrono::seconds(15));
    // Both last_move = 9; now = 15; silent 6s each < 10s.
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
}

TEST(KaylesGameTimeouts2, CheckTimeoutsOnFinishedGameIsNoOp) {
    // A finished game (WIN_A / WIN_B) must not be demoted by check_timeouts.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.give_up(2u);
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    clk->set(std::chrono::seconds(10000));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameTimeouts2, CheckTimeoutsOnWaitingIsNoOp) {
    // WAITING_FOR_OPPONENT: check_timeouts should NOT set WIN_A/WIN_B.
    // (The game is removed via is_stale, not demoted via check_timeouts.)
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    clk->set(std::chrono::seconds(1000));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT);
}

TEST(KaylesGameTimeouts2, IsStaleWaitingTrueWhenASilent) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    // A's clock is set to now() at construction (0). Advance past timeout.
    clk->set(std::chrono::seconds(20));
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameTimeouts2, IsStaleWaitingFalseWhenAFresh) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    clk->set(std::chrono::seconds(5));
    g.keep_alive(1u);
    clk->set(std::chrono::seconds(10));
    // A silent 5s, timeout 10s → not stale.
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameTimeouts2, IsStaleFinishedRequiresBothSilent) {
    // Spec 5.3: finished game retained for server_timeout seconds from the
    // moment of the last valid message from EITHER player. Equivalently,
    // the game becomes stale only when the most recent of the two
    // last_move_times is itself older than server_timeout — i.e. BOTH
    // players have been silent longer than server_timeout.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.give_up(2u);  // WIN_A; B's clock bumped to 0, A's still at 0
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);

    // Advance 20s, keep A fresh: A-last=20, B-last=0. Only B silent → not stale.
    clk->set(std::chrono::seconds(20));
    g.keep_alive(1u);
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));

    // Advance to t=25. A silent 5s, B silent 25s. Still only B. Not stale.
    clk->set(std::chrono::seconds(25));
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));

    // Advance to t=35. A silent 15s > 10, B silent 35s > 10. Now both silent.
    clk->set(std::chrono::seconds(35));
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameTimeouts2, IsStaleTurnStatesAlwaysFalse) {
    // While a game is in progress (TURN_A/TURN_B), it is never stale in the
    // removal sense. check_timeouts is the mechanism that transitions an
    // active game to a finished one; only finished games can become stale.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B
    clk->set(std::chrono::seconds(1000));
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
    g.move(2u, 0, 1);  // TURN_A
    clk->set(std::chrono::seconds(2000));
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameTimeouts2, IsStaleBoundaryStrictlyGreater) {
    // The predicate uses `now - last > timeout` (strict). So at exactly
    // == timeout the game is NOT yet stale.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, /*max_pawn=*/3u, make_row("1111"), clk);
    clk->t = time_point_t{} + std::chrono::seconds(10);
    // A-last = 0, now = 10, delta = 10s == timeout → not stale.
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
    clk->t = time_point_t{} + std::chrono::seconds(10) + std::chrono::nanoseconds(1);
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)));
}

// ===========================================================================
// KaylesGameMapEdge — map-level edge cases
// ===========================================================================

TEST(KaylesGameMapEdge, MoveKeepAliveGiveUpOnMissingGameIdAllReturnInvalidGameId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/3u, make_row("1111"), clk);
    auto r1 = m.move(1u, /*game_id=*/42u, 0, 1);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().type(), ErrorType::INVALID_GAME_ID);

    auto r2 = m.keep_alive(1u, /*game_id=*/42u);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().type(), ErrorType::INVALID_GAME_ID);

    auto r3 = m.give_up(1u, /*game_id=*/42u);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapEdge, OperationsByNonMemberAllReturnInvalidPlayerId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r1 = m.move(/*player=*/99u, 0u, 0, 1);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().type(), ErrorType::INVALID_PLAYER_ID);
    auto r2 = m.keep_alive(/*player=*/99u, 0u);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().type(), ErrorType::INVALID_PLAYER_ID);
    auto r3 = m.give_up(/*player=*/99u, 0u);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapEdge, SamePlayerBothSidesCanPlay) {
    // Spec: "Gracz może grać jako obaj gracze w rozgrywce."
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/1u, make_row("11"), clk);
    auto r1 = m.join(7u);
    ASSERT_TRUE(r1.has_value());
    auto r2 = m.join(7u);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::TURN_B);
    EXPECT_EQ(r2->player_a_id, 7u);
    EXPECT_EQ(r2->player_b_id, 7u);
    // Player 7 as B takes pawn 0 → TURN_A; same player 7 now as A.
    auto r3 = m.move(7u, 0u, 0, 1);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->status, GameStatus::TURN_A);
    auto r4 = m.move(7u, 0u, 1, 1);
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->status, GameStatus::WIN_A);
}

TEST(KaylesGameMapEdge, TwoConcurrentGamesIsolated) {
    // Four players: {1,2} in game 0; {3,4} in game 1. Moves in one must not
    // leak into the other.
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    ASSERT_TRUE(m.join(3u).has_value());
    ASSERT_TRUE(m.join(4u).has_value());

    // Game 0: B=2 knocks pawn 0. Game 1: B=4 knocks pawn 3.
    auto r0 = m.move(2u, /*game_id=*/0u, /*pawn=*/0, /*no=*/1);
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(r0->status, GameStatus::TURN_A);
    EXPECT_EQ(r0->pawn_row, make_row("0111"));

    auto r1 = m.move(4u, /*game_id=*/1u, /*pawn=*/3, /*no=*/1);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, GameStatus::TURN_A);
    EXPECT_EQ(r1->pawn_row, make_row("1110"));

    // Game 0's state must be untouched by the game-1 move.
    auto s0 = m.keep_alive(1u, 0u);
    ASSERT_TRUE(s0.has_value());
    EXPECT_EQ(s0->pawn_row, make_row("0111"));

    // And vice versa.
    auto s1 = m.keep_alive(3u, 1u);
    ASSERT_TRUE(s1.has_value());
    EXPECT_EQ(s1->pawn_row, make_row("1110"));

    // Crossover: player from game 0 operating on game 1 must get INVALID_PLAYER_ID.
    auto cross = m.move(1u, /*game_id=*/1u, 0, 1);
    ASSERT_FALSE(cross.has_value());
    EXPECT_EQ(cross.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapEdge, ErrorFactoryExistsForGameIdsExhausted) {
    // Full exhaustion (2^32 games) is infeasible to exercise; but the error
    // factory and its type tag must be wired correctly.
    auto e = KaylesError::game_ids_exhausted();
    EXPECT_EQ(e.type(), ErrorType::EXHAUSTED_GAME_IDS);
    EXPECT_FALSE(e.what().empty());
}

// ===========================================================================
// KaylesGameMapTimeouts2 — stale-removal invariants
// ===========================================================================

TEST(KaylesGameMapTimeouts2, StaleWaitingGameRemovedAndIdNotReused) {
    // Game 0 stales out; next join must not reuse id 0.
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/3u, make_row("1111"), clk);
    auto r1 = m.join(1u);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->game_id, 0u);

    clk->set(std::chrono::seconds(20));
    // Any op now should evict game 0. Issue a join; it must create a new
    // game with id 1 — NOT reuse id 0.
    auto r2 = m.join(2u);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(r2->game_id, 1u) << "game_id 0 must not be reused after eviction";
    EXPECT_EQ(r2->player_a_id, 2u);

    // Old game_id 0 is gone.
    auto check = m.keep_alive(1u, 0u);
    ASSERT_FALSE(check.has_value());
    EXPECT_EQ(check.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapTimeouts2, MultipleStaleGamesAllRemovedOnSingleSweep) {
    // Start several waiting games in sequence (each waits until the next B
    // joins) — but to keep this simple, make them all finished+stale: join
    // two pairs, then give_up both, then advance past timeout.
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());  // game 0: A=1, B=2, TURN_B
    ASSERT_TRUE(m.join(3u).has_value());
    ASSERT_TRUE(m.join(4u).has_value());  // game 1: A=3, B=4, TURN_B

    ASSERT_TRUE(m.give_up(2u, 0u).has_value());  // game 0 → WIN_A
    ASSERT_TRUE(m.give_up(4u, 1u).has_value());  // game 1 → WIN_A

    // Now advance so BOTH players in each game have been silent > timeout.
    // give_up bumped B but not A; A-last = 0 for both games. Advance so
    // A-last stays at 0 and now = 50, B-last = 0 as well (both at t=0).
    clk->set(std::chrono::seconds(50));

    // One sweep must remove both.
    auto r = m.join(5u);  // triggers check_timeouts_and_remove_stale
    ASSERT_TRUE(r.has_value());
    // New game should be id 2 (not reusing 0 or 1).
    EXPECT_EQ(r->game_id, 2u);
    // Old game_ids 0 and 1 must be gone.
    auto k0 = m.keep_alive(1u, 0u);
    ASSERT_FALSE(k0.has_value());
    EXPECT_EQ(k0.error().type(), ErrorType::INVALID_GAME_ID);
    auto k1 = m.keep_alive(3u, 1u);
    ASSERT_FALSE(k1.has_value());
    EXPECT_EQ(k1.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapTimeouts2, FreshWaitingGameNotRemovedOnSweep) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, /*max_pawn=*/3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    // Still within timeout.
    clk->set(std::chrono::seconds(5));
    // A operation triggers sweep but game is fresh, not removed.
    auto ka = m.keep_alive(1u, 0u);
    ASSERT_TRUE(ka.has_value());
    EXPECT_EQ(ka->status, GameStatus::WAITING_FOR_OPPONENT);
    // Subsequent join should become B, same game_id.
    auto r = m.join(2u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, 0u);
    EXPECT_EQ(r->status, GameStatus::TURN_B);
}

// ===========================================================================
// KaylesGameRoundTrip — GameState serialize ↔ deserialize smoke test
// ===========================================================================

TEST(KaylesGameRoundTrip, GameStateSerializationIsLosslessMidGame) {
    auto clk = make_fake_clock();
    KaylesGame g(/*game_id=*/42u, /*player_a=*/7u, /*max_pawn=*/7u, make_row("11011101"), clk);
    g.join_player_b(8u);
    g.move(8u, 0, 1);  // knock pawn 0 → TURN_A, row "01011101"

    const GameState& gs = g.get_game_state();
    auto bytes = gs.serialize();
    // Expected size: 4 + 4 + 4 + 1 + 1 + (max_pawn/8 + 1) = 14 + 1 = 15 bytes.
    EXPECT_EQ(bytes.size(), 4u + 4u + 4u + 1u + 1u + (7u / 8 + 1));

    auto round = deserialize_game_state(bytes);
    ASSERT_TRUE(round.has_value()) << round.error().what();
    EXPECT_EQ(round->game_id, 42u);
    EXPECT_EQ(round->player_a_id, 7u);
    EXPECT_EQ(round->player_b_id, 8u);
    EXPECT_EQ(round->status, GameStatus::TURN_A);
    EXPECT_EQ(round->max_pawn, 7u);
    EXPECT_EQ(round->pawn_row, make_row("01011101"));
}

TEST(KaylesGameRoundTrip, GameStateSerializationLosslessMaxPawn255) {
    auto clk = make_fake_clock();
    pawn_row_t row = solid_row(256);
    KaylesGame g(/*game_id=*/0xDEADBEEFu, /*player_a=*/1u, /*max_pawn=*/255u, row, clk);
    g.join_player_b(2u);
    // Knock a pawn in the middle of the row to make the bitmap non-trivial.
    g.move(2u, /*pawn=*/128, /*no_of_pawns=*/1);

    const GameState& gs = g.get_game_state();
    auto bytes = gs.serialize();
    // 14 header + 32 bitmap bytes.
    EXPECT_EQ(bytes.size(), 14u + 32u);

    auto round = deserialize_game_state(bytes);
    ASSERT_TRUE(round.has_value());
    EXPECT_EQ(round->game_id, 0xDEADBEEFu);
    EXPECT_EQ(round->max_pawn, 255u);
    ASSERT_EQ(round->pawn_row.size(), 256u);
    EXPECT_FALSE(round->pawn_row[128]);
    EXPECT_TRUE(round->pawn_row[127]);
    EXPECT_TRUE(round->pawn_row[129]);
    EXPECT_TRUE(round->pawn_row[0]);
    EXPECT_TRUE(round->pawn_row[255]);
}

TEST(KaylesGameRoundTrip, GameStateSerializationLosslessMaxPawn0) {
    auto clk = make_fake_clock();
    KaylesGame g(/*game_id=*/1u, /*player_a=*/1u, /*max_pawn=*/0u, make_row("1"), clk);
    const GameState& gs = g.get_game_state();
    auto bytes = gs.serialize();
    // 14 header + 1 bitmap byte.
    EXPECT_EQ(bytes.size(), 15u);
    auto round = deserialize_game_state(bytes);
    ASSERT_TRUE(round.has_value());
    EXPECT_EQ(round->max_pawn, 0u);
    ASSERT_EQ(round->pawn_row.size(), 1u);
    EXPECT_TRUE(round->pawn_row[0]);
    // Verify top bit of first byte is the pawn_0 bit (MSB ordering).
    EXPECT_EQ(bytes[14] & 0x80u, 0x80u);
}
