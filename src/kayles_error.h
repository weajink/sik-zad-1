#ifndef KAYLES_ERROR_H
#define KAYLES_ERROR_H

#include <cstddef>
#include <string>

namespace kayles::error {
    enum class ErrorType : uint8_t {
        INVALID_MESSAGE_LENGTH,
        INVALID_MESSAGE_ARGUMENT,
        EXHAUSTED_GAME_IDS,
        INVALID_GAME_ID,
        INVALID_PLAYER_ID,
        PARSE_ERROR
    };

    class KaylesError {
       private:
        ErrorType type_;
        std::string what_;
        size_t error_index_;

        KaylesError(ErrorType type, std::string what, size_t error_index)
            : type_(type), what_(std::move(what)), error_index_(error_index) {}

       public:
        ErrorType type() const {
            return type_;
        }
        const std::string &what() const {
            return what_;
        }
        size_t error_index() const {
            return error_index_;
        }

        // Factories — centralize error-message strings.
        static KaylesError invalid_length(size_t error_index) {
            return {ErrorType::INVALID_MESSAGE_LENGTH, "Invalid message length.", error_index};
        }
        static KaylesError invalid_msg_type(size_t error_index = 0) {
            return {ErrorType::INVALID_MESSAGE_ARGUMENT, "Invalid message type.", error_index};
        }
        static KaylesError game_ids_exhausted() {
            return {ErrorType::EXHAUSTED_GAME_IDS, "No more game IDs available.", 0};
        }
        // Defined out-of-line in kayles_protocol.h so MSG_TYPE_SIZE / PLAYER_ID_SIZE
        // are visible without circular include.
        static KaylesError invalid_game_id();
        static KaylesError invalid_player_id();
        static KaylesError parse_error(const std::string &message) {
            return {ErrorType::PARSE_ERROR, "CLI parse error: " + message, 0};
        }
    };
}  // namespace kayles::error

#endif
