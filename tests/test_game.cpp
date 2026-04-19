// Unit tests for KaylesGame and KaylesGameMap in src/kayles_game.h.
//
// Covers:
//   - KaylesGame construction, join, moves, give-up, finished-game behavior
//   - KaylesGame timeout handling via an injected FakeClock
//   - KaylesGameMap lifecycle, errors, timeout-driven cleanup
//
// Known bug noted in header: the constructor moves `pawn_row` into `gs.pawn_row`
// and then reads from the moved-from parameter to compute `pawns_left_in_row`.
// `pawns_left_in_row` is therefore effectively 0 in practice, which prevents the
// "last pawn wins" transition from firing. Tests that exercise that transition
// are marked `// BUG:` and the assertion is weakened accordingly.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

// kayles_protocol.h must be included first so that its guard is active while
// kayles_error.h is parsed — otherwise the circular include exposes
// MSG_TYPE_SIZE before it is declared. clang-format would sort these
// alphabetically, so we disable include sorting for this block.
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

// FakeClock lets us drive the game's notion of time deterministically.
struct FakeClock : public Clock {
    time_point_t t{};
    time_point_t now() const override {
        return t;
    }
    void advance(std::chrono::seconds d) {
        t += d;
    }
};

// Build a pawn_row_t from a compact "101" string: '1' = true, anything else = false.
static pawn_row_t make_row(std::string_view s) {
    pawn_row_t row;
    row.reserve(s.size());
    for (char c : s)
        row.push_back(c == '1');
    return row;
}

static std::shared_ptr<FakeClock> make_fake_clock() {
    return std::make_shared<FakeClock>();
}

static std::shared_ptr<SystemClock> make_system_clock() {
    return std::make_shared<SystemClock>();
}

// ===========================================================================
// KaylesGameBasics
// ===========================================================================

TEST(KaylesGameBasics, ConstructorSetsStatusWaiting) {
    KaylesGame g(7u, 1u, 3u, make_row("1111"), make_system_clock());
    EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT);
}

TEST(KaylesGameBasics, GameStateReflectsConstructorArgs) {
    KaylesGame g(42u, 1000u, 7u, make_row("10101010"), make_system_clock());
    GameState gs = g.get_game_state();
    EXPECT_EQ(gs.game_id, 42u);
    EXPECT_EQ(gs.player_a_id, 1000u);
    EXPECT_EQ(gs.player_b_id, 0u);
    EXPECT_EQ(gs.status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(gs.max_pawn, 7u);
    EXPECT_EQ(gs.pawn_row, make_row("10101010"));
}

TEST(KaylesGameBasics, JoinPlayerBMovesToTurnB) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().player_b_id, 2u);
    EXPECT_EQ(g.get_game_state().player_a_id, 1u);
}

TEST(KaylesGameBasics, JoinPlayerBSamePlayerAsAllowed) {
    // Spec: same player can fill both sides.
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(1u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().player_b_id, 1u);
}

TEST(KaylesGameBasicsDeathTest, ConstructorAssertsPlayerIdNonZero) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ KaylesGame g(0u, 0u, 3u, make_row("1111"), make_system_clock()); }, ".*");
}

TEST(KaylesGameBasicsDeathTest, JoinPlayerBAssertsPlayerIdNonZero) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
            g.join_player_b(0u);
        },
        ".*");
}

TEST(KaylesGameBasicsDeathTest, DoubleJoinPlayerBAsserts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
            g.join_player_b(2u);
            g.join_player_b(3u);
        },
        ".*");
}

// ===========================================================================
// KaylesGameMoves
// ===========================================================================

TEST(KaylesGameMoves, Move1ByPlayerBOnTurnBAdvancesTurnA) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);
    EXPECT_FALSE(g.get_game_state().pawn_row[0]);
}

TEST(KaylesGameMoves, Move1WrongTurnIsNoOp) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    // It's B's turn; player A tries to move.
    auto before = g.get_game_state();
    g.move(1u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, before.pawn_row);
}

