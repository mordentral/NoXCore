# The game modes built in (noxmodes): each modes/<id>/ folder adds its sources (mode.cmake) and a
# registerMode_<id>(Registry&); builtin_modes.cpp, made here from NOX_MODES, registers them all
# (registerBuiltinModes, builtin.hpp). The core (noxcore) never sees this folder: hosts, the
# client and the tests link nox::modes and call registerBuiltinModes at start-up.
set(NOX_MODES "delve;tide;coop" CACHE STRING "The game modes built in (folders of modes/), ;-separated")

add_library(noxmodes)
add_library(nox::modes ALIAS noxmodes)
target_include_directories(noxmodes PUBLIC include)
target_link_libraries(noxmodes PUBLIC nox::core PRIVATE nox_warnings)

# What the modes share (common/): the map generator (mapgen: a pure library, map files from
# parameters), the records' file fields and class byte, mode_shared's helpers, and the generic
# blocks a mode's frame loops run on (the watchers, the ready timers, satellites, kept lists, a
# horde, gatherables), set up by its script.
target_sources(noxmodes PRIVATE
    common/src/mapgen/arena.cpp
    common/src/mapgen/biome.cpp
    common/src/mapgen/blocks.cpp
    common/src/mapgen/cell_set.cpp
    common/src/mapgen/gen_bindings.cpp
    common/src/mapgen/layout.cpp
    common/src/mapgen/map_writer.cpp
    common/src/mapgen/script_builder.cpp
    common/src/mapgen/writer_bindings.cpp
    common/src/server/gather.cpp
    common/src/server/horde.cpp
    common/src/server/kept.cpp
    common/src/server/ready_timers.cpp
    common/src/server/satellites.cpp
    common/src/server/watchers.cpp
)
target_include_directories(noxmodes PUBLIC common/include PRIVATE common/src/server)
# (A mod's generator builds through the map writer: script/src/mod_generate.cpp.)
target_include_directories(noxmodes PRIVATE common/src/mapgen)

# Script modes (script/): mods' folders run in sandboxed Luau VMs, whatever modes are built in.
target_sources(noxmodes PRIVATE
    script/src/gen_runtime.cpp
    script/src/json.cpp
    script/src/luau_vm.cpp
    script/src/manifest.cpp
    script/src/mod_assets.cpp
    script/src/mod_draw_scale.cpp
    script/src/mod_events.cpp
    script/src/mod_generate.cpp
    script/src/mod_items.cpp
    script/src/mod_parts.cpp
    script/src/mod_records.cpp
    script/src/mod_scheduler.cpp
    script/src/mod_store.cpp
    script/src/mod_weather.cpp
    script/src/mods.cpp
    script/src/script_data.cpp
    script/src/script_ui.cpp
    script/src/scripted_mode.cpp
    script/src/ui_preview.cpp
    script/src/value_codec.cpp
)
target_link_libraries(noxmodes PRIVATE Luau.VM Luau.Compiler)
# (A mod's PNGs, decoded on the host: mod_assets.cpp.)
target_include_directories(noxmodes SYSTEM PRIVATE ${stb_image_SOURCE_DIR})

# A built-in mode's Luau sources (its folder's script/<path>), built into the program
# (builtin_scripts.hpp): a mode.cmake names each with nox_mode_script(<path>).
set(NOX_MODE_SCRIPT_LINES "")
set(NOX_MODE_SCRIPT_FILES "")
macro(nox_mode_script path)
    string(APPEND NOX_MODE_SCRIPT_LINES "${mode}|${path}|${NOX_MODE_DIR}/script/${path}\n")
    list(APPEND NOX_MODE_SCRIPT_FILES "${NOX_MODE_DIR}/script/${path}")
endmacro()

# The map generator's design half (common/script/mapgen),
# built in under the id "mapgen" for both modes, run in its own VM (gen_runtime.hpp).
set(mode mapgen)
set(NOX_MODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")
nox_mode_script(mapgen/util.luau)
nox_mode_script(mapgen/biomes.luau)
nox_mode_script(mapgen/layout.luau)
nox_mode_script(mapgen/features.luau)
nox_mode_script(mapgen/floors.luau)
nox_mode_script(mapgen/arena.luau)

# The server toolbox's and the UI API's declarations (script/defs: Luau definition files with a
# line of doc each), built in under the id "defs" for the map editor's diagnostics and its Mod
# Reference; never run.
set(mode defs)
set(NOX_MODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
nox_mode_script(defs/server.d.luau)
nox_mode_script(defs/ui.d.luau)

set(NOX_MODE_DECLS "")
set(NOX_MODE_CALLS "")
foreach(mode IN LISTS NOX_MODES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${mode}/mode.cmake")
        message(FATAL_ERROR "NOX_MODES names \"${mode}\", but modes/${mode}/mode.cmake isn't there")
    endif()
    set(NOX_MODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${mode}")
    include("${mode}/mode.cmake")
    string(APPEND NOX_MODE_DECLS "void registerMode_${mode}(Registry& registry);\n")
    string(APPEND NOX_MODE_CALLS "        registerMode_${mode}(r);\n")
endforeach()

file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin_modes.cpp" CONTENT [[
// Made by modes/CMakeLists.txt from NOX_MODES ("@NOX_MODES@"): don't edit.
#include "noxcore/modes/builtin.hpp"

#include "noxcore/modes/registry.hpp"

#include <mutex>

namespace nox::modes {

@NOX_MODE_DECLS@
void registerBuiltinModes()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Registry& r = registry();
@NOX_MODE_CALLS@    });
}

} // namespace nox::modes
]] @ONLY)
target_sources(noxmodes PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_modes.cpp")

file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin_scripts.list" CONTENT "${NOX_MODE_SCRIPT_LINES}")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin_scripts.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DLIST=${CMAKE_CURRENT_BINARY_DIR}/builtin_scripts.list"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/builtin_scripts.cpp" -P "${CMAKE_CURRENT_SOURCE_DIR}/script/embed_scripts.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/script/embed_scripts.cmake" "${CMAKE_CURRENT_BINARY_DIR}/builtin_scripts.list"
            ${NOX_MODE_SCRIPT_FILES}
    COMMENT "Building the built-in modes' scripts in"
    VERBATIM)
target_sources(noxmodes PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_scripts.cpp")
