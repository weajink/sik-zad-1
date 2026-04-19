// Randomized, parametrized property tests for the Kayles game API.
//
// Each test case is seeded deterministically from kBaseSeed XOR'd with the
// parametric iteration index and a per-suite tag so different suites using the
// same iteration index get independent streams. A failing case prints enough
// context (seed, max_pawn, move history) to reproduce it exactly.
//
// The seed can be overridden via the KAYLES_TEST_SEED environment variable
// (interpreted as hex if prefixed with 0x, decimal otherwise).
//
// These tests exercise only the in-process game logic. No networking, no
// sockets, no file I/O, no sleeps.
//
// Scope vs. the existing test_game.cpp / test_game2.cpp:
//   - test_game.cpp / test_game2.cpp assert fixed scenarios at specific
//     boundaries (max_pawn=0, 255, alternation, give_up states, timeouts,
//     map lifecycle, serialization round-trip).
//   - THIS file runs many *random* games and checks invariants that must hold
//     for every game: strict turn alternation, monotonic pawn-count decrease,
//     final status is WIN_* (never TURN_*), bounded number of turns, winner
//     identity matches whose-turn-it-was, illegal moves are pure no-ops,
//     bitmap round-trip over random rows, concurrent games don't leak state.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

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
// Seeding and iteration count
// ===========================================================================

namespace {

    constexpr uint64_t kBaseSeed = 0xC0FFEEULL;
    constexpr int kIterations = 200;

    // Per-suite tag so that iteration i in suite A uses a different seed than
    // iteration i in suite B. These are arbitrary but fixed.
    enum class SuiteTag : uint64_t {
        Playout = 0x11111111ULL,
        Winner = 0x22222222ULL,
        IllegalMove = 0x33333333ULL,
        BitmapRoundTrip = 0x44444444ULL,
        EdgeBoundaries = 0x55555555ULL,
        TwoGamesIndependence = 0x66666666ULL,
        KeepAliveInvariance = 0x77777777ULL,
        GiveUpInvariance = 0x88888888ULL,
    };

    // Returns the base seed, optionally overridden by KAYLES_TEST_SEED.
    uint64_t base_seed() {
        static const uint64_t cached = []() -> uint64_t {
            const char *env = std::getenv("KAYLES_TEST_SEED");
            if (!env || !*env) {
                return kBaseSeed;
            }
            std::string s(env);
            try {
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    return static_cast<uint64_t>(std::stoull(s.substr(2), nullptr, 16));
                }
                return static_cast<uint64_t>(std::stoull(s, nullptr, 10));
            } catch (...) {
                return kBaseSeed;
            }
        }();
        return cached;
    }

    uint64_t seed_for(SuiteTag tag, int iter) {
        return base_seed() ^ static_cast<uint64_t>(tag) ^ static_cast<uint64_t>(iter);
    }

    pawn_row_t make_row_sized(size_t len, bool fill = true) {
        return pawn_row_t(len, fill);
    }

    struct FakeClock : public Clock {
        time_point_t t{};
        time_point_t now() const override {
            return t;
        }
        void advance(std::chrono::seconds d) {
            t += d;
        }
    };

    std::shared_ptr<FakeClock> make_fake_clock() {
        return std::make_shared<FakeClock>();
    }

    // Count up pins in a row.
    size_t popcount_row(const pawn_row_t &row) {
        return static_cast<size_t>(std::count(row.begin(), row.end(), true));
    }

    // Enumerate every currently-legal move as (pawn, no_of_pawns).
    std::vector<std::pair<size_t, uint8_t>> legal_moves(const pawn_row_t &row, pawn_t max_pawn) {
        std::vector<std::pair<size_t, uint8_t>> out;
        for (size_t p = 0; p <= max_pawn; ++p) {
            if (row[p]) {
                out.emplace_back(p, uint8_t{1});
            }
        }
        // move_2 requires pawn and pawn+1 both present; first_pawn + 1 <= max_pawn.
        if (max_pawn >= 1) {
            for (size_t p = 0; p + 1 <= max_pawn; ++p) {
                if (row[p] && row[p + 1]) {
                    out.emplace_back(p, uint8_t{2});
                }
            }
        }
        return out;
    }

    // Produce a compact, human-readable trace string for diagnostics.
    struct MoveTrace {
        uint64_t seed{};
        pawn_t max_pawn{};
        pawn_row_t initial;
        std::vector<std::tuple<player_id_t, size_t, uint8_t, GameStatus>>
            history;  // (who, pawn, kind, resulting_status)

        std::string as_string() const {
            std::ostringstream os;
            os << "\n  seed=0x" << std::hex << seed << std::dec;
            os << "\n  max_pawn=" << static_cast<unsigned>(max_pawn);
            os << "\n  initial_row=";
            for (bool b : initial)
                os << (b ? '1' : '0');
            os << "\n  moves(" << history.size() << "):";
            for (const auto &[who, p, k, st] : history) {
                os << "\n    player=" << who << " pawn=" << p
                   << " kind=" << static_cast<unsigned>(k) << " → status=" << st;
            }
            return os.str();
        }
    };

}  // namespace

