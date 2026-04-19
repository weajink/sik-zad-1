// Deeper brutal tests for KaylesGame and KaylesGameMap timeout behavior and
// state-machine invariants that aren't covered elsewhere.
//
// Focus:
//   - Timeout comparison asymmetry (check_timeouts with equal A/B timestamps).
//   - The strict > comparison semantics (boundary behavior at exactly timeout).
//   - keep_alive by non-player or player 0 must not mutate anything.
//   - Long playouts at max_pawn=255 — every pawn knocked, then WIN_*.
//   - Move-2 on adjacent pins straddling byte 7/8 boundary.
//   - Map: game_id never regresses, even on stale-removal sweeps.
//   - Map: after a finished game's sweep window, the same game_id is NOT reused.
//   - Map operations against missing and stale-just-removed game_ids.
//   - Join into a finished game does not resurrect it.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct FakeClock : public Clock {
    time_point_t t{};
    time_point_t now() const override { return t; }
    void set(std::chrono::seconds s) { t = time_point_t{} + s; }
    void advance(std::chrono::seconds d) { t += d; }
};

static std::shared_ptr<FakeClock> fake_clock() {
    return std::make_shared<FakeClock>();
}

static pawn_row_t solid(size_t n) { return pawn_row_t(n, true); }

// ===========================================================================
// Timeout-comparison: both silent past timeout
//
// Spec 5.3 says: "the silent player loses." When both are silent past the
// timeout, both satisfy the losing condition and the spec does not specify
// a tie-break. These tests only assert that the game transitions to some
// terminal WIN_* state and does not remain in TURN_*. They deliberately do
// NOT pin which player wins.
// ===========================================================================

TEST(KaylesGameBothSilentPastTimeout, TurnAEqualTimestampsTransitionsToTerminal) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);         // TURN_B, both last = 0
    g.move(2u, 0, 1);            // TURN_A, B-last bumped to 0; A-last = 0 (from ctor)
    clk->set(std::chrono::seconds(100));
    g.check_timeouts(std::chrono::seconds(10));
    auto st = g.get_status();
    EXPECT_TRUE(st == GameStatus::WIN_A || st == GameStatus::WIN_B)
        << "Both silent past timeout on TURN_A must end the game";
}

TEST(KaylesGameBothSilentPastTimeout, TurnBEqualTimestampsTransitionsToTerminal) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);         // TURN_B, both last = 0
    clk->set(std::chrono::seconds(100));
    g.check_timeouts(std::chrono::seconds(10));
    auto st = g.get_status();
    EXPECT_TRUE(st == GameStatus::WIN_A || st == GameStatus::WIN_B)
        << "Both silent past timeout on TURN_B must end the game";
}

// ===========================================================================
// Strict > boundary
// ===========================================================================

TEST(KaylesGameTimeoutBoundary, ExactlyAtTimeoutIsNotYetStale) {
    // now - last == timeout → NOT stale (strict >).
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);  // TURN_B, both 0
    clk->set(std::chrono::seconds(10));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B)
        << "At exactly timeout, game is not yet timed out";
}

TEST(KaylesGameTimeoutBoundary, OneNanoPastTimeoutTriggersLoss) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);  // TURN_B
    clk->t = time_point_t{} + std::chrono::seconds(10) + std::chrono::nanoseconds(1);
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_NE(g.get_status(), GameStatus::TURN_B)
        << "One nanosecond past timeout must be past the strict > boundary";
}

// ===========================================================================
// keep_alive safety
// ===========================================================================

TEST(KaylesGameKeepAlive, KeepAliveByUnknownPlayerDoesNothing) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);
    auto before = g.get_game_state().pawn_row;
    g.keep_alive(9999u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, before);
}

TEST(KaylesGameKeepAlive, KeepAliveByPlayerZeroDoesNothing) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);
    auto before = g.get_game_state().pawn_row;
    g.keep_alive(0u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, before);
}

TEST(KaylesGameKeepAlive, KeepAliveByUnknownPlayerDoesNotResetClock) {
    // If keep_alive(unknown) DID reset a timestamp, it could accidentally keep
    // the game alive past timeout. Verify: an unknown player's keep_alive
    // should not save the game from going stale.
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    g.join_player_b(2u);  // both last = 0 (WAITING->TURN_B)
    clk->set(std::chrono::seconds(50));
    g.keep_alive(9999u);  // unknown — should not bump
    g.check_timeouts(std::chrono::seconds(10));
    // Both silent 50s > 10s timeout → should be finished.
    EXPECT_NE(g.get_status(), GameStatus::TURN_B);
}

// ===========================================================================
// Long playout with max_pawn=255 — exhaustive correctness at extreme size.
// ===========================================================================

