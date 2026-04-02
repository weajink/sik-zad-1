#ifndef KAYLES_COMMON_H
#define KAYLES_COMMON_H

#include <arpa/inet.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace kayles_common {
// Server constants
constexpr static uint8_t MIN_SERVER_TIMEOUT = 1;
constexpr static uint8_t MAX_SERVER_TIMEOUT = 99;

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

inline std::expected<ClientMessage, error_index_t> get_message_from_buffer(char *buf, ssize_t len) {
    ClientMessage res{};
    // 1. get message type
    if (len < static_cast<ssize_t>(MSG_TYPE_SIZE)) {
        return std::unexpected(static_cast<error_index_t>(len));
    }
    size_t offset = 0;
    uint8_t msg_type = buf[offset];
    if (msg_type > MAX_VALID_CLIENT_MESSAGE) {
        return std::unexpected(offset);
    }
    res.msg_type = static_cast<ClientMessageType>(msg_type);
    offset += MSG_TYPE_SIZE;

    // 2. get player id
    if (len < static_cast<ssize_t>(offset + PLAYER_ID_SIZE)) {
        return std::unexpected(static_cast<error_index_t>(len));
    }
    uint32_t player_id_n;
    std::memcpy(&player_id_n, buf + offset, PLAYER_ID_SIZE);
    res.player_id = ntohl(player_id_n);
    offset += PLAYER_ID_SIZE;

    // 3. get game id (if not join)
    if (res.msg_type != ClientMessageType::MSG_JOIN) {
        if (len < static_cast<ssize_t>(offset + GAME_ID_SIZE)) {
            return std::unexpected(static_cast<error_index_t>(len));
        }
        uint32_t game_id_n;
        std::memcpy(&game_id_n, buf + offset, GAME_ID_SIZE);
        res.game_id = ntohl(game_id_n);
        offset += GAME_ID_SIZE;
    }

    // 4. get pawn (if move)
    if (res.msg_type == ClientMessageType::MSG_MOVE_1 ||
        res.msg_type == ClientMessageType::MSG_MOVE_2) {
        if (len < static_cast<ssize_t>(offset + PAWN_SIZE)) {
            return std::unexpected(static_cast<error_index_t>(len));
        }
        res.pawn = static_cast<uint8_t>(buf[offset]);
        offset += PAWN_SIZE;
    }

    // Check if we parsed everything
    if (offset != static_cast<size_t>(len)) {
        return std::unexpected(static_cast<error_index_t>(offset));
    }

    return res;
}

};  // namespace kayles_common
#endif