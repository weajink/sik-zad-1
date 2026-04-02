# Add test executables here, e.g.:
# add_executable(test_protocol test_protocol.cpp)
# target_link_libraries(test_protocol PRIVATE GTest::gtest_main)
# target_include_directories(test_protocol PRIVATE ${PROJECT_SOURCE_DIR}/include)
# add_test(NAME test_protocol COMMAND test_protocol)

add_executable(test_server_parsing test_server_parsing.cpp)
target_link_libraries(test_server_parsing PRIVATE GTest::gtest_main)
target_compile_options(test_server_parsing PRIVATE -Wno-return-type -Wno-reorder -fpermissive -Wno-error)
add_test(NAME test_server_parsing COMMAND test_server_parsing)

add_executable(test_game test_game.cpp)
target_link_libraries(test_game PRIVATE GTest::gtest_main)
target_compile_options(test_game PRIVATE -Wno-return-type -Wno-reorder -fpermissive -Wno-error)
add_test(NAME test_game COMMAND test_game)
