#include "kayles_server.h"

#include <arpa/inet.h>
#include <getopt.h>

#include <cstring>
#include <iostream>
#include <optional>
#include <string>

#include "kayles_parse.h"

using namespace kayles::server;
using namespace kayles::parse;

constexpr std::string_view PROG = "kayles_server";
constexpr std::string_view USAGE_STR =
    "usage: kayles_server -r <row> -p <port> -a <address> -t <server_timeout>\n";

int main(int argc, char *argv[]) {
    std::optional<pawn_row_t> opt_row;
    std::optional<address_t> opt_address;
    std::optional<uint16_t> opt_port;
    std::optional<timeout_t> opt_timeout;

    int opt;
    while ((opt = getopt(argc, argv, "r:a:p:t:")) != -1) {
        switch (opt) {
            case 'r': {
                if (!assign_or_report(PROG, opt_row, parse_pawn_row(optarg))) {
                    return 1;
                }
                break;
            }
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
                break;
            }
            case 't': {
                if (!assign_or_report(PROG, opt_timeout, parse_timeout(optarg))) {
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
    if (!opt_row || !opt_port || !opt_address || !opt_timeout || optind < argc) {
        std::cerr << PROG << ": " << USAGE_STR;
        return 1;
    }

    uint8_t max_pawn = opt_row.value().size() - 1;
    KaylesServer server(opt_address.value(), opt_port.value(), opt_timeout.value(), max_pawn,
                        opt_row.value());
    try {
        server.start();
        server.run();
    } catch (const std::exception &e) {
        std::cerr << PROG << ": " << e.what() << "\n";
        return 1;
    }
    return 0;
}