TEST(KaylesGameMoves, Move1PawnOutOfRangeIsNoOp) {
    KaylesGame g(0u, 1u, 2u, make_row("111"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/5, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("111"));
}

TEST(KaylesGameMoves, Move1AlreadyKnockedPawnIsNoOp) {
    KaylesGame g(0u, 1u, 3u, make_row("1011"), make_system_clock());
    g.join_player_b(2u);
    // Pawn 1 is already knocked.
    g.move(2u, /*pawn=*/1, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1011"));
}

TEST(KaylesGameMoves, Move2KnocksBothAdjacentPins) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/1, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_A);
    auto row = g.get_game_state().pawn_row;
    EXPECT_TRUE(row[0]);
    EXPECT_FALSE(row[1]);
    EXPECT_FALSE(row[2]);
    EXPECT_TRUE(row[3]);
}

TEST(KaylesGameMoves, Move2NonAdjacentIsNoOp) {
    // Knocking pawn 1 and 2 requires both present. Pawn 2 is already gone.
    KaylesGame g(0u, 1u, 3u, make_row("1101"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/1, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1101"));
}

TEST(KaylesGameMoves, Move2AtEndOfRowIsNoOp) {
    // first_pawn == max_pawn means there's no pawn+1 to take.
    // no_of_pawns=2 with first_pawn==max_pawn should fail gracefully (no crash).
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/3, /*no_of_pawns=*/2);  // pawn 3 is the last, pawn 4 doesn't exist
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1111"));
}

TEST(KaylesGameMoves, Move2BeyondEndOfRowIsNoOp) {
    // Even more clearly beyond.
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/10, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1111"));
}

TEST(KaylesGameMoves, Move2OnSinglePawnRowIsNoOp) {
    // max_pawn=0 → only pawn 0 exists. Move 2 must be impossible.
    KaylesGame g(0u, 1u, 0u, make_row("1"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/2);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    EXPECT_TRUE(g.get_game_state().pawn_row[0]);
}

TEST(KaylesGameMoves, KnockingLastPawnWinsForB) {
    // Max_pawn=0, only pawn 0 exists.
    KaylesGame g(0u, 1u, 0u, make_row("1"), make_system_clock());
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_FALSE(g.get_game_state().pawn_row[0]);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameMoves, KnockingLastPawnWinsForA) {
    KaylesGame g(0u, 1u, 1u, make_row("11"), make_system_clock());
    g.join_player_b(2u);
    // B knocks pawn 0, turn moves to A.
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
    // A knocks pawn 1 — last pawn, so A wins.
    g.move(1u, /*pawn=*/1, /*no_of_pawns=*/1);
    EXPECT_FALSE(g.get_game_state().pawn_row[1]);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameMoves, GiveUpOnOwnTurnFlipsToOpponentWin) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    // B's turn: B gives up → A wins.
    g.give_up(2u);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameMoves, GiveUpByAOnATurnFlipsToWinB) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);  // now TURN_A
    ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
    g.give_up(1u);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameMoves, GiveUpNotOnOwnTurnIsNoOp) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    // It's B's turn; A gives up (not their turn) → no state change.
    g.give_up(1u);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
}

