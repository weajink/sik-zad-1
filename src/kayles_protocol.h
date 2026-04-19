#ifndef KAYLES_PROTOCOL_H
#define KAYLES_PROTOCOL_H

#include <arpa/inet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <ostream>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "kayles_error.h"
#include "kayles_types.h"

namespace kayles::protocol {
    using namespace kayles::types;
    using namespace kayles::error;

    // ========================================================================
    // DEFINITIONS
    // ========================================================================

    constexpr size_t MSG_TYPE_SIZE = 1;
    constexpr size_t PLAYER_ID_SIZE = 4;
    constexpr size_t GAME_ID_SIZE = 4;
    constexpr size_t PAWN_SIZE = 1;
    constexpr size_t STATUS_SIZE = 1;
    constexpr size_t ERROR_INDEX_SIZE = 1;
    constexpr size_t CLIENT_MESSAGE_SIZE =
        MSG_TYPE_SIZE + PLAYER_ID_SIZE + GAME_ID_SIZE + PAWN_SIZE;
    constexpr size_t CLIENT_MESSAGE_SIZE_WITH_BUF =
        CLIENT_MESSAGE_SIZE + 2;  // for wrong message, we may need to read up to 2 extra bytes
    constexpr size_t MAX_VALID_CLIENT_MESSAGE = 4;
    constexpr uint8_t MSG_WRONG_STATUS = 255;

    enum class ClientMessageType : uint8_t {
        MSG_JOIN,
        MSG_MOVE_1,
        MSG_MOVE_2,
        MSG_KEEP_ALIVE,
        MSG_GIVE_UP
    };

    struct ClientMessage {
        ClientMessageType msg_type;
        player_id_t player_id;
        game_id_t game_id;
        pawn_t pawn;

        std::vector<uint8_t> serialize() const;
    };

    enum class GameStatus : uint8_t { WAITING_FOR_OPPONENT, TURN_A, TURN_B, WIN_A, WIN_B };

    struct GameState {
        game_id_t game_id;
        player_id_t player_a_id;
        player_id_t player_b_id;
        GameStatus status;
        pawn_t max_pawn;
        pawn_row_t pawn_row;

        std::vector<uint8_t> serialize() const;
    };

    struct MessageWrong {
        uint8_t client_bytes[CLIENT_MESSAGE_SIZE_WITH_BUF];
        uint8_t status = MSG_WRONG_STATUS;
        uint8_t error_index;

        std::vector<uint8_t> serialize() const;
    };

    // Either message the server may send. Both types put `status` at byte offset 12 by design,
    // which is what deserialize_server_message uses to discriminate.
    using ServerMessage = std::variant<GameState, MessageWrong>;
    constexpr size_t SERVER_MESSAGE_STATUS_OFFSET = GAME_ID_SIZE + 2 * PLAYER_ID_SIZE;
    static_assert(SERVER_MESSAGE_STATUS_OFFSET == CLIENT_MESSAGE_SIZE_WITH_BUF,
                  "status byte must align between GameState and MessageWrong");

    // ========================================================================
    // HELPERS
    // ========================================================================

    inline void append_u8(std::vector<uint8_t> &v, uint8_t x) {
        v.push_back(x);
    }

    inline void append_u32(std::vector<uint8_t> &v, uint32_t x) {
        uint32_t n = htonl(x);
        auto *p = reinterpret_cast<const uint8_t *>(&n);
        v.insert(v.end(), p, p + sizeof(n));
    }

    inline void append_bitmap(std::vector<uint8_t> &v, const pawn_row_t &row, pawn_t max_pawn) {
        size_t bitmap_size = max_pawn / 8 + 1;
        size_t start = v.size();
        v.resize(start + bitmap_size, 0);  // zero-initialize → excess bits are 0
        for (size_t i = 0; i <= max_pawn; ++i) {
            if (row[i]) {
                v[start + i / 8] |= static_cast<uint8_t>(1u << (7 - (i % 8)));
            }
        }
    }

    inline uint8_t read_u8(std::span<const uint8_t> &bytes) {
        uint8_t v = bytes.front();
        bytes = bytes.subspan(1);
        return v;
    }

    inline uint32_t read_u32(std::span<const uint8_t> &bytes) {
        uint32_t n;
        std::memcpy(&n, bytes.data(), sizeof(n));
        bytes = bytes.subspan(sizeof(n));
        return ntohl(n);
    }

    inline pawn_row_t read_bitmap(std::span<const uint8_t> &bytes, pawn_t max_pawn) {
        size_t bitmap_size = max_pawn / 8 + 1;
        pawn_row_t row;
        row.resize(max_pawn + 1);
        for (size_t i = 0; i <= max_pawn; ++i) {
            row[i] = (bytes[i / 8] >> (7 - (i % 8))) & 1;
        }
        bytes = bytes.subspan(bitmap_size);
        return row;
    }

