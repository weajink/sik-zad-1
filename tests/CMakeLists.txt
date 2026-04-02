# Add test executables here, e.g.:
# add_executable(test_protocol test_protocol.cpp)
# target_link_libraries(test_protocol PRIVATE GTest::gtest_main)
# target_include_directories(test_protocol PRIVATE ${PROJECT_SOURCE_DIR}/include)
# add_test(NAME test_protocol COMMAND test_protocol)

add_executable(test_server_parsing test_server_parsing.cpp)
target_link_libraries(test_server_parsing PRIVATE GTest::gtest_main)
target_include_directories(test_server_parsing PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_server_parsing COMMAND test_server_parsing)

add_executable(test_game test_game.cpp)
target_link_libraries(test_game PRIVATE GTest::gtest_main)
target_include_directories(test_game PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_game COMMAND test_game)

add_executable(test_client_message test_client_message.cpp)
target_link_libraries(test_client_message PRIVATE GTest::gtest_main)
target_include_directories(test_client_message PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_client_message COMMAND test_client_message)

add_executable(test_game_state test_game_state.cpp)
target_link_libraries(test_game_state PRIVATE GTest::gtest_main)
target_include_directories(test_game_state PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_game_state COMMAND test_game_state)
