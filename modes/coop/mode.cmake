# The co-op campaign (modes/CMakeLists.txt includes this with NOX_MODE_DIR set): its server and
# client sides in noxmodes, its UI in noxsdl (NOX_MODE_UI_SOURCES, registerModeUi_coop).
target_sources(noxmodes PRIVATE
    "${NOX_MODE_DIR}/src/coop.cpp"
    "${NOX_MODE_DIR}/src/coop_balance.cpp"
    "${NOX_MODE_DIR}/src/coop_cine.cpp"
    "${NOX_MODE_DIR}/src/coop_client.cpp"
    "${NOX_MODE_DIR}/src/coop_proto.cpp"
    "${NOX_MODE_DIR}/src/coop_party.cpp"
    "${NOX_MODE_DIR}/src/coop_saves.cpp"
    "${NOX_MODE_DIR}/src/coop_script.cpp"
    "${NOX_MODE_DIR}/coop_mode.cpp"
)
target_include_directories(noxmodes PUBLIC "${NOX_MODE_DIR}/include")
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_SOURCES "${NOX_MODE_DIR}/ui/coop_ui.cpp")
set_property(GLOBAL APPEND PROPERTY NOX_MODE_UI_IDS coop)