// ===========================================================================
// Suite 1: RandomLegalPlayout
//
// Start from a random (max_pawn, pawn_row) and play *only* legal moves,
// chosen uniformly at random each turn, until the game ends. Assert the
// global invariants.
// ===========================================================================

class RandomLegalPlayout : public ::testing::TestWithParam<int> {};

TEST_P(RandomLegalPlayout, Invariants) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::Playout, iter);
    std::mt19937_64 rng(seed);

    // Sample max_pawn across [0, 255]. Small values overrepresented intentionally
    // (short games stress the end-of-game logic per-iteration more heavily).
    pawn_t max_pawn = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 255)(rng));

    // Random initial row — at least one pin must be up so the game is playable.
    pawn_row_t row(static_cast<size_t>(max_pawn) + 1, false);
    const size_t n = row.size();
    std::bernoulli_distribution coin(0.75);
    for (size_t i = 0; i < n; ++i) {
        row[i] = coin(rng);
    }
    if (popcount_row(row) == 0) {
        // guarantee at least one pawn
        row[std::uniform_int_distribution<size_t>(0, n - 1)(rng)] = true;
    }

    const size_t initial_pins = popcount_row(row);

    MoveTrace trace{};
    trace.seed = seed;
    trace.max_pawn = max_pawn;
    trace.initial = row;

    auto clk = make_fake_clock();
    KaylesGame g(0u, /*player_a=*/1u, max_pawn, row, clk);
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B) << trace.as_string();

    // Invariants across turns.
    GameStatus prev_status = g.get_status();
    size_t prev_pins = initial_pins;
    size_t turn_count = 0;
    const size_t max_turns = initial_pins;  // each turn removes ≥1 pin, so bounded

    while (prev_status == GameStatus::TURN_A || prev_status == GameStatus::TURN_B) {
        // Whose turn?
        const player_id_t mover = (prev_status == GameStatus::TURN_A) ? 1u : 2u;

        // Pick a legal move. There must be ≥1 legal move (at least move_1 on
        // any remaining pawn).
        auto moves = legal_moves(g.get_game_state().pawn_row, max_pawn);
        ASSERT_FALSE(moves.empty())
            << "No legal move available but status=" << prev_status << trace.as_string();

        auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
        const size_t pre_pins = popcount_row(g.get_game_state().pawn_row);

        g.move(mover, choice.first, choice.second);
        const GameStatus now_status = g.get_status();
        const size_t now_pins = popcount_row(g.get_game_state().pawn_row);

        trace.history.emplace_back(mover, choice.first, choice.second, now_status);
        SCOPED_TRACE(trace.as_string());

        // Pin count must strictly decrease by exactly choice.second.
        ASSERT_EQ(pre_pins - now_pins, static_cast<size_t>(choice.second)) << "pin delta mismatch";

        // Monotonic non-increasing (actually strictly decreasing here).
        ASSERT_LT(now_pins, prev_pins);

        // Turn alternation: after a legal move, status must either flip
        // to the opposite turn, or resolve to WIN_* for the mover.
        if (now_pins == 0) {
            ASSERT_TRUE(now_status == GameStatus::WIN_A || now_status == GameStatus::WIN_B);
            const GameStatus expected_winner =
                (mover == 1u) ? GameStatus::WIN_A : GameStatus::WIN_B;
            ASSERT_EQ(now_status, expected_winner)
                << "winner must be the mover who emptied the row";
        } else {
            const GameStatus expected_next =
                (prev_status == GameStatus::TURN_A) ? GameStatus::TURN_B : GameStatus::TURN_A;
            ASSERT_EQ(now_status, expected_next) << "turn did not alternate";
        }

        prev_status = now_status;
        prev_pins = now_pins;
        ++turn_count;
        ASSERT_LE(turn_count, max_turns) << "game ran past upper bound on turns";
    }

    // Final status must be a terminal WIN_*.
    ASSERT_TRUE(prev_status == GameStatus::WIN_A || prev_status == GameStatus::WIN_B)
        << trace.as_string();
    // And row must be fully empty.
    ASSERT_EQ(popcount_row(g.get_game_state().pawn_row), 0u) << trace.as_string();
}

