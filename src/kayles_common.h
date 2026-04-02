#ifndef KAYLES_COMMON_H
#define KAYLES_COMMON_H

#include <arpa/inet.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
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
constexpr size_t CLIENT_MESSAGE_SIZE = MSG_TYPE_SIZE + PLAYER_ID_SIZE + GAME_ID_SIZE + PAWN_SIZE;
constexpr uint8_t MAX_VALID_CLIENT_MESSAGE = 4;

template <typename T>
bool from_chars(const char *first, const char *last, T &value) {
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

inline std::expected<ClientMessage, error_index_t> get_message_from_buffer(const char *buf,
                                                                           size_t len) {
    ClientMessage res{};
    // 1. get message type
    if (len < MSG_TYPE_SIZE) {
        return std::unexpected(len);
    }
    size_t offset = 0;
    uint8_t msg_type = static_cast<uint8_t>(buf[offset]);
    if (msg_type > MAX_VALID_CLIENT_MESSAGE) {
        return std::unexpected(offset);
    }
    res.msg_type = static_cast<ClientMessageType>(msg_type);
    offset += MSG_TYPE_SIZE;

    // 2. get player id
    if (len < offset + PLAYER_ID_SIZE) {
        return std::unexpected(len);
    }
    std::memcpy(&res.player_id, buf + offset, PLAYER_ID_SIZE);
    res.player_id = ntohl(res.player_id);
    offset += PLAYER_ID_SIZE;

    // 3. get game id (if not join)
    if (res.msg_type != ClientMessageType::MSG_JOIN) {
        if (len < offset + GAME_ID_SIZE) {
            return std::unexpected(len);
        }
        std::memcpy(&res.game_id, buf + offset, GAME_ID_SIZE);
        res.game_id = ntohl(res.game_id);
        offset += GAME_ID_SIZE;
    }

    // 4. get pawn (if move)
    if (res.msg_type == ClientMessageType::MSG_MOVE_1 ||
        res.msg_type == ClientMessageType::MSG_MOVE_2) {
        if (len < offset + PAWN_SIZE) {
            return std::unexpected(len);
        }
        res.pawn = static_cast<uint8_t>(buf[offset]);
        offset += PAWN_SIZE;
    }

    // Check if we parsed everything
    if (offset != len) {
        return std::unexpected(offset);
    }

    return res;
}

};  // namespace kayles_common
#endif