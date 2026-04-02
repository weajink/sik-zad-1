// Rename main to avoid conflict when including the source file.
// This lets us test the static helper functions directly.
#define main kayles_server_main
#include "../src/kayles_server.cpp"
#undef main

#include <gtest/gtest.h>

// ---------- string_to_pawn_row tests ----------

TEST(StringToPawnRow, SinglePin) {
    auto result = string_to_pawn_row("1");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1);
    EXPECT_EQ((*result)[0], true);
}

TEST(StringToPawnRow, TwoPins) {
    auto result = string_to_pawn_row("11");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2);
    EXPECT_EQ((*result)[0], true);
    EXPECT_EQ((*result)[1], true);
}

TEST(StringToPawnRow, MixedPins) {
    auto result = string_to_pawn_row("10101");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 5);
    EXPECT_EQ((*result)[0], true);
    EXPECT_EQ((*result)[1], false);
    EXPECT_EQ((*result)[2], true);
    EXPECT_EQ((*result)[3], false);
    EXPECT_EQ((*result)[4], true);
}

TEST(StringToPawnRow, AllZerosExceptEnds) {
    auto result = string_to_pawn_row("10001");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 5);
    EXPECT_EQ((*result)[0], true);
    EXPECT_EQ((*result)[4], true);
}

TEST(StringToPawnRow, MaxLength256) {
    std::string s(256, '0');
    s.front() = '1';
    s.back() = '1';
    auto result = string_to_pawn_row(s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 256);
}

// --- Invalid inputs ---

TEST(StringToPawnRow, EmptyString) {
    auto result = string_to_pawn_row("");
    EXPECT_FALSE(result.has_value());
}

TEST(StringToPawnRow, TooLong) {
    std::string s(257, '1');
    auto result = string_to_pawn_row(s);
    EXPECT_FALSE(result.has_value());
}

TEST(StringToPawnRow, FirstPinZero) {
    auto result = string_to_pawn_row("01");
    EXPECT_FALSE(result.has_value());
}

TEST(StringToPawnRow, LastPinZero) {
    auto result = string_to_pawn_row("10");
    EXPECT_FALSE(result.has_value());
}

TEST(StringToPawnRow, BothEndsZero) {
    auto result = string_to_pawn_row("010");
    EXPECT_FALSE(result.has_value());
}

TEST(StringToPawnRow, InvalidCharacters) {
    EXPECT_FALSE(string_to_pawn_row("12101").has_value());
    EXPECT_FALSE(string_to_pawn_row("1a1").has_value());
    EXPECT_FALSE(string_to_pawn_row("1 1").has_value());
    EXPECT_FALSE(string_to_pawn_row("1\n1").has_value());
}

TEST(StringToPawnRow, SingleZero) {
    auto result = string_to_pawn_row("0");
    EXPECT_FALSE(result.has_value());
}
