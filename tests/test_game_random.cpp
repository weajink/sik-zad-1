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
//
// Suite index (in order of appearance):
//   1.  RandomLegalPlayout
//   2.  WinnerIdentity
//   3.  IllegalMoveInvariance
//   4.  BitmapRoundTrip
//   5.  EdgeBoundaries
//   6.  TwoGamesIndependence
//   7.  KeepAliveInvariance
//   8.  GiveUpInvariance
//   9.  TimeoutAwardsOlderPlayerLoses
//   10. TimeoutBoundaryExactness
//   11. TimeoutDoesNothingInTerminalOrWaitingStatus
//   12. IsStaleWaitingVsTerminal
//   13. KeepAliveDefersTimeout
//   14. MoveDefersTimeout
//   15. GiveUpDefersStaleness
//   16. TimeoutVsGiveUpRace
//   17. NonPlayerKeepAliveDoesNotDeferTimeout
//   18. MoveOnFinishedGameIsNoOp

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
        TimeoutAwardsOlder = 0x99999999ULL,
        TimeoutBoundaryExact = 0xAAAAAAAAULL,
        TimeoutNoopInTerminalOrWaiting = 0xBBBBBBBBULL,
        IsStaleCases = 0xCCCCCCCCULL,
        KeepAliveDefers = 0xDDDDDDDDULL,
        MoveDefers = 0xEEEEEEEEULL,
        GiveUpDefersStale = 0xFFFFFFFFULL,
        TimeoutVsGiveUp = 0x1212121212121212ULL,
        NonPlayerKeepAlive = 0x1313131313131313ULL,
        MoveOnFinished = 0x1414141414141414ULL,
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
        // Fine-grained advance used by timeout-boundary tests (ns resolution).
        template <class Rep, class Period>
        void advance_by(const std::chrono::duration<Rep, Period> &d) {
            t += std::chrono::duration_cast<time_point_t::duration>(d);
        }
        void set(const time_point_t &tp) {
            t = tp;
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

// Variant-coverage counters (roughly even distribution expected; printed by
// an Environment::TearDown hook below).
namespace {
    struct IllegalVariantStats {
        size_t out_of_range{};
        size_t already_knocked_move1{};
        size_t move2_at_boundary{};
        size_t move2_neighbor_knocked{};
        size_t wrong_player{};
        size_t unknown_player{};
        size_t regenerated_mid_games{};
        size_t chosen_total{};
    };
    IllegalVariantStats &illegal_stats() {
        static IllegalVariantStats s;
        return s;
    }

    class IllegalStatsEnv : public ::testing::Environment {
       public:
        ~IllegalStatsEnv() override = default;
        void TearDown() override {
            auto &s = illegal_stats();
            if (s.chosen_total == 0)
                return;
            // Expected distribution rationale: 6 variants, roughly uniform.
            // Since some are not constructible for every random mid-game, we
            // tolerate asymmetry but each variant should still fire many
            // times across kIterations=200.
            std::cerr << "[IllegalMoveInvariance coverage] total=" << s.chosen_total
                      << " regen=" << s.regenerated_mid_games << " out_of_range=" << s.out_of_range
                      << " already_knocked_move1=" << s.already_knocked_move1
                      << " move2_at_boundary=" << s.move2_at_boundary
                      << " move2_neighbor_knocked=" << s.move2_neighbor_knocked
                      << " wrong_player=" << s.wrong_player
                      << " unknown_player=" << s.unknown_player << "\n";
        }
    };
    const ::testing::Environment *const kIllegalStatsEnv =
        ::testing::AddGlobalTestEnvironment(new IllegalStatsEnv);
}  // namespace

TEST_P(IllegalMoveInvariance, NoMutationOnIllegalMove) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::IllegalMove, iter);
    std::mt19937_64 rng(seed);

    MoveTrace trace{};
    trace.seed = seed;

    // Helper: try to produce a mid-game state. Returns the game by out-param
    // and fills `trace`. Returns the max_pawn on success.
    auto build_mid_game = [&](pawn_t &max_pawn_out,
                              pawn_row_t &row_out) -> std::unique_ptr<KaylesGame> {
        pawn_t max_pawn = static_cast<pawn_t>(std::uniform_int_distribution<int>(2, 40)(rng));
        pawn_row_t row(static_cast<size_t>(max_pawn) + 1, false);
        std::bernoulli_distribution coin(0.5);
        for (size_t i = 0; i < row.size(); ++i)
            row[i] = coin(rng);
        while (popcount_row(row) < 3) {
            row[std::uniform_int_distribution<size_t>(0, row.size() - 1)(rng)] = true;
        }

        auto clk = make_fake_clock();
        auto g = std::make_unique<KaylesGame>(0u, 1u, max_pawn, row, clk);
        g->join_player_b(2u);

        int prelude = std::uniform_int_distribution<int>(1, 2)(rng);
        for (int i = 0; i < prelude; ++i) {
            auto st = g->get_status();
            if (st != GameStatus::TURN_A && st != GameStatus::TURN_B)
                break;
            player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
            auto moves = legal_moves(g->get_game_state().pawn_row, max_pawn);
            if (moves.empty())
                break;
            auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
            g->move(mover, choice.first, choice.second);
        }

        max_pawn_out = max_pawn;
        row_out = row;
        return g;
    };

    // Variant kinds.
    enum class Kind {
        OutOfRange,
        AlreadyKnockedMove1,
        Move2AtBoundary,
        Move2NeighborKnocked,
        WrongPlayer,
        UnknownPlayer,
    };

    struct Selection {
        Kind kind;
        player_id_t mover;
        size_t pawn;
        uint8_t num;
        const char *desc;
    };

    // Try up to 20 regenerations to find a mid-game state where we can pick
    // at least one constructible variant.
    std::unique_ptr<KaylesGame> gptr;
    pawn_t max_pawn = 0;
    pawn_row_t initial_row;
    std::vector<Selection> constructible;
    int attempts = 0;
    const int kMaxAttempts = 20;
    for (; attempts < kMaxAttempts; ++attempts) {
        gptr = build_mid_game(max_pawn, initial_row);
        auto st = gptr->get_status();
        if (st != GameStatus::TURN_A && st != GameStatus::TURN_B) {
            illegal_stats().regenerated_mid_games++;
            continue;
        }

        const pawn_row_t &row_now = gptr->get_game_state().pawn_row;
        const player_id_t on_turn = (st == GameStatus::TURN_A) ? 1u : 2u;
        const player_id_t off_turn = (on_turn == 1u) ? 2u : 1u;

        constructible.clear();

        // Variant: out-of-range pawn index. Constructible iff max_pawn < 255.
        if (max_pawn < 255) {
            size_t p = static_cast<size_t>(max_pawn) + 1 +
                       std::uniform_int_distribution<size_t>(0, 10)(rng);
            uint8_t k = static_cast<uint8_t>(std::uniform_int_distribution<int>(1, 2)(rng));
            constructible.push_back({Kind::OutOfRange, on_turn, p, k, "out_of_range"});
        }

        // Variant: already-knocked pawn via move_1. Constructible iff at least one
        // pawn is already knocked.
        {
            std::vector<size_t> knocked;
            for (size_t i = 0; i <= max_pawn; ++i)
                if (!row_now[i])
                    knocked.push_back(i);
            if (!knocked.empty()) {
                size_t p =
                    knocked[std::uniform_int_distribution<size_t>(0, knocked.size() - 1)(rng)];
                constructible.push_back(
                    {Kind::AlreadyKnockedMove1, on_turn, p, 1, "already_knocked_move1"});
            }
        }

        // Variant: move_2 at boundary (first_pawn == max_pawn). Always constructible
        // regardless of row state — it's illegal because first_pawn+1 > max_pawn.
        constructible.push_back({Kind::Move2AtBoundary, on_turn, static_cast<size_t>(max_pawn), 2,
                                 "move2_at_boundary"});

        // Variant: move_2 where first_pawn is up but pawn+1 is knocked.
        {
            std::vector<size_t> candidates;
            for (size_t i = 0; i + 1 <= max_pawn; ++i) {
                if (row_now[i] && !row_now[i + 1])
                    candidates.push_back(i);
            }
            if (!candidates.empty()) {
                size_t p = candidates[std::uniform_int_distribution<size_t>(
                    0, candidates.size() - 1)(rng)];
                constructible.push_back(
                    {Kind::Move2NeighborKnocked, on_turn, p, 2, "move2_neighbor_knocked"});
            }
        }

        // Variant: wrong player's turn. Constructible iff there is any pin up.
        {
            std::vector<size_t> up;
            for (size_t i = 0; i <= max_pawn; ++i)
                if (row_now[i])
                    up.push_back(i);
            if (!up.empty()) {
                size_t p = up[std::uniform_int_distribution<size_t>(0, up.size() - 1)(rng)];
                constructible.push_back({Kind::WrongPlayer, off_turn, p, 1, "wrong_player"});
            }
        }

        // Variant: unknown player. Always constructible: any stranger id != 1, != 2, != 0.
        {
            size_t p_idx = 0;
            std::vector<size_t> up;
            for (size_t i = 0; i <= max_pawn; ++i)
                if (row_now[i])
                    up.push_back(i);
            if (!up.empty()) {
                p_idx = up[std::uniform_int_distribution<size_t>(0, up.size() - 1)(rng)];
            }
            // player_id 9999 is neither a, b, nor 0.
            constructible.push_back({Kind::UnknownPlayer, 9999u, p_idx, 1, "unknown_player"});
        }

        if (!constructible.empty()) {
            break;
        }
        illegal_stats().regenerated_mid_games++;
    }
    ASSERT_LT(attempts, kMaxAttempts)
        << "failed to find a mid-game with any constructible illegal variant after " << kMaxAttempts
        << " attempts (seed=0x" << std::hex << seed << ")";

    trace.max_pawn = max_pawn;
    trace.initial = initial_row;

    const pawn_row_t row_before = gptr->get_game_state().pawn_row;
    const GameStatus status_before = gptr->get_status();
    ASSERT_TRUE(status_before == GameStatus::TURN_A || status_before == GameStatus::TURN_B);

    const auto &sel =
        constructible[std::uniform_int_distribution<size_t>(0, constructible.size() - 1)(rng)];

    // Update coverage counters.
    illegal_stats().chosen_total++;
    switch (sel.kind) {
        case Kind::OutOfRange:
            illegal_stats().out_of_range++;
            break;
        case Kind::AlreadyKnockedMove1:
            illegal_stats().already_knocked_move1++;
            break;
        case Kind::Move2AtBoundary:
            illegal_stats().move2_at_boundary++;
            break;
        case Kind::Move2NeighborKnocked:
            illegal_stats().move2_neighbor_knocked++;
            break;
        case Kind::WrongPlayer:
            illegal_stats().wrong_player++;
            break;
        case Kind::UnknownPlayer:
            illegal_stats().unknown_player++;
            break;
    }

    gptr->move(sel.mover, sel.pawn, sel.num);

    SCOPED_TRACE(trace.as_string());
    EXPECT_EQ(gptr->get_status(), status_before)
        << "illegal move (" << sel.desc << ") changed status";
    EXPECT_EQ(gptr->get_game_state().pawn_row, row_before)
        << "illegal move (" << sel.desc << ") changed pawn_row";
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