INSTANTIATE_TEST_SUITE_P(GameRandom, RandomLegalPlayout, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 2: WinnerIdentity
//
// After a random legal playout, the player who made the final move is the
// winner. This is already covered in suite 1's assertions but is repeated
// here as a separate test so a failure pinpoints the specific invariant.
// ===========================================================================

class WinnerIdentity : public ::testing::TestWithParam<int> {};

TEST_P(WinnerIdentity, LastMoverIsWinner) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::Winner, iter);
    std::mt19937_64 rng(seed);

    pawn_t max_pawn = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 64)(rng));
    pawn_row_t row(static_cast<size_t>(max_pawn) + 1, true);

    MoveTrace trace{};
    trace.seed = seed;
    trace.max_pawn = max_pawn;
    trace.initial = row;

    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, max_pawn, row, clk);
    g.join_player_b(2u);

    player_id_t last_mover = 0u;
    while (true) {
        auto st = g.get_status();
        if (st == GameStatus::WIN_A || st == GameStatus::WIN_B)
            break;

        player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
        auto moves = legal_moves(g.get_game_state().pawn_row, max_pawn);
        ASSERT_FALSE(moves.empty()) << trace.as_string();
        auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
        g.move(mover, choice.first, choice.second);
        trace.history.emplace_back(mover, choice.first, choice.second, g.get_status());
        last_mover = mover;
    }

    const GameStatus expected = (last_mover == 1u) ? GameStatus::WIN_A : GameStatus::WIN_B;
    ASSERT_EQ(g.get_status(), expected) << trace.as_string();
}

