# Undead Tide (modes/CMakeLists.txt includes this with NOX_MODE_DIR set): its server and client
# sides in noxmodes, its UI in noxsdl (NOX_MODE_UI_SOURCES, registerModeUi_tide).
target_sources(noxmodes PRIVATE
    "${NOX_MODE_DIR}/src/tide.cpp"
    "${NOX_MODE_DIR}/src/tide_data.cpp"
    "${NOX_MODE_DIR}/src/tide_host.cpp"
    "${NOX_MODE_DIR}/src/tide_records.cpp"
    "${NOX_MODE_DIR}/tide_mode.cpp"
    "${NOX_MODE_DIR}/tide_client.cpp"
)
target_include_directories(noxmodes PUBLIC "${NOX_MODE_DIR}/include")
# Its scripts, built in (script/...): its tables, read at set-up (src/tide_data.hpp), and its rules
# (main.luau, the director's own script mode).
nox_mode_script(server/data.luau)
nox_mode_script(server/main.luau)
nox_mode_script(server/rules.luau)
nox_mode_script(server/pickups.luau)
nox_mode_script(server/spawns.luau)
nox_mode_script(server/levels.luau)
nox_mode_script(server/run.luau)
nox_mode_script(server/util.luau)
nox_mode_script(server/shrines.luau)
nox_mode_script(server/chat.luau)
nox_mode_script(server/weapons.luau)
nox_mode_script(server/fall.luau)
nox_mode_script(server/lobby.luau)
nox_mode_script(server/told.luau)
nox_mode_script(server/arena.luau)
nox_mode_script(server/params.luau)
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_SOURCES
    "${NOX_MODE_DIR}/ui/tide_window.cpp"
    "${NOX_MODE_DIR}/ui/tide_ui.cpp"
)
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_INCLUDES "${NOX_MODE_DIR}/ui")
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_IDS tide)