TEST(KaylesGameEdge, MaxPawn255AlternatingAllMove1Playout) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 255u, solid(256), clk);
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
    player_id_t who = 2u;
    int moves = 0;
    for (int k = 0; k < 256; ++k) {
        g.move(who, static_cast<size_t>(k), 1);
        ++moves;
        ASSERT_FALSE(g.get_game_state().pawn_row[k])
            << "pawn " << k << " must be knocked";
        if (k == 255) {
            // 256 pins, B started. After 256 moves, who knocked last = B (even count).
            // Move count: B=1,A=2,B=3,...,k+1 move. B moves on odd counts (1,3,5,...).
            // k=255 means the 256th move. 256 is even → A moved last.
            EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
        } else {
            GameStatus expected = (moves % 2 == 1) ? GameStatus::TURN_A : GameStatus::TURN_B;
            ASSERT_EQ(g.get_status(), expected) << "move#=" << moves;
        }
        who = (who == 2u) ? 1u : 2u;
    }
}

TEST(KaylesGameEdge, MaxPawn255Move2At128CrossesByteBoundary) {
    // Pawn 128 is MSB of byte 16; pawn 129 is bit 6 of byte 16. Both in same byte.
    // Test pawn 127 and 128: 127 is LSB of byte 15, 128 is MSB of byte 16.
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 255u, solid(256), clk);
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/127, 2);  // knock 127 & 128
    EXPECT_FALSE(g.get_game_state().pawn_row[127]);
    EXPECT_FALSE(g.get_game_state().pawn_row[128]);
    EXPECT_TRUE(g.get_game_state().pawn_row[126]);
    EXPECT_TRUE(g.get_game_state().pawn_row[129]);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);

    // Confirm bitmap encoding is correct after the straddling move.
    auto bytes = g.get_game_state().serialize();
    ASSERT_EQ(bytes.size(), 14u + 32u);
    // byte 14+15 has pins 120..127. Pin 127 is LSB: was 0x01, now 0x00. The
    // other 7 bits (pins 120..126) are still 1: 0b11111110 = 0xFE.
    EXPECT_EQ(bytes[14 + 15], 0xFEu);
    // byte 14+16 has pins 128..135. Pin 128 is MSB: was 0x80, now 0x00. Pins
    // 129..135 still up: 0b01111111 = 0x7F.
    EXPECT_EQ(bytes[14 + 16], 0x7Fu);
}

// ===========================================================================
// KaylesGameMap — game_id allocation never regresses
// ===========================================================================

TEST(KaylesGameMapIdAllocation, IdMonotonicAcrossFinishedAndStale) {
    // Join many games, finish some, let some stale out. The next game_id must
    // never reuse an old ID.
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_EQ(m.join(1u)->game_id, 0u);
    ASSERT_EQ(m.join(2u)->game_id, 0u);          // B joined game 0
    ASSERT_EQ(m.join(3u)->game_id, 1u);          // new game 1 waiting
    ASSERT_EQ(m.join(4u)->game_id, 1u);          // B joined game 1
    // Game 0: finish via give_up(B).
    ASSERT_TRUE(m.give_up(2u, 0u).has_value());
    // Advance past timeout so game 0 is stale.
    clk->set(std::chrono::seconds(50));
    // Next join triggers stale-removal sweep of game 0, then creates game 2
    // (NEVER reuses id 0).
    auto j = m.join(5u);
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->game_id, 2u) << "Stale game_id must never be reused";

    // Old game 0 must be gone.
    auto probe0 = m.keep_alive(1u, 0u);
    ASSERT_FALSE(probe0.has_value());
    EXPECT_EQ(probe0.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapIdAllocation, AfterJoinFillsWaitingSubsequentJoinAllocatesNewId) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_EQ(m.join(1u)->game_id, 0u);
    ASSERT_EQ(m.join(2u)->game_id, 0u);
    // Now game 0 is TURN_B, not WAITING. Next join must create game 1.
    auto j = m.join(3u);
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->game_id, 1u);
    EXPECT_EQ(j->status, GameStatus::WAITING_FOR_OPPONENT);
}

// ===========================================================================
// Waiting-singleton invariant — only one WAITING game at a time.
// ===========================================================================