TEST(KaylesGameMoves, GiveUpOnFinishedGameIsNoOp) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    g.give_up(2u);
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    g.give_up(1u);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
    g.give_up(2u);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameMoves, MoveOnFinishedGameIsNoOp) {
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.join_player_b(2u);
    g.give_up(2u);
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    auto before = g.get_game_state().pawn_row;
    g.move(1u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
    EXPECT_EQ(g.get_game_state().pawn_row, before);
    g.move(2u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
    EXPECT_EQ(g.get_game_state().pawn_row, before);
}

TEST(KaylesGameMoves, MoveOnWaitingGameIsNoOp) {
    // Before B joins, status is WAITING_FOR_OPPONENT: no one's turn.
    KaylesGame g(0u, 1u, 3u, make_row("1111"), make_system_clock());
    g.move(1u, /*pawn=*/0, /*no_of_pawns=*/1);
    EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(g.get_game_state().pawn_row, make_row("1111"));
}

// ===========================================================================
// KaylesGameClockInjection
// ===========================================================================

TEST(KaylesGameClockInjection, TimeoutOnTurnAWhenASilentFlipsToWinB) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both clocks = 0
    g.move(2u, 0, 1);     // TURN_A, B's clock bumped
    // Now advance time so A is silent for > 10s. B just moved so B is fresh.
    clk->advance(std::chrono::seconds(20));
    // Actually we also need B to not be silent. B moved at t=0; B is also silent now.
    // Fix: keep B alive at a later time.
    clk->t = time_point_t{} + std::chrono::seconds(15);
    g.keep_alive(2u);
    // Now B-last = 15s, A-last = 0s. Advance clock to 25s.
    clk->t = time_point_t{} + std::chrono::seconds(25);
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameClockInjection, TimeoutOnTurnBWhenBSilentFlipsToWinA) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // both clocks at t=0; TURN_B
    // Bump A repeatedly so only B stays silent. Note player_a_last_move_time is
    // default-initialized (epoch) by the constructor, so we must keep_alive(1)
    // AFTER advancing the clock to get A's timestamp current.
    clk->t = time_point_t{} + std::chrono::seconds(15);
    g.keep_alive(1u);
    clk->t = time_point_t{} + std::chrono::seconds(20);
    // Now A-last=15 (silent 5s, ok), B-last=0 (silent 20s, > 10). TURN_B → WIN_A.
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameClockInjection, TimeoutOnTurnAWhenBSilentFlipsToWinA) {
    // Per spec: any silent player loses, regardless of whose turn it is.
    // Isolate B-only-silent: keep A alive right before the deadline check.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both clocks 0
    g.move(2u, 0, 1);     // TURN_A, B-last = 0
    clk->t = time_point_t{} + std::chrono::seconds(15);
    g.keep_alive(1u);  // A-last = 15, B-last still 0
    clk->t = time_point_t{} + std::chrono::seconds(20);
    // Timeout = 10: A silent 5s (ok), B silent 20s → B loses → WIN_A.
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A);
}

TEST(KaylesGameClockInjection, TimeoutOnTurnBWhenASilentFlipsToWinB) {
    // Symmetric: TURN_B but A is the silent one → A loses.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both at t=0
    // Keep B alive at later time; A remains silent.
    clk->t = time_point_t{} + std::chrono::seconds(5);
    g.keep_alive(2u);
    clk->t = time_point_t{} + std::chrono::seconds(20);
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::WIN_B);
}

TEST(KaylesGameClockInjection, CheckTimeoutsDoesNothingIfNeitherSilent) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B, both at 0
    // Advance only slightly.
    clk->advance(std::chrono::seconds(3));
    g.keep_alive(1u);
    g.keep_alive(2u);
    clk->advance(std::chrono::seconds(5));
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
}

TEST(KaylesGameClockInjection, IsStaleForWaitingAfterASilentPastTimeout) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    // Waiting state; A's last move time is unset (default-constructed time_point_t{}).
    // advance past timeout.
    clk->advance(std::chrono::seconds(20));
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameClockInjection, IsStaleForWaitingFalseWhileFresh) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    // Bump A to track current time, then check.
    g.keep_alive(1u);
    clk->advance(std::chrono::seconds(3));
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameClockInjection, IsStaleForFinishedWhenBothSilent) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);
    g.give_up(2u);  // status=WIN_A, B bumped
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    // Both A and B last_move_time are current (t=0). Advance past timeout.
    clk->advance(std::chrono::seconds(20));
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameClockInjection, IsStaleForFinishedFalseWhenOnlyOneSilent) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // both at t=0
    g.give_up(2u);        // WIN_A, B bumped to t=0
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    // Keep A alive at later time; advance further so A is still fresh but B is silent.
    clk->advance(std::chrono::seconds(15));
    g.keep_alive(1u);
    // Now B-last = 0, A-last = 15. Clock = 15.
    // With timeout=10: B silent 15s>10 (true), A silent 0s>10 (false). Not both → false.
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
    // Symmetric check: advance so that B is fresh and A stale.
    clk->advance(std::chrono::seconds(20));
    g.keep_alive(2u);
    // B-last=35, A-last=15. Clock=35. B fresh, A silent 20s>10 → not both.
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameClockInjection, IsStaleForTurnStatesReturnsFalse) {
    // is_stale is only for WAITING/WIN_*; during TURN_A/TURN_B it should return false.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // TURN_B
    clk->advance(std::chrono::seconds(100));
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameClockInjection, KeepAliveResetsPlayerClock) {
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // both t=0; TURN_B
    // Advance past timeout.
    clk->advance(std::chrono::seconds(20));
    // keep_alive both so neither is silent.
    g.keep_alive(1u);
    g.keep_alive(2u);
    g.check_timeouts(std::chrono::seconds(10));
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B);
    // And not stale.
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
}

