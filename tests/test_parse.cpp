// Unit tests for the CLI parsing module: src/kayles_parse.h.
//
// Exercises parse_address, parse_port, parse_timeout, parse_pawn_row and
// parse_client_message against the specification in docs/task.txt.
//
// NOTE on parse_address: the implementation calls getaddrinfo(), so a few of
// these tests perform real DNS/resolver lookups (notably "localhost"). This is
// a platform limitation rather than a test bug; on a hermetic environment the
// "localhost" test may fail or hang on DNS. It should work on typical dev
// machines where /etc/hosts resolves "localhost".

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

#include "kayles_parse.h"

using namespace kayles::parse;
using namespace kayles::types;
using namespace kayles::error;
using namespace kayles::protocol;

// ---------------------------------------------------------------------------
// parse_address
// ---------------------------------------------------------------------------

TEST(ParseAddress, Loopback) {
    auto r = parse_address("127.0.0.1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->s_addr, inet_addr("127.0.0.1"));
}

TEST(ParseAddress, AllZeros) {
    auto r = parse_address("0.0.0.0");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->s_addr, inet_addr("0.0.0.0"));
}

TEST(ParseAddress, Broadcast) {
    auto r = parse_address("255.255.255.255");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->s_addr, inet_addr("255.255.255.255"));
}

