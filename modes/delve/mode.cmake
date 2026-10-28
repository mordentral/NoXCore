# Delving Mode (modes/CMakeLists.txt includes this with NOX_MODE_DIR set): its server and client
# sides in noxmodes, its UI in noxsdl (NOX_MODE_UI_SOURCES, registerModeUi_delve).
target_sources(noxmodes PRIVATE
    "${NOX_MODE_DIR}/src/delve.cpp"
    "${NOX_MODE_DIR}/src/delve_data.cpp"
    "${NOX_MODE_DIR}/src/delve_host.cpp"
    "${NOX_MODE_DIR}/src/delve_map.cpp"
    "${NOX_MODE_DIR}/src/delve_records.cpp"
    "${NOX_MODE_DIR}/delve_mode.cpp"
    "${NOX_MODE_DIR}/delve_client.cpp"
)
target_include_directories(noxmodes PUBLIC "${NOX_MODE_DIR}/include")
# Its scripts, built in (script/...): its tables, read at set-up (src/delve_data.hpp), and its
# rules (main.luau, the director's own script mode).
nox_mode_script(server/data.luau)
nox_mode_script(server/main.luau)
nox_mode_script(server/rules.luau)
nox_mode_script(server/acts.luau)
nox_mode_script(server/levels.luau)
nox_mode_script(server/draws.luau)
nox_mode_script(server/commands.luau)
nox_mode_script(server/run.luau)
nox_mode_script(server/words.luau)
nox_mode_script(server/lobby.luau)
nox_mode_script(server/told.luau)
nox_mode_script(server/level.luau)
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_SOURCES
    "${NOX_MODE_DIR}/ui/delve_map_window.cpp"
    "${NOX_MODE_DIR}/ui/campfire_window.cpp"
    "${NOX_MODE_DIR}/ui/blessing_window.cpp"
    "${NOX_MODE_DIR}/ui/delve_ui.cpp"
)
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_INCLUDES "${NOX_MODE_DIR}/ui")
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_IDS delve)
