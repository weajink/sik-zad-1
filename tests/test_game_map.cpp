// Include the server source directly, renaming main to avoid conflicts.
#define main kayles_server_main
#include "../src/kayles_server.cpp"
#undef main

#include <gtest/gtest.h>

using namespace kayles_game;

// ============================================================
// Tests for KaylesGameMap.
// ============================================================

// Helper: simple pawn row "111" (3 pins, max_pawn=2)
static pawn_row_t make_row_3() {
    return {true, true, true};
}

// Helper: single pin row "1" (max_pawn=0)
static pawn_row_t make_row_1() {
    return {true};
}

// --- 1. Join: first creates WAITING, second transitions to TURN_B, third creates new game ---

TEST(KaylesGameMap, JoinFirstCreatesWaiting) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto state = gm.join(1).value();
    EXPECT_EQ(ntohl(state.game_id), 0u);
    EXPECT_EQ(ntohl(state.player_a_id), 1u);
    EXPECT_EQ(ntohl(state.player_b_id), 0u);
    EXPECT_EQ(state.status, 0);  // WAITING_FOR_OPPONENT
}

TEST(KaylesGameMap, JoinSecondTransitionsToTurnB) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    auto state = gm.join(2).value();
    EXPECT_EQ(ntohl(state.game_id), 0u);
    EXPECT_EQ(ntohl(state.player_a_id), 1u);
    EXPECT_EQ(ntohl(state.player_b_id), 2u);
    EXPECT_EQ(state.status, 2);  // TURN_B
}

TEST(KaylesGameMap, JoinThirdCreatesNewGame) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);
    auto state = gm.join(3).value();
    EXPECT_EQ(ntohl(state.game_id), 1u);
    EXPECT_EQ(ntohl(state.player_a_id), 3u);
    EXPECT_EQ(ntohl(state.player_b_id), 0u);
    EXPECT_EQ(state.status, 0);  // WAITING_FOR_OPPONENT
}

// --- 2. Sequential game IDs starting from 0 ---

TEST(KaylesGameMap, SequentialGameIds) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto s0 = gm.join(1).value();
    EXPECT_EQ(ntohl(s0.game_id), 0u);

    gm.join(2);  // fills game 0

    auto s1 = gm.join(3).value();
    EXPECT_EQ(ntohl(s1.game_id), 1u);

    gm.join(4);  // fills game 1

    auto s2 = gm.join(5).value();
    EXPECT_EQ(ntohl(s2.game_id), 2u);
}

// --- 3. Invalid game_id returns INVALID_GAME_ID ---

TEST(KaylesGameMap, MoveInvalidGameId) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto result = gm.move(1, 999, 0, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_GAME_ID);
}

TEST(KaylesGameMap, KeepAliveInvalidGameId) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto result = gm.keep_alive(1, 999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_GAME_ID);
}

TEST(KaylesGameMap, GiveUpInvalidGameId) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto result = gm.give_up(1, 999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_GAME_ID);
}

// --- 4. Valid game but wrong player_id returns INVALID_PLAYER_ID ---

TEST(KaylesGameMap, MoveWrongPlayerId) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);
    auto result = gm.move(99, 0, 0, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_PLAYER_ID);
}

TEST(KaylesGameMap, KeepAliveWrongPlayerId) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);
    auto result = gm.keep_alive(99, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_PLAYER_ID);
}

TEST(KaylesGameMap, GiveUpWrongPlayerId) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);
    auto result = gm.give_up(99, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_PLAYER_ID);
}

// --- 5. Valid move updates status ---

TEST(KaylesGameMap, ValidMoveUpdatesStatus) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);  // status = TURN_B

    // Player B makes a valid move (knock pawn 0)
    auto result = gm.move(2, 0, 0, 1);
    ASSERT_TRUE(result.has_value());
    // After B moves, it should be A's turn
    EXPECT_EQ(result.value().status, 1);  // TURN_A
}

TEST(KaylesGameMap, ValidMoveTwoPawns) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);  // status = TURN_B

    // Player B knocks pawns 0 and 1
    auto result = gm.move(2, 0, 0, 2);
    ASSERT_TRUE(result.has_value());
    // After B moves, it should be A's turn (one pawn left)
    EXPECT_EQ(result.value().status, 1);  // TURN_A
}

TEST(KaylesGameMap, WinByKnockingLastPawn) {
    // Single pin row: "1", max_pawn=0
    KaylesGameMap gm(60, 0, make_row_1());
    gm.join(1);
    gm.join(2);  // status = TURN_B

    // Player B knocks the only pawn
    auto result = gm.move(2, 0, 0, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().status, 4);  // WIN_B
}