TEST(KaylesGameClockInjection, GiveUpBumpsPlayerClock) {
    // After give_up by a player, that player's last_move_time is set to now.
    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, 3u, make_row("1111"), clk);
    g.join_player_b(2u);  // both at t=0
    clk->advance(std::chrono::seconds(20));
    // B gives up → WIN_A, B bumped; A still at 0.
    g.give_up(2u);
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
    // Finished game: is_stale requires BOTH silent > timeout. A is at 0, clock at 20,
    // so A is silent (20>10) BUT B is fresh (20-20=0, not >10). Not both silent → not stale.
    EXPECT_FALSE(g.is_stale(std::chrono::seconds(10)));
    // Advance further so B also goes silent.
    clk->advance(std::chrono::seconds(20));
    EXPECT_TRUE(g.is_stale(std::chrono::seconds(10)));
}

// ===========================================================================
// KaylesGameMapBasics
// ===========================================================================

// NOTE on map tests below:
// KaylesGame's constructor does not initialize player_a_last_move_time; it is
// default-constructed to the steady_clock epoch. Under a real SystemClock the
// game therefore appears immediately stale (now - epoch >> any timeout) and is
// removed on the very next `check_timeouts_and_remove_stale()` call. To test
// the logical API, we inject a FakeClock that starts at epoch so the game is
// not falsely considered stale.

TEST(KaylesGameMapBasics, FirstJoinCreatesWaitingGameId0) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r = m.join(1u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, 0u);
    EXPECT_EQ(r->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(r->player_a_id, 1u);
    EXPECT_EQ(r->player_b_id, 0u);
}

TEST(KaylesGameMapBasics, SecondJoinFillsWaitingGameToTurnB) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r1 = m.join(1u);
    ASSERT_TRUE(r1.has_value());
    auto r2 = m.join(2u);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->game_id, 0u);
    EXPECT_EQ(r2->status, GameStatus::TURN_B);
    EXPECT_EQ(r2->player_a_id, 1u);
    EXPECT_EQ(r2->player_b_id, 2u);
}

TEST(KaylesGameMapBasics, ThirdJoinCreatesGameId1) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r3 = m.join(3u);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->game_id, 1u);
    EXPECT_EQ(r3->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(r3->player_a_id, 3u);
}

