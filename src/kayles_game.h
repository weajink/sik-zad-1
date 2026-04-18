#ifndef KAYLES_GAME_H
#define KAYLES_GAME_H

#include "kayles_common.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <ctime>
#include <expected>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
#include <chrono>

namespace kayles_game {
    using namespace kayles_common;

    class KaylesGame {
       public:
        enum class Status : uint8_t { WAITING_FOR_OPPONENT, TURN_A, TURN_B, WIN_A, WIN_B };

       private:
        uint32_t game_id;
        uint32_t player_a_id;
        uint32_t player_b_id = 0;
        std::chrono::steady_clock::time_point player_a_last_move_time;
        std::chrono::steady_clock::time_point player_b_last_move_time;

        Status status;
        uint8_t max_pawn;
        pawn_row_t pawn_row;
        uint8_t pawns_left_in_row;

        bool check_if_my_turn(uint32_t player_id) const {
            return (player_id == player_a_id && status == Status::TURN_A) ||
                   (player_id == player_b_id && status == Status::TURN_B);
        }

        bool take_pawn(uint32_t pawn) {
            if (pawn > max_pawn || !pawn_row[pawn]) {
                return false;
            }
            pawn_row[pawn] = false;
            pawns_left_in_row--;
            return true;
        }

        bool take_two_consecutive_pawns(uint32_t first_pawn) {
            if (first_pawn + 1 > max_pawn || !pawn_row[first_pawn] || !pawn_row[first_pawn + 1]) {
                return false;
            }
            pawn_row[first_pawn] = pawn_row[first_pawn + 1] = false;
            pawns_left_in_row -= 2;
            return true;
        }

       public:
        KaylesGame(uint32_t game_id, uint32_t player_a_id, uint8_t max_pawn, pawn_row_t pawn_row)
            : game_id(game_id),
              player_a_id(player_a_id),
              player_a_last_move_time(std::chrono::steady_clock::now()),
              status(Status::WAITING_FOR_OPPONENT),
              max_pawn(max_pawn),
              pawn_row(pawn_row) {
            if (player_a_id == 0) {
                throw std::invalid_argument("Player id must be positive.");
            }
            pawns_left_in_row = std::count(pawn_row.begin(), pawn_row.end(), true);
        }

        // Updates player move time.
        void keep_alive(uint32_t player_id) {
            if (player_id == player_a_id) {
                player_a_last_move_time = std::chrono::steady_clock::now();
            }
            if (player_id == player_b_id) {
                player_b_last_move_time = std::chrono::steady_clock::now();
            }
        }

        void join_player_b(uint32_t player_b_id) {
            if (player_b_id == 0) {
                throw std::invalid_argument("Player id must be positive.");
            }
            assert(this->player_b_id == 0);
            this->player_b_id = player_b_id;
            player_b_last_move_time = std::chrono::steady_clock::now();
            status = Status::TURN_B;
        }

        void give_up(uint32_t player_id) {
            keep_alive(player_id);
            if (player_id == player_a_id && status == Status::TURN_A) {
                status = Status::WIN_B;
            } else if (player_id == player_b_id && status == Status::TURN_B) {
                status = Status::WIN_A;
            }
        }

        // no_of_pawns: 1 or 2
        void move(uint32_t player_id, uint8_t pawn, uint8_t no_of_pawns) {
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
            } else
                throw std::invalid_argument("Number of pawsn can be only 1 or 2.");

            if (pawns_left_in_row == 0) {
                if (status == Status::TURN_A)
                    status = Status::WIN_A;
                else
                    status = Status::WIN_B;
                return;
            }

            if (status == Status::TURN_A)
                status = Status::TURN_B;
            else if (status == Status::TURN_B)
                status = Status::TURN_A;
        }

        bool is_player_joined(uint32_t player_id) const {
            return player_id == player_a_id || player_id == player_b_id;
        }

        Status get_status() const {
            return status;
        }

        // Checks if the game can be qualified as
        // stale according to server_timeout
        // and deleted.
        bool check_timeouts(std::chrono::seconds server_timeout) {
            auto now = std::chrono::steady_clock::now();
            switch (status) {
                case Status::TURN_A: case Status::TURN_B: {
                    if (now - player_a_last_move_time > server_timeout) {
                        status = Status::WIN_B;
                    }
                    if (now - player_b_last_move_time > server_timeout) {
                        status = Status::WIN_A;
                    }
                    return false;
                } 
                case Status::WAITING_FOR_OPPONENT: {
                    return now - player_a_last_move_time > server_timeout;
                } 
                default: {
                    return (now - player_a_last_move_time > server_timeout)
                        && (now - player_b_last_move_time > server_timeout);
                }
            }
        }