// --- 6. Give up changes status correctly ---

TEST(KaylesGameMap, GiveUpPlayerBWinsA) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);  // status = TURN_B

    auto result = gm.give_up(2, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().status, 3);  // WIN_A (B gave up)
}

TEST(KaylesGameMap, GiveUpPlayerAWinsB) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);  // status = TURN_B

    // B moves first so it becomes A's turn
    gm.move(2, 0, 0, 1);

    auto result = gm.give_up(1, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().status, 4);  // WIN_B (A gave up)
}

TEST(KaylesGameMap, GiveUpNotYourTurnDoesNotChangeStatus) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);  // status = TURN_B

    // Player A gives up but it's B's turn — should not change status
    auto result = gm.give_up(1, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().status, 2);  // still TURN_B
}

// --- 7. Keep alive returns current state without changing it ---

TEST(KaylesGameMap, KeepAliveDoesNotChangeState) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    auto state_before = gm.join(2).value();  // TURN_B

    auto result = gm.keep_alive(2, 0);
    ASSERT_TRUE(result.has_value());
    auto state_after = result.value();

    EXPECT_EQ(ntohl(state_after.game_id), ntohl(state_before.game_id));
    EXPECT_EQ(ntohl(state_after.player_a_id), ntohl(state_before.player_a_id));
    EXPECT_EQ(ntohl(state_after.player_b_id), ntohl(state_before.player_b_id));
    EXPECT_EQ(state_after.status, state_before.status);
    EXPECT_EQ(state_after.max_pawn, state_before.max_pawn);
}

TEST(KaylesGameMap, KeepAlivePlayerAAlsoWorks) {
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);

    auto result = gm.keep_alive(1, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().status, 2);  // still TURN_B
}

// --- 8. Multiple games coexist ---

TEST(KaylesGameMap, MultipleGamesCoexist) {
    KaylesGameMap gm(60, 2, make_row_3());

    // Game 0: players 1 and 2
    gm.join(1);
    gm.join(2);

    // Game 1: players 3 and 4
    gm.join(3);
    gm.join(4);

    // Play in game 0: player B (id=2) moves
    auto r0 = gm.move(2, 0, 0, 1);
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(ntohl(r0.value().game_id), 0u);
    EXPECT_EQ(r0.value().status, 1);  // TURN_A

    // Play in game 1: player B (id=4) moves
    auto r1 = gm.move(4, 1, 1, 1);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(ntohl(r1.value().game_id), 1u);
    EXPECT_EQ(r1.value().status, 1);  // TURN_A

    // Games are independent: check game 0 state via keep_alive
    auto check0 = gm.keep_alive(1, 0);
    ASSERT_TRUE(check0.has_value());
    EXPECT_EQ(check0.value().status, 1);  // TURN_A in game 0

    // Cross-game player check: player 1 should not be in game 1
    auto cross = gm.move(1, 1, 0, 1);
    ASSERT_FALSE(cross.has_value());
    EXPECT_EQ(cross.error(), KaylesGameError::INVALID_PLAYER_ID);
}

// --- 9. Same player on both sides ---

TEST(KaylesGameMap, SamePlayerBothSides) {
    KaylesGameMap gm(60, 0, make_row_1());

    gm.join(42);
    auto state = gm.join(42).value();  // same player joins as B
    EXPECT_EQ(ntohl(state.player_a_id), 42u);
    EXPECT_EQ(ntohl(state.player_b_id), 42u);
    EXPECT_EQ(state.status, 2);  // TURN_B

    // Player 42 can move (it's B's turn, and 42 is player B)
    auto result = gm.move(42, 0, 0, 1);
    ASSERT_TRUE(result.has_value());
    // Knocked the only pawn. It was B's turn, so B wins.
    EXPECT_EQ(result.value().status, 4);  // WIN_B
}

// --- Game ID sequential assignment ---

TEST(KaylesGameMap, GameIdSequentialAcrossManyGames) {
    KaylesGameMap gm(60, 0, make_row_1());
    for (uint32_t i = 0; i < 10; i++) {
        auto s = gm.join(i * 2 + 1).value();  // player A
        EXPECT_EQ(ntohl(s.game_id), i);
        gm.join(i * 2 + 2);  // player B fills the game
    }
}

// --- Multiple concurrent games with interleaved moves ---

