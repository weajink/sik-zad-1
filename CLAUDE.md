# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Kayles — a two-player UDP network game in C++23 (SIK university assignment). Players take turns knocking down pins: either one pin or two adjacent pins. The player who knocks down the last pin wins. See `docs/task.txt` for the full specification in Polish.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

The assignment also requires a flat `Makefile` producing `kayles_server` and `kayles_client` (for submission). `make clean` must remove all build artifacts.

## Test

```bash
ctest --test-dir build --output-on-failure
```

Run a single test:
```bash
ctest --test-dir build --output-on-failure -R <test_name>
```

## Lint

```bash
find src include tests -name '*.cpp' -o -name '*.hpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
```

## Architecture

- `src/` — server (`kayles_server`) and client (`kayles_client`) executables
- `include/` — shared headers (protocol, serialization, game logic)
- `tests/` — Google Test unit tests
- `docs/task.txt` — full assignment specification (reference only)

## Constraints (from assignment)

- Raw POSIX sockets only — no Boost, no networking libraries
- No threads, no processes, no `poll`, no `select` — server is a single-threaded state machine driven by `recvfrom`
- IPv4 only
- All multi-byte values in network byte order
- Client timeout via socket options (e.g., `SO_RCVTIMEO`)
- Server timeout handling at message-receive time (no timers)

## Protocol Summary

### Client → Server messages (all values big-endian):

| Type | msg_type | Fields |
|------|----------|--------|
| MSG_JOIN (0) | 0 | player_id (u32) |
| MSG_MOVE_1 (1) | 1 | player_id (u32), game_id (u32), pawn (u8) |
| MSG_MOVE_2 (2) | 2 | player_id (u32), game_id (u32), pawn (u8) — knocks pawn and pawn+1 |
| MSG_KEEP_ALIVE (3) | 3 | player_id (u32), game_id (u32) |
| MSG_GIVE_UP (4) | 4 | player_id (u32), game_id (u32) |

### Server → Client messages:

- **MSG_GAME_STATE**: game_id (u32), player_a_id (u32), player_b_id (u32), status (u8), max_pawn (u8), pawn_row (bitmap, `floor(max_pawn/8)+1` bytes). Pawn 0 is the MSB of byte 0.
- **MSG_WRONG_MSG**: 12 bytes (first 12 bytes of client msg, zero-padded), status=255 (u8), error_index (u8).

### Game status values:
- 0 = WAITING_FOR_OPPONENT
- 1 = TURN_A, 2 = TURN_B
- 3 = WIN_A, 4 = WIN_B

### Key rules:
- At most one game in WAITING_FOR_OPPONENT state at a time
- Player IDs are 1..2^32-1 (nonzero), game IDs are 0..2^32-1
- pawn_row bitmap: max_pawn+1 bits, pawn 0 = MSB of first byte, excess bits zeroed
- Server responds to valid messages with MSG_GAME_STATE, invalid with MSG_WRONG_MSG
- Valid MSG_JOIN may be silently ignored if server can't create a game
- Illegal moves don't change game state but still get a MSG_GAME_STATE response
- Finished games retained for server_timeout seconds after last valid message

### CLI:
- Server: `-r pawn_row -a address -p port -t server_timeout`
- Client: `-a address -p port -m message -t client_timeout`
- Client `-m` format: field values separated by `/` in base-10

## Conventions

- C++23, compiled with `-Wall -Wextra -Wpedantic -Werror`
- Google style formatting (4-space indent, 100 col limit) via `.clang-format`
- Test files named `test_<module>.cpp`, linked against `GTest::gtest_main`

## Agent Roles

**Important: The user writes all production code. Agents only write tests and review code.**

### Writing Tests

- Place tests in `tests/test_<module>.cpp`
- Register each test file in `tests/CMakeLists.txt` following the commented pattern
- Focus on: serialization/deserialization, protocol message parsing, pawn_row bitmap manipulation, game logic (legal/illegal moves, win conditions, turn order), CLI argument parsing
- Verify correct network byte order in serialized messages
- Test edge cases: max_pawn=0 (single pin), max_pawn=255, empty bitmap, adjacent pins at boundaries
- Test MSG_WRONG_MSG generation for malformed inputs
- Do NOT test socket I/O directly — test the logic layers

### Code Review

- Verify compliance with `docs/task.txt` specification
- Check for undefined behavior, memory safety, buffer overflows
- Ensure `htons`/`ntohs`/`htonl`/`ntohl` on all multi-byte network fields
- Verify proper error handling on all syscalls (`socket`, `bind`, `sendto`, `recvfrom`)
- Confirm no use of threads, processes, `poll`, or `select`
- Check that server is a proper state machine driven only by incoming messages
- Verify timeout handling: client via `SO_RCVTIMEO`, server at message-receive time
- Check pawn_row bitmap correctness (MSB ordering, excess bits zeroed)
- Verify game lifecycle: WAITING → TURN_B → play → WIN_A/WIN_B, cleanup after timeout
- Flag any blocking calls that could stall the server loop