    inline size_t get_client_message_size(ClientMessageType t) {
        switch (t) {
            case ClientMessageType::MSG_JOIN:
                return MSG_TYPE_SIZE + PLAYER_ID_SIZE;
            case ClientMessageType::MSG_MOVE_1:
            case ClientMessageType::MSG_MOVE_2:
                return MSG_TYPE_SIZE + PLAYER_ID_SIZE + GAME_ID_SIZE + PAWN_SIZE;
            case ClientMessageType::MSG_KEEP_ALIVE:
            case ClientMessageType::MSG_GIVE_UP:
                return MSG_TYPE_SIZE + PLAYER_ID_SIZE + GAME_ID_SIZE;
        }
        std::unreachable();
    }

    // Implementation

    inline std::vector<uint8_t> ClientMessage::serialize() const {
        std::vector<uint8_t> res;
        res.reserve(CLIENT_MESSAGE_SIZE);

        append_u8(res, std::to_underlying(msg_type));
        append_u32(res, player_id);
        switch (msg_type) {
            case ClientMessageType::MSG_JOIN:
                break;
            case ClientMessageType::MSG_KEEP_ALIVE:
            case ClientMessageType::MSG_GIVE_UP:
                append_u32(res, game_id);
                break;
            case ClientMessageType::MSG_MOVE_1:
            case ClientMessageType::MSG_MOVE_2:
                append_u32(res, game_id);
                append_u8(res, pawn);
                break;
        }

        return res;
    }

    inline std::expected<ClientMessage, KaylesError> deserialize_client_message(
        std::span<const uint8_t> bytes) {
        ClientMessage res{};
        if (bytes.empty()) {
            return std::unexpected(KaylesError::invalid_length(0));
        }
        uint8_t msg_type = read_u8(bytes);
        if (msg_type > MAX_VALID_CLIENT_MESSAGE) {
            return std::unexpected(KaylesError::invalid_msg_type(0));
        }
        res.msg_type = static_cast<ClientMessageType>(msg_type);
        size_t size = get_client_message_size(res.msg_type);
        size_t received = bytes.size() + MSG_TYPE_SIZE;
        if (size != bytes.size() + MSG_TYPE_SIZE) {  // we already consumed one byte
            return std::unexpected(KaylesError::invalid_length(std::min(size, received)));
        }
        res.player_id = read_u32(bytes);
        if (res.player_id == 0) {
            return std::unexpected(KaylesError::player_id_zero());
        }
        switch (res.msg_type) {
            case ClientMessageType::MSG_GIVE_UP:
            case ClientMessageType::MSG_KEEP_ALIVE:
                res.game_id = read_u32(bytes);
                break;
            case ClientMessageType::MSG_MOVE_1:
            case ClientMessageType::MSG_MOVE_2:
                res.game_id = read_u32(bytes);
                res.pawn = read_u8(bytes);
                break;
            default:
                break;
        }
        return res;
    }

    inline std::vector<uint8_t> GameState::serialize() const {
        std::vector<uint8_t> res;
        append_u32(res, game_id);
        append_u32(res, player_a_id);
        append_u32(res, player_b_id);
        append_u8(res, std::to_underlying(status));
        append_u8(res, max_pawn);
        append_bitmap(res, pawn_row, max_pawn);
        return res;
    }

    inline constexpr size_t GAME_STATE_HEADER_SIZE =
        GAME_ID_SIZE + 2 * PLAYER_ID_SIZE + STATUS_SIZE + PAWN_SIZE;

    inline std::expected<GameState, KaylesError> deserialize_game_state(
        std::span<const uint8_t> bytes) {
        if (bytes.size() < GAME_STATE_HEADER_SIZE) {
            return std::unexpected(KaylesError::invalid_length(bytes.size()));
        }
        GameState res{};
        res.game_id = read_u32(bytes);
        res.player_a_id = read_u32(bytes);
        res.player_b_id = read_u32(bytes);
        res.status = static_cast<GameStatus>(read_u8(bytes));
        res.max_pawn = read_u8(bytes);
        size_t expected_bitmap_size = res.max_pawn / 8 + 1;
        if (bytes.size() != expected_bitmap_size) {
            return std::unexpected(KaylesError::invalid_length(GAME_STATE_HEADER_SIZE));
        }
        res.pawn_row = read_bitmap(bytes, res.max_pawn);
        return res;
    }

    inline std::vector<uint8_t> MessageWrong::serialize() const {
        std::vector<uint8_t> res;
        res.reserve(CLIENT_MESSAGE_SIZE_WITH_BUF + STATUS_SIZE + ERROR_INDEX_SIZE);
        res.insert(res.end(), client_bytes, client_bytes + CLIENT_MESSAGE_SIZE_WITH_BUF);
        append_u8(res, status);
        append_u8(res, error_index);
        return res;
    }

    inline std::expected<MessageWrong, KaylesError> deserialize_message_wrong(
        std::span<const uint8_t> bytes) {
        if (bytes.size() != CLIENT_MESSAGE_SIZE_WITH_BUF + STATUS_SIZE + ERROR_INDEX_SIZE) {
            return std::unexpected(KaylesError::invalid_length(bytes.size()));
        }
        MessageWrong res{};
        std::memcpy(res.client_bytes, bytes.data(), CLIENT_MESSAGE_SIZE_WITH_BUF);
        res.status = bytes[CLIENT_MESSAGE_SIZE_WITH_BUF];
        res.error_index = bytes[CLIENT_MESSAGE_SIZE_WITH_BUF + STATUS_SIZE];
        return res;
    }