        game_state_t get_game_state() {
            game_state_t res{};
            res.game_id = htonl(game_id);
            res.player_a_id = htonl(player_a_id);
            res.player_b_id = htonl(player_b_id);
            res.status = std::to_underlying(status);
            res.max_pawn = max_pawn;

            std::memset(res.pawn_row_bitmap, 0, MAX_BITMAP_SIZE);
            for (size_t i = 0; i <= max_pawn; i++) {
                if (pawn_row[i]) {
                    res.pawn_row_bitmap[i / 8] |= (1 << (7 - (i % 8)));
                }
            }
            return res;
        }
    };

    enum class KaylesGameError { INVALID_PLAYER_ID, INVALID_GAME_ID };

    class KaylesGameMap {
       private:
        uint32_t next_game_id = 0;
        std::map<uint32_t, KaylesGame> games;
        timeout_t timeout;
        uint8_t max_pawn;
        pawn_row_t pawn_row;

        void check_timeouts() {
            for (auto it = games.begin(); it != games.end();) {
                if (it->second.check_timeouts(std::chrono::seconds(timeout))) {
                    it = games.erase(it);
                } else {
                    ++it;
                }
            }
        }

       public:
        KaylesGameMap(timeout_t timeout, uint8_t max_pawn, pawn_row_t pawn_row)
            : timeout(timeout), max_pawn(max_pawn), pawn_row(pawn_row) {}

        std::optional<game_state_t> join(uint32_t player_id) {
            check_timeouts();

            // check if the last game is waiting for opponent
            if (!games.empty() &&
                games.rbegin()->second.get_status() == KaylesGame::Status::WAITING_FOR_OPPONENT) {
                auto &game = games.rbegin()->second;
                game.join_player_b(player_id);
                return game.get_game_state();
            }

            // All 2^32 game IDs in use — spec says silently ignore the JOIN.
            if (games.size() > std::numeric_limits<uint32_t>::max()) {
                return std::nullopt;
            }

            // Find an unused game_id (after wraparound, some IDs may be taken).
            while (games.contains(next_game_id)) {
                ++next_game_id;
            }
            uint32_t game_id = next_game_id++;
            auto [it, _] =
                games.emplace(game_id, KaylesGame(game_id, player_id, max_pawn, pawn_row));
            return it->second.get_game_state();
        }

        std::expected<game_state_t, KaylesGameError> move(uint32_t player_id, uint32_t game_id,
                                                          uint8_t pawn, uint8_t no_of_pawns) {
            check_timeouts();

            auto it = games.find(game_id);
            if (it == games.end()) {
                return std::unexpected(KaylesGameError::INVALID_GAME_ID);
            }
            auto &game = it->second;
            if (!game.is_player_joined(player_id)) {
                return std::unexpected(KaylesGameError::INVALID_PLAYER_ID);
            }

            game.move(player_id, pawn, no_of_pawns);
            return game.get_game_state();
        }

        std::expected<game_state_t, KaylesGameError> keep_alive(uint32_t player_id,
                                                                uint32_t game_id) {
            check_timeouts();

            auto it = games.find(game_id);
            if (it == games.end()) {
                return std::unexpected(KaylesGameError::INVALID_GAME_ID);
            }
            auto &game = it->second;
            if (!game.is_player_joined(player_id)) {
                return std::unexpected(KaylesGameError::INVALID_PLAYER_ID);
            }

            game.keep_alive(player_id);
            return game.get_game_state();
        }

        std::expected<game_state_t, KaylesGameError> give_up(uint32_t player_id, uint32_t game_id) {
            check_timeouts();

            auto it = games.find(game_id);
            if (it == games.end()) {
                return std::unexpected(KaylesGameError::INVALID_GAME_ID);
            }
            auto &game = it->second;
            if (!game.is_player_joined(player_id)) {
                return std::unexpected(KaylesGameError::INVALID_PLAYER_ID);
            }

            game.give_up(player_id);
            return game.get_game_state();
        }
    };
}  // namespace kayles_game

#endif