TEST(KaylesGameMap, InterleavedMovesAcrossGames) {
    KaylesGameMap gm(60, 2, make_row_3());

    // Create 3 games
    gm.join(1);
    gm.join(2);  // game 0: players 1,2
    gm.join(3);
    gm.join(4);  // game 1: players 3,4
    gm.join(5);
    gm.join(6);  // game 2: players 5,6

    // Interleaved moves
    auto r0 = gm.move(2, 0, 0, 1);  // game 0: B moves
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(r0->status, 1);  // TURN_A

    auto r1 = gm.move(4, 1, 0, 2);  // game 1: B takes two pawns
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, 1);  // TURN_A

    auto r2 = gm.move(6, 2, 0, 1);  // game 2: B moves
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, 1);  // TURN_A

    // Now A moves in game 0
    auto r0a = gm.move(1, 0, 1, 2);  // game 0: A takes pawns 1,2 -> WIN_A
    ASSERT_TRUE(r0a.has_value());
    EXPECT_EQ(r0a->status, 3);  // WIN_A

    // Game 1: A takes remaining pawn
    auto r1a = gm.move(3, 1, 2, 1);  // game 1: A takes pawn 2 -> WIN_A
    ASSERT_TRUE(r1a.has_value());
    EXPECT_EQ(r1a->status, 3);  // WIN_A

    // Game 2 should be unaffected
    auto check2 = gm.keep_alive(5, 2);
    ASSERT_TRUE(check2.has_value());
    EXPECT_EQ(check2->status, 1);  // TURN_A in game 2
}

// --- JOIN when WAITING game exists but player_id matches player_a ---

TEST(KaylesGameMap, SamePlayerJoinsOwnWaitingGame) {
    KaylesGameMap gm(60, 2, make_row_3());

    // Player 1 creates a game
    auto s1 = gm.join(1).value();
    EXPECT_EQ(s1.status, 0);  // WAITING_FOR_OPPONENT
    EXPECT_EQ(ntohl(s1.player_a_id), 1u);

    // Player 1 joins again -> joins as player B (same player both sides)
    auto s2 = gm.join(1).value();
    EXPECT_EQ(s2.status, 2);  // TURN_B
    EXPECT_EQ(ntohl(s2.player_a_id), 1u);
    EXPECT_EQ(ntohl(s2.player_b_id), 1u);
    EXPECT_EQ(ntohl(s2.game_id), 0u);  // same game
}

// --- Error cases: move/keep_alive/give_up on nonexistent game ---

TEST(KaylesGameMap, MoveOnNonexistentGame) {
    KaylesGameMap gm(60, 2, make_row_3());
    // No games created
    auto result = gm.move(1, 0, 0, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_GAME_ID);
}

TEST(KaylesGameMap, KeepAliveOnNonexistentGame) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto result = gm.keep_alive(1, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_GAME_ID);
}

TEST(KaylesGameMap, GiveUpOnNonexistentGame) {
    KaylesGameMap gm(60, 2, make_row_3());
    auto result = gm.give_up(1, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), KaylesGameError::INVALID_GAME_ID);
}

// --- Timeout-based cleanup: finished game with small timeout ---
// Note: We can't easily manipulate time(NULL), but we can verify that
// check_timeouts is called on every operation and that freshly created
// games are not prematurely deleted.

TEST(KaylesGameMap, FreshGameNotDeletedByTimeout) {
    // Even with timeout=1, a freshly created game should survive
    KaylesGameMap gm(1, 2, make_row_3());
    gm.join(1);
    gm.join(2);

    // Immediately try to access the game - should still exist
    auto result = gm.keep_alive(1, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, 2);  // TURN_B
}

TEST(KaylesGameMap, MultipleGamesIndependentPlayerValidation) {
    // Players from game 0 cannot operate on game 1
    KaylesGameMap gm(60, 2, make_row_3());
    gm.join(1);
    gm.join(2);  // game 0
    gm.join(3);
    gm.join(4);  // game 1

    // Player 1 (game 0) tries to move in game 1
    auto r1 = gm.move(1, 1, 0, 1);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error(), KaylesGameError::INVALID_PLAYER_ID);

    // Player 3 (game 1) tries to give up in game 0
    auto r2 = gm.give_up(3, 0);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), KaylesGameError::INVALID_PLAYER_ID);

    // Player 4 (game 1) tries keep_alive in game 0
    auto r3 = gm.keep_alive(4, 0);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error(), KaylesGameError::INVALID_PLAYER_ID);
}
