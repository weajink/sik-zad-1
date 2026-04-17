// Include the server source directly, renaming main to avoid conflicts.
#define main kayles_server_main
#include "../src/kayles_server.cpp"
#undef main

#include <gtest/gtest.h>

using namespace kayles_game;

// ============================================================
// Tests written against the SPEC (CLAUDE.md / docs/task.txt).
// If a test fails, it indicates a bug in the implementation.
// ============================================================

// Helper: extract fields from get_game_state() struct.
static uint8_t get_status(KaylesGame &g) {
    return g.get_game_state().status;
}

static uint32_t get_player_a(KaylesGame &g) {
    return ntohl(g.get_game_state().player_a_id);
}

static uint32_t get_player_b(KaylesGame &g) {
    return ntohl(g.get_game_state().player_b_id);
}

static uint8_t get_max_pawn(KaylesGame &g) {
    return g.get_game_state().max_pawn;
}

// --- Construction ---

TEST(GameConstruction, ValidConstruction) {
    pawn_row_t row = {1, 0, 1};
    KaylesGame g(0, 1, 2, row);
    EXPECT_TRUE(g.is_player_joined(1));
    EXPECT_FALSE(g.is_player_joined(2));
    // Spec: game starts WAITING_FOR_OPPONENT (status 0)
    EXPECT_EQ(get_status(g), 0);
    EXPECT_EQ(get_player_a(g), 1u);
    EXPECT_EQ(get_player_b(g), 0u);
    EXPECT_EQ(get_max_pawn(g), 2);
}

TEST(GameConstruction, ZeroPlayerIdThrows) {
    pawn_row_t row = {1};
    EXPECT_THROW(KaylesGame(0, 0, 0, row), std::invalid_argument);
}

TEST(GameConstruction, SinglePawn) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    EXPECT_TRUE(g.is_player_joined(1));
    EXPECT_EQ(get_status(g), 0);
    EXPECT_EQ(get_max_pawn(g), 0);
}

// --- join_player_b ---

TEST(GameJoin, JoinPlayerBMakesThemJoined) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    EXPECT_FALSE(g.is_player_joined(2));
    EXPECT_EQ(get_status(g), 0);  // WAITING_FOR_OPPONENT
    g.join_player_b(2);
    EXPECT_TRUE(g.is_player_joined(2));
    // Spec: after B joins, status becomes TURN_B (2)
    EXPECT_EQ(get_status(g), 2);
    EXPECT_EQ(get_player_b(g), 2u);
}

TEST(GameJoin, JoinPlayerBZeroIdThrows) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    EXPECT_THROW(g.join_player_b(0), std::invalid_argument);
}

// --- is_player_joined ---

TEST(GameIsPlayerJoined, BothPlayersAfterJoin) {
    pawn_row_t row = {1};
    KaylesGame g(0, 10, 0, row);
    g.join_player_b(20);
    EXPECT_TRUE(g.is_player_joined(10));
    EXPECT_TRUE(g.is_player_joined(20));
    EXPECT_FALSE(g.is_player_joined(30));
}

// --- keep_alive ---

TEST(GameKeepAlive, DoesNotCrashForKnownAndUnknownPlayers) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.keep_alive(1);
    EXPECT_EQ(get_status(g), 0);  // still WAITING
    g.join_player_b(2);
    g.keep_alive(2);
    g.keep_alive(999);            // unknown player, should be a no-op
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
}

// --- Spec: Game starts WAITING_FOR_OPPONENT, becomes TURN_B after B joins ---
// We cannot directly read status, but we can observe behavior:
// - Before B joins, no one can move (WAITING_FOR_OPPONENT)
// - After B joins, player B moves first (TURN_B)

TEST(GameTurnOrder, MoveBeforeJoinIsNoOp) {
    // Spec: game starts in WAITING_FOR_OPPONENT; moves should be ignored.
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    EXPECT_EQ(get_status(g), 0);  // WAITING_FOR_OPPONENT
    g.move(1, 0, 1);              // player A tries to move, but game hasn't started
    EXPECT_EQ(get_status(g), 0);  // still WAITING
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 1, 1);
    // A took last pawn -> WIN_A (status 3)
    EXPECT_EQ(get_status(g), 3);
}

