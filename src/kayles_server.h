#ifndef KAYLES_SERVER_H
#define KAYLES_SERVER_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <expected>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

#include "kayles_error.h"
#include "kayles_game.h"
#include "kayles_protocol.h"
#include "kayles_types.h"

namespace kayles::server {
    using namespace kayles::protocol;
    using namespace kayles::error;
    using namespace kayles::types;
    using namespace kayles::game;

    class KaylesServer {
       private:
        // Server configuration
        address_t address;
        uint16_t port;
        timeout_t server_timeout;
        pawn_t max_pawn;
        pawn_row_t row;

        // Server bindings
        int socket_fd = -1;
        struct sockaddr_in server_address {};

        KaylesGameMap game_map;

       public:
        KaylesServer(address_t address, uint16_t port, timeout_t server_timeout, pawn_t max_pawn,
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
                throw std::runtime_error("socket: " + std::string(strerror(errno)));
            }
            server_address.sin_family = AF_INET;
            server_address.sin_addr = address;
            server_address.sin_port = htons(port);

            if (bind(socket_fd, (struct sockaddr *)&server_address,
                     (socklen_t)sizeof(server_address)) < 0) {
                throw std::runtime_error("bind: " + std::string(strerror(errno)));
            }

            struct sockaddr_in bound {};
            socklen_t bound_len = sizeof(bound);
            if (getsockname(socket_fd, (struct sockaddr *)&bound, &bound_len) < 0) {
                throw std::runtime_error("getsockname: " + std::string(strerror(errno)));
            }
            std::cerr << "kayles_server: listening on " << inet_ntoa(bound.sin_addr) << ":"
                      << ntohs(bound.sin_port) << "\n";
        }

        void shut() {
            if (socket_fd >= 0) {
                close(socket_fd);
                socket_fd = -1;
            }
        }

        std::expected<GameState, KaylesError> process_message(const ClientMessage &msg) {
            switch (msg.msg_type) {
                case ClientMessageType::MSG_JOIN:
                    return game_map.join(msg.player_id);
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

        void handle_error(struct sockaddr_in &client_address, const char *client_msg_buf,
                          const KaylesError &error) {
            std::cerr << "kayles_server: client " << inet_ntoa(client_address.sin_addr) << ":"
                      << ntohs(client_address.sin_port) << ": " << error.what()
                      << " (index: " << error.error_index() << ")\n";
            switch (error.type()) {
                case ErrorType::EXHAUSTED_GAME_IDS:
                    break;
                default: {
                    MessageWrong msg{};
                    std::memcpy(msg.client_bytes, client_msg_buf, sizeof(msg.client_bytes));
                    msg.error_index = static_cast<uint8_t>(error.error_index());
                    auto bytes = msg.serialize();
                    if (sendto(socket_fd, bytes.data(), bytes.size(), 0,
                               (struct sockaddr *)&client_address, sizeof(client_address)) < 0) {
                        std::cerr << "kayles_server: sendto: " << strerror(errno) << "\n";
                    }
                    break;
                }
            }
        }

        void run_server_loop() {
            static char buffer[CLIENT_MESSAGE_SIZE_WITH_BUF];
            memset(buffer, 0, sizeof(buffer));

            int flags = 0;
            struct sockaddr_in client_address {};
            socklen_t address_length = (socklen_t)sizeof(client_address);

            ssize_t received_length =
                recvfrom(socket_fd, buffer, CLIENT_MESSAGE_SIZE_WITH_BUF, flags,
                         (struct sockaddr *)&client_address, &address_length);
            if (received_length < 0) {
                throw std::runtime_error("recvfrom: " + std::string(strerror(errno)));
            }

            auto msg = deserialize_client_message(
                std::span<const uint8_t>((const uint8_t *)buffer, (size_t)received_length));

            if (msg.has_value()) {
                auto result = process_message(msg.value());
                if (result.has_value()) {
                    auto bytes = result.value().serialize();
                    if (sendto(socket_fd, bytes.data(), bytes.size(), 0,
                               (struct sockaddr *)&client_address, sizeof(client_address)) < 0) {
                        std::cerr << "kayles_server: sendto: " << strerror(errno) << "\n";
                    }
                } else {
                    handle_error(client_address, buffer, result.error());
                }
            } else {
                handle_error(client_address, buffer, msg.error());
            }
        }

        void run() {
            for (;;) {
                run_server_loop();
            }
        }
    };

}  // namespace kayles::server

#endif