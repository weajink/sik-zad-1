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