// ===========================================================================
// Suite 9: TimeoutAwardsOlderPlayerLoses
//
// Per `check_timeouts`:
//   if (a_last < b_last && now - a_last > timeout) -> WIN_B
//   else if (now - b_last > timeout)               -> WIN_A
// Interpretation: whoever's last-move-time is older loses *if* that stale
// delta exceeds the timeout. When BOTH players are past timeout, the first
// branch ("a older, a loses") fires only when a is strictly older than b;
// otherwise we fall through to the else-if which awards WIN_A (b loses).
// ===========================================================================

class TimeoutAwardsOlderPlayerLoses : public ::testing::TestWithParam<int> {};

TEST_P(TimeoutAwardsOlderPlayerLoses, CorrectWinnerOnTimeout) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::TimeoutAwardsOlder, iter);
    std::mt19937_64 rng(seed);

    // Random but modest timeout in seconds.
    const int timeout_sec = std::uniform_int_distribution<int>(1, 30)(rng);
    const timeout_t server_timeout = std::chrono::seconds(timeout_sec);

    // We exercise three scenarios:
    //   0: only A is past timeout  -> WIN_B (A is older)
    //   1: only B is past timeout  -> WIN_A (B is older)
    //   2: both past, A older      -> WIN_B
    //   3: both past, B older      -> WIN_A
    const int scenario = std::uniform_int_distribution<int>(0, 3)(rng);

    auto clk = make_fake_clock();
    // Start at a non-zero baseline so we can move the clock backwards mentally
    // without getting negative time_points.
    clk->advance(std::chrono::seconds(1000));

    KaylesGame g(0u, 1u, /*max_pawn=*/8, pawn_row_t(9, true), clk);
    g.join_player_b(2u);  // status -> TURN_B, b_last = a_last = t (1000s)

    // Randomly choose: stay in TURN_B (no extra moves) or play one legal move
    // so we end in TURN_A. Either way both players' last_move_time values
    // equal the current clock (since move/join_player_b both invoke
    // keep_alive on the mover first).
    if (std::bernoulli_distribution(0.5)(rng)) {
        // Take one pawn as B so status flips to TURN_A. B's timer resets to now
        // as a side effect of move->keep_alive. A's timer is unchanged (still
        // equal since clock hasn't advanced since join).
        g.move(2u, /*pawn=*/0, /*num=*/1);
        ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
    }

    // At this point both a_last and b_last equal the current clock.
    const GameStatus status_before = g.get_status();
    ASSERT_TRUE(status_before == GameStatus::TURN_A || status_before == GameStatus::TURN_B);

    // We manipulate player timers via keep_alive calls after advancing clock.
    // Strategy: advance clock by `skew_a` seconds, call keep_alive(2) to freshen
    // B, then advance further by `skew_b` seconds, then call keep_alive(1) to
    // freshen A at that fresh-but-still-old point. This way, at time T_end,
    // we can pick which player is older and by how much.
    //
    // Simpler approach: after construction, both timers equal the current t.
    // Advance clock, then for whichever player we want to be *younger*, call
    // keep_alive(that_player) to reset their timer to `now`.

    // Advance clock far beyond timeout so both players are "stale" initially.
    const auto huge = std::chrono::seconds(timeout_sec * 5 + 10);
    clk->advance(huge);

    // Now freshen whichever player is supposed to be younger.
    auto freshen_and_stale = [&](player_id_t younger, int stale_amount_seconds) {
        // Freshen the "younger" player: set their timer to now.
        g.keep_alive(younger);
        // Now advance further so older player is past timeout while younger
        // player is within timeout (since we just freshened them).
        (void)stale_amount_seconds;
    };

    std::string desc;
    GameStatus expected{};
    // We want one of the following final setups (relative timestamps at test's
    // final check_timeouts call, with `server_timeout = T`):
    switch (scenario) {
        case 0: {
            // A older, only A past timeout.
            // a_last = t0 (very old), b_last = now - (T/2).
            // Advance clock by T/2 + 1s, freshen B, advance again carefully so
            // now - a_last > T but now - b_last <= T.
            // Currently: both timers = t0; we advanced by `huge` > T.
            // Reset B to now.
            g.keep_alive(2u);
            // Now: a_last very old, b_last = now. now - b_last = 0 < T. now - a_last ~= huge > T.
            // -> WIN_B
            desc = "only_A_past_timeout_expect_WIN_B";
            expected = GameStatus::WIN_B;
            break;
        }
        case 1: {
            // B older, only B past timeout.
            g.keep_alive(1u);  // reset A
            desc = "only_B_past_timeout_expect_WIN_A";
            expected = GameStatus::WIN_A;
            break;
        }
        case 2: {
            // Both past timeout, A strictly older than B.
            // Step 1: freshen B (so B is a bit younger), step 2: advance past
            // timeout again so both become stale, but A is strictly older.
            g.keep_alive(2u);
            // Now A is older. Advance past T so both become stale.
            clk->advance(std::chrono::seconds(timeout_sec + 5));
            // a_last = very old; b_last = prior_now; both > T; A < B strictly.
            desc = "both_past_A_older_expect_WIN_B";
            expected = GameStatus::WIN_B;
            break;
        }
        case 3: {
            // Both past timeout, B strictly older than A.
            g.keep_alive(1u);
            clk->advance(std::chrono::seconds(timeout_sec + 5));
            desc = "both_past_B_older_expect_WIN_A";
            expected = GameStatus::WIN_A;
            break;
        }
    }
    (void)freshen_and_stale;

    g.check_timeouts(server_timeout);

    EXPECT_EQ(g.get_status(), expected)
        << desc << " scenario=" << scenario << " timeout=" << timeout_sec << "s seed=0x" << std::hex
        << seed << " pre_status=" << status_before;
}

