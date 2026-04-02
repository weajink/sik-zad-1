// Include the server source directly, renaming main to avoid conflicts.
#define main kayles_server_main
#include "../src/kayles_server.cpp"
#undef main

#include <gtest/gtest.h>

// ============================================================
// Tests written against the SPEC (CLAUDE.md / docs/task.txt).
// If a test fails, it indicates a bug in the implementation.
// ============================================================

// Helper: extract status byte from get_game_state() buffer.
// Layout: game_id(4) + player_a_id(4) + player_b_id(4) + status(1) + max_pawn(1) + bitmap
static uint8_t get_status(KaylesGame &g) {
    auto buf = g.get_game_state();
    EXPECT_GE(buf.size(), 14u);
    return buf[12];
}

static uint32_t get_player_a(KaylesGame &g) {
    auto buf = g.get_game_state();
    uint32_t val;
    std::memcpy(&val, buf.data() + 4, 4);
    return ntohl(val);
}

static uint32_t get_player_b(KaylesGame &g) {
    auto buf = g.get_game_state();
    uint32_t val;
    std::memcpy(&val, buf.data() + 8, 4);
    return ntohl(val);
}

static uint8_t get_max_pawn(KaylesGame &g) {
    auto buf = g.get_game_state();
    return buf[13];
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
