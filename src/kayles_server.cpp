#include <arpa/inet.h>
#include <kayles_common.h>
#include <kayles_server.h>

#include <cstring>
#include <iostream>
#include <optional>
#include <string>

using namespace kayles_common;
using namespace kayles_server;

// A correct pawn row:
// 1. consists of 0s and 1s
// 2. has a length between 1 to 256
// 3. first and the last cells are equal to 1
//
// This function returns the pawn row if the string
// represents a correct pawn row and nullopt otherwise.
static std::optional<pawn_row_t> string_to_pawn_row(const std::string_view &s) {
    if (s.size() < 1 || s.size() > 256) {
        return std::nullopt;
    }

    pawn_row_t res(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '0' && s[i] != '1') {
            return std::nullopt;
        }
        res[i] = (s[i] == '1');
    }

    if (res.front() != true || res.back() != true) {
        return std::nullopt;
    }

    return res;
}

constexpr std::string_view USAGE_STR =
    "Usage: ./kayles_server -r <row> -p <port> -a <address> -t "
    "<server_timeout>\n";

int main(int argc, char *argv[]) {
    pawn_row_t row;
    bool has_row = false;
    address_t address;
    bool has_address = false;
    uint16_t port;
    bool has_port = false;
    timeout_t server_timeout;
    bool has_timeout = false;

    int opt;
    while ((opt = getopt(argc, argv, "r:a:p:t:")) != -1) {
        switch (opt) {
            case 'r': {
                auto opt_vec = string_to_pawn_row(optarg);
                if (!opt_vec.has_value()) {
                    std::cerr << "Invalid pawn row sequence.\n";
                    return 1;
                }
                row = std::move(opt_vec.value());
                has_row = true;
                break;
            }
            case 'a': {
                // TODO: Add handling of domain names, not just IP addresses
                int result = inet_pton(AF_INET, optarg, &address);
                if (result == 0) {
                    std::cerr << "Invalid IP address format.\n";
                    return 1;
                } else if (result < 1) {
                    std::cerr << "inet_pton failed.\n";
                    return 1;
                }
                has_address = true;
                break;
            }
            case 'p': {
                if (!from_chars(optarg, optarg + std::strlen(optarg), port)) {
                    std::cerr << "Invalid port number.\n";
                    return 1;
                }
                has_port = true;
                break;
            }
            case 't': {
                if (!from_chars(optarg, optarg + std::strlen(optarg), server_timeout) ||
                    !(server_timeout >= MIN_SERVER_TIMEOUT &&
                      server_timeout <= MAX_SERVER_TIMEOUT)) {
                    std::cerr << "Invalid server timeout.\n";
                    return 1;
                }
                has_timeout = true;
                break;
            }
            default: {
                std::cerr << USAGE_STR;
                return 1;
            }
        }
    }
    if (!has_row || !has_port || !has_address || !has_timeout) {
        std::cerr << USAGE_STR;
        return 1;
    }

    uint8_t max_pawn = row.size() - 1;
    KaylesServer server(address, port, server_timeout, max_pawn, row);
    return 0;
}