INSTANTIATE_TEST_SUITE_P(GameRandom, TimeoutAwardsOlderPlayerLoses,
                         ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 10: TimeoutBoundaryExactness
//
// The code uses strict `>` not `>=`:
//   now - last > server_timeout   -> trigger.
// So `now - last == server_timeout` must NOT trigger.
// `now - last == server_timeout + 1ns` must trigger.
// We exercise both sides of the boundary for random server_timeout values.
// ===========================================================================

class TimeoutBoundaryExactness : public ::testing::TestWithParam<int> {};

TEST_P(TimeoutBoundaryExactness, StrictGreaterThanOnly) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::TimeoutBoundaryExact, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(1, 50)(rng);
    const timeout_t server_timeout = std::chrono::seconds(timeout_sec);

    auto build = [&]() {
        auto clk = make_fake_clock();
        clk->advance(std::chrono::seconds(1000));
        auto g = std::make_unique<KaylesGame>(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
        g->join_player_b(2u);
        return std::pair{std::move(g), clk};
    };

    // Case A: exactly equal — must NOT trigger.
    {
        auto [g, clk] = build();
        // Both timers = clk now. Advance by exactly `server_timeout`.
        clk->advance(server_timeout);
        g->check_timeouts(server_timeout);
        EXPECT_EQ(g->get_status(), GameStatus::TURN_B)
            << "equal-to-timeout must NOT trigger (seed=0x" << std::hex << seed
            << " T=" << timeout_sec << "s)";
    }

    // Case B: just past by 1ns — MUST trigger (both stale, A==B so else-if
    // branch hits: WIN_A).
    {
        auto [g, clk] = build();
        clk->advance(server_timeout);
        clk->advance_by(std::chrono::nanoseconds(1));
        g->check_timeouts(server_timeout);
        EXPECT_NE(g->get_status(), GameStatus::TURN_B)
            << "1ns past timeout must trigger (seed=0x" << std::hex << seed << " T=" << timeout_sec
            << "s)";
        EXPECT_TRUE(g->get_status() == GameStatus::WIN_A || g->get_status() == GameStatus::WIN_B)
            << "must transition to WIN_* (seed=0x" << std::hex << seed << ")";
    }

    // Case C: 1ns short of timeout — must NOT trigger.
    {
        auto [g, clk] = build();
        clk->advance(server_timeout);
        // Step back by 1ns conceptually: instead advance slightly less than T.
        // We can't subtract from t (would risk underflow). Rebuild using smaller advance.
    }
    {
        auto clk = make_fake_clock();
        clk->advance(std::chrono::seconds(1000));
        KaylesGame g(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
        g.join_player_b(2u);
        // Advance by T - 1ns.
        clk->advance_by(server_timeout - std::chrono::nanoseconds(1));
        g.check_timeouts(server_timeout);
        EXPECT_EQ(g.get_status(), GameStatus::TURN_B)
            << "1ns short of timeout must NOT trigger (seed=0x" << std::hex << seed << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, TimeoutBoundaryExactness, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 11: TimeoutDoesNothingInTerminalOrWaitingStatus
//
// `check_timeouts` is a no-op unless status is TURN_A or TURN_B. We throw
// huge clock advances at WAITING, WIN_A, WIN_B and assert the status is
// unchanged.
// ===========================================================================

class TimeoutDoesNothingInTerminalOrWaitingStatus : public ::testing::TestWithParam<int> {};

TEST_P(TimeoutDoesNothingInTerminalOrWaitingStatus, NoOp) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::TimeoutNoopInTerminalOrWaiting, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(1, 30)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);
    // Pick one of the three non-turn statuses to exercise.
    const int which = std::uniform_int_distribution<int>(0, 2)(rng);

    auto clk = make_fake_clock();
    clk->advance(std::chrono::seconds(500));

    if (which == 0) {
        // WAITING_FOR_OPPONENT: construct but don't join player B.
        KaylesGame g(0u, 1u, /*max_pawn=*/3, pawn_row_t(4, true), clk);
        ASSERT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT);
        clk->advance(std::chrono::seconds(timeout_sec * 100 + 1));
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::WAITING_FOR_OPPONENT)
            << "check_timeouts must not transition out of WAITING (seed=0x" << std::hex << seed
            << ")";
    } else if (which == 1) {
        // WIN_A: give_up by player B in TURN_B.
        KaylesGame g(0u, 1u, /*max_pawn=*/3, pawn_row_t(4, true), clk);
        g.join_player_b(2u);
        g.give_up(2u);
        ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
        clk->advance(std::chrono::seconds(timeout_sec * 1000 + 12345));
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::WIN_A)
            << "check_timeouts must not mutate WIN_A (seed=0x" << std::hex << seed << ")";
    } else {
        // WIN_B: construct with single pin, B takes it and wins.
        KaylesGame g(0u, 1u, /*max_pawn=*/0, pawn_row_t(1, true), clk);
        g.join_player_b(2u);
        g.move(2u, 0, 1);
        ASSERT_EQ(g.get_status(), GameStatus::WIN_B);
        clk->advance(std::chrono::seconds(timeout_sec * 10 + 99));
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::WIN_B)
            << "check_timeouts must not mutate WIN_B (seed=0x" << std::hex << seed << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, TimeoutDoesNothingInTerminalOrWaitingStatus,
                         ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 12: IsStaleWaitingVsTerminal