INSTANTIATE_TEST_SUITE_P(GameRandom, WinnerIdentity, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 3: IllegalMoveInvariance
//
// From a random mid-game position, issue a move that is guaranteed illegal
// under at least one of these categories:
//   (a) pawn index out of [0, max_pawn],
//   (b) pawn already knocked down (empty),
//   (c) move_2 at boundary (first_pawn == max_pawn),
//   (d) move_2 where pawn+1 is already knocked,
//   (e) wrong player's turn,
//   (f) unknown player (neither A nor B).
// After each illegal move, pawn_row / status must be unchanged.
// ===========================================================================

class IllegalMoveInvariance : public ::testing::TestWithParam<int> {};

TEST_P(IllegalMoveInvariance, NoMutationOnIllegalMove) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::IllegalMove, iter);
    std::mt19937_64 rng(seed);

    // Build a random mid-game: random max_pawn, random row with ≥2 pins so at
    // least the game doesn't immediately terminate after we remove one.
    pawn_t max_pawn = static_cast<pawn_t>(std::uniform_int_distribution<int>(2, 40)(rng));
    pawn_row_t row(static_cast<size_t>(max_pawn) + 1, false);
    std::bernoulli_distribution coin(0.5);
    for (size_t i = 0; i < row.size(); ++i)
        row[i] = coin(rng);
    // Force at least 3 pins up so we can play some and still be mid-game.
    while (popcount_row(row) < 3) {
        row[std::uniform_int_distribution<size_t>(0, row.size() - 1)(rng)] = true;
    }

    MoveTrace trace{};
    trace.seed = seed;
    trace.max_pawn = max_pawn;
    trace.initial = row;

    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, max_pawn, row, clk);
    g.join_player_b(2u);

    // Play 1–2 legal moves so we are firmly mid-game.
    int prelude = std::uniform_int_distribution<int>(1, 2)(rng);
    for (int i = 0; i < prelude; ++i) {
        auto st = g.get_status();
        if (st != GameStatus::TURN_A && st != GameStatus::TURN_B)
            break;
        player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
        auto moves = legal_moves(g.get_game_state().pawn_row, max_pawn);
        if (moves.empty())
            break;
        auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
        g.move(mover, choice.first, choice.second);
        trace.history.emplace_back(mover, choice.first, choice.second, g.get_status());
    }

    // If we accidentally ended the game, skip — illegal-move-on-finished-game is
    // covered elsewhere; we need TURN_A or TURN_B here.
    if (g.get_status() != GameStatus::TURN_A && g.get_status() != GameStatus::TURN_B) {
        return;
    }

    // Snapshot state.
    const pawn_row_t row_before = g.get_game_state().pawn_row;
    const GameStatus status_before = g.get_status();

    // Pick one illegal-move variant at random.
    const int variant = std::uniform_int_distribution<int>(0, 5)(rng);
    player_id_t illegal_mover = 0u;
    size_t illegal_pawn = 0;
    uint8_t illegal_kind = 1;
    std::string variant_desc;

    const player_id_t on_turn = (status_before == GameStatus::TURN_A) ? 1u : 2u;
    const player_id_t off_turn = (on_turn == 1u) ? 2u : 1u;

    auto pick_knocked_index = [&]() -> std::optional<size_t> {
        std::vector<size_t> knocked;
        for (size_t i = 0; i <= max_pawn; ++i)
            if (!row_before[i])
                knocked.push_back(i);
        if (knocked.empty())
            return std::nullopt;
        return knocked[std::uniform_int_distribution<size_t>(0, knocked.size() - 1)(rng)];
    };

    auto pick_up_index = [&]() -> std::optional<size_t> {
        std::vector<size_t> up;
        for (size_t i = 0; i <= max_pawn; ++i)
            if (row_before[i])
                up.push_back(i);
        if (up.empty())
            return std::nullopt;
        return up[std::uniform_int_distribution<size_t>(0, up.size() - 1)(rng)];
    };

    switch (variant) {
        case 0: {
            // Pawn index out of range (> max_pawn). Use 255 if max_pawn<255, else
            // fall back to variant (a) would be impossible — skip to another variant.
            if (max_pawn < 255) {
                illegal_mover = on_turn;
                illegal_pawn = static_cast<size_t>(max_pawn) + 1 +
                               std::uniform_int_distribution<size_t>(0, 10)(rng);
                illegal_kind = std::uniform_int_distribution<int>(1, 2)(rng);
                variant_desc = "out_of_range";
                break;
            }
            [[fallthrough]];
        }
        case 1: {
            // Already-knocked pawn (move_1).
            auto ki = pick_knocked_index();
            if (!ki.has_value()) {
                // All pins up; can't construct variant (b); fall through.
                illegal_mover = off_turn;
                illegal_pawn = 0;
                illegal_kind = 1;
                variant_desc = "wrong_player_fallback_from_knocked";
                break;
            }
            illegal_mover = on_turn;
            illegal_pawn = *ki;
            illegal_kind = 1;
            variant_desc = "already_knocked_move1";
            break;
        }
        case 2: {
            // move_2 at boundary (first_pawn == max_pawn). Requires row[max_pawn]
            // to still be up to isolate *this* illegality (otherwise it's also
            // illegal under "already knocked" which is fine). Either way it's
            // illegal.
            illegal_mover = on_turn;
            illegal_pawn = max_pawn;
            illegal_kind = 2;
            variant_desc = "move2_at_boundary";
            break;
        }
        case 3: {
            // move_2 where pawn+1 is knocked. Find i such that row[i] && !row[i+1].
            std::vector<size_t> candidates;
            for (size_t i = 0; i + 1 <= max_pawn; ++i) {
                if (row_before[i] && !row_before[i + 1])
                    candidates.push_back(i);
            }
            if (candidates.empty()) {
                // Fall back to boundary case.
                illegal_mover = on_turn;
                illegal_pawn = max_pawn;
                illegal_kind = 2;
                variant_desc = "move2_at_boundary_fallback";
                break;
            }
            illegal_mover = on_turn;
            illegal_pawn =
                candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(rng)];
            illegal_kind = 2;
            variant_desc = "move2_neighbor_knocked";
            break;
        }
        case 4: {
            // Wrong player's turn. Pick any currently-legal-shape move but use
            // the off-turn player.
            auto ui = pick_up_index();
            if (!ui.has_value())
                return;  // row empty; game should have ended
            illegal_mover = off_turn;
            illegal_pawn = *ui;
            illegal_kind = 1;
            variant_desc = "wrong_player";
            break;
        }
        case 5: {
            // Unknown player.
            auto ui = pick_up_index();
            if (!ui.has_value())
                return;
            illegal_mover = 9999u;  // neither A (1) nor B (2)
            illegal_pawn = *ui;
            illegal_kind = 1;
            variant_desc = "unknown_player";
            break;
        }
        default:
            GTEST_FAIL() << "unreachable variant";
    }

    g.move(illegal_mover, illegal_pawn, illegal_kind);

    SCOPED_TRACE(trace.as_string());
    EXPECT_EQ(g.get_status(), status_before)
        << "illegal move (" << variant_desc << ") changed status";
    EXPECT_EQ(g.get_game_state().pawn_row, row_before)
        << "illegal move (" << variant_desc << ") changed pawn_row";
}

