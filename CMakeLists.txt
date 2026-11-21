cmake_minimum_required(VERSION 3.25)

project(noxcpp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NOX_BUILD_SDL "Build the SDL front end (noxsdl)" ON)
# The unit tests and the test commands below default to whether this tree has the tests: a tree
# with tests/ builds both, one without (the published source) builds neither. -D values win.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    set(NOX_DEV_DEFAULT ON)
else()
    set(NOX_DEV_DEFAULT OFF)
endif()
option(NOX_BUILD_TESTS "Build the unit tests (default: ON when tests/ is present)" ${NOX_DEV_DEFAULT})
option(NOX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in our targets" OFF)
# noxcpp's "nx" test commands (spawn, give, god...) on a host's console: for development and the
# tests only. Release packages build with it OFF, so a player hosting a game can't use them to
# cheat; the code is left out of the program, not just switched off.
option(NOX_TEST_COMMANDS "Build noxcpp's 'nx' test commands and noxsdl's view debug switches (default: ON when tests/ is present; OFF for release packages)" ${NOX_DEV_DEFAULT})
if(NOT NOX_TEST_COMMANDS)
    add_compile_definitions(NOX_NO_TEST_COMMANDS)
    if(NOX_BUILD_TESTS)
        message(FATAL_ERROR "NOX_TEST_COMMANDS=OFF needs NOX_BUILD_TESTS=OFF (the tests use the nx commands)")
    endif()
endif()

# Real game data used as test fixtures (tests that need a missing file are skipped, not failed),
# and the programs' last-resort game folder. Empty by default: none (no fixtures, no last resort).
# A developer's own builds set them on the command line or in an untracked CMakeUserPresets.json.
set(NOX_GAME_DIR "" CACHE PATH "Nox install for the test fixtures and the programs' last resort: maps, thing.bin, video.bag, audio (empty: none)")
set(NOX_SAVE_DIR "" CACHE PATH "Save directory with .plr and saved-map fixtures (empty: none)")
# A mod library kept outside this tree: a folder with mods/<id>/ (copied into the mods folder beside
# noxsdl, as mods/ is) and, optionally, tests/modes/*.cpp (built into noxcore_tests when the tests
# are). Empty by default: none, and the build is as without it.
set(NOX_MODS_LIBRARY "" CACHE PATH "Mod library outside this tree: its mods/ copied beside noxsdl, its tests/modes/*.cpp in noxcore_tests, its tools/<name>/*.cpp as executables beside the tests (empty: none)")
set(NOX_MODS_LIBRARY_DIR "")
if(NOX_MODS_LIBRARY)
    get_filename_component(nox_mods_library_abs "${NOX_MODS_LIBRARY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(TO_CMAKE_PATH "${nox_mods_library_abs}" nox_mods_library_abs)
    if(IS_DIRECTORY "${nox_mods_library_abs}/mods")
        set(NOX_MODS_LIBRARY_DIR "${nox_mods_library_abs}")
        message(STATUS "Mod library: ${NOX_MODS_LIBRARY_DIR}")
    else()
        message(WARNING "NOX_MODS_LIBRARY (${NOX_MODS_LIBRARY}) has no mods/ folder: no mod library used")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NoxWarnings)
# Development harnesses, when the tree has them (the published source has none): before the
# dependencies, so a harness build compiles them with the same sanitizers.
set(NOX_HAS_FUZZ OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
    set(NOX_HAS_FUZZ ON)
    include(NoxFuzzing)
endif()
include(NoxDependencies)
include(NoxWindowsResources)

option(NOX_BUILD_PUBLIC_CHECKS "Build portable regression checks for the public source tree" OFF)
if(NOX_BUILD_PUBLIC_CHECKS)
    enable_testing()
endif()

add_subdirectory(noxcore)
add_subdirectory(modes) # (the game modes built in: hosts and the client link nox::modes)

# Development tools, built when the tree has them (the published source has none).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

if(NOX_BUILD_SDL)
    add_subdirectory(noxsdl)
endif()

if(NOX_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

# The translations' check (noxsdl/l10n/l10n.py check: every language current with the English, no
# new player-facing English outside NOX_TR), run by ctest here and in the public build.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/noxsdl/l10n/l10n.py")
    enable_testing()
    add_test(NAME l10n_check COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/noxsdl/l10n/l10n.py" check)
endif()

if(NOX_HAS_FUZZ AND NOX_BUILD_FUZZERS)
    enable_testing()
    add_subdirectory(fuzz)
endif()

# The NoX Content Editor (noxcontent, in videotools/): an editor for the game's files; needs SDL
# (and noxsdl's ImGui host, which it shares with the map editor).
option(NOX_BUILD_VIDEOTOOLS "Build the NoX Content Editor (videotools)" ON)
if(NOX_BUILD_SDL AND NOX_BUILD_VIDEOTOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/videotools/CMakeLists.txt")
    add_subdirectory(videotools)
endif()

