# Legacy tests (pre-refactor, kept for reference). Opt-in with -DBUILD_LEGACY_TESTS=ON.
option(BUILD_LEGACY_TESTS "Build legacy (pre-refactor) tests" OFF)
if(BUILD_LEGACY_TESTS)
    add_subdirectory(legacy-tests)
endif()

# New tests for the refactored code go here.
# Pattern:
#   add_executable(test_name test_name.cpp)
#   target_link_libraries(test_name PRIVATE GTest::gtest_main)
#   target_include_directories(test_name PRIVATE ${PROJECT_SOURCE_DIR}/src)
#   add_test(NAME test_name COMMAND test_name)

add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE GTest::gtest_main)
target_include_directories(test_protocol PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_protocol COMMAND test_protocol)

add_executable(test_game test_game.cpp)
target_link_libraries(test_game PRIVATE GTest::gtest_main)
target_include_directories(test_game PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_game COMMAND test_game)

add_executable(test_game2 test_game2.cpp)
target_link_libraries(test_game2 PRIVATE GTest::gtest_main)
target_include_directories(test_game2 PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_game2 COMMAND test_game2)

add_executable(test_game_random test_game_random.cpp)
target_link_libraries(test_game_random PRIVATE GTest::gtest_main)
target_include_directories(test_game_random PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_game_random COMMAND test_game_random)

add_executable(test_server test_server.cpp)
target_link_libraries(test_server PRIVATE GTest::gtest_main)
target_include_directories(test_server PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_server COMMAND test_server)

add_executable(test_parse test_parse.cpp)
target_link_libraries(test_parse PRIVATE GTest::gtest_main)
target_include_directories(test_parse PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_parse COMMAND test_parse)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE GTest::gtest_main)
target_include_directories(test_integration PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_integration COMMAND test_integration)
set_tests_properties(test_integration PROPERTIES TIMEOUT 120)

# --- Brutal, spec-driven tests added to fill coverage gaps ---

add_executable(test_protocol_deeper test_protocol_deeper.cpp)
target_link_libraries(test_protocol_deeper PRIVATE GTest::gtest_main)
target_include_directories(test_protocol_deeper PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_protocol_deeper COMMAND test_protocol_deeper)

add_executable(test_game_deeper test_game_deeper.cpp)
target_link_libraries(test_game_deeper PRIVATE GTest::gtest_main)
target_include_directories(test_game_deeper PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_game_deeper COMMAND test_game_deeper)

add_executable(test_integration_deeper test_integration_deeper.cpp)
target_link_libraries(test_integration_deeper PRIVATE GTest::gtest_main)
target_include_directories(test_integration_deeper PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_integration_deeper COMMAND test_integration_deeper)
set_tests_properties(test_integration_deeper PROPERTIES TIMEOUT 180)

add_executable(test_cli test_cli.cpp)
target_link_libraries(test_cli PRIVATE GTest::gtest_main)
target_include_directories(test_cli PRIVATE ${PROJECT_SOURCE_DIR}/src)
# Tell the CLI tests where the binaries live. We use CMake's target-file
# generator expression so the paths point at the actual built binaries.
target_compile_definitions(test_cli PRIVATE
    KAYLES_SERVER_BIN="$<TARGET_FILE:kayles_server>"
    KAYLES_CLIENT_BIN="$<TARGET_FILE:kayles_client>")
add_dependencies(test_cli kayles_server kayles_client)
add_test(NAME test_cli COMMAND test_cli)
set_tests_properties(test_cli PROPERTIES TIMEOUT 180)

# -----------------------------------------------------------------------------
# Shell-based integration tests under tests/integration/test_*.sh.
# Each script drives the real kayles_server / kayles_client binaries over
# UDP via loopback. The scripts assume both binaries exist at
# ${PROJECT_SOURCE_DIR}/src/ — build with `cmake --build build` first.
# -----------------------------------------------------------------------------
file(GLOB INTEGRATION_TEST_SCRIPTS
     "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_*.sh")

foreach(script ${INTEGRATION_TEST_SCRIPTS})
    get_filename_component(script_name "${script}" NAME_WE)
    add_test(
        NAME integration_${script_name}
        COMMAND bash "${script}"
    )
    # Server_timeout / retention / keep-alive-extends-waiting tests sleep
    # for a few seconds; give all shell tests headroom. Still bounded so
    # any hang fails the test.
    set_tests_properties(integration_${script_name} PROPERTIES
        TIMEOUT 60
        REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/kayles_server;${PROJECT_SOURCE_DIR}/src/kayles_client"
    )
endforeach()

# -----------------------------------------------------------------------------
# Randomized / fuzz-style integration tests (test_fuzz_*.sh). These are
# heavier than the deterministic integration tests above: they run thousands
# of packets through the server, spawn many subprocesses, and sleep across
# server_timeout windows. Give them a much larger TIMEOUT.
#
# Each test reads RANDOM_SEED from the environment for reproducibility. CTest
# inherits the caller's environment by default, so running
#
#     RANDOM_SEED=12345 ctest --test-dir build -R fuzz
#
# will pin every fuzz test to seed 12345. The seed is printed at the top of
# every run.
# -----------------------------------------------------------------------------
file(GLOB FUZZ_INTEGRATION_TEST_SCRIPTS
     "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_fuzz_*.sh")
foreach(script ${FUZZ_INTEGRATION_TEST_SCRIPTS})
    get_filename_component(script_name "${script}" NAME_WE)
    # Override the TIMEOUT set by the previous foreach. These tests can
    # legitimately take a minute or more (e.g. the sequential 200-JOIN test,
    # the fuzz-wire 3000-packet sweep, the timeout-retention tests that wait
    # 2*server_timeout per sub-check, and the scale test that plays out 60
    # concurrent games).
    set_tests_properties(integration_${script_name} PROPERTIES
        TIMEOUT 300
    )
endforeach()