INSTANTIATE_TEST_SUITE_P(GameRandom, IllegalMoveInvariance, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 4: BitmapRoundTrip
//
// For a random max_pawn and random bool vector, serialize the GameState
// carrying that row through GameState::serialize, then deserialize via
// deserialize_game_state. The row must round-trip bit-for-bit. We also check
// two structural invariants of the serialized form:
//   - pawn 0 must be the MSB of the first bitmap byte,
//   - any bit beyond index max_pawn must be zero.
// ===========================================================================

class BitmapRoundTrip : public ::testing::TestWithParam<int> {};

TEST_P(BitmapRoundTrip, StructurallyCorrectAndLossless) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::BitmapRoundTrip, iter);
    std::mt19937_64 rng(seed);

    pawn_t max_pawn = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 255)(rng));
    pawn_row_t row(static_cast<size_t>(max_pawn) + 1, false);
    std::bernoulli_distribution coin(0.5);
    for (size_t i = 0; i < row.size(); ++i)
        row[i] = coin(rng);

    GameState gs{};
    gs.game_id = static_cast<game_id_t>(
        std::uniform_int_distribution<uint32_t>(0, std::numeric_limits<uint32_t>::max())(rng));
    gs.player_a_id = 1u + (std::uniform_int_distribution<uint32_t>(
                              0, std::numeric_limits<uint32_t>::max() - 1)(rng));
    gs.player_b_id = 1u + (std::uniform_int_distribution<uint32_t>(
                              0, std::numeric_limits<uint32_t>::max() - 1)(rng));
    gs.status = GameStatus::TURN_B;
    gs.max_pawn = max_pawn;
    gs.pawn_row = row;

    const auto bytes = gs.serialize();
    ASSERT_EQ(bytes.size(), 4u + 4u + 4u + 1u + 1u + (static_cast<size_t>(max_pawn) / 8 + 1))
        << "seed=0x" << std::hex << seed;

    // Structural check: pawn 0 is the MSB of byte 14.
    const uint8_t first_bitmap_byte = bytes[14];
    EXPECT_EQ((first_bitmap_byte >> 7) & 1u, row[0] ? 1u : 0u)
        << "MSB of first bitmap byte must encode pawn 0 (seed=0x" << std::hex << seed << ")";

    // Structural check: any bit past max_pawn must be zero.
    const size_t total_bits = (static_cast<size_t>(max_pawn) / 8 + 1) * 8;
    for (size_t bit = static_cast<size_t>(max_pawn) + 1; bit < total_bits; ++bit) {
        const size_t byte_idx = 14 + bit / 8;
        const size_t in_byte = bit % 8;
        const uint8_t mask = static_cast<uint8_t>(1u << (7 - in_byte));
        ASSERT_EQ(bytes[byte_idx] & mask, 0u)
            << "excess bit " << bit << " must be zero (seed=0x" << std::hex << seed << ")";
    }

    auto round = deserialize_game_state(bytes);
    ASSERT_TRUE(round.has_value())
        << "deserialize failed (seed=0x" << std::hex << seed << "): " << round.error().what();
    EXPECT_EQ(round->game_id, gs.game_id);
    EXPECT_EQ(round->player_a_id, gs.player_a_id);
    EXPECT_EQ(round->player_b_id, gs.player_b_id);
    EXPECT_EQ(round->status, gs.status);
    EXPECT_EQ(round->max_pawn, gs.max_pawn);
    ASSERT_EQ(round->pawn_row.size(), row.size()) << "seed=0x" << std::hex << seed;
    for (size_t i = 0; i < row.size(); ++i) {
        ASSERT_EQ(round->pawn_row[i], row[i])
            << "bit " << i << " mismatch (seed=0x" << std::hex << seed << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, BitmapRoundTrip, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 5: EdgeBoundaries
//
// Targeted per-iteration edge cases sampled at random. Unlike test_game.cpp
// which hardcodes these, this suite picks which edge to poke at each
// iteration and runs hundreds of permutations of them.
// ===========================================================================

class EdgeBoundaries : public ::testing::TestWithParam<int> {};

TEST_P(EdgeBoundaries, Varies) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::EdgeBoundaries, iter);
    std::mt19937_64 rng(seed);

    const int flavor = std::uniform_int_distribution<int>(0, 3)(rng);

    auto clk = make_fake_clock();

    switch (flavor) {
        case 0: {
            // max_pawn = 0 (single pin). First legal move wins for mover.
            KaylesGame g(0u, 1u, /*max_pawn=*/0u, make_row_sized(1, true), clk);
            g.join_player_b(2u);
            ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
            g.move(2u, 0, 1);
            ASSERT_EQ(g.get_status(), GameStatus::WIN_B) << "seed=0x" << std::hex << seed;
            break;
        }
        case 1: {
            // max_pawn = 255, all ones. Knock a random pawn at a random boundary.
            KaylesGame g(0u, 1u, /*max_pawn=*/255u, make_row_sized(256, true), clk);
            g.join_player_b(2u);
            // Bitmap bytes = 256/8 + 1 = 33.
            // Pick boundary: 0, 7, 8, 254, 255.
            const std::vector<size_t> boundaries = {0, 7, 8, 127, 128, 254, 255};
            size_t pawn =
                boundaries[std::uniform_int_distribution<size_t>(0, boundaries.size() - 1)(rng)];
            g.move(2u, pawn, 1);
            const auto &gs = g.get_game_state();
            ASSERT_EQ(gs.pawn_row.size(), 256u);
            ASSERT_FALSE(gs.pawn_row[pawn]);
            ASSERT_EQ(g.get_status(), GameStatus::TURN_A) << "seed=0x" << std::hex << seed;
            // 255 pins remain → not terminal.

            auto bytes = gs.serialize();
            // 14 header + 32 bitmap bytes (255/8 + 1 = 32).
            ASSERT_EQ(bytes.size(), 14u + 32u);
            // High bit of (byte 14 + pawn/8) at position 7 - pawn%8 must be 0.
            size_t byte_idx = 14 + pawn / 8;
            uint8_t mask = static_cast<uint8_t>(1u << (7 - pawn % 8));
            ASSERT_EQ(bytes[byte_idx] & mask, 0u) << "downed pin not cleared in bitmap";
            break;
        }
        case 2: {
            // All-ones row, move_2 at the last adjacent pair.
            pawn_t mp = static_cast<pawn_t>(std::uniform_int_distribution<int>(1, 255)(rng));
            pawn_row_t row(static_cast<size_t>(mp) + 1, true);
            KaylesGame g(0u, 1u, mp, row, clk);
            g.join_player_b(2u);
            g.move(2u, /*pawn=*/static_cast<size_t>(mp) - 1, /*no=*/2);
            const auto &gs = g.get_game_state();
            ASSERT_FALSE(gs.pawn_row[mp - 1]);
            ASSERT_FALSE(gs.pawn_row[mp]);
            if (mp >= 2) {
                ASSERT_EQ(g.get_status(), GameStatus::TURN_A)
                    << "seed=0x" << std::hex << seed << " mp=" << static_cast<unsigned>(mp);
                ASSERT_TRUE(gs.pawn_row[mp - 2]);
            } else {
                // mp == 1: only 2 pins, taking both wins.
                ASSERT_EQ(g.get_status(), GameStatus::WIN_B) << "seed=0x" << std::hex << seed;
            }
            break;
        }
        case 3: {
            // move_2 at first_pawn == max_pawn: always illegal. Random max_pawn.
            pawn_t mp = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 255)(rng));
            pawn_row_t row(static_cast<size_t>(mp) + 1, true);
            KaylesGame g(0u, 1u, mp, row, clk);
            g.join_player_b(2u);
            const auto before = g.get_game_state().pawn_row;
            g.move(2u, /*pawn=*/static_cast<size_t>(mp), /*no=*/2);
            ASSERT_EQ(g.get_status(), GameStatus::TURN_B) << "seed=0x" << std::hex << seed;
            ASSERT_EQ(g.get_game_state().pawn_row, before);
            break;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, EdgeBoundaries, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 6: TwoGamesIndependence
