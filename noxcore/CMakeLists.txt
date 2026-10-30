# noxcore: engine-agnostic library (formats, crypto, protocol, client state, later the server sim).
# It must never depend on SDL or any other front-end library.
add_library(noxcore
    src/version.cpp
    src/audio/audio_bag.cpp
    src/audio/dr_mp3.cpp
    src/audio/mixer.cpp
    src/audio/music.cpp
    src/audio/pcm.cpp
    src/audio/sound_engine.cpp
    src/audio/sound_events.cpp
    src/audio/sound_names.cpp
    src/bot/bot.cpp
    src/bot/bot_host.cpp
    src/bot/bot_squad.cpp
    src/bot/banter.cpp
    src/bot/personas.cpp
    src/bot/emulated_host.cpp
    src/bot/net_bot.cpp
    src/bot/player_brain.cpp
    src/bot/warrior_brain.cpp
    src/bot/caster_brain.cpp
    src/client/headless_client.cpp
    src/client/edit_session_client.cpp
    src/content/audio_writer.cpp
    src/content/content_pack.cpp
    src/content/csf_document.cpp
    src/content/text_table.cpp
    src/content/thing_patch.cpp
    src/content/thing_writer.cpp
    src/client/inform_text.cpp
    src/client/movies.cpp
    src/client/navigation.cpp
    src/client/sight.cpp
    src/client/solo_menu.cpp
    src/client/solo_saves.cpp
    src/client/spell_bars.cpp
    src/client/world.cpp
    src/config/nox_config.cpp
    src/crypto/blowfish.cpp
    src/crypto/blowfish_tables.cpp
    src/crypto/crc.cpp
    src/crypto/noxcrypt.cpp
    src/crypto/sha256.cpp
    src/io/byte_reader.cpp
    src/io/file.cpp
    src/io/game_dir.cpp
    src/io/png.cpp
    src/io/wol_login.cpp
    src/io/zip.cpp
    src/map/map_file.cpp
    src/map/map_save.cpp
    src/map/objects.cpp
    src/map/sections_geometry.cpp
    src/map/sections_info.cpp
    src/map/sections_markers.cpp
    src/map/ai_actions.cpp
    src/map/xfer.cpp
    src/map/xfer_monster.cpp
    src/modes/registry.cpp
    src/mapedit/content.cpp
    src/mapedit/document.cpp
    src/mapedit/drag_preview.cpp
    src/mapedit/locks.cpp
    src/mapedit/markers.cpp
    src/mapedit/mod_files.cpp
    src/mapedit/new_map.cpp
    src/mapedit/object_edit.cpp
    src/mapedit/ops.cpp
    src/mapedit/paint.cpp
    src/mapedit/pending.cpp
    src/mapedit/placement.cpp
    src/mapedit/prefab.cpp
    src/mapedit/properties.cpp
    src/mapedit/quest_pictures.cpp
    src/mapedit/script_doc.cpp
    src/mapedit/selection.cpp
    src/mapedit/session_doc.cpp
    src/mapedit/session_merge.cpp
    src/mapedit/session_proto.cpp
    src/mapedit/sidecar.cpp
    src/mapedit/validate.cpp
    src/mapedit/walls.cpp
    src/mapedit/writer.cpp
    src/lobby/browser.cpp
    src/lobby/client_keys.cpp
    src/lobby/listing.cpp
    src/lobby/wol_chat.cpp
    src/lobby/xwis.cpp
    src/lobby/xwis_host.cpp
    src/lobby/xwis_login.cpp
    src/net/framing.cpp
    src/net/fx.cpp
    src/net/messages.cpp
    src/net/player_input.cpp
    src/net/tcp_socket.cpp
    src/net/server_transport.cpp
    src/net/transport.cpp
    src/net/udp_socket.cpp
    src/net/update_stream.cpp
    src/player/player_file.cpp
    src/player/sections.cpp
    src/gui/window_file.cpp
    src/lobby/extended.cpp
    src/script/arg_values.cpp
    src/script/builtins.cpp
    src/script/builtin_docs.cpp
    src/script/compiler.cpp
    src/script/debug_info.cpp
    src/script/decompiler.cpp
    src/script/opcodes.cpp
    src/script/script_file.cpp
    src/script/vm.cpp
    src/server/abilities.cpp
    src/server/books.cpp
    src/server/clock.cpp
    src/server/collide.cpp
    src/server/combat.cpp
    src/server/dialog.cpp
    src/server/enchant.cpp
    src/server/game_host.cpp
    src/server/host_console.cpp
    src/server/items.cpp
    src/server/item_effects.cpp
    src/server/missiles.cpp
    src/server/map_cycle.cpp
    src/server/map_send.cpp
    src/server/map_vote.cpp
    src/server/map_state.cpp
    src/server/modes.cpp
    src/server/monsters.cpp
    src/server/name.cpp
    src/server/map_grid.cpp
    src/server/npc.cpp
    src/server/objects.cpp
    src/server/pathfind.cpp
    src/server/physics.cpp
    src/server/player.cpp
    src/server/object_save.cpp
    src/server/player_save.cpp
    src/server/quest.cpp
    src/server/quest_status.cpp
    src/server/rewards.cpp
    src/server/rules.cpp
    src/server/script_builtins_creatures.cpp
    src/server/script_builtins_screen.cpp
    src/server/script_host.cpp
    src/server/script_reload.cpp
    src/server/script_tests.cpp
    src/server/script_builtins_dialog.cpp
    src/server/script_names.cpp
    src/server/sight.cpp
    src/server/edit_session.cpp
    src/server/play_from_here.cpp
    src/server/solo.cpp
    src/server/solo_save.cpp
    src/server/teleport.cpp
    src/server/trade.cpp
    src/server/server.cpp
    src/server/sound.cpp
    src/server/spells.cpp
    src/server/glyphs.cpp
    src/server/walls.cpp
    src/server/update_writer.cpp
    src/server/world.cpp
    src/server/xfer.cpp
    src/strings/string_table.cpp
    src/things/draw.cpp
    src/things/field_guide.cpp
    src/things/game_data.cpp
    src/things/modifier.cpp
    src/things/monster_db.cpp
    src/things/object_class.cpp
    src/things/sound_sets.cpp
    src/things/spell_ids.cpp
    src/things/thing_db.cpp
    src/video/image_pack.cpp
    src/video/nxz.cpp
    src/video/sprite.cpp
    src/video/sprite_encode.cpp
    src/video/tile_edges.cpp
    src/video/video_bag.cpp
    src/util/rounding.cpp
    src/video/video_bag_writer.cpp
    src/video/vqa.cpp
)
add_library(nox::core ALIAS noxcore)

