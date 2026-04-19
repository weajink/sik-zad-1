#ifndef KAYLES_GAME_H
#define KAYLES_GAME_H

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <ctime>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "kayles_clock.h"
#include "kayles_protocol.h"

namespace kayles::game {
    using namespace kayles::protocol;

    class KaylesGame {
       private:
        GameState gs;

        std::shared_ptr<kayles::clock::Clock> clock;
        time_point_t player_a_last_move_time;
        time_point_t player_b_last_move_time;
        size_t pawns_left_in_row;

        bool check_if_my_turn(player_id_t player_id) const {
            return (player_id == gs.player_a_id && gs.status == GameStatus::TURN_A) ||
                   (player_id == gs.player_b_id && gs.status == GameStatus::TURN_B);
        }

        bool take_pawn(size_t pawn) {
            if (pawn > gs.max_pawn || !gs.pawn_row[pawn]) {
                return false;
            }
            gs.pawn_row[pawn] = false;
            pawns_left_in_row--;
            return true;
        }

        bool take_two_consecutive_pawns(size_t first_pawn) {
            if (first_pawn + 1 > gs.max_pawn || !gs.pawn_row[first_pawn] ||
                !gs.pawn_row[first_pawn + 1]) {
                return false;
            }
            gs.pawn_row[first_pawn] = gs.pawn_row[first_pawn + 1] = false;
            pawns_left_in_row -= 2;
            return true;
        }

       public:
        KaylesGame(player_id_t game_id, player_id_t player_a_id, pawn_t max_pawn,
                   pawn_row_t pawn_row, std::shared_ptr<kayles::clock::Clock> clock)
            : gs{.game_id = game_id,
                 .player_a_id = player_a_id,
                 .player_b_id = 0,
                 .status = GameStatus::WAITING_FOR_OPPONENT,
                 .max_pawn = max_pawn,
                 .pawn_row = std::move(pawn_row)},
              clock(clock),
              player_a_last_move_time(clock->now()) {
            assert(player_a_id != 0);
            pawns_left_in_row = std::count(gs.pawn_row.begin(), gs.pawn_row.end(), true);
        }

        // Updates player move time.
        void keep_alive(player_id_t player_id) {
            if (player_id == gs.player_a_id) {
                player_a_last_move_time = clock->now();
            }
            if (player_id == gs.player_b_id) {
                player_b_last_move_time = clock->now();
            }
        }

        void join_player_b(player_id_t player_b_id) {
            assert(player_b_id != 0);
            assert(gs.status == GameStatus::WAITING_FOR_OPPONENT);
            assert(this->gs.player_b_id == 0);

            this->gs.player_b_id = player_b_id;
            keep_alive(player_b_id);
            gs.status = GameStatus::TURN_B;
        }

        void give_up(player_id_t player_id) {
            keep_alive(player_id);
            if (player_id == gs.player_a_id && gs.status == GameStatus::TURN_A) {
                gs.status = GameStatus::WIN_B;
            } else if (player_id == gs.player_b_id && gs.status == GameStatus::TURN_B) {
                gs.status = GameStatus::WIN_A;
            }
        }

        // no_of_pawns: 1 or 2
        void move(player_id_t player_id, size_t pawn, uint8_t no_of_pawns) {
            keep_alive(player_id);
            if (!check_if_my_turn(player_id)) {
                return;
            }

            if (no_of_pawns == 1) {
                if (!take_pawn(pawn))
                    return;
            } else if (no_of_pawns == 2) {
                if (!take_two_consecutive_pawns(pawn))
                    return;
            } else {
                assert(false);
            }

            if (pawns_left_in_row == 0) {
                if (gs.status == GameStatus::TURN_A)
                    gs.status = GameStatus::WIN_A;
                else
                    gs.status = GameStatus::WIN_B;
                return;
            }

            if (gs.status == GameStatus::TURN_A)
                gs.status = GameStatus::TURN_B;
            else if (gs.status == GameStatus::TURN_B)
                gs.status = GameStatus::TURN_A;
        }

        bool is_player_joined(player_id_t player_id) const {
            return player_id == gs.player_a_id || player_id == gs.player_b_id;
        }

        void check_timeouts(timeout_t server_timeout) {
            auto now = clock->now();
            switch (gs.status) {
                case GameStatus::TURN_A:
                case GameStatus::TURN_B: {
                    if (player_a_last_move_time < player_b_last_move_time &&
                        now - player_a_last_move_time > server_timeout) {
                        gs.status = GameStatus::WIN_B;
                    } else if (now - player_b_last_move_time > server_timeout) {
                        gs.status = GameStatus::WIN_A;
                    }
                    break;
                }
                default: {
                    break;
                }
            }
        }

