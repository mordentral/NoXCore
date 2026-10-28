# videotools: noxcontent, the NoX Content Editor GUI (every game file: thing.bin, the text tables,
# strings, sounds, video.bag and image packs...; SDL3 + Dear ImGui's docking branch through
# noxsdl's editor host, the map editor's), on videotools_core and videotools_content.
include(FetchContent)

# PNG output with real deflate compression (noxcore's encoder only stores).
FetchContent_Declare(stb_image_write
    URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image_write.h
    URL_HASH SHA256=CBD5F0AD7A9CF4468AFFB36354A1D2338034F2C12473CF1A8E32053CB6914A05
    DOWNLOAD_NO_EXTRACT TRUE
)
# Reading PNGs back and JSON (image sidecars; the remaster pipeline's worker replies and workflows).
FetchContent_Declare(stb_image
    URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
    URL_HASH SHA256=594C2FE35D49488B4382DBFAEC8F98366DEFCA819D916AC95BECF3E75F4200B3
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Declare(nlohmann_json_header
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
    URL_HASH SHA256=AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63
    DOWNLOAD_NO_EXTRACT TRUE
)
# BC7 encoding for the image pack (bc7enc_rdo: MIT or public domain; only its MIT files are built,
# not bc7e.ispc; LodePNG, which its utils use, is zlib). Tools only: noxsdl decodes with bcdec.
# SOURCE_SUBDIR points nowhere, so its own CMakeLists (an executable) isn't added.
FetchContent_Declare(bc7enc_rdo
    URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
    URL_HASH SHA256=BBB33D1DBB6178A3A2C044956C3CC0B8EA9FC2F2903824379DD86F87D53B64C6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(stb_image_write stb_image nlohmann_json_header bc7enc_rdo)
add_library(videotools_bc7enc STATIC
    ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp
    ${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp
    ${bc7enc_rdo_SOURCE_DIR}/bc7decomp_ref.cpp
    ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp
    ${bc7enc_rdo_SOURCE_DIR}/utils.cpp
    ${bc7enc_rdo_SOURCE_DIR}/ert.cpp
    ${bc7enc_rdo_SOURCE_DIR}/rdo_bc_encoder.cpp
    ${bc7enc_rdo_SOURCE_DIR}/lodepng.cpp
)
target_include_directories(videotools_bc7enc SYSTEM PUBLIC ${bc7enc_rdo_SOURCE_DIR})
target_compile_definitions(videotools_bc7enc PUBLIC SUPPORT_BC7E=0)
if(MSVC)
    target_compile_options(videotools_bc7enc PRIVATE /W0 /wd4068)
else()
    # (ert.h uses uint8_t / uint32_t without <cstdint>, which MSVC's headers bring in anyway)
    target_compile_options(videotools_bc7enc PRIVATE -w -include cstdint)
endif()
# json.hpp is included as <nlohmann/json.hpp>.
set(VIDEOTOOLS_JSON_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/json_include)
file(COPY ${nlohmann_json_header_SOURCE_DIR}/json.hpp DESTINATION ${VIDEOTOOLS_JSON_INCLUDE}/nlohmann)

# videotools_core: what the editor GUI needs (the catalog and its export, video.bag and image pack
# editing, PNG and BC7). No AI tools, no network, no accounts: it can go out with the GUI.
add_library(videotools_core STATIC
    src/catalog.cpp
    src/exporter.cpp
    src/image_source.cpp
    src/png_write.cpp
    src/common/bc7_encode.cpp
    src/common/image.cpp
    src/common/registration.cpp
    src/editor/documents.cpp
    src/editor/image_files.cpp
    src/editor/insert.cpp
)
target_include_directories(videotools_core PUBLIC src)
target_include_directories(videotools_core SYSTEM PUBLIC ${VIDEOTOOLS_JSON_INCLUDE})
target_include_directories(videotools_core SYSTEM PRIVATE ${stb_image_write_SOURCE_DIR} ${stb_image_SOURCE_DIR})
target_link_libraries(videotools_core PUBLIC nox::core PRIVATE nox_warnings videotools_bc7enc)
# (NOX_GAME_DIR empty, the default: no built-in game folder, export or ai folder; image_source.hpp
# defaultGameDir looks for the installed game instead.)
target_compile_definitions(videotools_core PUBLIC NOXVIDEO_DEFAULT_GAME_DIR="${NOX_GAME_DIR}")

# videotools_content: the game's files as the editor sees them (the content browser's records,
# their fields, where each file comes from); no UI, so its tests run without a window.
add_library(videotools_content STATIC
    src/content/character.cpp
    src/content/font_palette.cpp
    src/content/game_files.cpp
    src/content/map_usage.cpp
    src/content/records.cpp
    src/content/pack_edit.cpp
    src/content/schema.cpp
    src/content/text_files.cpp
)
target_include_directories(videotools_content PUBLIC src)
target_link_libraries(videotools_content PUBLIC nox::core PRIVATE nox::modes nox_warnings)

# noxcontent: the NoX Content Editor GUI, shipped beside noxcore.exe. Links videotools_core,
# videotools_content and noxsdl's editor host (Dear ImGui's docking branch, the themes, the
# floating panels), never the remaster pipeline.
add_executable(noxcontent WIN32
    src/gui/app_main.cpp
    src/gui/audio_player.cpp
    src/gui/bottom_panels.cpp
    src/gui/catalog_view.cpp
    src/gui/character_form.cpp
    src/gui/compare_view.cpp
    src/gui/content_browser.cpp
    src/gui/font_view.cpp
    src/gui/images_view.cpp
    src/gui/mod_form.cpp
    src/gui/preview_panel.cpp
    src/gui/record_panel.cpp
    src/gui/session.cpp
    src/gui/shell.cpp
    src/gui/table_form.cpp
    src/gui/text_form.cpp
    src/gui/textures.cpp
)
target_link_libraries(noxcontent PRIVATE videotools_core videotools_content nox::modes noxsdl_editor_host SDL3::SDL3 nox_warnings)
# Its icon (res/noxcontent.ico, made from res/noxcontent.png; committed): the exe's resource and
# version information on Windows, and the window's (app_main.cpp).
nox_app_icon(noxcontent ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/noxcontent.ico" DESCRIPTION "NoX Content Editor")