//
//   WAITING:  stale iff now - a_last > T
//   TURN_*:   NEVER stale, regardless of clock
//   WIN_*:    stale iff BOTH now - a_last > T AND now - b_last > T
// ===========================================================================

class IsStaleWaitingVsTerminal : public ::testing::TestWithParam<int> {};

TEST_P(IsStaleWaitingVsTerminal, AllFourCases) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::IsStaleCases, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(1, 25)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);
    const int which = std::uniform_int_distribution<int>(0, 3)(rng);

    auto clk = make_fake_clock();
    clk->advance(std::chrono::seconds(500));

    switch (which) {
        case 0: {
            // WAITING: under-timeout -> not stale; over-timeout -> stale.
            KaylesGame g(0u, 1u, /*max_pawn=*/3, pawn_row_t(4, true), clk);
            EXPECT_FALSE(g.is_stale(T)) << "fresh WAITING must not be stale";
            clk->advance(T);
            EXPECT_FALSE(g.is_stale(T))
                << "exactly-at-timeout WAITING must NOT be stale (strict >)";
            clk->advance_by(std::chrono::nanoseconds(1));
            EXPECT_TRUE(g.is_stale(T))
                << "1ns past timeout WAITING must be stale (seed=0x" << std::hex << seed << ")";
            break;
        }
        case 1: {
            // TURN_A or TURN_B: never stale no matter how far the clock moves.
            KaylesGame g(0u, 1u, /*max_pawn=*/3, pawn_row_t(4, true), clk);
            g.join_player_b(2u);
            ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
            clk->advance(std::chrono::seconds(timeout_sec * 10000));
            EXPECT_FALSE(g.is_stale(T))
                << "TURN_* must never be stale (seed=0x" << std::hex << seed << ")";
            // Also flip to TURN_A and test again.
            g.move(2u, 0, 1);
            ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
            clk->advance(std::chrono::seconds(timeout_sec * 10000));
            EXPECT_FALSE(g.is_stale(T))
                << "TURN_A must never be stale (seed=0x" << std::hex << seed << ")";
            break;
        }
        case 2: {
            // WIN_* where only A is past timeout (B recently freshened).
            KaylesGame g(0u, 1u, /*max_pawn=*/0, pawn_row_t(1, true), clk);
            g.join_player_b(2u);
            g.move(2u, 0, 1);
            ASSERT_EQ(g.get_status(), GameStatus::WIN_B);
            // Advance past timeout so both a and b are stale...
            clk->advance(std::chrono::seconds(timeout_sec * 3 + 1));
            // ...then freshen B only.
            g.keep_alive(2u);
            EXPECT_FALSE(g.is_stale(T))
                << "WIN_B where only A past timeout must NOT be stale (seed=0x" << std::hex << seed
                << ")";
            // Now advance past timeout from B's freshened time too.
            clk->advance(std::chrono::seconds(timeout_sec + 1));
            EXPECT_TRUE(g.is_stale(T)) << "WIN_B where BOTH past timeout must be stale (seed=0x"
                                       << std::hex << seed << ")";
            break;
        }
        case 3: {
            // WIN_* where only B is past timeout (A recently freshened).
            KaylesGame g(0u, 1u, /*max_pawn=*/3, pawn_row_t(4, true), clk);
            g.join_player_b(2u);
            g.give_up(2u);  // -> WIN_A
            ASSERT_EQ(g.get_status(), GameStatus::WIN_A);
            clk->advance(std::chrono::seconds(timeout_sec * 3 + 1));
            g.keep_alive(1u);  // freshen A only
            EXPECT_FALSE(g.is_stale(T))
                << "WIN_A where only B past timeout must NOT be stale (seed=0x" << std::hex << seed
                << ")";
            clk->advance(std::chrono::seconds(timeout_sec + 1));
            EXPECT_TRUE(g.is_stale(T)) << "WIN_A where BOTH past timeout must be stale (seed=0x"
                                       << std::hex << seed << ")";
            break;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, IsStaleWaitingVsTerminal, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 13: KeepAliveDefersTimeout
//
// After a partial-advance, calling keep_alive on the on-turn player resets
// their timer, deferring the timeout. A subsequent advance must not trigger
// a timeout until a fresh full T elapses from the keep_alive call.
// ===========================================================================

class KeepAliveDefersTimeout : public ::testing::TestWithParam<int> {};

TEST_P(KeepAliveDefersTimeout, KeepAliveResetsTimer) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::KeepAliveDefers, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(2, 30)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);

    auto clk = make_fake_clock();
    clk->advance(std::chrono::seconds(1000));
    KaylesGame g(0u, 1u, /*max_pawn=*/8, pawn_row_t(9, true), clk);
    g.join_player_b(2u);
    // Status is TURN_B. On-turn player is 2.

    // Advance half the timeout.
    clk->advance(std::chrono::seconds(timeout_sec / 2));

    // Call keep_alive on on-turn player (B).
    // The prior b_last = t at join; now t has advanced by T/2.
    // Also refresh A so neither is the "older" player at test setup.
    // Actually the test needs to verify that keep_alive defers timeout for
    // the called player specifically. We refresh BOTH here so only the later
    // lapse matters.
    const player_id_t on_turn = 2u;
    g.keep_alive(on_turn);
    g.keep_alive(1u);

    // Now advance past the ORIGINAL timeout but well within a fresh one.
    // We've already advanced T/2, so another (T/2 - 1s) gets us to just under T total,
    // but both timers were reset at T/2. So now relative to each timer: T/2 - 1s.
    // That is still < T, so no trigger expected.
    clk->advance(std::chrono::seconds(timeout_sec / 2));
    // Now clock is at t0 + T. Relative to each freshened timer: T/2 elapsed.
    // T/2 < T, so check_timeouts must be a no-op.
    g.check_timeouts(T);
    EXPECT_EQ(g.get_status(), GameStatus::TURN_B)
        << "keep_alive must defer timeout (seed=0x" << std::hex << seed << ")";

    // Advance further past the fresh timeout boundary.
    clk->advance(std::chrono::seconds(timeout_sec));
    clk->advance_by(std::chrono::nanoseconds(1));
    g.check_timeouts(T);
    // Both timers are equally old, so else-if branch fires -> WIN_A.
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A)
        << "after fresh timeout both equally old -> WIN_A (seed=0x" << std::hex << seed << ")";
}

