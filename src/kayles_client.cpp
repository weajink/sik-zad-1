#include <getopt.h>
#include "kayles_client.h"
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string_view>

using namespace kayles_common;
using namespace kayles_client;

constexpr std::string_view USAGE_STR =
    "Usage: ./kayles_client -p <port> -a <address> -m <message> -t <client_timeout>\n";

int main(int argc, char *argv[]) {
    address_t address;
    bool has_address = false;
    uint16_t port;
    bool has_port = false;
    WireMessage message;
    bool has_message = false;
    timeout_t client_timeout;
    bool has_timeout = false;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:m:t:")) != -1) {
        switch (opt) {
            case 'a': {
                if (!parse_address(address, optarg)) {
                    return 1;
                }
                has_address = true;
                break;
            }
            case 'p': {
                auto res = parse_port(optarg);
                if (!res.has_value()) {
                    return 1;
                }
                if (res.value() == 0) {
                    std::cerr << "Port number must be non-zero for client messages.\n";
                    return 1;
                }
                port = res.value();
                has_port = true;
                break;
            }
            case 't': {
                auto res = parse_timeout(optarg);
                if (!res.has_value()) {
                    return 1;
                }
                client_timeout = res.value();
                has_timeout = true;
                break;
            }
            case 'm': {
                auto res = parse_client_message(optarg);
                if (!res.has_value()) {
                    return 1;
                }
                message = res.value();
                has_message = true;
                break;
            }
            default: {
                std::cerr << USAGE_STR;
                return 1;
            }
        }
    }
    if (!has_address || !has_port || !has_message || !has_timeout
        || optind < argc) {
        std::cerr << USAGE_STR;
        return 1;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr = address;
    server_addr.sin_port = htons(port);

    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        std::cerr << "Failed to create socket.\n";
        return 1;
    }

    // Set receive timeout
    struct timeval timeout;
    timeout.tv_sec = client_timeout;
    timeout.tv_usec = 0;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "Failed to set socket timeout.\n";
        close(sock_fd);
        return 1;
    }

    ssize_t sent_bytes = sendto(sock_fd, message.data, message.len, 0,
                                (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sent_bytes < 0) {
        std::cerr << "Failed to send message to server.\n";
        close(sock_fd);
        return 1;
    } else if (static_cast<size_t>(sent_bytes) != message.len) {
        std::cerr << "Incomplete message sent to server.\n";
        close(sock_fd);
        return 1;
    }

    char buffer[1024];
    struct sockaddr_in from_addr{};
    socklen_t from_len = sizeof(from_addr);
    ssize_t recv_bytes =
        recvfrom(sock_fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
    if (recv_bytes < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            std::cout << "No response from server (timeout).\n";
            close(sock_fd);
            return 0;
        } else {
            std::cerr << "Failed to receive response from server.\n";
            close(sock_fd);
            return 1;
        }
    }

    // Verify response came from the server we sent to
    if (from_addr.sin_addr.s_addr != server_addr.sin_addr.s_addr ||
        from_addr.sin_port != server_addr.sin_port) {
        std::cerr << "Response from unexpected source, ignoring.\n";
        close(sock_fd);
        return 1;
    }

    // Dispatch based on status byte at offset 12
    constexpr size_t STATUS_OFFSET = GAME_ID_SIZE + PLAYER_ID_SIZE + PLAYER_ID_SIZE;
    if (static_cast<size_t>(recv_bytes) <= STATUS_OFFSET) {
        std::cerr << "Response too short.\n";
        close(sock_fd);
        return 1;
    }

    uint8_t status = static_cast<uint8_t>(buffer[STATUS_OFFSET]);
    if (status == MSG_WRONG_STATUS) {
        WrongMessage wrong{};
        std::memcpy(&wrong, buffer, sizeof(wrong));
        std::cout << wrong << "\n";
    } else {
        game_state_t state{};
        std::memcpy(&state, buffer, static_cast<size_t>(recv_bytes));
        std::cout << state << "\n";
    }

    close(sock_fd);
    return 0;
}