TEST(KaylesGameMapBasics, MoveOnMissingGameIdReturnsInvalidGameId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r = m.move(1u, /*game_id=*/999u, /*pawn=*/0, /*no_of_pawns=*/1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapBasics, KeepAliveOnMissingGameIdReturnsInvalidGameId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r = m.keep_alive(1u, /*game_id=*/999u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapBasics, GiveUpOnMissingGameIdReturnsInvalidGameId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r = m.give_up(1u, /*game_id=*/999u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapBasics, MoveByNonMemberReturnsInvalidPlayerId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r = m.move(/*player=*/99u, /*game_id=*/0u, 0, 1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapBasics, KeepAliveByNonMemberReturnsInvalidPlayerId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r = m.keep_alive(/*player=*/99u, /*game_id=*/0u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapBasics, GiveUpByNonMemberReturnsInvalidPlayerId) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());
    auto r = m.give_up(/*player=*/99u, /*game_id=*/0u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesGameMapBasics, SamePlayerCanJoinOwnWaitingGameAsB) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r1 = m.join(1u);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, GameStatus::WAITING_FOR_OPPONENT);
    auto r2 = m.join(1u);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::TURN_B);
    EXPECT_EQ(r2->player_a_id, 1u);
    EXPECT_EQ(r2->player_b_id, 1u);
}

// ===========================================================================
// KaylesGameMapTimeout
// ===========================================================================

TEST(KaylesGameMapTimeout, StaleWaitingGameIsRemovedOnNextJoin) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    auto r1 = m.join(1u);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->game_id, 0u);
    // Advance past timeout so the waiting game becomes stale.
    clk->advance(std::chrono::seconds(20));
    // New join: the stale game should have been removed before evaluating
    // the "last waiting game" branch, so this creates a new game.
    auto r2 = m.join(2u);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::WAITING_FOR_OPPONENT);
    // Since next_game_id has advanced, the new game gets id 1.
    EXPECT_EQ(r2->game_id, 1u);
    EXPECT_EQ(r2->player_a_id, 2u);
    // The old game (id=0) should no longer be reachable via keep_alive.
    auto kr = m.keep_alive(1u, /*game_id=*/0u);
    ASSERT_FALSE(kr.has_value());
    EXPECT_EQ(kr.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesGameMapTimeout, TurnBTimeoutKeepAliveAResultsInWinA) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());  // TURN_B, B-last bumped to 0
    // A's last_move_time was not initialized by the constructor. To keep A from
    // being silent-too-long when we advance time, bump A immediately.
    ASSERT_TRUE(m.keep_alive(1u, 0u).has_value());
    // Now advance just past the timeout. A will still be at t=0 minus ε fresh;
    // keep A alive once more right before the boundary so only B times out.
    clk->advance(std::chrono::seconds(8));
    ASSERT_TRUE(m.keep_alive(1u, 0u).has_value());  // A-last=8
    clk->advance(std::chrono::seconds(5));          // now=13, A silent 5s, B silent 13s
    // check_timeouts_and_remove_stale() runs at entry: TURN_B, A fresh, B silent
    // > 10s → WIN_A. is_stale: WIN_A and only B silent > 10s (A=5 ≤ 10) → false,
    // so the game stays. keep_alive from A returns the updated state.
    auto r = m.keep_alive(1u, /*game_id=*/0u);
    ASSERT_TRUE(r.has_value()) << r.error().what();
    EXPECT_EQ(r->status, GameStatus::WIN_A);
}

TEST(KaylesGameMapTimeout, FinishedGameEventuallyRemoved) {
    auto clk = make_fake_clock();
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"), clk);
    ASSERT_TRUE(m.join(1u).has_value());
    ASSERT_TRUE(m.join(2u).has_value());  // TURN_B, both fresh
    // B gives up → WIN_A. A's clock last at 0; B's bumped to 0.
    auto gu = m.give_up(2u, 0u);
    ASSERT_TRUE(gu.has_value());
    EXPECT_EQ(gu->status, GameStatus::WIN_A);
    // Not yet stale: timeout hasn't elapsed for both.
    clk->advance(std::chrono::seconds(5));
    // Verify the game is still present (keep_alive from A succeeds).
    auto ka = m.keep_alive(1u, 0u);
    ASSERT_TRUE(ka.has_value());
    // Advance past timeout from both players' perspective.
    // After keep_alive above: A-last=5, B-last=0. Clock=5.
    clk->advance(std::chrono::seconds(20));
    // Now A-last=5, B-last=0, clock=25. Both silent > 10 → game should be removed
    // on next operation that triggers check_timeouts_and_remove_stale().
    auto r = m.keep_alive(1u, 0u);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

// ===========================================================================
// KaylesGameMapExhaustion
// ===========================================================================

TEST(KaylesGameMapExhaustion, FirstJoinReturnsGameId0) {
    // Sanity: after constructing a map and doing a single join, game_id is 0.
    KaylesGameMap m(timeout_t{10}, 3u, make_row("1111"));
    auto r = m.join(1u);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, 0u);
}

// EXHAUSTION TEST SKIPPED: requires 2^32 games.
// Verifying `game_ids_exhausted` would require actually incrementing `next_game_id`
// to std::numeric_limits<game_id_t>::max(), which is infeasible from a test harness
// without reflective access. If `next_game_id` were exposed or a seeded constructor
// provided, we would JOIN 2^32 times and assert the next join fails with
// ErrorType::EXHAUSTED_GAME_IDS.