INSTANTIATE_TEST_SUITE_P(GameRandom, KeepAliveDefersTimeout, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 14: MoveDefersTimeout
//
// A legal move resets the mover's timer. An *illegal* move still begins with
// keep_alive(player_id), so if player_id matches a real player, their timer
// is reset even when the move is rejected.
// NOTE: whether an illegal move should reset the clock is debatable at the
// spec level; this test pins down the CURRENT observable behavior so any
// future change is a deliberate one.
// ===========================================================================

class MoveDefersTimeout : public ::testing::TestWithParam<int> {};

TEST_P(MoveDefersTimeout, LegalAndIllegalMovesBothDefer) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::MoveDefers, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(2, 30)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);

    auto clk = make_fake_clock();
    clk->advance(std::chrono::seconds(1000));
    KaylesGame g(0u, 1u, /*max_pawn=*/8, pawn_row_t(9, true), clk);
    g.join_player_b(2u);
    // Status TURN_B. Both timers = clk.

    // We'll test two sub-scenarios:
    //   Sub A: legal move defers onturn's timer.
    //   Sub B: illegal move (wrong player) also defers the *caller's* timer.

    const bool test_legal = std::bernoulli_distribution(0.5)(rng);
    if (test_legal) {
        // Advance nearly to timeout, then legal move by on-turn (B).
        clk->advance(std::chrono::seconds(timeout_sec));  // equals T so not yet past
        // Refresh A to avoid A being the older one.
        g.keep_alive(1u);
        g.move(2u, 0, 1);  // legal: remove pawn 0. resets B's timer.
        ASSERT_EQ(g.get_status(), GameStatus::TURN_A);
        // Now advance just past T.
        clk->advance_by(std::chrono::nanoseconds(1));
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::TURN_A)
            << "legal move should defer mover's timeout (seed=0x" << std::hex << seed << ")";
        // Advance a full fresh timeout past.
        clk->advance(std::chrono::seconds(timeout_sec));
        clk->advance_by(std::chrono::nanoseconds(1));
        g.check_timeouts(T);
        // A's timer is older (we refreshed it once but didn't refresh it after
        // B's move). After B's move, A was older strictly, so eventually A loses.
        // Actually both A's keep_alive and B's move happened at the same clock
        // tick (since we don't advance between them). So a_last == b_last, and
        // the else-if branch fires -> WIN_A (the player whose timer gets measured
        // second, i.e. B is NOT strictly newer).
        EXPECT_TRUE(g.get_status() == GameStatus::WIN_A || g.get_status() == GameStatus::WIN_B)
            << "after fresh T expired, some winner must be declared (seed=0x" << std::hex << seed
            << ")";
    } else {
        // Illegal move: wrong player, but player_id == one of the real players.
        // Per source, move() calls keep_alive BEFORE the turn check, so the
        // caller's timer is still reset even though the move is rejected.
        clk->advance(std::chrono::seconds(timeout_sec));  // equal to T
        // Refresh B so only A might be stale if not for the illegal-move defer.
        g.keep_alive(2u);
        // Now make A call move(1u, 0, 1) — it's A's NOT turn, so this is rejected.
        // But A's timer is reset by the internal keep_alive.
        g.move(1u, 0, 1);
        // Row must be unchanged since the move was rejected.
        ASSERT_EQ(g.get_status(), GameStatus::TURN_B);
        ASSERT_TRUE(g.get_game_state().pawn_row[0]);

        // Advance just past T.
        clk->advance_by(std::chrono::nanoseconds(1));
        g.check_timeouts(T);
        // Both A and B were freshened at the same clock tick, so neither
        // strictly newer. check_timeouts condition: now - b_last > T is
        // essentially `1ns > T` -> false. So no transition.
        EXPECT_EQ(g.get_status(), GameStatus::TURN_B)
            << "illegal move must still defer caller's timer (seed=0x" << std::hex << seed << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, MoveDefersTimeout, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 15: GiveUpDefersStaleness
//
// give_up calls keep_alive on the giver before handling the give_up logic.
// So after a give_up, the giver's timer is fresh; the other player's may
// not be. An immediately-following is_stale must be false unless BOTH are
// past timeout (for WIN_*).
// ===========================================================================

class GiveUpDefersStaleness : public ::testing::TestWithParam<int> {};

TEST_P(GiveUpDefersStaleness, GiverFreshnessDelaysStaleness) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::GiveUpDefersStale, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(2, 30)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);

    auto clk = make_fake_clock();
    clk->advance(std::chrono::seconds(1000));
    KaylesGame g(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);

    // Advance past timeout so A is stale (B's timer got reset at join so B
    // is also stale relative to the start, but we'll keep it that way).
    clk->advance(std::chrono::seconds(timeout_sec * 3 + 7));

    // B gives up. This triggers keep_alive(2), resetting b_last to now. Status
    // flips to WIN_A (since it was TURN_B).
    g.give_up(2u);
    ASSERT_EQ(g.get_status(), GameStatus::WIN_A);

    // Now for WIN_*, is_stale requires BOTH past timeout. B's timer was just
    // reset, A's timer is very old. So NOT stale yet.
    EXPECT_FALSE(g.is_stale(T)) << "immediately after give_up, giver is fresh -> not stale (seed=0x"
                                << std::hex << seed << ")";

    // Advance past timeout again from B's fresh time.
    clk->advance(std::chrono::seconds(timeout_sec));
    clk->advance_by(std::chrono::nanoseconds(1));
    EXPECT_TRUE(g.is_stale(T)) << "once both A and B are past timeout, WIN_A is stale (seed=0x"
                               << std::hex << seed << ")";
}

