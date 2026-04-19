// Unit tests for kayles::server::KaylesServer::process_message (pure dispatch layer).
//
// We deliberately do NOT exercise start()/run()/run_server_loop()/handle_error():
//   - they require a real UDP socket (covered by integration tests).
//   - process_message is a straight map from ClientMessage -> KaylesGameMap method
//     and returns std::expected<GameState, KaylesError>. That is what we test here.
//
// Rules:
//   - We never touch the socket. The constructor is side-effect-free.
//   - Tests are written against the spec (docs/task.txt section 3.3 + 5.1/5.2/5.3),
//     not the current implementation. A failing test means a real bug.
//
// Key spec points exercised:
//   - 3.3: "Komunikat zawierający niepoprawną wartość pola pawn uznaje się za poprawny,
//          ale taki ruch jest nielegalny." → illegal move, state unchanged, still MSG_GAME_STATE.
//   - 3.3: "Ruch jest nielegalny również wtedy, gdy (...) próbuje go wykonać gracz,
//          którego nie jest kolej (dotyczy to także komunikatu MSG_GIVE_UP)."
//   - 5.1: At most one game in WAITING_FOR_OPPONENT. Second JOIN fills it; third
//          JOIN creates a new waiting game.
//   - A player may be both A and B in the same game (2.).
//   - Give-up after the game is finished is illegal → state unchanged.

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string_view>

// clang-format off
#include "kayles_protocol.h"
#include "kayles_server.h"
#include "kayles_error.h"
// clang-format on

using namespace kayles::server;
using namespace kayles::protocol;
using namespace kayles::error;
using kayles::types::address_t;
using kayles::types::pawn_row_t;
using kayles::types::pawn_t;
using kayles::types::player_id_t;
using kayles::types::timeout_t;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a pawn_row_t from a compact "101" string: '1' = true, anything else = false.
static pawn_row_t make_row(std::string_view s) {
    pawn_row_t row;
    row.reserve(s.size());
    for (char c : s)
        row.push_back(c == '1');
    return row;
}

// Build a server pointing at 127.0.0.1 on port 0. start() is never called.
static KaylesServer make_server(pawn_t max_pawn, pawn_row_t row,
                                timeout_t timeout = std::chrono::seconds{60}) {
    address_t addr{};
    addr.s_addr = htonl(INADDR_LOOPBACK);
    return KaylesServer(addr, /*port=*/0, timeout, max_pawn, std::move(row));
}

static ClientMessage join_msg(player_id_t p) {
    return ClientMessage{ClientMessageType::MSG_JOIN, p, 0, 0};
}
static ClientMessage move1_msg(player_id_t p, uint32_t gid, pawn_t pawn) {
    return ClientMessage{ClientMessageType::MSG_MOVE_1, p, gid, pawn};
}
static ClientMessage move2_msg(player_id_t p, uint32_t gid, pawn_t pawn) {
    return ClientMessage{ClientMessageType::MSG_MOVE_2, p, gid, pawn};
}
static ClientMessage keep_alive_msg(player_id_t p, uint32_t gid) {
    return ClientMessage{ClientMessageType::MSG_KEEP_ALIVE, p, gid, 0};
}
static ClientMessage give_up_msg(player_id_t p, uint32_t gid) {
    return ClientMessage{ClientMessageType::MSG_GIVE_UP, p, gid, 0};
}

// ===========================================================================
// KaylesServerDispatch / Join
// ===========================================================================