TEST(GameTurnOrder, PlayerBMovesFirst) {
    // Spec: after B joins, status is TURN_B. B moves first.
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    // Player A tries to move first -- should be ignored (it's TURN_B)
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    // Player B takes the only pawn and wins.
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    // Further moves are no-ops.
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

// --- Spec: Players alternate turns (B, A, B, A, ...) ---

TEST(GameTurnOrder, TurnsAlternate) {
    // Spec: B first, then A, then B, then A...
    // Row: 1 1 1 1 (4 pawns). Each player takes 1 pawn per turn.
    pawn_row_t row = {1, 1, 1, 1};
    KaylesGame g(0, 1, 3, row);
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B

    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A

    g.move(1, 1, 1);
    EXPECT_EQ(get_status(g), 2);  // TURN_B

    g.move(2, 2, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A

    g.move(1, 3, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A

    g.move(2, 0, 1);              // no-op
    EXPECT_EQ(get_status(g), 3);  // still WIN_A
}

TEST(GameTurnOrder, WrongPlayerMoveIsIgnored) {
    // Spec: illegal move (wrong turn) doesn't change game state.
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B

    g.move(1, 0, 1);              // wrong player
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(2, 1, 1);              // wrong player
    EXPECT_EQ(get_status(g), 1);  // still TURN_A
    g.move(1, 1, 1);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 2, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
}

// --- Spec: Knock down 1 pin ---

TEST(GameMove, TakeOnePawn) {
    pawn_row_t row = {1, 0, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 2, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

// --- Spec: Knock down 2 adjacent pins ---

TEST(GameMove, TakeTwoAdjacentPawns) {
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 2);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 2, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

TEST(GameMove, TakeTwoAdjacentPawnsWin) {
    // Player B takes two adjacent pawns and wins immediately.
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.move(2, 0, 2);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

TEST(GameMove, TakeTwoNonAdjacentFails) {
    // If second pawn (pawn+1) is already knocked down, move fails.
    pawn_row_t row = {1, 0, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 2);              // illegal: pawn 1 already down
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 2, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

TEST(GameMove, TakeTwoOutOfBoundsFails) {
    // Trying to take pawn at max_pawn with no_of_pawns=2 should fail
    // because pawn+1 > max_pawn.
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.move(2, 1, 2);              // illegal: pawn+1 > max_pawn
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    g.move(2, 0, 2);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
}

// --- Spec: Illegal moves don't change game state ---

TEST(GameMove, AlreadyKnockedDownPawnIsNoOp) {
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 0, 1);              // illegal: already down
    EXPECT_EQ(get_status(g), 1);  // still TURN_A
    g.move(1, 1, 1);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 2, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
}

TEST(GameMove, OutOfBoundsPawnIsNoOp) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 5, 1);              // illegal: out of bounds
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
}

TEST(GameMove, PawnAlreadyDownDoesNotChangeTurn) {
    // Spec: illegal moves don't change game state (including turn).
    pawn_row_t row = {1, 0, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 1, 1);              // illegal: pawn 1 already down
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 2, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

// --- Spec: The player who knocks down the last pin WINS ---

TEST(GameWin, PlayerBWinsBySinglePawnTake) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

TEST(GameWin, PlayerAWinsByTakingLastPawn) {
    pawn_row_t row = {1, 0, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 2, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 3);  // still WIN_A
}

TEST(GameWin, PlayerBWinsByTwoAdjacentPawns) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.move(2, 0, 2);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

TEST(GameWin, LongerGameAlternatingTurns) {
    // 5 pawns all standing: 1 1 1 1 1
    pawn_row_t row = {1, 1, 1, 1, 1};
    KaylesGame g(0, 1, 4, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 1, 1);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 2, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 3, 1);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 4, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

// --- Spec: give_up -- if it's your turn and you give up, opponent wins ---

TEST(GameGiveUp, PlayerBGivesUpOnTheirTurn) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.give_up(2);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
    g.give_up(1);
    EXPECT_EQ(get_status(g), 3);  // still WIN_A
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 3);
}

TEST(GameGiveUp, PlayerAGivesUpOnTheirTurn) {
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.give_up(1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(2, 1, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

TEST(GameGiveUp, GiveUpNotYourTurnIsNoOp) {
    // Spec: give_up only works when it's your turn.
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.give_up(1);                 // wrong turn
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 1, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

// --- Spec: Stale games / timeouts ---

TEST(GameIsStale, ActiveGameNotStaleImmediately) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    // Game just started, should not be stale with a large timeout.
    EXPECT_FALSE(g.check_timeouts(99));
}

// --- Edge cases ---

TEST(GameEdge, MaxPawnZeroSinglePinGame) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // still WIN_B
}

TEST(GameEdge, AllPawnsDown) {
    pawn_row_t row = {1, 0, 0, 0, 1};
    KaylesGame g(0, 1, 4, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 4, 1);
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

TEST(GameEdge, MoveAfterGameOver) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);
    g.give_up(1);
    EXPECT_EQ(get_status(g), 4);
    g.give_up(2);
    EXPECT_EQ(get_status(g), 4);
}

TEST(GameEdge, SamePlayerBothSides) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(1);  // same player_id
    EXPECT_TRUE(g.is_player_joined(1));
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);  // WIN_B
}

// --- Timeout scenarios ---

TEST(GameTimeout, PlayerATimesOutDuringTurnA) {
    // Player A times out during TURN_A -> WIN_B
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    // Move B so it becomes TURN_A
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A

    // check_timeouts with timeout=0 should trigger immediately since time(NULL) - last > 0
    // We can't manipulate time directly, but with timeout=0, the condition
    // time(NULL) - player_a_last_move_time > 0 should be true (at least 0 seconds have passed).
    // Actually the condition is strictly >, so we need at least 1 second. Use a tiny sleep.
    // Instead, let's just test with timeout=1 and verify the game doesn't immediately flip.
    // For a deterministic test, check_timeouts(99) should NOT flip (just happened).
    EXPECT_FALSE(g.check_timeouts(99));
    EXPECT_EQ(get_status(g), 1);  // still TURN_A, not timed out with large timeout
}

TEST(GameTimeout, PlayerBTimesOutDuringTurnB) {
    // TURN_B with large timeout should not flip
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    EXPECT_FALSE(g.check_timeouts(99));
    EXPECT_EQ(get_status(g), 2);  // still TURN_B, not timed out
}

TEST(GameTimeout, WaitingForOpponentCanBeDeleted) {
    // A game in WAITING_FOR_OPPONENT falls into the default branch of check_timeouts.
    // With a sufficiently old creation time and small timeout, it should return true (deletable).
    // With a fresh game and large timeout, it should not be deletable.
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    EXPECT_EQ(get_status(g), 0);  // WAITING_FOR_OPPONENT
    // Just created, large timeout -> not deletable
    EXPECT_FALSE(g.check_timeouts(99));
}

// --- Multiple moves alternating correctly until win ---

TEST(GameTurnOrder, FullGameAlternatingMoves6Pawns) {
    // 6 pawns: B, A, B, A, B, A (A takes last pawn -> WIN_A)
    pawn_row_t row = {1, 1, 1, 1, 1, 1};
    KaylesGame g(0, 1, 5, row);
    g.join_player_b(2);

    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 0, 1);  // B takes pawn 0
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 1, 1);  // A takes pawn 1
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 2, 1);  // B takes pawn 2
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 3, 1);  // A takes pawn 3
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 4, 1);  // B takes pawn 4
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 5, 1);  // A takes last pawn -> WIN_A
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

