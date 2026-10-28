# nox_app_icon(<target> ICON <file.ico> DESCRIPTION <text>): a program's icon.
#  - On Windows: compiled into the exe as a resource with version information (product NoX Core,
#    the project's version, DESCRIPTION as the file description). Explorer, the taskbar and
#    shortcuts show it, and SDL registers its window class with it.
#  - Everywhere: the .ico's bytes embedded as kWindowIcon / kWindowIconSize, for the window's icon
#    through SDL: the target gets noxsdl/src/icon (window_icon.hpp's setWindowIcon) compiled in
#    and on its include path.
function(nox_app_icon target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ICON;DESCRIPTION" "")
    set(cpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.cpp")
    add_custom_command(
        OUTPUT "${cpp}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${arg_ICON} -DOUTPUT=${cpp} -DSYMBOL=kWindowIcon
                -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NoxEmbedFile.cmake
        DEPENDS "${arg_ICON}" ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NoxEmbedFile.cmake
        COMMENT "Embedding ${arg_ICON}"
        VERBATIM
    )
    set(dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../noxsdl/src/icon")
    target_sources(${target} PRIVATE "${cpp}" "${dir}/window_icon.cpp")
    target_include_directories(${target} PRIVATE "${dir}")
    if(NOT WIN32)
        return()
    endif()
    get_target_property(name ${target} OUTPUT_NAME)
    if(NOT name)
        set(name ${target})
    endif()
    set(NOX_RC_ICON "${arg_ICON}")
    set(NOX_RC_DESCRIPTION "${arg_DESCRIPTION}")
    set(NOX_RC_NAME "${name}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_resources.rc")
    configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nox_resources.rc.in "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
    # (rc.exe reports no dependencies: rebuild the resources when the icon changes.)
    set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${arg_ICON}")
endfunction()
