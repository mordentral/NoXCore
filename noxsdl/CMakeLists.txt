# The map editor's panels (src/editor): Dear ImGui's docking branch (MIT), pinned by tag and hash.
# Fetched under its own name: the image editor (videotools) uses the release branch.
include(FetchContent)
FetchContent_Declare(imgui_docking
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90DED916BD57DB2E0E171B6B098940A47C6F5042725DCDC67FB19940CA8BFDCC
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui_docking)
add_library(noxsdl_imgui STATIC
    ${imgui_docking_SOURCE_DIR}/imgui.cpp
    ${imgui_docking_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_docking_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_docking_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_docking_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_docking_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(noxsdl_imgui SYSTEM PUBLIC ${imgui_docking_SOURCE_DIR} ${imgui_docking_SOURCE_DIR}/backends)
target_link_libraries(noxsdl_imgui PUBLIC SDL3::SDL3)
# (No nox_warnings: Dear ImGui's sources compile in this target.)

# The editors' ImGui host: Dear ImGui on an SDL renderer with the Dark / Medium / Light themes, the
# docked layout and the floating panels' windows, the embedded font. The map editor's, and the
# NoX Content Editor's (videotools), so both look and behave the same.
add_library(noxsdl_editor_host STATIC
    src/editor/imgui_host.cpp
    src/editor/panel_textures.cpp
    src/editor/texture_mirror.cpp
    src/editor/theme.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/embedded_font.cpp
)
target_include_directories(noxsdl_editor_host PUBLIC src) # (its headers: "editor/imgui_host.hpp" ...)
target_link_libraries(noxsdl_editor_host PUBLIC noxsdl_imgui nox::core SDL3::SDL3 PRIVATE nox_warnings)

# noxsdl: thin 2D front end on top of noxcore. A GUI program on Windows (WIN32: SDL_main.h gives
# it its WinMain), so no console window opens; -cmd gives the log one (src/log_output.hpp).
add_executable(noxsdl WIN32
    src/app.cpp
    src/app_settings.cpp
    src/audio.cpp
    src/bloom.cpp
    src/browser_screen.cpp
    src/centred_messages.cpp
    src/chat_entry.cpp
    src/cinematics.cpp
    src/command_line.cpp
    src/console.cpp
    src/console_commands.cpp
    src/cursor.cpp
    src/editor/builtin_list.cpp
    src/editor/chat_log.cpp
    src/editor/chat_panel.cpp
    src/editor/code_editor.cpp
    src/editor/code_lexer.cpp
    src/editor/commands.cpp
    src/editor/content_browser.cpp
    src/editor/editor_settings.cpp
    src/editor/map_editor.cpp
    src/editor/map_editor_tools.cpp
    src/editor/map_editor_ui.cpp
    src/editor/map_editor_view.cpp
    src/editor/marker_tools.cpp
    src/editor/mod_analysis.cpp
    src/editor/mod_defs.cpp
    src/editor/mod_panel.cpp
    src/editor/mod_project.cpp
    src/editor/object_list.cpp
    src/editor/object_tools.cpp
    src/editor/ordered_json.cpp
    src/editor/play_view.cpp
    src/editor/pref_dir.cpp
    src/editor/property_panel.cpp
    src/editor/script_panel.cpp
    src/editor/session_panel.cpp
    src/editor/session_setup.cpp
    src/editor/thing_thumbnails.cpp
    src/effects.cpp
    src/effects_beams.cpp
    src/effects_enchant.cpp
    src/effects_rain.cpp
    src/effects_objects.cpp
    src/effects_puffs.cpp
    src/game_session.cpp
    src/game_windows.cpp
    src/gpu_atlas.cpp
    src/gpu_shader_code.cpp
    src/gpu_shaders.cpp
    src/gui.cpp
    src/hd_video.cpp
    src/hosting.cpp
    src/bottle_slots.cpp
    src/hud.cpp
    src/interpolator.cpp
    src/move_predictor.cpp
    src/input.cpp
    src/input_config_window.cpp
    src/inventory.cpp
    src/inventory_grid.cpp
    src/item_names.cpp
    src/summon_window.cpp
    src/trade_screens.cpp
    src/light_cone.cpp
    src/lighting.cpp
    src/legal_screen.cpp
    src/live_scene.cpp
    src/live_session.cpp
    src/local_host.cpp
    src/log_output.cpp
    src/main.cpp
    src/map_download.cpp
    src/map_view.cpp
    src/map_preload.cpp
    src/image_decoder.cpp
    src/menus.cpp
    src/mode_ui.cpp
    src/mods_folder.cpp
    src/menus_modes.cpp
    src/mode_list.cpp
    src/script_mode_ui.cpp
    src/menus_wolchat.cpp
    src/options_window.cpp
    src/overlays.cpp
    src/minimap.cpp
    src/map_thumbnail.cpp
    src/map_vote_window.cpp
    src/motd_window.cpp
    src/discon_window.cpp
    src/movie_player.cpp
    src/local_bots.cpp
    src/net_stat.cpp
    src/player_art.cpp
    src/port_mapper.cpp
    src/quest_windows.cpp
    src/quickbar.cpp
    src/quit_menu.cpp
    src/rank_board.cpp
    src/screen_particles.cpp
    src/server_options.cpp
    src/server_options_advanced.cpp
    src/server_options_server.cpp
    src/sight.cpp
    src/spell_chant.cpp
    src/spellbook.cpp
    src/spell_limits.cpp
    src/sprite_cache.cpp
    src/stb_truetype.cpp
    src/text.cpp
    src/test_drive.cpp
    src/thing_art.cpp
    src/view3d.cpp
    src/view3d_gpu.cpp
    src/weather.cpp
)
# The program is noxcore.exe (Linux: noxcore); the target keeps its name, noxsdl, for the build
# commands. No clash with the library target noxcore: its noxcore.lib / .a is built in noxcore/,
# not beside the exe, and its compile PDB stays in its own CMakeFiles folder.
set_target_properties(noxsdl PROPERTIES OUTPUT_NAME noxcore)

# The icon (res/noxcore.ico, made from res/noxcore.png; committed, so the
# build needs no converter): the exe's resource and version information on Windows, and embedded
# for the window's icon (src/icon/window_icon.hpp).
nox_app_icon(noxsdl ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/noxcore.ico" DESCRIPTION "NoX Core")

# The fallback font, compiled in so the exe needs no data files of its own.
set(NOXSDL_FONT_FILE "${noto_sans_SOURCE_DIR}/NotoSans-SemiBold.ttf")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded_font.cpp
    COMMAND ${CMAKE_COMMAND} -DINPUT=${NOXSDL_FONT_FILE} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded_font.cpp
            -DSYMBOL=kEmbeddedFont -P ${PROJECT_SOURCE_DIR}/cmake/NoxEmbedFile.cmake
    DEPENDS ${NOXSDL_FONT_FILE} ${PROJECT_SOURCE_DIR}/cmake/NoxEmbedFile.cmake
    COMMENT "Embedding ${NOXSDL_FONT_FILE}"
    VERBATIM
)
# Ship the font's licence beside the exe.
add_custom_command(TARGET noxsdl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${noto_sans_license_SOURCE_DIR}/OFL.txt
            $<TARGET_FILE_DIR:noxsdl>/NotoSans-OFL.txt
    VERBATIM
)
# The mods in mods/ (script modes) into the mods folder beside the exe, where a host finds them.
# (Copied over each build: a mod edited there is replaced.)
add_custom_command(TARGET noxsdl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/mods $<TARGET_FILE_DIR:noxsdl>/mods
    VERBATIM
)

# Shaders for the GPU renderer (--gpu), from the same HLSL (shaders/), embedded in the exe
# (src/gpu_shader_code.hpp lists them): DXIL for Direct3D 12 with dxc, the Windows SDK's; SPIR-V
# for Vulkan (SDL's GPU API on Linux) with glslang. The device takes whichever it can at run
# time. Without either compiler the GPU renderer still runs and recolours sprites on the CPU.
set(NOXSDL_SHADERS
    material.frag material_split.frag fx_disc.frag fx_rect.frag fx_glow.frag fx_glowline.frag fx_puff.frag
    fx_barrier.frag bloom_blur.frag bloom_bright.frag fx_light.frag gamma.frag
    # A map editor session's pending edits.
    pending.frag
    # nox.weather's rain, snow, storm and wind (weather.cpp).
    weather.frag
    view3d_scene.frag view3d_material.frag view3d_particle.frag
    # The 3D view's vertex shader (view3d_gpu.cpp draws with its own pipelines, not SDL's).
    view3d_scene.vert)

# shaders/<shader>.hlsl compiled to <format> (dxil, spv) by `compiler` and embedded as
# kShader_<name>_<format>: <name> is a fragment shader's own name, view3d_scene_vert the vertex
# shader's.
function(noxsdl_add_shader shader format)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl)
    set(bin ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.${format})
    string(REPLACE "." "_" stem "${shader}")
    set(cpp ${CMAKE_CURRENT_BINARY_DIR}/shaders/${stem}_${format}.cpp)
    string(REGEX REPLACE "\\.frag$" "" name "${shader}")
    string(REPLACE "." "_" name "${name}")
    if(shader MATCHES "\\.vert$")
        set(dxil_profile vs_6_0)
        set(glslang_stage vert)
    else()
        set(dxil_profile ps_6_0)
        set(glslang_stage frag)
    endif()
    if(format STREQUAL "dxil")
        set(compile ${NOXSDL_DXC} -nologo -T ${dxil_profile} -E main -Fo ${bin} ${src})
    else()
        # HLSL in (-D), Vulkan SPIR-V out (-V). The registers' spaces are already SDL_GPU's
        # descriptor sets (fragment textures space2, uniforms space3), and the stage inputs and
        # outputs take locations in declaration order (TEXCOORD0 colour 0, TEXCOORD1 texture
        # coordinate 1, as SDL's own vertex shader's outputs are numbered).
        set(compile ${NOXSDL_GLSLANG} -D -V -S ${glslang_stage} -e main --auto-map-locations --quiet -o ${bin} ${src})
    endif()
    add_custom_command(
        OUTPUT ${bin}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${compile}
        # (pending_common.hlsli: included by pending.frag and the material shaders)
        DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pending_common.hlsli
        COMMENT "Compiling ${shader}.hlsl"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${cpp}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${bin} -DOUTPUT=${cpp} -DSYMBOL=kShader_${name}_${format}
                -P ${PROJECT_SOURCE_DIR}/cmake/NoxEmbedFile.cmake
        DEPENDS ${bin} ${PROJECT_SOURCE_DIR}/cmake/NoxEmbedFile.cmake
        VERBATIM
    )
    target_sources(noxsdl PRIVATE ${cpp})
endfunction()

find_program(NOXSDL_DXC dxc)
if(NOT NOXSDL_DXC AND WIN32)
    file(GLOB NOXSDL_DXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
    if(NOXSDL_DXC_CANDIDATES)
        list(SORT NOXSDL_DXC_CANDIDATES)
        list(GET NOXSDL_DXC_CANDIDATES -1 NOXSDL_DXC)
    endif()
endif()
if(NOXSDL_DXC)
    message(STATUS "noxsdl shaders: DXIL with ${NOXSDL_DXC}")
    foreach(shader ${NOXSDL_SHADERS})
        noxsdl_add_shader(${shader} dxil)
    endforeach()
    target_compile_definitions(noxsdl PRIVATE NOXSDL_HAVE_DXIL=1)
endif()

find_program(NOXSDL_GLSLANG NAMES glslangValidator glslang)
if(NOXSDL_GLSLANG)
    message(STATUS "noxsdl shaders: SPIR-V with ${NOXSDL_GLSLANG}")
    foreach(shader ${NOXSDL_SHADERS})
        noxsdl_add_shader(${shader} spv)
    endforeach()
    target_compile_definitions(noxsdl PRIVATE NOXSDL_HAVE_SPIRV=1)
endif()

if(NOT NOXSDL_DXC AND NOT NOXSDL_GLSLANG)
    message(STATUS "noxsdl shaders: neither dxc nor glslang found; the GPU renderer will shade sprites on the CPU")
endif()

target_include_directories(noxsdl SYSTEM PRIVATE ${stb_truetype_SOURCE_DIR})
target_include_directories(noxsdl PRIVATE src) # (src/editor's files include "editor/..." and the views beside them)
target_link_libraries(noxsdl PRIVATE nox::core nox::modes SDL3::SDL3 nox_warnings miniupnpc::miniupnpc noxsdl_imgui noxsdl_editor_host)
# Luau's analysis library: the mod editor's diagnostics (src/editor/mod_analysis.cpp). noxsdl's
# alone: the hosts, the server and the modes library never link it.
target_link_libraries(noxsdl PRIVATE Luau.Analysis)
# The built-in modes' UIs (modes/<id>/mode.cmake: NOX_MODE_UI_SOURCES, NOX_MODE_UI_IDS), compiled
# here with noxsdl's own headers, registered by builtin_mode_uis.cpp (mode_ui.hpp).
get_property(NOX_MODE_UI_SOURCES GLOBAL PROPERTY NOX_MODE_UI_SOURCES)
get_property(NOX_MODE_UI_IDS GLOBAL PROPERTY NOX_MODE_UI_IDS)
get_property(NOX_MODE_UI_INCLUDES GLOBAL PROPERTY NOX_MODE_UI_INCLUDES)
target_sources(noxsdl PRIVATE ${NOX_MODE_UI_SOURCES})
target_include_directories(noxsdl PRIVATE ${NOX_MODE_UI_INCLUDES})
set(NOX_MODE_UI_DECLS "")
set(NOX_MODE_UI_CALLS "")
foreach(mode IN LISTS NOX_MODE_UI_IDS)
    string(APPEND NOX_MODE_UI_DECLS "void registerModeUi_${mode}(std::vector<ModeUiEntry>& entries);\n")
    string(APPEND NOX_MODE_UI_CALLS "        registerModeUi_${mode}(entries);\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin_mode_uis.cpp" CONTENT [[
// Made by noxsdl/CMakeLists.txt from the built-in modes' UIs: don't edit.
#include "mode_ui.hpp"

#include <mutex>

namespace noxsdl {

@NOX_MODE_UI_DECLS@
void registerBuiltinModeUis()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<ModeUiEntry>& entries = modeUiEntries();
@NOX_MODE_UI_CALLS@    });
}

} // namespace noxsdl
]] @ONLY)
target_sources(noxsdl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_mode_uis.cpp")
# The game folder's last resort, after the program's own folder and the installs found
# (io/game_dir.hpp): empty, the default, means none.
target_compile_definitions(noxsdl PRIVATE NOXSDL_DEFAULT_GAME_DIR="${NOX_GAME_DIR}")

# Private developer tools built in where the source tree has them (a public tree has none): the
# NOX_PRIVATE_TOOLS hooks in main.cpp and map_editor.cpp. Off by default, and never in a release
# package (NOX_TEST_COMMANDS=OFF).
option(NOX_PRIVATE_TOOLS "Build private developer tools into noxsdl, where present (OFF for release packages)" OFF)
if(NOX_PRIVATE_TOOLS AND NOX_TEST_COMMANDS AND EXISTS "${PROJECT_SOURCE_DIR}/tools/noxsdl_private.cmake")
    include("${PROJECT_SOURCE_DIR}/tools/noxsdl_private.cmake")
else()
    target_compile_definitions(noxsdl PRIVATE NOX_PRIVATE_TOOLS=0)
endif()

# The HD movies through FFmpeg's DLLs (hd_video.cpp), delay-loaded: noxsdl starts and plays the
# VQAs without them. The DLLs' names come from the build fetched (NoxDependencies.cmake), in the
# order they depend on each other.
if(NOXSDL_HD_VIDEO AND WIN32 AND MSVC AND ffmpeg_lgpl_SOURCE_DIR)
    set(NOXSDL_FFMPEG_DLLS "")
    foreach(lib avutil swresample swscale avcodec avformat)
        file(GLOB dll "${ffmpeg_lgpl_SOURCE_DIR}/bin/${lib}-*.dll")
        list(LENGTH dll n)
        if(NOT n EQUAL 1)
            message(FATAL_ERROR "FFmpeg build: no single ${lib}-*.dll in ${ffmpeg_lgpl_SOURCE_DIR}/bin")
        endif()
        get_filename_component(name "${dll}" NAME)
        list(APPEND NOXSDL_FFMPEG_DLL_NAMES "${name}")
        string(APPEND NOXSDL_FFMPEG_DLLS "\"${name}\",")
        # The build's own .lib files are MinGW's (dlltool), which MSVC's linker can't delay-load:
        # import libraries made again from its .def files with lib.exe.
        file(GLOB def "${ffmpeg_lgpl_SOURCE_DIR}/lib/${lib}-*.def")
        set(implib "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_lib/${lib}.lib")
        if(NOT EXISTS "${implib}" OR "${def}" IS_NEWER_THAN "${implib}")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_lib")
            execute_process(COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${def}" "/name:${name}" "/out:${implib}"
                            RESULT_VARIABLE rc OUTPUT_QUIET)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "lib.exe couldn't make ${implib} from ${def}")
            endif()
        endif()
        target_link_libraries(noxsdl PRIVATE "${implib}")
        target_link_options(noxsdl PRIVATE "/DELAYLOAD:${name}")
    endforeach()
    message(STATUS "noxsdl HD video: FFmpeg ${NOXSDL_FFMPEG_DLL_NAMES} (delay-loaded from HDVideo)")
    target_include_directories(noxsdl SYSTEM PRIVATE "${ffmpeg_lgpl_SOURCE_DIR}/include")
    target_link_libraries(noxsdl PRIVATE delayimp)
    target_compile_definitions(noxsdl PRIVATE NOXSDL_HD_VIDEO=1 "NOXSDL_FFMPEG_DLLS=${NOXSDL_FFMPEG_DLLS}")
else()
    target_compile_definitions(noxsdl PRIVATE NOXSDL_HD_VIDEO=0)
endif()