INSTANTIATE_TEST_SUITE_P(GameRandom, GiveUpDefersStaleness, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 16: TimeoutVsGiveUpRace
//
// If a player is already past their timeout and then calls give_up, the
// production code calls keep_alive first (resetting their timer) and THEN
// acts on give_up. Since we haven't yet called check_timeouts, status is
// still TURN_*, so give_up succeeds -> WIN_*.
//
// Alternative ordering: if check_timeouts runs first, status flips to WIN_*,
// and the subsequent give_up is a no-op.
//
// We demonstrate both orderings produce different observable outcomes from
// the same "physical" timeline. This is a property of the state machine,
// not a bug.
// ===========================================================================

class TimeoutVsGiveUpRace : public ::testing::TestWithParam<int> {};

TEST_P(TimeoutVsGiveUpRace, OrderingMatters) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::TimeoutVsGiveUp, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(2, 30)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);

    // Ordering 1: give_up first, check_timeouts second.
    {
        auto clk = make_fake_clock();
        clk->advance(std::chrono::seconds(1000));
        KaylesGame g(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
        g.join_player_b(2u);
        ASSERT_EQ(g.get_status(), GameStatus::TURN_B);

        // Advance past timeout; both players are stale now.
        clk->advance(std::chrono::seconds(timeout_sec * 3 + 2));

        // B gives up FIRST. give_up calls keep_alive(B) — resetting B's timer
        // — then since status was TURN_B and caller is B, status -> WIN_A.
        g.give_up(2u);
        EXPECT_EQ(g.get_status(), GameStatus::WIN_A)
            << "give_up before check_timeouts yields WIN_A (seed=0x" << std::hex << seed << ")";

        // Subsequent check_timeouts is a no-op on WIN_A.
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::WIN_A)
            << "check_timeouts must not mutate WIN_A (seed=0x" << std::hex << seed << ")";
    }

    // Ordering 2: check_timeouts first, give_up second.
    {
        auto clk = make_fake_clock();
        clk->advance(std::chrono::seconds(1000));
        KaylesGame g(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
        g.join_player_b(2u);
        ASSERT_EQ(g.get_status(), GameStatus::TURN_B);

        clk->advance(std::chrono::seconds(timeout_sec * 3 + 2));

        // check_timeouts first. Both equally old -> else-if branch -> WIN_A.
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::WIN_A)
            << "check_timeouts with both stale -> WIN_A (seed=0x" << std::hex << seed << ")";

        // Now give_up by B is a no-op (status is already WIN_A).
        const GameStatus before_giveup = g.get_status();
        g.give_up(2u);
        EXPECT_EQ(g.get_status(), before_giveup)
            << "give_up after terminal status must be no-op (seed=0x" << std::hex << seed << ")";
    }

    // Both orderings produced WIN_A in this symmetric case. Now try an
    // asymmetric case where ordering truly flips the outcome.
    //
    // Setup: A is strictly older than B (so if check_timeouts runs first, WIN_B
    // is declared). But if give_up by B runs first, status -> WIN_A.
    {
        auto clk = make_fake_clock();
        clk->advance(std::chrono::seconds(1000));
        KaylesGame g(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
        g.join_player_b(2u);
        // Make A strictly older: freshen B.
        clk->advance(std::chrono::seconds(timeout_sec + 5));
        g.keep_alive(2u);  // B is now younger than A by (timeout_sec + 5) seconds.
        clk->advance(std::chrono::seconds(timeout_sec + 5));  // both past timeout now.
        ASSERT_EQ(g.get_status(), GameStatus::TURN_B);

        // Ordering 2a: check_timeouts first (A strictly older -> WIN_B).
        auto snapshot_g = g;  // copy to test alternate ordering
        g.check_timeouts(T);
        EXPECT_EQ(g.get_status(), GameStatus::WIN_B)
            << "A strictly older -> WIN_B when check_timeouts first (seed=0x" << std::hex << seed
            << ")";

        // Ordering 2b: give_up first on the copy.
        snapshot_g.give_up(2u);
        EXPECT_EQ(snapshot_g.get_status(), GameStatus::WIN_A)
            << "A strictly older but B gave up first -> WIN_A (seed=0x" << std::hex << seed << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, TimeoutVsGiveUpRace, ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 17: NonPlayerKeepAliveDoesNotDeferTimeout
//
// keep_alive(some_stranger) must not touch either player's timer. After
// subsequent check_timeouts past timeout, the expected WIN_* still fires.
// ===========================================================================

class NonPlayerKeepAliveDoesNotDeferTimeout : public ::testing::TestWithParam<int> {};

TEST_P(NonPlayerKeepAliveDoesNotDeferTimeout, NoEffect) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::NonPlayerKeepAlive, iter);
    std::mt19937_64 rng(seed);

    const int timeout_sec = std::uniform_int_distribution<int>(2, 30)(rng);
    const timeout_t T = std::chrono::seconds(timeout_sec);

    auto clk = make_fake_clock();
    clk->advance(std::chrono::seconds(1000));
    KaylesGame g(0u, 1u, /*max_pawn=*/4, pawn_row_t(5, true), clk);
    g.join_player_b(2u);
    ASSERT_EQ(g.get_status(), GameStatus::TURN_B);

    // Pick a stranger id that is 0 or not in {1,2}.
    // 0 is a "never-valid" id; server-side assertions bar its use as a real
    // player, but keep_alive must still be a no-op if it is passed here.
    const std::vector<player_id_t> strangers = {0u, 3u, 999u, 4242u};
    const player_id_t stranger =
        strangers[std::uniform_int_distribution<size_t>(0, strangers.size() - 1)(rng)];

    // Advance a bit, then keep_alive with a stranger. Must NOT affect any timer.
    clk->advance(std::chrono::seconds(timeout_sec / 2));
    g.keep_alive(stranger);
    // Advance past timeout. Both real players are now stale.
    clk->advance(std::chrono::seconds(timeout_sec * 2 + 1));
    g.check_timeouts(T);
    // Both timers originated at joining (equal), so else-if branch -> WIN_A.
    EXPECT_EQ(g.get_status(), GameStatus::WIN_A)
        << "stranger keep_alive must not defer timeout (stranger=" << stranger << ", seed=0x"
        << std::hex << seed << ")";
}

