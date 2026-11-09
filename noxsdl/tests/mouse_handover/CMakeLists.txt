# Standalone mouse policy regression checks; no SDL, game assets or engine test commands needed.
cmake_minimum_required(VERSION 3.25)
project(nox_mouse_tests LANGUAGES CXX)
enable_testing()
add_executable(mouse_handover_tests mouse_handover_tests.cpp)
target_compile_features(mouse_handover_tests PRIVATE cxx_std_20)
target_include_directories(mouse_handover_tests PRIVATE ../../src)
add_test(NAME mouse_handover COMMAND mouse_handover_tests)