    // Discriminates on the status byte at offset 12 and delegates to the right deserializer.
    inline std::expected<ServerMessage, KaylesError> deserialize_server_message(
        std::span<const uint8_t> bytes) {
        if (bytes.size() <= SERVER_MESSAGE_STATUS_OFFSET) {
            return std::unexpected(KaylesError::invalid_length(bytes.size()));
        }
        if (bytes[SERVER_MESSAGE_STATUS_OFFSET] == MSG_WRONG_STATUS) {
            auto w = deserialize_message_wrong(bytes);
            if (!w)
                return std::unexpected(w.error());
            return ServerMessage{*w};
        }
        auto s = deserialize_game_state(bytes);
        if (!s)
            return std::unexpected(s.error());
        return ServerMessage{*s};
    }

    // ========================================================================
    // PRETTY PRINTING
    // ========================================================================

    inline std::ostream &operator<<(std::ostream &os, ClientMessageType t) {
        switch (t) {
            case ClientMessageType::MSG_JOIN:
                return os << "MSG_JOIN";
            case ClientMessageType::MSG_MOVE_1:
                return os << "MSG_MOVE_1";
            case ClientMessageType::MSG_MOVE_2:
                return os << "MSG_MOVE_2";
            case ClientMessageType::MSG_KEEP_ALIVE:
                return os << "MSG_KEEP_ALIVE";
            case ClientMessageType::MSG_GIVE_UP:
                return os << "MSG_GIVE_UP";
        }
        std::unreachable();
    }

    inline std::ostream &operator<<(std::ostream &os, GameStatus s) {
        switch (s) {
            case GameStatus::WAITING_FOR_OPPONENT:
                return os << "WAITING_FOR_OPPONENT";
            case GameStatus::TURN_A:
                return os << "TURN_A";
            case GameStatus::TURN_B:
                return os << "TURN_B";
            case GameStatus::WIN_A:
                return os << "WIN_A";
            case GameStatus::WIN_B:
                return os << "WIN_B";
        }
        std::unreachable();
    }

    inline std::ostream &operator<<(std::ostream &os, const ClientMessage &m) {
        os << m.msg_type << " player_id=" << m.player_id;
        switch (m.msg_type) {
            case ClientMessageType::MSG_JOIN:
                break;
            case ClientMessageType::MSG_KEEP_ALIVE:
            case ClientMessageType::MSG_GIVE_UP:
                os << " game_id=" << m.game_id;
                break;
            case ClientMessageType::MSG_MOVE_1:
            case ClientMessageType::MSG_MOVE_2:
                os << " game_id=" << m.game_id << " pawn=" << static_cast<unsigned>(m.pawn);
                break;
        }
        return os;
    }

    inline std::ostream &operator<<(std::ostream &os, const GameState &s) {
        os << "GameState{game_id=" << s.game_id << " player_a=" << s.player_a_id
           << " player_b=" << s.player_b_id << " status=" << s.status
           << " max_pawn=" << static_cast<unsigned>(s.max_pawn) << " pawn_row=";
        for (bool p : s.pawn_row)
            os << (p ? '1' : '0');
        return os << '}';
    }

    inline std::ostream &operator<<(std::ostream &os, const MessageWrong &w) {
        os << "MessageWrong{client_bytes=";
        os << std::hex;
        for (size_t i = 0; i < CLIENT_MESSAGE_SIZE_WITH_BUF; ++i) {
            os << static_cast<unsigned>(w.client_bytes[i]);
            if (i + 1 < CLIENT_MESSAGE_SIZE_WITH_BUF)
                os << ' ';
        }
        os << std::dec;
        return os << " status=" << static_cast<unsigned>(w.status)
                  << " error_index=" << static_cast<unsigned>(w.error_index) << '}';
    }
}  // namespace kayles::protocol

namespace kayles::error {
    // Out-of-line factory bodies — defined here so protocol constants are visible.
    inline KaylesError KaylesError::invalid_game_id() {
        return {ErrorType::INVALID_GAME_ID, "Invalid game ID.",
                kayles::protocol::MSG_TYPE_SIZE + kayles::protocol::PLAYER_ID_SIZE};
    }
    inline KaylesError KaylesError::invalid_player_id() {
        return {ErrorType::INVALID_PLAYER_ID, "Player not in the provided game.",
                kayles::protocol::MSG_TYPE_SIZE + kayles::protocol::PLAYER_ID_SIZE};
    }
    inline KaylesError KaylesError::player_id_zero() {
        return {ErrorType::INVALID_PLAYER_ID, "Player ID cannot be zero.",
                kayles::protocol::MSG_TYPE_SIZE};
    }
}  // namespace kayles::error

#endif