        // Checks if the game can be qualified as
        // stale according to server_timeout
        // and deleted.
        bool is_stale(timeout_t server_timeout) {
            auto now = clock->now();
            switch (gs.status) {
                case GameStatus::WAITING_FOR_OPPONENT: {
                    return now - player_a_last_move_time > server_timeout;
                }
                case GameStatus::WIN_A:
                case GameStatus::WIN_B: {
                    return (now - player_a_last_move_time > server_timeout) &&
                           (now - player_b_last_move_time > server_timeout);
                }
                default: {
                    return false;
                }
            }
        }

        const GameState &get_game_state() {
            return gs;
        }

        GameStatus get_status() const {
            return gs.status;
        }
    };

    class KaylesGameMap {
       private:
        game_id_t next_game_id = 0;
        std::map<game_id_t, KaylesGame> games;

        timeout_t timeout;
        pawn_t max_pawn;
        pawn_row_t pawn_row;
        std::shared_ptr<kayles::clock::Clock> clock;

        void check_timeouts_and_remove_stale() {
            for (auto it = games.begin(); it != games.end();) {
                it->second.check_timeouts(timeout);
                if (it->second.is_stale(timeout)) {
                    it = games.erase(it);
                } else {
                    ++it;
                }
            }
        }

       public:
        KaylesGameMap(timeout_t timeout, pawn_t max_pawn, pawn_row_t pawn_row)
            : timeout(timeout),
              max_pawn(max_pawn),
              pawn_row(pawn_row),
              clock(std::make_shared<kayles::clock::SystemClock>()) {}

        KaylesGameMap(timeout_t timeout, pawn_t max_pawn, pawn_row_t pawn_row,
                      std::shared_ptr<kayles::clock::Clock> clock)
            : timeout(timeout), max_pawn(max_pawn), pawn_row(pawn_row), clock(std::move(clock)) {}

        std::expected<GameState, KaylesError> join(player_id_t player_id) {
            check_timeouts_and_remove_stale();

            // check if the last game is waiting for opponent
            if (!games.empty() &&
                games.rbegin()->second.get_status() == GameStatus::WAITING_FOR_OPPONENT) {
                auto &game = games.rbegin()->second;
                game.join_player_b(player_id);
                return game.get_game_state();
            }

            // All games in use
            if (next_game_id == std::numeric_limits<game_id_t>::max()) {
                return std::unexpected(KaylesError::game_ids_exhausted());
            }

            // Find an unused game_id.
            game_id_t game_id = next_game_id++;
            auto [it, _] =
                games.emplace(game_id, KaylesGame(game_id, player_id, max_pawn, pawn_row, clock));
            return it->second.get_game_state();
        }

        std::expected<GameState, KaylesError> move(player_id_t player_id, game_id_t game_id,
                                                   size_t pawn, size_t no_of_pawns) {
            check_timeouts_and_remove_stale();

            auto it = games.find(game_id);
            if (it == games.end()) {
                return std::unexpected(KaylesError::invalid_game_id());
            }
            auto &game = it->second;
            if (!game.is_player_joined(player_id)) {
                return std::unexpected(KaylesError::invalid_player_id());
            }

            game.move(player_id, pawn, no_of_pawns);
            return game.get_game_state();
        }

        std::expected<GameState, KaylesError> keep_alive(player_id_t player_id, game_id_t game_id) {
            check_timeouts_and_remove_stale();

            auto it = games.find(game_id);
            if (it == games.end()) {
                return std::unexpected(KaylesError::invalid_game_id());
            }
            auto &game = it->second;
            if (!game.is_player_joined(player_id)) {
                return std::unexpected(KaylesError::invalid_player_id());
            }

            game.keep_alive(player_id);
            return game.get_game_state();
        }

        std::expected<GameState, KaylesError> give_up(player_id_t player_id, game_id_t game_id) {
            check_timeouts_and_remove_stale();

            auto it = games.find(game_id);
            if (it == games.end()) {
                return std::unexpected(KaylesError::invalid_game_id());
            }
            auto &game = it->second;
            if (!game.is_player_joined(player_id)) {
                return std::unexpected(KaylesError::invalid_player_id());
            }

            game.give_up(player_id);
            return game.get_game_state();
        }
    };
}  // namespace kayles::game

#endif