TEST(ParseAddress, RegularIPv4) {
    auto r = parse_address("192.168.1.1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->s_addr, inet_addr("192.168.1.1"));
}

TEST(ParseAddress, OutOfRangeOctets) {
    auto r = parse_address("999.999.999.999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseAddress, GarbageString) {
    auto r = parse_address("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseAddress, EmptyString) {
    // getaddrinfo() with an empty name typically returns an error.
    auto r = parse_address("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// NOTE: requires a working resolver for the hostname "localhost". On any
// reasonable POSIX dev box this resolves via /etc/hosts without real DNS.
TEST(ParseAddress, LocalhostResolves) {
    auto r = parse_address("localhost");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    // We do not require a specific IPv4 for localhost (usually 127.0.0.1).
    // Just confirm the address is in 127.0.0.0/8.
    uint32_t host_order = ntohl(r->s_addr);
    EXPECT_EQ(host_order & 0xFF000000u, 0x7F000000u)
        << "expected 127.0.0.0/8, got " << std::hex << host_order;
}

// ---------------------------------------------------------------------------
// parse_port
// ---------------------------------------------------------------------------

TEST(ParsePort, Zero) {
    auto r = parse_port("0");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, 0u);
}

TEST(ParsePort, One) {
    auto r = parse_port("1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, 1u);
}

TEST(ParsePort, MidRange) {
    auto r = parse_port("12345");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, 12345u);
}

TEST(ParsePort, Max) {
    auto r = parse_port("65535");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, 65535u);
}

TEST(ParsePort, OverflowU16) {
    auto r = parse_port("65536");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, LargeOverflow) {
    auto r = parse_port("1000000");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, Negative) {
    auto r = parse_port("-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, Empty) {
    auto r = parse_port("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, TrailingGarbage) {
    auto r = parse_port("12a");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, PureGarbage) {
    auto r = parse_port("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, LeadingWhitespaceRejected) {
    // std::from_chars does not skip whitespace; a leading space is garbage.
    auto r = parse_port(" 80");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePort, PlusSignRejected) {
    // std::from_chars rejects a leading '+'.
    auto r = parse_port("+80");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_timeout
// ---------------------------------------------------------------------------

TEST(ParseTimeout, Min) {
    auto r = parse_timeout("1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, std::chrono::seconds(1));
    EXPECT_EQ(*r, MIN_TIMEOUT);
}

TEST(ParseTimeout, Max) {
    auto r = parse_timeout("99");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, std::chrono::seconds(99));
    EXPECT_EQ(*r, MAX_TIMEOUT);
}

TEST(ParseTimeout, MidRange) {
    auto r = parse_timeout("42");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(*r, std::chrono::seconds(42));
}

TEST(ParseTimeout, Zero) {
    // Spec: 1..99 inclusive; 0 is below MIN_TIMEOUT.
    auto r = parse_timeout("0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, AboveMax) {
    // 100 fits in uint8_t but is above MAX_TIMEOUT.
    auto r = parse_timeout("100");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, OverflowsU8Scratch) {
    // 256 cannot fit into the uint8_t scratch value — std::from_chars reports
    // result_out_of_range before the range check even runs.
    auto r = parse_timeout("256");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, VeryLarge) {
    auto r = parse_timeout("99999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, Negative) {
    auto r = parse_timeout("-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, Empty) {
    auto r = parse_timeout("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, TrailingGarbage) {
    auto r = parse_timeout("50x");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseTimeout, PureGarbage) {
    auto r = parse_timeout("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_pawn_row
// ---------------------------------------------------------------------------

TEST(ParsePawnRow, SinglePin) {
    auto r = parse_pawn_row("1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0], true);
}

TEST(ParsePawnRow, TwoPins) {
    auto r = parse_pawn_row("11");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0], true);
    EXPECT_EQ((*r)[1], true);
}

TEST(ParsePawnRow, SpecExample) {
    // The spec example string: "11101111011111".
    auto r = parse_pawn_row("11101111011111");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    ASSERT_EQ(r->size(), 14u);
    // Confirm first/last are on, middle matches.
    EXPECT_EQ((*r)[0], true);
    EXPECT_EQ((*r)[1], true);
    EXPECT_EQ((*r)[2], true);
    EXPECT_EQ((*r)[3], false);
    EXPECT_EQ((*r)[4], true);
    EXPECT_EQ((*r)[5], true);
    EXPECT_EQ((*r)[6], true);
    EXPECT_EQ((*r)[7], true);
    EXPECT_EQ((*r)[8], false);
    EXPECT_EQ((*r)[9], true);
    EXPECT_EQ((*r)[10], true);
    EXPECT_EQ((*r)[11], true);
    EXPECT_EQ((*r)[12], true);
    EXPECT_EQ((*r)[13], true);
}

TEST(ParsePawnRow, MixedPattern) {
    auto r = parse_pawn_row("10101");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    ASSERT_EQ(r->size(), 5u);
    EXPECT_EQ((*r)[0], true);
    EXPECT_EQ((*r)[1], false);
    EXPECT_EQ((*r)[2], true);
    EXPECT_EQ((*r)[3], false);
    EXPECT_EQ((*r)[4], true);
}

TEST(ParsePawnRow, MaxLength256) {
    std::string s(256, '1');
    auto r = parse_pawn_row(s);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->size(), 256u);
    for (size_t i = 0; i < 256; ++i)
        EXPECT_EQ((*r)[i], true) << "index " << i;
}

TEST(ParsePawnRow, MaxLength256WithGapsExceptEnds) {
    std::string s(256, '0');
    s.front() = '1';
    s.back() = '1';
    auto r = parse_pawn_row(s);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->size(), 256u);
    EXPECT_EQ((*r).front(), true);
    EXPECT_EQ((*r).back(), true);
}

TEST(ParsePawnRow, Empty) {
    auto r = parse_pawn_row("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, TooLong257) {
    std::string s(257, '1');
    auto r = parse_pawn_row(s);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, TooLongMany) {
    std::string s(1000, '1');
    auto r = parse_pawn_row(s);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, LastPinZero) {
    auto r = parse_pawn_row("10");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, FirstPinZero) {
    auto r = parse_pawn_row("01");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, BothEndsZero) {
    auto r = parse_pawn_row("010");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, SingleZero) {
    auto r = parse_pawn_row("0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, InvalidDigitTwo) {
    auto r = parse_pawn_row("12");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, InvalidDigitTwoMiddle) {
    auto r = parse_pawn_row("12101");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, InvalidAlphaMiddle) {
    auto r = parse_pawn_row("1a1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, InvalidSpace) {
    auto r = parse_pawn_row("1 1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParsePawnRow, InvalidNewline) {
    auto r = parse_pawn_row("1\n1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: valid messages
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, Join) {
    auto r = parse_client_message("0/42");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->msg_type, ClientMessageType::MSG_JOIN);
    EXPECT_EQ(r->player_id, 42u);
}

TEST(ParseClientMessage, Move1) {
    auto r = parse_client_message("1/42/7/3");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->msg_type, ClientMessageType::MSG_MOVE_1);
    EXPECT_EQ(r->player_id, 42u);
    EXPECT_EQ(r->game_id, 7u);
    EXPECT_EQ(r->pawn, 3u);
}

TEST(ParseClientMessage, Move2) {
    auto r = parse_client_message("2/42/7/3");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->msg_type, ClientMessageType::MSG_MOVE_2);
    EXPECT_EQ(r->player_id, 42u);
    EXPECT_EQ(r->game_id, 7u);
    EXPECT_EQ(r->pawn, 3u);
}

TEST(ParseClientMessage, KeepAlive) {
    auto r = parse_client_message("3/42/7");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->msg_type, ClientMessageType::MSG_KEEP_ALIVE);
    EXPECT_EQ(r->player_id, 42u);
    EXPECT_EQ(r->game_id, 7u);
}

TEST(ParseClientMessage, GiveUp) {
    auto r = parse_client_message("4/42/7");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->msg_type, ClientMessageType::MSG_GIVE_UP);
    EXPECT_EQ(r->player_id, 42u);
    EXPECT_EQ(r->game_id, 7u);
}

// ---------------------------------------------------------------------------
// parse_client_message: boundary values
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, PlayerIdMinJoin) {
    auto r = parse_client_message("0/1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->player_id, 1u);
}

TEST(ParseClientMessage, PlayerIdMaxJoin) {
    auto r = parse_client_message("0/4294967295");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->player_id, UINT32_MAX);
}

TEST(ParseClientMessage, PlayerIdMaxMove1) {
    auto r = parse_client_message("1/4294967295/7/0");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->player_id, UINT32_MAX);
}

TEST(ParseClientMessage, GameIdZeroKeepAlive) {
    auto r = parse_client_message("3/1/0");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->game_id, 0u);
}

TEST(ParseClientMessage, GameIdMaxKeepAlive) {
    auto r = parse_client_message("3/1/4294967295");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->game_id, UINT32_MAX);
}

TEST(ParseClientMessage, GameIdMaxMove2) {
    auto r = parse_client_message("2/1/4294967295/0");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->game_id, UINT32_MAX);
}

TEST(ParseClientMessage, PawnZero) {
    auto r = parse_client_message("1/1/0/0");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->pawn, 0u);
}

TEST(ParseClientMessage, PawnMax) {
    auto r = parse_client_message("1/1/0/255");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->pawn, 255u);
}

TEST(ParseClientMessage, PawnMaxMove2) {
    auto r = parse_client_message("2/1/0/255");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what());
    EXPECT_EQ(r->pawn, 255u);
}

// ---------------------------------------------------------------------------
// parse_client_message: invalid msg_type
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, InvalidType5) {
    auto r = parse_client_message("5/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, InvalidType5WithMoveFields) {
    auto r = parse_client_message("5/1/2/3");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, InvalidType99) {
    auto r = parse_client_message("99/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, InvalidType255) {
    auto r = parse_client_message("255/1/2/3");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, InvalidTypeOverflowsU8) {
    // 300 doesn't fit in uint8_t so std::from_chars fails; the parser reports
    // an invalid-message-type error.
    auto r = parse_client_message("300/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NegativeMsgType) {
    auto r = parse_client_message("-1/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: wrong field count
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, JoinTooFew) {
    // "0" splits into a single token.
    auto r = parse_client_message("0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, JoinTooMany) {
    auto r = parse_client_message("0/1/2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, Move1TooFew) {
    auto r = parse_client_message("1/1/2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, Move1TooMany) {
    auto r = parse_client_message("1/1/2/3/4");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, Move2TooFew) {
    auto r = parse_client_message("2/1/2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, Move2TooMany) {
    auto r = parse_client_message("2/1/2/3/4");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, KeepAliveTooFew) {
    auto r = parse_client_message("3/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, KeepAliveTooMany) {
    auto r = parse_client_message("3/1/2/3");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, GiveUpTooFew) {
    auto r = parse_client_message("4/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, GiveUpTooMany) {
    auto r = parse_client_message("4/1/2/3");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: player_id = 0 (invalid)
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, PlayerIdZeroJoin) {
    auto r = parse_client_message("0/0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PlayerIdZeroMove1) {
    auto r = parse_client_message("1/0/1/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PlayerIdZeroMove2) {
    auto r = parse_client_message("2/0/1/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PlayerIdZeroKeepAlive) {
    auto r = parse_client_message("3/0/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PlayerIdZeroGiveUp) {
    auto r = parse_client_message("4/0/1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: non-numeric tokens
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, NonNumericWholeMessage) {
    auto r = parse_client_message("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NonNumericPlayerId) {
    auto r = parse_client_message("1/abc/2/3");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, MixedAlphaNumericPlayerId) {
    auto r = parse_client_message("0/12abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NonNumericGameId) {
    auto r = parse_client_message("3/1/oops");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NonNumericPawn) {
    auto r = parse_client_message("1/1/0/xx");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: empty / slashes
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, Empty) {
    auto r = parse_client_message("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, TrailingSlash) {
    auto r = parse_client_message("0/42/");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, LeadingSlash) {
    auto r = parse_client_message("/0/42");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, DoubleSlash) {
    auto r = parse_client_message("0//42");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, OnlySlashes) {
    auto r = parse_client_message("///");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: negatives in various positions
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, NegativePlayerIdJoin) {
    auto r = parse_client_message("0/-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NegativeGameIdKeepAlive) {
    auto r = parse_client_message("3/1/-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NegativePawnMove1) {
    auto r = parse_client_message("1/1/1/-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, NegativeGameIdMove2) {
    auto r = parse_client_message("2/1/-1/0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// parse_client_message: overflows
// ---------------------------------------------------------------------------

TEST(ParseClientMessage, PlayerIdOverflowsU32) {
    auto r = parse_client_message("0/4294967296");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PlayerIdVeryLarge) {
    auto r = parse_client_message("0/99999999999999999999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, GameIdOverflowsU32) {
    auto r = parse_client_message("3/1/4294967296");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, GameIdOverflowsU32Move1) {
    auto r = parse_client_message("1/1/4294967296/0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PawnOverflowsU8) {
    auto r = parse_client_message("1/1/1/256");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}

TEST(ParseClientMessage, PawnVeryLarge) {
    auto r = parse_client_message("2/1/1/9999");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().type(), ErrorType::PARSE_ERROR);
}
