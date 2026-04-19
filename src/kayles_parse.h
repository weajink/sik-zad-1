#ifndef KAYLES_PARSE_H
#define KAYLES_PARSE_H

#include <arpa/inet.h>
#include <netdb.h>

#include <charconv>
#include <cstddef>
#include <cstring>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kayles_error.h"
#include "kayles_protocol.h"
#include "kayles_types.h"

namespace kayles::parse {
    using namespace kayles::types;
    using namespace kayles::error;
    using namespace kayles::protocol;

    template <typename T>
    bool assign_or_report(std::string_view prog, std::optional<T> &slot,
                          std::expected<T, KaylesError> res) {
        if (!res.has_value()) {
            std::cerr << prog << ": arg: " << res.error().what() << "\n";
            return false;
        }
        slot = std::move(res.value());
        return true;
    }

    inline std::expected<address_t, KaylesError> parse_address(std::string_view address_str) {
        struct addrinfo hints {};
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_DGRAM;  // UDP
        hints.ai_protocol = IPPROTO_UDP;

        struct addrinfo *result = nullptr;
        int errcode = getaddrinfo(address_str.data(), nullptr, &hints, &result);
        if (errcode != 0) {
            return std::unexpected(KaylesError::parse_error("getaddrinfo failed: " +
                                                            std::string(gai_strerror(errcode))));
        }

        address_t address;
        address.s_addr = ((struct sockaddr_in *)(result->ai_addr))->sin_addr.s_addr;
        freeaddrinfo(result);
        return address;
    }

    inline std::expected<uint16_t, KaylesError> parse_port(std::string_view port_str) {
        uint16_t port;
        auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
        if (ec != std::errc() || ptr != port_str.data() + port_str.size()) {
            return std::unexpected(KaylesError::parse_error("Invalid port format."));
        }
        return port;
    }

    inline std::expected<timeout_t, KaylesError> parse_timeout(std::string_view timeout_str) {
        uint8_t seconds;
        auto [ptr, ec] =
            std::from_chars(timeout_str.data(), timeout_str.data() + timeout_str.size(), seconds);
        if (ec != std::errc() || ptr != timeout_str.data() + timeout_str.size()) {
            return std::unexpected(KaylesError::parse_error("Invalid timeout."));
        }
        timeout_t timeout{seconds};
        if (timeout < MIN_TIMEOUT || timeout > MAX_TIMEOUT) {
            return std::unexpected(KaylesError::parse_error("Invalid timeout."));
        }
        return timeout;
    }

    inline std::expected<pawn_row_t, KaylesError> parse_pawn_row(std::string_view row_str) {
        if (row_str.size() < 1 || row_str.size() > 256) {
            return std::unexpected(KaylesError::parse_error("Invalid pawn row length."));
        }

        pawn_row_t res(row_str.size());
        for (size_t i = 0; i < row_str.size(); i++) {
            if (row_str[i] != '0' && row_str[i] != '1') {
                return std::unexpected(KaylesError::parse_error(
                    "Invalid pawn row format - only '0' and '1' allowed."));
            }
            res[i] = (row_str[i] == '1');
        }

        if (res.front() != true || res.back() != true) {
            return std::unexpected(KaylesError::parse_error(
                "Invalid pawn row format - first and last pawn must be present."));
        }

        return res;
    }

    inline std::expected<ClientMessage, KaylesError> parse_client_message(
        std::string_view message_str) {
        if (message_str.empty()) {
            return std::unexpected(KaylesError::parse_error("Message cannot be empty."));
        }

        // Split on '/'. A trailing slash produces an empty final token, which
        // causes the field-count check to fail — same for leading/double slashes.
        std::vector<std::string_view> tokens;
        size_t start = 0;
        while (start <= message_str.size()) {
            size_t end = message_str.find('/', start);
            if (end == std::string_view::npos)
                end = message_str.size();
            tokens.push_back(message_str.substr(start, end - start));
            start = end + 1;
        }

        // Parse msg_type.
        uint8_t msg_type;
        auto [ptr_mt, ec_mt] =
            std::from_chars(tokens[0].data(), tokens[0].data() + tokens[0].size(), msg_type);
        if (ec_mt != std::errc() || ptr_mt != tokens[0].data() + tokens[0].size() ||
            msg_type > MAX_VALID_CLIENT_MESSAGE) {
            return std::unexpected(KaylesError::parse_error("Invalid message type."));
        }

        // Validate field count against message type.
        size_t expected_tokens;
        switch (msg_type) {
            case 0:
                expected_tokens = 2;
                break;
            case 1:
            case 2:
                expected_tokens = 4;
                break;
            case 3:
            case 4:
                expected_tokens = 3;
                break;
            default:
                std::unreachable();
        }
        if (tokens.size() != expected_tokens) {
            return std::unexpected(
                KaylesError::parse_error("Wrong number of fields for message type."));
        }

        ClientMessage msg{};
        msg.msg_type = static_cast<ClientMessageType>(msg_type);

        // player_id (must be nonzero).
        auto [ptr_pid, ec_pid] =
            std::from_chars(tokens[1].data(), tokens[1].data() + tokens[1].size(), msg.player_id);
        if (ec_pid != std::errc() || ptr_pid != tokens[1].data() + tokens[1].size() ||
            msg.player_id == 0) {
            return std::unexpected(KaylesError::parse_error("Invalid player ID."));
        }

        // game_id (if not MSG_JOIN).
        if (msg_type >= 1) {
            auto [ptr_gid, ec_gid] =
                std::from_chars(tokens[2].data(), tokens[2].data() + tokens[2].size(), msg.game_id);
            if (ec_gid != std::errc() || ptr_gid != tokens[2].data() + tokens[2].size()) {
                return std::unexpected(KaylesError::parse_error("Invalid game ID."));
            }
        }

        // pawn (if MSG_MOVE_1 or MSG_MOVE_2).
        if (msg_type == 1 || msg_type == 2) {
            auto [ptr_p, ec_p] =
                std::from_chars(tokens[3].data(), tokens[3].data() + tokens[3].size(), msg.pawn);
            if (ec_p != std::errc() || ptr_p != tokens[3].data() + tokens[3].size()) {
                return std::unexpected(KaylesError::parse_error("Invalid pawn."));
            }
        }

        return msg;
    }
}  // namespace kayles::parse

#endif