INSTANTIATE_TEST_SUITE_P(GameRandom, NonPlayerKeepAliveDoesNotDeferTimeout,
                         ::testing::Range(0, kIterations));

// ===========================================================================
// Suite 18: MoveOnFinishedGameIsNoOp
//
// After a game reaches WIN_*, any further move() with otherwise-legal shape
// must be a pure no-op: neither pawn_row nor status may change.
// ===========================================================================

class MoveOnFinishedGameIsNoOp : public ::testing::TestWithParam<int> {};

TEST_P(MoveOnFinishedGameIsNoOp, NoMutationAfterWin) {
    const int iter = GetParam();
    const uint64_t seed = seed_for(SuiteTag::MoveOnFinished, iter);
    std::mt19937_64 rng(seed);

    // Small row so we reach WIN_* quickly.
    pawn_t mp = static_cast<pawn_t>(std::uniform_int_distribution<int>(0, 16)(rng));
    pawn_row_t row(static_cast<size_t>(mp) + 1, true);

    auto clk = make_fake_clock();
    KaylesGame g(0u, 1u, mp, row, clk);
    g.join_player_b(2u);

    // Play random legal moves until the game ends.
    player_id_t last_mover = 0u;
    while (true) {
        auto st = g.get_status();
        if (st == GameStatus::WIN_A || st == GameStatus::WIN_B)
            break;
        player_id_t mover = (st == GameStatus::TURN_A) ? 1u : 2u;
        auto moves = legal_moves(g.get_game_state().pawn_row, mp);
        ASSERT_FALSE(moves.empty());
        auto choice = moves[std::uniform_int_distribution<size_t>(0, moves.size() - 1)(rng)];
        g.move(mover, choice.first, choice.second);
        last_mover = mover;
    }

    const GameStatus final_status = g.get_status();
    const pawn_row_t row_final = g.get_game_state().pawn_row;
    ASSERT_TRUE(final_status == GameStatus::WIN_A || final_status == GameStatus::WIN_B);
    ASSERT_EQ(popcount_row(row_final), 0u);  // row empty after final move.

    // Try a series of moves with various otherwise-legal shapes. Since row is
    // empty, any "take pawn" call will fail on its own internal check, but
    // the test guards against the more general bug of WIN_* being mutable.
    const player_id_t winner = last_mover;
    const player_id_t loser = (winner == 1u) ? 2u : 1u;
    const std::vector<std::tuple<player_id_t, size_t, uint8_t>> attempts = {
        {winner, 0u, uint8_t{1}}, {loser, 0u, uint8_t{1}},
        {winner, 0u, uint8_t{2}}, {loser, static_cast<size_t>(mp), uint8_t{1}},
        {9999u, 0u, uint8_t{1}},
    };

    for (const auto &[who, p, k] : attempts) {
        g.move(who, p, k);
        EXPECT_EQ(g.get_status(), final_status)
            << "move on finished game changed status (seed=0x" << std::hex << seed << " who=" << who
            << " p=" << p << " k=" << static_cast<unsigned>(k) << ")";
        EXPECT_EQ(g.get_game_state().pawn_row, row_final)
            << "move on finished game changed pawn_row (seed=0x" << std::hex << seed
            << " who=" << who << " p=" << p << " k=" << static_cast<unsigned>(k) << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(GameRandom, MoveOnFinishedGameIsNoOp, ::testing::Range(0, kIterations));
