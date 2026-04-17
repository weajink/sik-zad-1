#ifndef KAYLES_COMMON_H
#define KAYLES_COMMON_H

#include <arpa/inet.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kayles_common {
    using address_t = in_addr;
    using timeout_t = uint8_t;
    using pawn_row_t = std::vector<bool>;

    // Server constants
    static constexpr timeout_t MIN_SERVER_TIMEOUT = 1;
    static constexpr timeout_t MAX_SERVER_TIMEOUT = 99;

    // Game constants
    constexpr size_t MSG_TYPE_SIZE = 1;
    constexpr size_t PLAYER_ID_SIZE = 4;
    constexpr size_t GAME_ID_SIZE = 4;
    constexpr size_t PAWN_SIZE = 1;
    constexpr size_t STATUS_SIZE = 1;
    constexpr size_t CLIENT_MESSAGE_SIZE =
        MSG_TYPE_SIZE + PLAYER_ID_SIZE + GAME_ID_SIZE + PAWN_SIZE + 2;
    constexpr uint8_t MAX_VALID_CLIENT_MESSAGE = 4;
    constexpr size_t MAX_BITMAP_SIZE = 32;  // max_pawn is uint8_t, so at most ceil(256/8)

    template <typename T>
    inline bool from_chars(const char *first, const char *last, T &value) {
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc() && ptr == last) {
            return true;
        }
        return false;
    }

    using error_index_t = uint8_t;

    enum class ClientMessageType : uint8_t {
        MSG_JOIN,
        MSG_MOVE_1,
        MSG_MOVE_2,
        MSG_KEEP_ALIVE,
        MSG_GIVE_UP
    };

    struct ClientMessage {
        ClientMessageType msg_type;
        uint32_t player_id;
        uint32_t game_id;
        uint8_t pawn;
    };

    struct __attribute__((__packed__)) game_state_t {
        uint32_t game_id;
        uint32_t player_a_id;
        uint32_t player_b_id;
        uint8_t status;
        uint8_t max_pawn;
        uint8_t pawn_row_bitmap[MAX_BITMAP_SIZE];
    };

    // MSG_WRONG_MSG
    struct __attribute__((__packed__)) WrongMessage {                                                         
      uint8_t client_bytes[12];                                                                             
      uint8_t status = 255;                                                                               
      error_index_t error_index;
    };
}; // namespace kayles_common
#endif