TEST(KaylesGameMapWaitingInvariant, AtMostOneWaitingGameAcrossSweep) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_EQ(m.join(1u)->game_id, 0u);  // game 0 waiting
    // Second join must pair up with game 0, not create a second waiting game.
    auto j2 = m.join(2u);
    ASSERT_TRUE(j2.has_value());
    EXPECT_EQ(j2->game_id, 0u);
    EXPECT_EQ(j2->status, GameStatus::TURN_B);
    // Now NO game is waiting. Third join creates a new waiting game.
    auto j3 = m.join(3u);
    ASSERT_TRUE(j3.has_value());
    EXPECT_EQ(j3->game_id, 1u);
    EXPECT_EQ(j3->status, GameStatus::WAITING_FOR_OPPONENT);
    // Fourth join must pair up with game 1 (not game 0).
    auto j4 = m.join(4u);
    ASSERT_TRUE(j4.has_value());
    EXPECT_EQ(j4->game_id, 1u);
    EXPECT_EQ(j4->status, GameStatus::TURN_B);
    EXPECT_EQ(j4->player_b_id, 4u);
}

TEST(KaylesGameMapWaitingInvariant, FreshJoinDoesNotAcceptIntoStaleWaiting) {
    // Waiting game 0 has grown stale. New JOIN must NOT land as B in the dead
    // game — must create game 1.
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_EQ(m.join(1u)->game_id, 0u);
    clk->set(std::chrono::seconds(50));
    auto j = m.join(2u);
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->game_id, 1u)
        << "Stale waiting game must be removed before pairing; new JOIN must make a fresh game";
    EXPECT_EQ(j->player_a_id, 2u);
    EXPECT_EQ(j->player_b_id, 0u);
    EXPECT_EQ(j->status, GameStatus::WAITING_FOR_OPPONENT);
}

// ===========================================================================
// Map: join into a finished game never resurrects it
// ===========================================================================

TEST(KaylesGameMapResurrection, FinishedGameNeverAcceptsNewJoin) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());  // game 0, TURN_B
    ASSERT_TRUE(m.give_up(2u, 0u).has_value());  // WIN_A
    // Now send JOIN again — this must create a NEW waiting game (id 1), not
    // resurrect game 0.
    auto j = m.join(3u);
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->game_id, 1u) << "finished game must not be reopened";
    EXPECT_EQ(j->player_a_id, 3u);
    EXPECT_EQ(j->player_b_id, 0u);
}

// ===========================================================================
// Map: operations on recently-removed game_ids surface INVALID_GAME_ID
// ===========================================================================

TEST(KaylesGameMapRemoval, AllOpsOnStaleRemovedGameFail) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());  // game 0, waiting
    clk->set(std::chrono::seconds(50));
    // Trigger sweep via a new join:
    ASSERT_TRUE(m.join(2u).has_value());
    // game 0 is gone. Every op on game 0 must fail with INVALID_GAME_ID.
    for (auto [op_name, r] : std::initializer_list<std::pair<const char*, std::expected<GameState, KaylesError>>>{
             {"move1", m.move(1u, 0u, 0, 1)},
             {"move2", m.move(1u, 0u, 0, 2)},
             {"keep_alive", m.keep_alive(1u, 0u)},
             {"give_up", m.give_up(1u, 0u)},
         }) {
        ASSERT_FALSE(r.has_value()) << op_name;
        EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID) << op_name;
    }
}

// ===========================================================================
// Map: large max_pawn end-to-end
// ===========================================================================

TEST(KaylesGameMapIntegration, MaxPawn255MoveOnMap) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 255u, solid(256), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());  // TURN_B
    auto r = m.move(2u, 0u, /*pawn=*/128, /*no=*/2);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_A);
    EXPECT_FALSE(r->pawn_row[128]);
    EXPECT_FALSE(r->pawn_row[129]);
    EXPECT_TRUE(r->pawn_row[127]);
    EXPECT_TRUE(r->pawn_row[130]);
}

// ===========================================================================
// Map: move on waiting game returns MSG_GAME_STATE-equivalent expected (not error)
// ===========================================================================

TEST(KaylesGameMapWaiting, MoveOnWaitingGameByMemberIsValidMessageButIllegalMove) {
    // Spec 3.3: message valid → server returns MSG_GAME_STATE, state unchanged.
    // At the Map level, this surfaces as a successful std::expected with
    // unchanged state.
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());  // waiting; no B
    auto r = m.move(1u, 0u, 0, 1);
    ASSERT_TRUE(r.has_value()) << "a valid player in the game must yield a state, not an error";
    EXPECT_EQ(r->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(r->pawn_row, solid(4));
}

// ===========================================================================
// Map: player_id=0 is never a valid member — move/give_up/keep_alive fail
// with INVALID_PLAYER_ID (since no game contains player 0).
// ===========================================================================