# The NoxScript builtins' documentation and stock-map usage (script/builtin_docs.hpp), compiled in
# from their text files so the editor needs no data files of its own.
foreach(doc IN ITEMS builtin_docs:kBuiltinDocsText builtin_usage:kBuiltinUsageText)
    string(REPLACE ":" ";" parts "${doc}")
    list(GET parts 0 docFile)
    list(GET parts 1 docSymbol)
    set(docIn "${CMAKE_CURRENT_SOURCE_DIR}/src/script/${docFile}.txt")
    set(docOut "${CMAKE_CURRENT_BINARY_DIR}/embedded_${docFile}.cpp")
    add_custom_command(
        OUTPUT ${docOut}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${docIn} -DOUTPUT=${docOut} -DSYMBOL=${docSymbol}
                -P ${PROJECT_SOURCE_DIR}/cmake/NoxEmbedFile.cmake
        DEPENDS ${docIn} ${PROJECT_SOURCE_DIR}/cmake/NoxEmbedFile.cmake
        COMMENT "Embedding ${docFile}.txt"
        VERBATIM
    )
    target_sources(noxcore PRIVATE ${docOut})
endforeach()

target_include_directories(noxcore PUBLIC include)
target_compile_features(noxcore PUBLIC cxx_std_20)
target_compile_definitions(noxcore PRIVATE NOXCORE_VERSION="${PROJECT_VERSION}")
target_link_libraries(noxcore PRIVATE nox_warnings libzstd_static)
target_include_directories(noxcore PRIVATE "${zstd_SOURCE_DIR}/lib")
target_include_directories(noxcore SYSTEM PRIVATE "${dr_mp3_SOURCE_DIR}")
target_include_directories(noxcore SYSTEM PRIVATE "${bcdec_SOURCE_DIR}")
if(WIN32)
    target_link_libraries(noxcore PUBLIC ws2_32)
endif()
# The server's floats round as the game's x87 (util/x87.hpp: the thread's rounding towards zero
# for its figuring): no fused multiply-adds, whose single rounding the game's never had. MSVC
# contracts only under /fp:contract or /fp:fast (neither set); GCC and Clang are told. Constant
# expressions still fold to nearest at compile time, as the game's own compiler folded them.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(noxcore PRIVATE -ffp-contract=off)
endif()
