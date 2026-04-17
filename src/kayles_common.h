#ifndef KAYLES_COMMON_H
#define KAYLES_COMMON_H

#include <arpa/inet.h>
#include <netdb.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace kayles_common {
    using address_t = in_addr;
    using timeout_t = uint8_t;
    using pawn_row_t = std::vector<bool>;

    // Server constants
    static constexpr timeout_t MIN_TIMEOUT = 1;
    static constexpr timeout_t MAX_TIMEOUT = 99;

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

    constexpr uint8_t MSG_WRONG_STATUS = 255;
    // MSG_WRONG_MSG
    struct __attribute__((__packed__)) WrongMessage {
        uint8_t client_bytes[CLIENT_MESSAGE_SIZE];
        uint8_t status = MSG_WRONG_STATUS;
        error_index_t error_index;
    };

    inline std::ostream &operator<<(std::ostream &os, const game_state_t &state) {
        os << "Game ID: " << ntohl(state.game_id) << "\n";
        os << "Player A ID: " << ntohl(state.player_a_id) << "\n";
        os << "Player B ID: " << ntohl(state.player_b_id) << "\n";
        os << "Status: " << static_cast<int>(state.status) << "\n";
        os << "Max Pawn: " << static_cast<int>(state.max_pawn) << "\n";
        os << "Pawn Row: ";
        for (size_t i = 0; i <= state.max_pawn; ++i) {
            os << ((state.pawn_row_bitmap[i / 8] >> (7 - (i % 8))) & 1);
        }
        return os;
    }

    inline std::ostream &operator<<(std::ostream &os, const WrongMessage &msg) {
        os << "Wrong Message:\n";
        os << "Client Bytes: ";
        for (size_t i = 0; i < CLIENT_MESSAGE_SIZE; ++i) {
            os << std::hex << static_cast<int>(msg.client_bytes[i]) << " ";
        }
        os << std::dec << "\n";
        os << "\nStatus: " << static_cast<int>(msg.status) << "\n";
        os << "Error Index: " << static_cast<int>(msg.error_index) << std::dec;
        return os;
    }

    inline bool parse_address(address_t &address, std::string_view address_str) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_DGRAM;  // UDP
        hints.ai_protocol = IPPROTO_UDP;

        struct addrinfo *addres_result;
        int errcode = getaddrinfo(address_str.data(), nullptr, &hints, &addres_result);
        if (errcode != 0) {
            std::cerr << "getaddrinfo failed: " << gai_strerror(errcode) << "\n";
            return false;
        }

        address.s_addr = ((struct sockaddr_in *)(addres_result->ai_addr))->sin_addr.s_addr;
        freeaddrinfo(addres_result);
        return true;
    }

    inline std::optional<uint16_t> parse_port(std::string_view port_str) {
        uint16_t port;
        if (!from_chars(port_str.data(), port_str.data() + port_str.size(), port)) {
            std::cerr << "Invalid port format.\n";
            return std::nullopt;
        }
        return port;
    }

    inline std::optional<timeout_t> parse_timeout(std::string_view timeout_str) {
        timeout_t timeout;
        if (!from_chars(timeout_str.data(), timeout_str.data() + timeout_str.size(), timeout) ||
            !(timeout >= MIN_TIMEOUT && timeout <= MAX_TIMEOUT)) {
            std::cerr << "Invalid timeout.\n";
            return std::nullopt;
        }
        return timeout;
    }
};  // namespace kayles_common
#endif