//
// Two distinct KaylesGame instances evolving in parallel. Interleave random
// legal moves on both. Each game's state at any point must match a solo
// replay of its own move log — i.e. the moves on the other game cannot bleed
// in. We verify this by replaying the recorded log on a fresh game and
// comparing resulting pawn_row and status.
// ===========================================================================

class TwoGamesIndependence : public ::testing::TestWithParam<int> {};

TEST_P(TwoGamesIndependence, NoCrossTalk) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::TwoGamesIndependence, iter);
    std::mt19937_64 rng(seed);

    auto make_random_game = [&](pawn_t &max_pawn, pawn_row_t &row) {
        max_pawn = static_cast<pawn_t>(std::uniform_int_distribution<int>(1, 16)(rng));
        row.assign(static_cast<size_t>(max_pawn) + 1, false);
        std::bernoulli_distribution coin(0.7);
        for (size_t i = 0; i < row.size(); ++i)
            row[i] = coin(rng);
        while (popcount_row(row) == 0) {
            row[std::uniform_int_distribution<size_t>(0, row.size() - 1)(rng)] = true;
        }
    };

    pawn_t mp1 = 0, mp2 = 0;
    pawn_row_t row1, row2;
    make_random_game(mp1, row1);
    make_random_game(mp2, row2);

    auto clk = make_fake_clock();
    KaylesGame g1(1u, 1u, mp1, row1, clk);
    KaylesGame g2(2u, 1u, mp2, row2, clk);
    g1.join_player_b(2u);
    g2.join_player_b(2u);  // same player IDs across games on purpose

    // Move logs per game (just the user-level calls on that game).
    std::vector<std::tuple<player_id_t, size_t, uint8_t>> log1, log2;

    // Interleave up to 50 moves.
    for (int step = 0; step < 50; ++step) {
        const bool pick_g1 = std::bernoulli_distribution(0.5)(rng);
        KaylesGame &g = pick_g1 ? g1 : g2;
        auto &log = pick_g1 ? log1 : log2;
        const pawn_t mp = pick_g1 ? mp1 : mp2;
        auto st = g.get_status();
        if (st != GameStatus::TURN_A && st != GameStatus::TURN_B)
            continue;

        player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
        auto moves = legal_moves(g.get_game_state().pawn_row, mp);
        if (moves.empty())
            continue;
        auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
        g.move(mover, choice.first, choice.second);
        log.emplace_back(mover, choice.first, choice.second);
    }

    // Replay log1 on a fresh game; must match g1.
    auto clk1 = make_fake_clock();
    KaylesGame r1(1u, 1u, mp1, row1, clk1);
    r1.join_player_b(2u);
    for (auto &[who, p, k] : log1)
        r1.move(who, p, k);

    auto clk2 = make_fake_clock();
    KaylesGame r2(2u, 1u, mp2, row2, clk2);
    r2.join_player_b(2u);
    for (auto &[who, p, k] : log2)
        r2.move(who, p, k);

    SCOPED_TRACE("seed=0x" + [&] {
        std::ostringstream os;
        os << std::hex << seed;
        return os.str();
    }());
    EXPECT_EQ(g1.get_status(), r1.get_status());
    EXPECT_EQ(g1.get_game_state().pawn_row, r1.get_game_state().pawn_row);
    EXPECT_EQ(g2.get_status(), r2.get_status());
    EXPECT_EQ(g2.get_game_state().pawn_row, r2.get_game_state().pawn_row);
}

