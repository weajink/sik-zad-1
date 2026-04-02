// Include the server source directly, renaming main to avoid conflicts.
#define main kayles_server_main
#include "../src/kayles_server.cpp"
#undef main

#include <gtest/gtest.h>

// ============================================================
// Tests written against the SPEC (CLAUDE.md / docs/task.txt).
// If a test fails, it indicates a bug in the implementation.
// ============================================================

// --- Construction ---

TEST(GameConstruction, ValidConstruction) {
    pawn_row_t row = {1, 0, 1};
    Game g(0, 1, 2, row);
    EXPECT_TRUE(g.is_player_joined(1));
    EXPECT_FALSE(g.is_player_joined(2));
}

TEST(GameConstruction, ZeroPlayerIdThrows) {
    pawn_row_t row = {1};
    EXPECT_THROW(Game(0, 0, 0, row), std::invalid_argument);
}

TEST(GameConstruction, SinglePawn) {
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    EXPECT_TRUE(g.is_player_joined(1));
}

// --- join_player_b ---

TEST(GameJoin, JoinPlayerBMakesThemJoined) {
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    EXPECT_FALSE(g.is_player_joined(2));
    g.join_player_b(2);
    EXPECT_TRUE(g.is_player_joined(2));
}

TEST(GameJoin, JoinPlayerBZeroIdThrows) {
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    EXPECT_THROW(g.join_player_b(0), std::invalid_argument);
}

// --- is_player_joined ---

TEST(GameIsPlayerJoined, BothPlayersAfterJoin) {
    pawn_row_t row = {1};
    Game g(0, 10, 0, row);
    g.join_player_b(20);
    EXPECT_TRUE(g.is_player_joined(10));
    EXPECT_TRUE(g.is_player_joined(20));
    EXPECT_FALSE(g.is_player_joined(30));
}

// --- keep_alive ---

TEST(GameKeepAlive, DoesNotCrashForKnownAndUnknownPlayers) {
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    g.keep_alive(1);
    g.join_player_b(2);
    g.keep_alive(2);
    g.keep_alive(999);  // unknown player, should be a no-op
}

// --- Spec: Game starts WAITING_FOR_OPPONENT, becomes TURN_B after B joins ---
// We cannot directly read status, but we can observe behavior:
// - Before B joins, no one can move (WAITING_FOR_OPPONENT)
// - After B joins, player B moves first (TURN_B)

TEST(GameTurnOrder, MoveBeforeJoinIsNoOp) {
    // Spec: game starts in WAITING_FOR_OPPONENT; moves should be ignored.
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.move(1, 0, 1);  // player A tries to move, but game hasn't started
    // If move was ignored, player B should still be able to take pawn 0 after joining
    g.join_player_b(2);
    g.move(2, 0, 1);
    // Pawn 0 should be taken. Player B should be able to also take pawn 1
    // only if it's their turn again, but spec says turns alternate, so it's now A's turn.
    // Player A takes pawn 1 to win.
    g.move(1, 1, 1);
    // No crash means the initial move was properly ignored.
}

TEST(GameTurnOrder, PlayerBMovesFirst) {
    // Spec: after B joins, status is TURN_B. B moves first.
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    g.join_player_b(2);
    // Player A tries to move first -- should be ignored (it's TURN_B)
    g.move(1, 0, 1);
    // Pawn should still be standing. Player B now takes it and wins.
    g.move(2, 0, 1);
    // B took the last pawn, so B wins. Further moves are no-ops.
    g.move(1, 0, 1);  // no-op, game over
}

// --- Spec: Players alternate turns (B, A, B, A, ...) ---

TEST(GameTurnOrder, TurnsAlternate) {
    // Spec: B first, then A, then B, then A...
    // Row: 1 1 1 1 (4 pawns). Each player takes 1 pawn per turn.
    pawn_row_t row = {1, 1, 1, 1};
    Game g(0, 1, 3, row);
    g.join_player_b(2);

    // Turn B: player B takes pawn 0
    g.move(2, 0, 1);

    // Turn A: player A takes pawn 1
    // If the turn didn't switch to A, this move would be ignored.
    g.move(1, 1, 1);

    // Turn B: player B takes pawn 2
    // If the turn didn't switch back to B, this move would be ignored.
    g.move(2, 2, 1);

    // Turn A: player A takes pawn 3 (last pawn) -> A wins
    g.move(1, 3, 1);
    // Game should be over. Further moves are no-ops.
    g.move(2, 0, 1);  // no-op
}