TEST(KaylesGameMapPlayerZero, MoveByPlayerZeroFailsInvalidPlayerId) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r = m.move(0u, 0u, 0, 1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapPlayerZero, KeepAliveByPlayerZeroFailsInvalidPlayerId) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r = m.keep_alive(0u, 0u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapPlayerZero, GiveUpByPlayerZeroFailsInvalidPlayerId) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r = m.give_up(0u, 0u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

// ===========================================================================
// Map: keep_alive by a valid player does not mutate pawn_row / status
// ===========================================================================

TEST(KaylesGameMapKeepAlive, KeepAliveByValidPlayerDoesNotMutateRow) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto ka1 = m.keep_alive(1u, 0u);
    ASSERT_TRUE(ka1.has_value());
    EXPECT_EQ(ka1->pawn_row, solid(4));
    EXPECT_EQ(ka1->status, GameStatus::TURN_B);
    auto ka2 = m.keep_alive(2u, 0u);
    ASSERT_TRUE(ka2.has_value());
    EXPECT_EQ(ka2->pawn_row, solid(4));
    EXPECT_EQ(ka2->status, GameStatus::TURN_B);
}

// ===========================================================================
// Long scenario: A and B alternate via Map, ending in WIN_A.
// ===========================================================================

TEST(KaylesGameMapScenario, FullGameEndsInWinA) {
    auto clk = fake_clock();
    // 6 pawns, B starts. Sequence with move_1 each: B,A,B,A,B,A. A takes last → WIN_A.
    KaylesGameMap m(timeout_t{10}, 5u, solid(6), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r1 = m.move(2u, 0u, 0, 1); ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, GameStatus::TURN_A);
    auto r2 = m.move(1u, 0u, 1, 1); ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::TURN_B);
    auto r3 = m.move(2u, 0u, 2, 1); ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->status, GameStatus::TURN_A);
    auto r4 = m.move(1u, 0u, 3, 1); ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->status, GameStatus::TURN_B);
    auto r5 = m.move(2u, 0u, 4, 1); ASSERT_TRUE(r5.has_value());
    EXPECT_EQ(r5->status, GameStatus::TURN_A);
    auto r6 = m.move(1u, 0u, 5, 1); ASSERT_TRUE(r6.has_value());
    EXPECT_EQ(r6->status, GameStatus::WIN_A);
}

// ===========================================================================
// Map/Game: check_timeouts must demote WAITING (spec 5.1 says game is removed,
// not demoted). Verify that WAITING is NOT transitioned to WIN_*; only the
// is_stale removal path should trigger.
// ===========================================================================

TEST(KaylesGameWaitingTimeout, CheckTimeoutsDoesNotAssignWinToWaitingGame) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    clk->set(std::chrono::seconds(1000));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT)
        << "check_timeouts must NOT transition WAITING → WIN_* per spec 5.1";
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)))
        << "is_stale must report true so the map removes the game";
}

// ===========================================================================
// Concurrent games with overlapping player_ids — moves must not cross games
// ===========================================================================

TEST(KaylesGameMapCross, MoveInOneGameByPlayerOfOtherReturnsInvalidPlayerId) {
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());  // g0: A=1
    ASSERT_TRUE(m.join(2u).has_value());  // g0: B=2
    ASSERT_TRUE(m.join(3u).has_value());  // g1: A=3
    ASSERT_TRUE(m.join(4u).has_value());  // g1: B=4
    // 4 is not a member of g0.
    auto r = m.move(4u, /*game_id=*/0u, 0, 1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapCross, MoveByPlayerInMultipleGamesAffectsOnlyTargetedGame) {
    // Player 1 is in BOTH games. A move in g0 must not change g1.
    auto clk = fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, solid(4), clk);
    ASSERT_TRUE(m.join(1u).has_value());       // g0, A=1
    ASSERT_TRUE(m.join(5u).has_value());       // g0, B=5
    ASSERT_TRUE(m.join(1u).has_value());       // g1, A=1 (same player)
    ASSERT_TRUE(m.join(1u).has_value());       // g1, B=1 (same player)
    // g0 TURN_B → player 5 plays. g1 TURN_B → player 1 plays as B.
    ASSERT_TRUE(m.move(5u, 0u, 0, 1).has_value());  // knock pawn 0 in g0
    auto s1 = m.keep_alive(1u, 1u);
    ASSERT_TRUE(s1.has_value());
    // g1 row must still be untouched.
    EXPECT_EQ(s1->pawn_row, solid(4));
    EXPECT_EQ(s1->status, GameStatus::TURN_B);
}

// ===========================================================================
// Timeout-at-construction safety: KaylesGame's constructor does NOT
// initialize player_b_last_move_time. If the FakeClock starts at epoch, the
// game appears "fresh" until someone advances time. Verify no bogus demotion.
// ===========================================================================

TEST(KaylesGameConstruction, FreshGameIsNotImmediatelyStale) {
    auto clk = fake_clock();
    KaylesGame g(0u, 1u, 3u, solid(4), clk);
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}
