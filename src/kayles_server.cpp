#include <arpa/inet.h>
#include <inttypes.h>
#include <kayles_common.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace kayles_common;

using address_t = in_addr;
using timeout_t = uint8_t;
using pawn_row_t = std::vector<bool>;

class KaylesGame {
   public:
    enum class Status : uint8_t { WAITING_FOR_OPPONENT, TURN_A, TURN_B, WIN_A, WIN_B };

   private:
    uint32_t game_id;
    uint32_t player_a_id;
    uint32_t player_b_id = 0;
    time_t player_a_last_move_time;
    time_t player_b_last_move_time = 0;

    Status status;
    uint8_t max_pawn;
    pawn_row_t pawn_row;
    uint8_t pawns_left_in_row;

    bool check_if_my_turn(uint32_t player_id) const {
        return (player_id == player_a_id && status == Status::TURN_A) ||
               (player_id == player_b_id && status == Status::TURN_B);
    }

    bool take_pawn(uint32_t pawn) {
        if (pawn > max_pawn || !pawn_row[pawn]) {
            return false;
        }
        pawn_row[pawn] = false;
        pawns_left_in_row--;
        return true;
    }

    bool take_two_consecutive_pawns(uint32_t first_pawn) {
        if (first_pawn + 1 > max_pawn || !pawn_row[first_pawn] || !pawn_row[first_pawn + 1]) {
            return false;
        }
        pawn_row[first_pawn] = pawn_row[first_pawn + 1] = false;
        pawns_left_in_row -= 2;
        return true;
    }

   public:
    KaylesGame(uint32_t game_id, uint32_t player_a_id, uint8_t max_pawn, pawn_row_t pawn_row)
        : game_id(game_id),
          player_a_id(player_a_id),
          player_a_last_move_time(time(NULL)),
          status(Status::WAITING_FOR_OPPONENT),
          max_pawn(max_pawn),
          pawn_row(pawn_row) {
        if (player_a_id == 0) {
            throw std::invalid_argument("Player id must be positive.");
        }
        pawns_left_in_row = std::count(pawn_row.begin(), pawn_row.end(), true);
    }

    // Updates player move time.
    void keep_alive(uint32_t player_id) {
        if (player_id == player_a_id) {
            player_a_last_move_time = time(NULL);
        }
        if (player_id == player_b_id) {
            player_b_last_move_time = time(NULL);
        }
    }

    void join_player_b(uint32_t player_b_id) {
        if (player_b_id == 0) {
            throw std::invalid_argument("Player id must be positive.");
        }
        assert(this->player_b_id == 0);
        this->player_b_id = player_b_id;
        player_b_last_move_time = time(NULL);
        status = Status::TURN_B;
    }

    void give_up(uint32_t player_id) {
        keep_alive(player_id);
        if (player_id == player_a_id && status == Status::TURN_A) {
            status = Status::WIN_B;
        } else if (player_id == player_b_id && status == Status::TURN_B) {
            status = Status::WIN_A;
        }
    }

    // no_of_pawns: 1 or 2
    void move(uint32_t player_id, uint8_t pawn, uint8_t no_of_pawns) {
        keep_alive(player_id);
        if (!check_if_my_turn(player_id)) {
            return;
        }

        if (no_of_pawns == 1) {
            if (!take_pawn(pawn))
                return;
        } else if (no_of_pawns == 2) {
            if (!take_two_consecutive_pawns(pawn))
                return;
        } else
            assert(false);

        if (pawns_left_in_row == 0) {
            if (player_id == player_a_id)
                status = Status::WIN_A;
            else
                status = Status::WIN_B;
            return;
        }

        if (status == Status::TURN_A)
            status = Status::TURN_B;
        else if (status == Status::TURN_B)
            status = Status::TURN_A;
    }

    bool is_player_joined(uint32_t player_id) const {
        return player_id == player_a_id || player_id == player_b_id;
    }

    // Checks if the game can be qualified as
    // stale according to server_timeout
    // and deleted.
    bool check_timeouts(timeout_t server_timeout) {
        switch (status) {
            case Status::TURN_A:
                if (time(NULL) - player_a_last_move_time > server_timeout) {
                    status = Status::WIN_B;
                }
                return false;
            case Status::TURN_B:
                if (time(NULL) - player_b_last_move_time > server_timeout) {
                    status = Status::WIN_A;
                }
                return false;
            default:
                // std::min of time differences = time since the most recent message
                return (std::min(time(NULL) - player_a_last_move_time,
                                 time(NULL) - player_b_last_move_time) > server_timeout);
        }
    }

    // maybe should be something like message_t
    std::vector<uint8_t> get_game_state() {
        size_t bitmap_size = max_pawn / 8 + 1;
        size_t game_state_size =
            GAME_ID_SIZE + 2 * PLAYER_ID_SIZE + STATUS_SIZE + PAWN_SIZE + bitmap_size;

        std::vector<uint8_t> res(game_state_size);
        size_t offset = 0;
        // 1. game_id
        uint32_t game_id_n = htonl(game_id);
        std::memcpy(res.data() + offset, &game_id_n, GAME_ID_SIZE);
        offset += GAME_ID_SIZE;
        // 2. player_a_id
        uint32_t player_a_id_n = htonl(player_a_id);
        std::memcpy(res.data() + offset, &player_a_id_n, PLAYER_ID_SIZE);
        offset += PLAYER_ID_SIZE;
        // 3. player_b_id
        uint32_t player_b_id_n = htonl(player_b_id);
        std::memcpy(res.data() + offset, &player_b_id_n, PLAYER_ID_SIZE);
        offset += PLAYER_ID_SIZE;
        // 4. status
        res[offset] = std::to_underlying(status);
        offset += STATUS_SIZE;
        // 5. max_pawn
        res[offset] = max_pawn;
        offset += PAWN_SIZE;
        // 6. pawn row
        // (might be beneficial to just rewrite pawn_row_t to vector<uint8_t> from vector<bool>)
        std::vector<uint8_t> bitmap(bitmap_size, 0);
        for (size_t i = 0; i <= max_pawn; i++) {
            if (pawn_row[i]) {
                bitmap[i / 8] |= (1 << (7 - (i % 8)));
            }
        }
        res.insert(res.end(), bitmap.begin(), bitmap.end());

        return res;
    }
};

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
        : address(address), port(port), server_timeout(server_timeout), row(row) {}

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
            socket_fd = -1;
        }
    }

    void run_server_loop() {
        static char buffer[CLIENT_MESSAGE_SIZE];
        memset(buffer, 0, sizeof(buffer));

        int flags = 0;
        struct sockaddr_in client_address;
        socklen_t address_length = (socklen_t)sizeof(client_address);

        ssize_t received_length = recvfrom(socket_fd, buffer, CLIENT_MESSAGE_SIZE, flags,
                                           (struct sockaddr *)&client_address, &address_length);
        if (received_length < 0) {
            throw std::runtime_error("recvfrom error");
        }

        // now run get_message_from_buffer and handle
        // different messages
    }

    void run() {
        std::cerr << "Server loop started.\n";
        for (;;) {
            run_server_loop();
        }
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

    if (res.front() != true || res.back() != true) {
        return std::nullopt;
    }

    return res;
}

constexpr std::string_view USAGE_STR =
    "Usage: ./kayles_server -r <row> -p <port> -a <address> -t <server_timeout>\n";

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
                    std::cerr << "Invalid port name.\n";
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

    KaylesServer server(address, port, server_timeout, row);
    return 0;
}