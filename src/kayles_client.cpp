#include <getopt.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "kayles_parse.h"
#include "kayles_protocol.h"
#include "kayles_signal.h"

using namespace kayles::protocol;
using namespace kayles::parse;

constexpr std::string_view PROG = "kayles_client";
constexpr std::string_view USAGE_STR =
    "usage: kayles_client -p <port> -a <address> -m <message> -t <client_timeout>\n";

static void safe_close(int fd) {
    if (close(fd) < 0) {
        std::cerr << PROG << ": close: " << strerror(errno) << "\n";
    }
}

int main(int argc, char *argv[]) {
    std::optional<address_t> opt_address;
    std::optional<uint16_t> opt_port;
    std::optional<ClientMessage> opt_message;
    std::optional<timeout_t> opt_timeout;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:m:t:")) != -1) {
        switch (opt) {
            case 'a': {
                if (!assign_or_report(PROG, opt_address, parse_address(optarg))) {
                    return 1;
                }
                break;
            }
            case 'p': {
                if (!assign_or_report(PROG, opt_port, parse_port(optarg))) {
                    return 1;
                }
                if (opt_port.value() == 0) {
                    std::cerr << PROG << ": arg: port must be non-zero\n";
                    return 1;
                }
                break;
            }
            case 't': {
                if (!assign_or_report(PROG, opt_timeout, parse_timeout(optarg))) {
                    return 1;
                }
                break;
            }
            case 'm': {
                if (!assign_or_report(PROG, opt_message, parse_client_message(optarg))) {
                    return 1;
                }
                break;
            }
            default: {
                std::cerr << PROG << ": " << USAGE_STR;
                return 1;
            }
        }
    }
    if (!opt_address || !opt_port || !opt_message || !opt_timeout || optind < argc) {
        std::cerr << PROG << ": " << USAGE_STR;
        return 1;
    }

    address_t address = opt_address.value();
    uint16_t port = opt_port.value();
    ClientMessage message = opt_message.value();
    timeout_t client_timeout = opt_timeout.value();

    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr = address;
    server_addr.sin_port = htons(port);

    if (!kayles::sig::install(PROG)) {
        return 1;
    }

    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        std::cerr << PROG << ": socket: " << strerror(errno) << "\n";
        return 1;
    }

    // Set receive timeout
    struct timeval timeout;
    timeout.tv_sec = client_timeout.count();
    timeout.tv_usec = 0;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << PROG << ": setsockopt: " << strerror(errno) << "\n";
        safe_close(sock_fd);
        return 1;
    }

    auto wire = message.serialize();
    ssize_t sent_bytes = sendto(sock_fd, wire.data(), wire.size(), 0,
                                (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sent_bytes < 0) {
        std::cerr << PROG << ": sendto: " << strerror(errno) << "\n";
        safe_close(sock_fd);
        return 1;
    } else if (static_cast<size_t>(sent_bytes) != wire.size()) {
        std::cerr << PROG << ": sendto: short write (" << sent_bytes << " of " << wire.size()
                  << " bytes)\n";
        safe_close(sock_fd);
        return 1;
    }

    char buffer[1024];
    struct sockaddr_in from_addr {};
    socklen_t from_len = sizeof(from_addr);
    ssize_t recv_bytes =
        recvfrom(sock_fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
    if (recv_bytes < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            std::cout << "No response from server (timeout).\n";
            safe_close(sock_fd);
            return 0;
        } else if (errno == EINTR) {
            safe_close(sock_fd);
            return 0;
        } else {
            std::cerr << PROG << ": recvfrom: " << strerror(errno) << "\n";
            safe_close(sock_fd);
            return 1;
        }
    }

    // Verify response came from the server we sent to
    if (from_addr.sin_addr.s_addr != server_addr.sin_addr.s_addr ||
        from_addr.sin_port != server_addr.sin_port) {
        std::cerr << PROG << ": source: response from unexpected peer\n";
        safe_close(sock_fd);
        return 1;
    }

    std::span<const uint8_t> response(reinterpret_cast<const uint8_t *>(buffer),
                                      static_cast<size_t>(recv_bytes));
    auto parsed = deserialize_server_message(response);
    if (!parsed) {
        std::cerr << PROG << ": response: " << parsed.error().what() << "\n";
        safe_close(sock_fd);
        return 1;
    }
    std::visit([](const auto &m) { std::cout << m << "\n"; }, *parsed);

    safe_close(sock_fd);
    return 0;
}