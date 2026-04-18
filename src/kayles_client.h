#ifndef KAYLES_CLIENT_H
#define KAYLES_CLIENT_H

#include <arpa/inet.h>
#include <errno.h>
#include "kayles_common.h"

#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace kayles_client {
    using namespace kayles_common;

    struct WireMessage {
        uint8_t data[CLIENT_MESSAGE_SIZE];
        size_t len;
    };

    inline std::optional<WireMessage> parse_client_message(std::string_view message_str) {
    if (message_str.empty()) {
        std::cerr << "Message cannot be empty.\n";
        return std::nullopt;
    }

    // Split on '/'
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (start <= message_str.size()) {
        size_t end = message_str.find('/', start);
        if (end == std::string_view::npos) {
            end = message_str.size();
        }
        tokens.push_back(message_str.substr(start, end - start));
        start = end + 1;
    }
    if (tokens.empty()) {
        std::cerr << "Invalid message format.\n";
        return std::nullopt;
    }

    // Parse message type
    uint8_t msg_type;
    if (!from_chars(tokens[0].data(), tokens[0].data() + tokens[0].size(), msg_type) ||
        msg_type > MAX_VALID_CLIENT_MESSAGE) {
        std::cerr << "Invalid message type.\n";
        return std::nullopt;
    }

    // Validate field count
    size_t expected;
    switch (msg_type) {
        case 0:
            expected = 2;
            break;
        case 1:
        case 2:
            expected = 4;
            break;
        case 3:
        case 4:
            expected = 3;
            break;
        default:
            return std::nullopt;
    }
    if (tokens.size() != expected) {
        std::cerr << "Wrong number of fields for message type " << static_cast<int>(msg_type)
                  << ".\n";
        return std::nullopt;
    }

    WireMessage wire{};

    // msg_type
    wire.data[wire.len++] = msg_type;

    // player_id (must be nonzero)
    uint32_t player_id;
    if (!from_chars(tokens[1].data(), tokens[1].data() + tokens[1].size(), player_id) ||
        player_id == 0) {
        std::cerr << "Invalid player ID.\n";
        return std::nullopt;
    }
    uint32_t net_player_id = htonl(player_id);
    std::memcpy(wire.data + wire.len, &net_player_id, PLAYER_ID_SIZE);
    wire.len += PLAYER_ID_SIZE;

    // game_id (if not join)
    if (msg_type >= 1) {
        uint32_t game_id;
        if (!from_chars(tokens[2].data(), tokens[2].data() + tokens[2].size(), game_id)) {
            std::cerr << "Invalid game ID.\n";
            return std::nullopt;
        }
        uint32_t net_game_id = htonl(game_id);
        std::memcpy(wire.data + wire.len, &net_game_id, GAME_ID_SIZE);
        wire.len += GAME_ID_SIZE;
    }

    // pawn (if move)
    if (msg_type == 1 || msg_type == 2) {
        uint8_t pawn;
        if (!from_chars(tokens[3].data(), tokens[3].data() + tokens[3].size(), pawn)) {
            std::cerr << "Invalid pawn.\n";
            return std::nullopt;
        }
        wire.data[wire.len++] = pawn;
    }

    return wire;
}
}  // namespace kayles_client

#endif