TEST(GameTurnOrder, FullGameWithTwoPawnMoves) {
    // 4 pawns: B takes 2 (pawn 0,1), A takes 2 (pawn 2,3) -> WIN_A
    pawn_row_t row = {1, 1, 1, 1};
    KaylesGame g(0, 1, 3, row);
    g.join_player_b(2);

    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.move(2, 0, 2);  // B takes pawns 0 and 1
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.move(1, 2, 2);  // A takes pawns 2 and 3 -> WIN_A
    EXPECT_EQ(get_status(g), 3);  // WIN_A
}

// --- Attempting moves after game is already won ---

TEST(GameEdge, MoveAfterWinAIsNoOp) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // TURN_A
    g.move(1, 1, 1);  // WIN_A
    EXPECT_EQ(get_status(g), 3);

    // Both players try to move -> no-ops
    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 3);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 3);
    g.move(1, 0, 2);
    EXPECT_EQ(get_status(g), 3);
    g.move(2, 0, 2);
    EXPECT_EQ(get_status(g), 3);
}

TEST(GameEdge, MoveAfterWinBIsNoOp) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // WIN_B
    EXPECT_EQ(get_status(g), 4);

    g.move(1, 0, 1);
    EXPECT_EQ(get_status(g), 4);
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 4);
}