TEST(KaylesServerDispatch, FirstJoinCreatesWaitingGame) {
    auto s = make_server(2, make_row("111"));
    auto r = s.process_message(join_msg(1u));
    ASSERT_TRUE(r.has_value()) << "first JOIN must succeed";
    EXPECT_EQ(r->game_id, 0u);
    EXPECT_EQ(r->player_a_id, 1u);
    EXPECT_EQ(r->player_b_id, 0u);
    EXPECT_EQ(r->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(r->max_pawn, 2u);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, SecondJoinFillsWaitingGameSameGameId) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    auto r = s.process_message(join_msg(2u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, 0u);
    EXPECT_EQ(r->player_a_id, 1u);
    EXPECT_EQ(r->player_b_id, 2u);
    EXPECT_EQ(r->status, GameStatus::TURN_B);
}

TEST(KaylesServerDispatch, ThirdJoinCreatesNewWaitingGameId1) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(join_msg(3u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->game_id, 1u);
    EXPECT_EQ(r->player_a_id, 3u);
    EXPECT_EQ(r->player_b_id, 0u);
    EXPECT_EQ(r->status, GameStatus::WAITING_FOR_OPPONENT);
}

TEST(KaylesServerDispatch, SamePlayerBothSides) {
    // Spec 2: "Gracz może grać jako obaj gracze w rozgrywce."
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    auto r = s.process_message(join_msg(1u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->player_a_id, 1u);
    EXPECT_EQ(r->player_b_id, 1u);
    EXPECT_EQ(r->status, GameStatus::TURN_B);
}

TEST(KaylesServerDispatch, JoinDoesNotLeakIntoOtherGamesPawnRow) {
    // The initial pawn_row passed to the server should be echoed in the GameState.
    auto s = make_server(7, make_row("10101011"));
    auto r = s.process_message(join_msg(42u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->pawn_row, make_row("10101011"));
    EXPECT_EQ(r->max_pawn, 7u);
}

// ===========================================================================
// KaylesServerDispatch / Move (errors)
// ===========================================================================

TEST(KaylesServerDispatch, Move1OnMissingGameIdReturnsInvalidGameId) {
    auto s = make_server(2, make_row("111"));
    auto r = s.process_message(move1_msg(1u, /*gid=*/42u, 0));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesServerDispatch, Move2OnMissingGameIdReturnsInvalidGameId) {
    auto s = make_server(2, make_row("111"));
    auto r = s.process_message(move2_msg(1u, /*gid=*/42u, 0));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesServerDispatch, Move1ByForeignPlayerReturnsInvalidPlayerId) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(move1_msg(/*p=*/99u, /*gid=*/0u, 0));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesServerDispatch, Move2ByForeignPlayerReturnsInvalidPlayerId) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(move2_msg(/*p=*/99u, /*gid=*/0u, 0));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

// ===========================================================================
// KaylesServerDispatch / Move (legal & illegal)
// ===========================================================================

TEST(KaylesServerDispatch, Move1ByPlayerBOnTurnBFlipsToTurnA) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move1_msg(2u, 0u, 0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_A);
    EXPECT_FALSE(r->pawn_row[0]);
    EXPECT_TRUE(r->pawn_row[1]);
    EXPECT_TRUE(r->pawn_row[2]);
}

TEST(KaylesServerDispatch, Move1NotPlayersTurnReturnsStateUnchanged) {
    // Spec 3.3: message is valid (player in game, pawn in range) so server still
    // responds with MSG_GAME_STATE; illegal move → state unchanged.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move1_msg(1u, 0u, 0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, Move1PawnOutOfRangeReturnsStateUnchanged) {
    // Spec 3.3: "Komunikat zawierający niepoprawną wartość pola pawn uznaje się
    // za poprawny, ale taki ruch jest nielegalny." → still MSG_GAME_STATE (value),
    // state unchanged.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(move1_msg(2u, 0u, /*pawn=*/250));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, Move1AlreadyKnockedPawnReturnsStateUnchanged) {
    // Start with pawn[1] already gone; still must satisfy spec "skrajne pola = 1"
    // which applies to CLI parsing, not to the API under test here.
    auto s = make_server(2, make_row("101"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move1_msg(2u, 0u, 1));          // pawn 1 already knocked
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("101"));
}

TEST(KaylesServerDispatch, Move2LegalKnocksBothPawnsAndFlipsTurn) {
    auto s = make_server(3, make_row("1111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move2_msg(2u, 0u, /*pawn=*/1));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_A);
    EXPECT_TRUE(r->pawn_row[0]);
    EXPECT_FALSE(r->pawn_row[1]);
    EXPECT_FALSE(r->pawn_row[2]);
    EXPECT_TRUE(r->pawn_row[3]);
}

TEST(KaylesServerDispatch, Move2FirstPawnEqualsMaxPawnReturnsStateUnchanged) {
    // Spec 3.1: max_pawn is the max index. With max_pawn=2 and pawn=2, pawn+1=3
    // does not exist → illegal move, state unchanged, still MSG_GAME_STATE.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move2_msg(2u, 0u, /*pawn=*/2));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, Move2PawnOutOfRangeReturnsStateUnchanged) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move2_msg(2u, 0u, /*pawn=*/200));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, Move2OnMaxPawnZeroReturnsStateUnchanged) {
    // max_pawn=0 means only pawn 0 exists. MOVE_2 is necessarily illegal.
    auto s = make_server(0, make_row("1"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move2_msg(2u, 0u, /*pawn=*/0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("1"));
}

TEST(KaylesServerDispatch, Move1LastPawnWinsForB) {
    // max_pawn=0 → only one pawn. B's move knocks the last one → WIN_B.
    auto s = make_server(0, make_row("1"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move1_msg(2u, 0u, 0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::WIN_B);
    EXPECT_FALSE(r->pawn_row[0]);
}

TEST(KaylesServerDispatch, Move1LastPawnWinsForA) {
    auto s = make_server(1, make_row("11"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    // B knocks pawn 0 → TURN_A
    auto r1 = s.process_message(move1_msg(2u, 0u, 0));
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1->status, GameStatus::TURN_A);
    // A knocks pawn 1 → WIN_A
    auto r2 = s.process_message(move1_msg(1u, 0u, 1));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::WIN_A);
    EXPECT_FALSE(r2->pawn_row[1]);
}

TEST(KaylesServerDispatch, Move2LastTwoPawnsWinsForB) {
    auto s = make_server(1, make_row("11"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(move2_msg(2u, 0u, /*pawn=*/0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::WIN_B);
    EXPECT_FALSE(r->pawn_row[0]);
    EXPECT_FALSE(r->pawn_row[1]);
}

TEST(KaylesServerDispatch, MoveOnWaitingGameIsNoOp) {
    // Game has only A; no one's turn. Any move must be treated as illegal.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    auto r = s.process_message(move1_msg(1u, 0u, 0));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, MoveOnFinishedGameIsNoOp) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    // B gives up → WIN_A, game finished.
    auto gu = s.process_message(give_up_msg(2u, 0u));
    ASSERT_TRUE(gu.has_value());
    ASSERT_EQ(gu->status, GameStatus::WIN_A);
    auto before = gu->pawn_row;
    // Further moves must not mutate state.
    auto r1 = s.process_message(move1_msg(1u, 0u, 0));
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, GameStatus::WIN_A);
    EXPECT_EQ(r1->pawn_row, before);
    auto r2 = s.process_message(move2_msg(1u, 0u, 0));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::WIN_A);
    EXPECT_EQ(r2->pawn_row, before);
}

// ===========================================================================
// KaylesServerDispatch / KeepAlive
// ===========================================================================

TEST(KaylesServerDispatch, KeepAliveOnMissingGameIdReturnsInvalidGameId) {
    auto s = make_server(2, make_row("111"));
    auto r = s.process_message(keep_alive_msg(1u, /*gid=*/7u));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesServerDispatch, KeepAliveByForeignPlayerReturnsInvalidPlayerId) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(keep_alive_msg(/*p=*/99u, 0u));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesServerDispatch, KeepAliveValidReturnsCurrentState) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    auto joined2 = s.process_message(join_msg(2u));
    ASSERT_TRUE(joined2.has_value());
    auto ka = s.process_message(keep_alive_msg(1u, 0u));
    ASSERT_TRUE(ka.has_value());
    EXPECT_EQ(ka->game_id, 0u);
    EXPECT_EQ(ka->player_a_id, 1u);
    EXPECT_EQ(ka->player_b_id, 2u);
    EXPECT_EQ(ka->status, GameStatus::TURN_B);
    EXPECT_EQ(ka->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, KeepAliveWaitingStateReturnedUnchanged) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    auto ka = s.process_message(keep_alive_msg(1u, 0u));
    ASSERT_TRUE(ka.has_value());
    EXPECT_EQ(ka->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(ka->player_a_id, 1u);
    EXPECT_EQ(ka->player_b_id, 0u);
}

// ===========================================================================
// KaylesServerDispatch / GiveUp
// ===========================================================================

TEST(KaylesServerDispatch, GiveUpOnMissingGameIdReturnsInvalidGameId) {
    auto s = make_server(2, make_row("111"));
    auto r = s.process_message(give_up_msg(1u, /*gid=*/7u));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_GAME_ID);
}

TEST(KaylesServerDispatch, GiveUpByForeignPlayerReturnsInvalidPlayerId) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(give_up_msg(/*p=*/99u, 0u));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::INVALID_PLAYER_ID);
}

TEST(KaylesServerDispatch, GiveUpOnOwnTurnFlipsToOpponentWin) {
    // On TURN_B, B gives up → WIN_A.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    auto r = s.process_message(give_up_msg(2u, 0u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::WIN_A);
}

TEST(KaylesServerDispatch, GiveUpByAOnATurnFlipsToWinB) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    // B knocks one pawn → TURN_A
    auto m = s.process_message(move1_msg(2u, 0u, 0));
    ASSERT_TRUE(m.has_value());
    ASSERT_EQ(m->status, GameStatus::TURN_A);
    auto r = s.process_message(give_up_msg(1u, 0u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::WIN_B);
}

TEST(KaylesServerDispatch, GiveUpNotOnOwnTurnIsNoOp) {
    // Spec 3.3: give-up by a non-turn player is illegal but still valid message →
    // current state returned, unchanged.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());  // TURN_B
    auto r = s.process_message(give_up_msg(1u, 0u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::TURN_B);
    EXPECT_EQ(r->pawn_row, make_row("111"));
}

TEST(KaylesServerDispatch, GiveUpOnFinishedGameIsNoOp) {
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    ASSERT_TRUE(s.process_message(give_up_msg(2u, 0u)).has_value());  // WIN_A
    // Now neither party can flip the outcome.
    auto r1 = s.process_message(give_up_msg(2u, 0u));
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, GameStatus::WIN_A);
    auto r2 = s.process_message(give_up_msg(1u, 0u));
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, GameStatus::WIN_A);
}

TEST(KaylesServerDispatch, GiveUpOnWaitingGameIsNoOp) {
    // Nobody is on turn yet → give_up must leave status WAITING.
    auto s = make_server(2, make_row("111"));
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    auto r = s.process_message(give_up_msg(1u, 0u));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, GameStatus::WAITING_FOR_OPPONENT);
}

// ===========================================================================
// KaylesServerDispatch / Multi-game isolation
// ===========================================================================

TEST(KaylesServerDispatch, ActionsOnOneGameDoNotAffectAnother) {
    auto s = make_server(1, make_row("11"));
    // Game 0: A=1, B=2. TURN_B.
    ASSERT_TRUE(s.process_message(join_msg(1u)).has_value());
    ASSERT_TRUE(s.process_message(join_msg(2u)).has_value());
    // Game 1: A=3, waiting.
    ASSERT_TRUE(s.process_message(join_msg(3u)).has_value());
    // Finish game 0 by B giving up.
    auto gu = s.process_message(give_up_msg(2u, 0u));
    ASSERT_TRUE(gu.has_value());
    EXPECT_EQ(gu->status, GameStatus::WIN_A);
    // Game 1 is independent.
    auto ka1 = s.process_message(keep_alive_msg(3u, 1u));
    ASSERT_TRUE(ka1.has_value());
    EXPECT_EQ(ka1->status, GameStatus::WAITING_FOR_OPPONENT);
    EXPECT_EQ(ka1->player_a_id, 3u);
    EXPECT_EQ(ka1->player_b_id, 0u);
    // Cross-wiring: player 3 is NOT in game 0.
    auto bad = s.process_message(move1_msg(3u, 0u, 0));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().type(), ErrorType::INVALID_PLAYER_ID);
}

// ===========================================================================
// KaylesServerDispatch / ErrorType wiring for EXHAUSTED_GAME_IDS
// ===========================================================================
//
// Exhausting game IDs would require 2^32 joins: infeasible in a test. Instead we
// verify that the factory constructs a KaylesError with the correct type — so
// that IF the server ever hits the exhaustion branch it produces the right
// ErrorType, which handle_error uses to suppress the reply per spec 3.3.

TEST(KaylesServerDispatch, GameIdsExhaustedFactoryHasCorrectType) {
    auto err = KaylesError::game_ids_exhausted();
    EXPECT_EQ(err.type(), ErrorType::EXHAUSTED_GAME_IDS);
    EXPECT_EQ(err.error_index(), 0u);
}