INSTANTIATE_TEST_SUITE_P(GameRandom, TwoGamesIndependence, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 7: KeepAliveInvariance
//
// keep_alive must never change pawn_row or status, regardless of who calls
// it or when. This is a small but important correctness invariant because
// the production code also uses keep_alive as a side effect of move/give_up.
// ===========================================================================

class KeepAliveInvariance : public ::testing::TestWithParam<int> {};

TEST_P(KeepAliveInvariance, NoGameStateMutation) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::KeepAliveInvariance, iter);
    std::mt19937_64 rng(seed);

    pawn_t mp = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 64)(rng));
    pawn_row_t row(static_cast<size_t>(mp) + 1, false);
    std::bernoulli_distribution coin(0.5);
    for (size_t i = 0; i < row.size(); ++i)
        row[i] = coin(rng);
    if (popcount_row(row) == 0) {
        row[std::uniform_int_distribution<size_t>(0, row.size() - 1)(rng)] = true;
    }

    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, mp, row, clk);
    g.join_player_b(2u);

    auto snapshot = [&]() {
        return std::pair<GameStatus, pawn_row_t>{g.get_status(), g.get_game_state().pawn_row};
    };

    const std::vector<player_id_t> callers = {1u, 2u, 99u /*non-player*/, 0u /*never-valid*/};
    for (int i = 0; i < 5; ++i) {
        auto before = snapshot();
        player_id_t who =
            callers[std::uniform_int_distribution<size_t>(0, callers.size() - 1)(rng)];
        g.keep_alive(who);
        auto after = snapshot();
        EXPECT_EQ(before.first, after.first)
            << "keep_alive by player " << who << " changed status (seed=0x" << std::hex << seed
            << ")";
        EXPECT_EQ(before.second, after.second)
            << "keep_alive by player " << who << " changed pawn_row (seed=0x" << std::hex << seed
            << ")";

        // Between invocations, occasionally play a legal move so that we're
        // not always checking the initial state.
        auto st = g.get_status();
        if (st == GameStatus::TURN_A || st == GameStatus::TURN_B) {
            auto moves = legal_moves(g.get_game_state().pawn_row, mp);
            if (!moves.empty() && std::bernoulli_distribution(0.5)(rng)) {
                player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
                auto choice =
                    moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
                g.move(mover, choice.first, choice.second);
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, KeepAliveInvariance, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 8: GiveUpInvariance
//
// give_up by the on-turn player in TURN_A / TURN_B immediately transitions
// to the opposite WIN_*. give_up by anyone else (off-turn player, unknown
// player) is a pure no-op. After WIN_*, no further give_up may change state.
// ===========================================================================

class GiveUpInvariance : public ::testing::TestWithParam<int> {};

TEST_P(GiveUpInvariance, TransitionsAndNoOps) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::GiveUpInvariance, iter);
    std::mt19937_64 rng(seed);

    pawn_t mp = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 32)(rng));
    pawn_row_t row(static_cast<size_t>(mp) + 1, true);

    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, mp, row, clk);
    g.join_player_b(2u);

    // Play a random number of moves (could be zero) before testing give_up.
    const int to_play = std::uniform_int_distribution<int>(0, 5)(rng);
    for (int i = 0; i < to_play; ++i) {
        auto st = g.get_status();
        if (st != GameStatus::TURN_A && st != GameStatus::TURN_B)
            break;
        player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
        auto moves = legal_moves(g.get_game_state().pawn_row, mp);
        if (moves.empty())
            break;
        auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
        g.move(mover, choice.first, choice.second);
    }

    const GameStatus status_before = g.get_status();
    const pawn_row_t row_before = g.get_game_state().pawn_row;
    SCOPED_TRACE("seed=0x" + [&] {
        std::ostringstream os;
        os << std::hex << seed;
        return os.str();
    }());

    if (status_before == GameStatus::TURN_A || status_before == GameStatus::TURN_B) {
        const player_id_t on_turn = (status_before == GameStatus::TURN_A) ? 1u : 2u;
        const player_id_t off_turn = (on_turn == 1u) ? 2u : 1u;
        const GameStatus expected_win = (on_turn == 1u) ? GameStatus::WIN_B : GameStatus::WIN_A;

        if (std::bernoulli_distribution(0.5)(rng)) {
            // Off-turn give_up: no-op.
            g.give_up(off_turn);
            EXPECT_EQ(g.get_status(), status_before);
            EXPECT_EQ(g.get_game_state().pawn_row, row_before);

            // Also: unknown player.
            g.give_up(9999u);
            EXPECT_EQ(g.get_status(), status_before);

            // Now the legit give_up.
            g.give_up(on_turn);
            EXPECT_EQ(g.get_status(), expected_win);
            EXPECT_EQ(g.get_game_state().pawn_row, row_before);
        } else {
            // Direct legit give_up.
            g.give_up(on_turn);
            EXPECT_EQ(g.get_status(), expected_win);
            EXPECT_EQ(g.get_game_state().pawn_row, row_before);
        }

        // After WIN_*: further give_ups must not change anything.
        const GameStatus finished = g.get_status();
        const pawn_row_t row_finished = g.get_game_state().pawn_row;
        g.give_up(on_turn);
        g.give_up(off_turn);
        g.give_up(9999u);
        EXPECT_EQ(g.get_status(), finished);
        EXPECT_EQ(g.get_game_state().pawn_row, row_finished);
    } else {
        // Already WIN_*: give_up is a no-op by any player.
        g.give_up(1u);
        g.give_up(2u);
        g.give_up(9999u);
        EXPECT_EQ(g.get_status(), status_before);
        EXPECT_EQ(g.get_game_state().pawn_row, row_before);
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, GiveUpInvariance, ::testing::Range(0, kIterations));