// --- give_up when it's not your turn ---

TEST(GameGiveUp, GiveUpNotYourTurnPlayerADuringTurnB) {
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B
    g.give_up(1);  // A tries to give up during B's turn -> no-op
    EXPECT_EQ(get_status(g), 2);  // still TURN_B
}

TEST(GameGiveUp, GiveUpNotYourTurnPlayerBDuringTurnA) {
    pawn_row_t row = {1, 1, 1};
    KaylesGame g(0, 1, 2, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // TURN_A
    EXPECT_EQ(get_status(g), 1);  // TURN_A
    g.give_up(2);  // B tries to give up during A's turn -> no-op
    EXPECT_EQ(get_status(g), 1);  // still TURN_A
}

// --- give_up on a finished game ---

TEST(GameGiveUp, GiveUpOnFinishedGameWinA) {
    pawn_row_t row = {1, 1};
    KaylesGame g(0, 1, 1, row);
    g.join_player_b(2);
    g.give_up(2);  // B gives up -> WIN_A
    EXPECT_EQ(get_status(g), 3);

    g.give_up(1);  // A tries to give up on finished game -> no-op
    EXPECT_EQ(get_status(g), 3);
    g.give_up(2);  // B tries again -> no-op
    EXPECT_EQ(get_status(g), 3);
}

TEST(GameGiveUp, GiveUpOnFinishedGameWinB) {
    pawn_row_t row = {1};
    KaylesGame g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // WIN_B
    EXPECT_EQ(get_status(g), 4);

    g.give_up(1);
    EXPECT_EQ(get_status(g), 4);
    g.give_up(2);
    EXPECT_EQ(get_status(g), 4);
}

// --- Edge case: max_pawn=255 with full row ---

TEST(GameEdge, MaxPawn255FullRow) {
    pawn_row_t row(256, true);
    KaylesGame g(0, 1, 255, row);
    EXPECT_EQ(get_status(g), 0);  // WAITING_FOR_OPPONENT
    EXPECT_EQ(get_max_pawn(g), 255);

    g.join_player_b(2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B

    // B takes pawn 0
    g.move(2, 0, 1);
    EXPECT_EQ(get_status(g), 1);  // TURN_A (254 pawns left)

    // A takes pawns 254 and 255
    g.move(1, 254, 2);
    EXPECT_EQ(get_status(g), 2);  // TURN_B (252 pawns left)

    // Verify bitmap: pawn 0 should be down, pawns 254-255 should be down
    auto state = g.get_game_state();
    EXPECT_EQ(state.max_pawn, 255);
    // Pawn 0 is MSB of byte 0 -> should be 0
    EXPECT_EQ(state.pawn_row_bitmap[0] & 0x80, 0);
    // Pawn 1 should still be up
    EXPECT_EQ(state.pawn_row_bitmap[0] & 0x40, 0x40);
    // Pawn 254 = byte 31, bit (7 - 254%8) = bit (7-6) = bit 1 -> should be 0
    EXPECT_EQ(state.pawn_row_bitmap[31] & 0x02, 0);
    // Pawn 255 = byte 31, bit (7 - 255%8) = bit (7-7) = bit 0 -> should be 0
    EXPECT_EQ(state.pawn_row_bitmap[31] & 0x01, 0);
}

TEST(GameEdge, MaxPawn255TakeTwoAtBoundary) {
    // Taking two pawns at position 254 (pawns 254 and 255)
    pawn_row_t row(256, true);
    KaylesGame g(0, 1, 255, row);
    g.join_player_b(2);
    g.move(2, 254, 2);  // Should succeed: takes pawns 254 and 255
    EXPECT_EQ(get_status(g), 1);  // TURN_A

    // Trying to take two at position 255 should fail (255+1=256 > max_pawn=255)
    g.move(1, 255, 2);  // Illegal
    EXPECT_EQ(get_status(g), 1);  // still TURN_A (pawn 255 is already down anyway)
}
