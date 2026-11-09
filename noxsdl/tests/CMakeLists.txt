# Asset-free client tests, independent of the optional private engine test suite.
cmake_minimum_required(VERSION 3.25)
project(nox_client_tests LANGUAGES CXX)
enable_testing()

if(APPLE)
    add_executable(user_data_tests user_data_tests.cpp ../src/user_data.cpp ../../noxcore/src/io/file.cpp)
    target_compile_features(user_data_tests PRIVATE cxx_std_20)
    target_include_directories(user_data_tests PRIVATE ../src ../../noxcore/include)
    add_test(NAME mac_writable_user_data COMMAND user_data_tests)

    add_executable(game_dir_tests game_dir_tests.cpp ../../noxcore/src/io/game_dir.cpp ../../noxcore/src/io/file.cpp)
    target_compile_features(game_dir_tests PRIVATE cxx_std_20)
    target_include_directories(game_dir_tests PRIVATE ../../noxcore/include)
    add_test(NAME mac_game_data_search COMMAND game_dir_tests)
endif()

add_executable(input_config_tests input_config_tests.cpp ../src/input_config_files.cpp
    ../../noxcore/src/config/nox_config.cpp ../../noxcore/src/io/file.cpp)
target_compile_features(input_config_tests PRIVATE cxx_std_20)
target_include_directories(input_config_tests PRIVATE ../src ../../noxcore/include)
add_test(NAME input_config_sources COMMAND input_config_tests)
