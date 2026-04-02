#include <arpa/inet.h>
#include <inttypes.h>
#include <unistd.h>

#include <charconv>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using address_t = in_addr;
using timeout_t = uint8_t;
using pawn_row_t = std::vector<bool>;

class KaylesServer {
   private:
    // Server configuration
    address_t address;
    uint16_t port;
    timeout_t server_timeout;
    pawn_row_t row;

    // Server bindings
    int socket_fd = -1;
    struct sockaddr_in server_address;

   public:
    KaylesServer(address_t address, uint16_t port, timeout_t server_timeout, pawn_row_t row)
        : address(address), port(port), server_timeout(server_timeout), row(row){};

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

        if (bind(socket_fd, (struct sockaddr *)&server_address, (socklen_t)sizeof(server_address)) <
            0) {
            throw std::runtime_error("bind failed");
        }

        std::cerr << "Server successfully started, listening on port " << port << ".\n";
    }

    void shut() {
        if (socket_fd >= 0) {
            close(socket_fd);
        }
    }

    void run_server_loop() {}

    void run() {
        std::cerr << "Server loop started.\n";
        for (;;)
            run_server_loop();
    }
};

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

    if (res.front() != 1 || res.back() != 1) {
        return std::nullopt;
    }

    return res;
}

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
                char *end_ptr = optarg + std::strlen(optarg);
                auto [ptr, ec] = std::from_chars(optarg, end_ptr, port);

                if (!(ec == std::errc() && ptr == end_ptr && port != 0)) {
                    std::cerr << "Invalid port name.\n";
                    return 1;
                }
                has_port = true;
                break;
            }
            case 't': {
                char *end_ptr = optarg + std::strlen(optarg);
                auto [ptr, ec] = std::from_chars(optarg, end_ptr, server_timeout);

                if (!(ec == std::errc() && ptr == end_ptr && server_timeout >= 1 &&
                      server_timeout <= 99)) {
                    std::cerr << "Invalid server timeout.";
                    return 1;
                }
                has_timeout = true;
                break;
            }
        }
    }
    if (!has_row || !has_port || !has_address || !has_timeout) {
        std::cerr << "Usage: ./kayles-server -r <row> -p <port> -a <address> -t <server_timeout>\n";
        return 1;
    }
    return 0;
}