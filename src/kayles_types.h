#ifndef KAYLES_TYPES_H
#define KAYLES_TYPES_H

#include <arpa/inet.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace kayles::types {
    using address_t = in_addr;
    using player_id_t = uint32_t;
    using game_id_t = uint32_t;
    using timeout_t = std::chrono::seconds;
    using pawn_t = uint8_t;
    using time_point_t = std::chrono::steady_clock::time_point;

    using pawn_row_t = std::vector<bool>;

    static constexpr timeout_t MIN_TIMEOUT = std::chrono::seconds(1);
    static constexpr timeout_t MAX_TIMEOUT = std::chrono::seconds(99);
} // namespace kayles::types

#endif