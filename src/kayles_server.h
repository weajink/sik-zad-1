#ifndef KAYLES_SERVER_H
#define KAYLES_SERVER_H

#include <arpa/inet.h>
#include "kayles_common.h"
#include "kayles_game.h"
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <expected>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace kayles_server {
    using namespace kayles_common;
    using namespace kayles_game;

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
        // player_id cannot be 0
        if (res.player_id == 0) {
            return std::unexpected(offset);
        }
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

    class KaylesServer {
       private:
        // Server configuration
        address_t address;
        uint16_t port;
        timeout_t server_timeout;
        uint8_t max_pawn;
        pawn_row_t row;

        // Server bindings
        int socket_fd = -1;
        struct sockaddr_in server_address{};

        KaylesGameMap game_map;

       public:
        KaylesServer(address_t address, uint16_t port, timeout_t server_timeout, uint8_t max_pawn,
                     pawn_row_t row)
            : address(address),
              port(port),
              server_timeout(server_timeout),
              max_pawn(max_pawn),
              row(row),
              game_map(server_timeout, max_pawn, row) {}

        ~KaylesServer() {
            shut();
        }

        void start() {
            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd < 0) {
                throw std::runtime_error("cannot create a socket");
            }
            server_address.sin_family = AF_INET;
            server_address.sin_addr = address;
            server_address.sin_port = htons(port);

            if (bind(socket_fd, (struct sockaddr *)&server_address,
                     (socklen_t)sizeof(server_address)) < 0) {
                throw std::runtime_error("bind failed");
            }

            std::cerr << "Server successfully started, listening on port " << port << ".\n";
        }

        void shut() {
            if (socket_fd >= 0) {
                close(socket_fd);
                socket_fd = -1;
            }
        }

        // Outer nullopt = silently ignore (spec 3.3: JOIN when no game can be created).
        std::optional<std::expected<game_state_t, KaylesGameError>> process_message(
            const ClientMessage &msg) {
            switch (msg.msg_type) {
                case ClientMessageType::MSG_JOIN: {
                    auto state = game_map.join(msg.player_id);
                    if (!state.has_value()) {
                        return std::nullopt;
                    }
                    return state.value();
                }
                case ClientMessageType::MSG_MOVE_1:
                    return game_map.move(msg.player_id, msg.game_id, msg.pawn, 1);
                case ClientMessageType::MSG_MOVE_2:
                    return game_map.move(msg.player_id, msg.game_id, msg.pawn, 2);
                case ClientMessageType::MSG_KEEP_ALIVE:
                    return game_map.keep_alive(msg.player_id, msg.game_id);
                case ClientMessageType::MSG_GIVE_UP:
                    return game_map.give_up(msg.player_id, msg.game_id);
                default:
                    std::unreachable();
            }
        }

        void respond_wrong_message(struct sockaddr_in &client_address, const char *buffer,
                                   error_index_t error_index) {
            WrongMessage wrong{};
            std::memcpy(wrong.client_bytes, buffer, sizeof(wrong.client_bytes));
            wrong.error_index = error_index;
            if (sendto(socket_fd, &wrong, sizeof(wrong), 0, (struct sockaddr *)&client_address,
                       sizeof(client_address)) < 0) {
                std::cerr << "Failed to send wrong message response: " << strerror(errno) << "\n";
            }
        }

        void run_server_loop() {
            static char buffer[CLIENT_MESSAGE_SIZE];
            memset(buffer, 0, sizeof(buffer));

            int flags = 0;
            struct sockaddr_in client_address{};
            socklen_t address_length = (socklen_t)sizeof(client_address);

            ssize_t received_length = recvfrom(socket_fd, buffer, CLIENT_MESSAGE_SIZE, flags,
                                               (struct sockaddr *)&client_address, &address_length);
            if (received_length < 0) {
                throw std::runtime_error("recvfrom error");
            }

            auto msg = get_message_from_buffer(buffer, received_length);
            if (msg.has_value()) {
                auto outcome = process_message(msg.value());
                if (!outcome.has_value()) {
                    return;  // silently ignore (e.g., game IDs exhausted on JOIN)
                }
                auto &result = outcome.value();
                if (result.has_value()) {
                    auto &state = result.value();
                    size_t state_size =
                        sizeof(game_state_t) - MAX_BITMAP_SIZE + (state.max_pawn / 8 + 1);
                    if (sendto(socket_fd, &state, state_size, 0, (struct sockaddr *)&client_address,
                               sizeof(client_address)) < 0) {
                        std::cerr << "Failed to send game state response: " << strerror(errno)
                                  << "\n";
                    }
                } else {
                    switch (result.error()) {
                        case KaylesGameError::INVALID_PLAYER_ID:
                            respond_wrong_message(client_address, buffer, MSG_TYPE_SIZE);
                            break;
                        case KaylesGameError::INVALID_GAME_ID:
                            respond_wrong_message(client_address, buffer,
                                                  MSG_TYPE_SIZE + PLAYER_ID_SIZE);
                            break;
                        default:
                            std::unreachable();
                    }
                }
            } else {
                respond_wrong_message(client_address, buffer, msg.error());
            }
        }

        void run() {
            std::cerr << "Server loop started.\n";
            for (;;) {
                run_server_loop();
            }
        }
    };

}  // namespace kayles_server

#endif