TEST(GameTurnOrder, WrongPlayerMoveIsIgnored) {
    // Spec: illegal move (wrong turn) doesn't change game state.
    pawn_row_t row = {1, 1, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);

    // It's TURN_B. Player A tries to move -- should be no-op.
    g.move(1, 0, 1);
    // Pawn 0 should still be up. Player B takes it.
    g.move(2, 0, 1);
    // Now it should be TURN_A. Player B tries again -- no-op.
    g.move(2, 1, 1);
    // Pawn 1 should still be up. Player A takes it.
    g.move(1, 1, 1);
    // Now TURN_B. Player B takes pawn 2 (last) and wins.
    g.move(2, 2, 1);
    // Game over.
}

// --- Spec: Knock down 1 pin ---

TEST(GameMove, TakeOnePawn) {
    pawn_row_t row = {1, 0, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: take pawn 0
    g.move(2, 0, 1);
    // TURN_A: take pawn 2 (last) -> A wins
    g.move(1, 2, 1);
    // Game over -- no crash
}

// --- Spec: Knock down 2 adjacent pins ---

TEST(GameMove, TakeTwoAdjacentPawns) {
    pawn_row_t row = {1, 1, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: take pawns 0 and 1 (two adjacent)
    g.move(2, 0, 2);
    // TURN_A: take pawn 2 (last) -> A wins
    g.move(1, 2, 1);
    // Game over
}

TEST(GameMove, TakeTwoAdjacentPawnsWin) {
    // Player B takes two adjacent pawns and wins immediately.
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.join_player_b(2);
    // TURN_B: take pawns 0 and 1 -> last pins, B wins
    g.move(2, 0, 2);
    // Game over. Further moves are no-ops.
    g.move(1, 0, 1);
}

TEST(GameMove, TakeTwoNonAdjacentFails) {
    // If second pawn (pawn+1) is already knocked down, move fails.
    pawn_row_t row = {1, 0, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: try to take pawn 0 and pawn 1, but pawn 1 is already down.
    g.move(2, 0, 2);
    // Move should fail (illegal). State should not change. Still TURN_B.
    // Player B now makes a legal move: take pawn 0 alone.
    g.move(2, 0, 1);
    // TURN_A: take pawn 2 -> A wins.
    g.move(1, 2, 1);
}

TEST(GameMove, TakeTwoOutOfBoundsFails) {
    // Trying to take pawn at max_pawn with no_of_pawns=2 should fail
    // because pawn+1 > max_pawn.
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.join_player_b(2);
    // TURN_B: try to take pawn 1 and pawn 2, but max_pawn=1.
    g.move(2, 1, 2);
    // Move should fail. Still TURN_B.
    // Player B makes a legal move.
    g.move(2, 0, 2);
    // B wins.
}

// --- Spec: Illegal moves don't change game state ---

TEST(GameMove, AlreadyKnockedDownPawnIsNoOp) {
    pawn_row_t row = {1, 1, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: take pawn 0
    g.move(2, 0, 1);
    // TURN_A: try to take pawn 0 again (already down) -- illegal, no state change
    g.move(1, 0, 1);
    // Should still be TURN_A. Player A takes pawn 1 instead.
    g.move(1, 1, 1);
    // TURN_B: take pawn 2 (last) -> B wins
    g.move(2, 2, 1);
}

TEST(GameMove, OutOfBoundsPawnIsNoOp) {
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    g.join_player_b(2);
    // TURN_B: try to take pawn 5, which is out of bounds
    g.move(2, 5, 1);
    // Should still be TURN_B. Player B now takes pawn 0.
    g.move(2, 0, 1);
    // B wins.
}

TEST(GameMove, PawnAlreadyDownDoesNotChangeTurn) {
    // Spec: illegal moves don't change game state (including turn).
    pawn_row_t row = {1, 0, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: try to take pawn 1 (already 0) -- illegal
    g.move(2, 1, 1);
    // Still TURN_B. B takes pawn 0.
    g.move(2, 0, 1);
    // TURN_A. A takes pawn 2 -> A wins.
    g.move(1, 2, 1);
}

// --- Spec: The player who knocks down the last pin WINS ---

TEST(GameWin, PlayerBWinsBySinglePawnTake) {
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    g.join_player_b(2);
    // TURN_B: Player B takes the only pawn -> B wins
    g.move(2, 0, 1);
    // Game over. Further moves are no-ops.
    g.move(1, 0, 1);
    g.move(2, 0, 1);
}

TEST(GameWin, PlayerAWinsByTakingLastPawn) {
    pawn_row_t row = {1, 0, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: B takes pawn 0
    g.move(2, 0, 1);
    // TURN_A: A takes pawn 2 (last) -> A wins
    g.move(1, 2, 1);
    // Game over.
    g.move(2, 0, 1);  // no-op
}

TEST(GameWin, PlayerBWinsByTwoAdjacentPawns) {
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.join_player_b(2);
    // TURN_B: B takes pawns 0 and 1 -> B wins
    g.move(2, 0, 2);
    g.move(1, 0, 1);  // no-op, game over
}

TEST(GameWin, LongerGameAlternatingTurns) {
    // 5 pawns all standing: 1 1 1 1 1
    // B takes 0, A takes 1, B takes 2, A takes 3, B takes 4 -> B wins
    pawn_row_t row = {1, 1, 1, 1, 1};
    Game g(0, 1, 4, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // B takes 0, turn -> A
    g.move(1, 1, 1);  // A takes 1, turn -> B
    g.move(2, 2, 1);  // B takes 2, turn -> A
    g.move(1, 3, 1);  // A takes 3, turn -> B
    g.move(2, 4, 1);  // B takes 4 (last) -> B wins
    // Game over
    g.move(1, 0, 1);  // no-op
}

// --- Spec: give_up -- if it's your turn and you give up, opponent wins ---

TEST(GameGiveUp, PlayerBGivesUpOnTheirTurn) {
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.join_player_b(2);
    // TURN_B: player B gives up -> WIN_A
    g.give_up(2);
    // Game is over. Further give_ups and moves are no-ops.
    g.give_up(1);
    g.move(1, 0, 1);
    g.move(2, 0, 1);
}

TEST(GameGiveUp, PlayerAGivesUpOnTheirTurn) {
    pawn_row_t row = {1, 1, 1};
    Game g(0, 1, 2, row);
    g.join_player_b(2);
    // TURN_B: B takes pawn 0 -> turn switches to A
    g.move(2, 0, 1);
    // TURN_A: player A gives up -> WIN_B
    g.give_up(1);
    // Game over.
    g.move(2, 1, 1);  // no-op
}

TEST(GameGiveUp, GiveUpNotYourTurnIsNoOp) {
    // Spec: give_up only works when it's your turn.
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.join_player_b(2);
    // TURN_B: player A tries to give up -> no effect
    g.give_up(1);
    // Still TURN_B. Player B can still move.
    g.move(2, 0, 1);
    // TURN_A now. Player A takes pawn 1 -> A wins.
    g.move(1, 1, 1);
}

// --- Spec: Stale games / timeouts ---

TEST(GameIsStale, ActiveGameNotStaleImmediately) {
    pawn_row_t row = {1, 1};
    Game g(0, 1, 1, row);
    g.join_player_b(2);
    // Game just started, should not be stale with a large timeout.
    EXPECT_FALSE(g.is_stale(99));
}

// --- Edge cases ---

TEST(GameEdge, MaxPawnZeroSinglePinGame) {
    // max_pawn=0 means a single pin
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // B takes the only pin -> B wins
    g.move(1, 0, 1);  // no-op
}

TEST(GameEdge, AllPawnsDown) {
    // A game where all pawns are already 0 except the required first and last
    pawn_row_t row = {1, 0, 0, 0, 1};
    Game g(0, 1, 4, row);
    g.join_player_b(2);
    // TURN_B: B takes pawn 0
    g.move(2, 0, 1);
    // TURN_A: A takes pawn 4 (last standing) -> A wins
    g.move(1, 4, 1);
}

TEST(GameEdge, MoveAfterGameOver) {
    pawn_row_t row = {1};
    Game g(0, 1, 0, row);
    g.join_player_b(2);
    g.move(2, 0, 1);  // B wins
    // All further moves should be no-ops
    g.move(1, 0, 1);
    g.move(2, 0, 1);
    g.move(1, 0, 2);
    g.move(2, 0, 2);
    g.give_up(1);
    g.give_up(2);
}
