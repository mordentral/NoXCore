# Third-party dependencies, pinned by version and hash.
include(FetchContent)

# An optional prebuilt zip (FFmpeg for the HD movies): unpacked once into the build's _deps folder,
# its hash checked, never fatal. Sets out_var to the unpacked folder, or leaves it empty when there
# is none (FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL, when set, is used as it is).
function(nox_fetch_ffmpeg url sha256 out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL)
        if(EXISTS "${FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL}/include/libavcodec/avcodec.h")
            set(${out_var} "${FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL}" PARENT_SCOPE)
        else()
            message(WARNING "FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL (${FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL}) holds no FFmpeg build")
        endif()
        return()
    endif()
    string(SUBSTRING "${sha256}" 0 12 tag)
    string(TOLOWER "${tag}" tag)
    set(dir "${FETCHCONTENT_BASE_DIR}/ffmpeg_lgpl-${tag}")
    if(EXISTS "${dir}/.nox_sha256")
        set(${out_var} "${dir}" PARENT_SCOPE)
        return()
    endif()
    if(FETCHCONTENT_FULLY_DISCONNECTED)
        return()
    endif()
    set(zip "${FETCHCONTENT_BASE_DIR}/ffmpeg_lgpl-${tag}.zip")
    message(STATUS "Downloading FFmpeg (${url})")
    file(DOWNLOAD "${url}" "${zip}" STATUS status TLS_VERIFY ON)
    list(GET status 0 rc)
    if(NOT rc EQUAL 0)
        list(GET status 1 why)
        message(STATUS "FFmpeg download failed: ${why}")
        file(REMOVE "${zip}")
        return()
    endif()
    file(SHA256 "${zip}" got)
    string(TOUPPER "${got}" got)
    string(TOUPPER "${sha256}" sha256)
    if(NOT got STREQUAL sha256)
        message(STATUS "FFmpeg download: SHA256 ${got}, expected ${sha256}")
        file(REMOVE "${zip}")
        return()
    endif()
    # The zip holds one folder (ffmpeg-<version>-win64-lgpl-shared-8.1): it becomes ${dir}.
    file(REMOVE_RECURSE "${dir}.tmp" "${dir}")
    file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${dir}.tmp")
    file(GLOB top LIST_DIRECTORIES true "${dir}.tmp/*")
    list(LENGTH top n)
    if(NOT n EQUAL 1 OR NOT EXISTS "${top}/include/libavcodec/avcodec.h")
        message(STATUS "FFmpeg download: not the expected layout")
        file(REMOVE_RECURSE "${dir}.tmp")
        file(REMOVE "${zip}")
        return()
    endif()
    file(RENAME "${top}" "${dir}")
    file(REMOVE_RECURSE "${dir}.tmp")
    file(REMOVE "${zip}")
    file(WRITE "${dir}/.nox_sha256" "${sha256}\n")
    set(${out_var} "${dir}" PARENT_SCOPE)
endfunction()

# zstd: the image pack's compression (noxcore video/image_pack). The static library only.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=EB33E51F49A15E023950CD7825CA74A4A2B43DB8354825AC24FC1B7EE09E6FA3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

# dr_mp3 (public domain / MIT-0): decodes the game's speech (dialog\*.wav holds MP3 data).
FetchContent_Declare(dr_mp3
    URL https://raw.githubusercontent.com/mackron/dr_libs/51e61d308dde6b437fce0c5fabb32cd86b40f4d7/dr_mp3.h
    URL_HASH SHA256=997B7EE18DE6E6B81E2A83F1EA9FC62AEF25C62B28D48DB95635F49E65DE0A2F
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(dr_mp3)

# bcdec (MIT / public domain): the image pack's BC7 decode on the CPU (noxcore video/image_pack),
# for renderers without BC7 textures and for the tools.
FetchContent_Declare(bcdec
    URL https://raw.githubusercontent.com/iOrange/bcdec/80859ed3b7afb1c527a2a99d70c61457bea72d0c/bcdec.h
    URL_HASH SHA256=134520764D96F70A27DB814173616C89AD85DB95FBD3328417C7A8C77E7CA189
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(bcdec)

# stb_image (MIT / public domain): a mod's PNG images, decoded on the host at the mod's load
# (modes/script mod_assets.hpp); clients only ever get the decoded pixels.
FetchContent_Declare(stb_image
    URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
    URL_HASH SHA256=594C2FE35D49488B4382DBFAEC8F98366DEFCA819D916AC95BECF3E75F4200B3
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb_image)

# Luau (MIT): the game modes' scripts (modes/script). Its VM and compiler only, static; no CLI or tests. Its other libraries (Analysis for
# the editor's diagnostics, later) are left out of the default build unless something links them.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(luau
    URL https://github.com/luau-lang/luau/archive/refs/tags/0.741.tar.gz
    URL_HASH SHA256=DEEE653D0F5971B809EF67F50DBE8109AD369D7575E344CFE2013774FED2F4C6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(luau)

if(NOX_BUILD_SDL)
    # Static SDL so noxcore.exe runs without a DLL next to it.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
        URL_HASH SHA256=7322236CD12090C3EB40B9728BE4D49C76F66AD17D04369584D4ECAD5CF77C68
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        # A D3D12 descriptor heap overflow on frames of ~2000 draws (see the script).
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/sdl3_d3d12_descriptor_overflow.cmake
    )
    FetchContent_MakeAvailable(SDL3)

    # Text: stb_truetype renders TrueType, and noxsdl embeds Noto Sans (SIL OFL 1.1) as the font it
    # uses when the game folder has no default.ttf / default.otf of its own.
    FetchContent_Declare(stb_truetype
        URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h
        URL_HASH SHA256=ECD30B05E0DD4FEA3A13C26810DD9E1992DC379049482C393D5A19E6B5090AAB
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_Declare(noto_sans
        URL https://raw.githubusercontent.com/notofonts/notofonts.github.io/3ec599d8ea849413786a184f20e473c3fcbc4082/fonts/NotoSans/hinted/ttf/NotoSans-SemiBold.ttf
        URL_HASH SHA256=A4E91FD530AC2B4EF5367240144FF37D7D65D66CF76F2E9A2187B93C676F92D0
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_Declare(noto_sans_license
        URL https://raw.githubusercontent.com/notofonts/latin-greek-cyrillic/023d7b73d5c1a5ed6489bd04a120244c1f2bff3f/OFL.txt
        URL_HASH SHA256=CEE9892F9F0CC8FE882C9E9537EE6A89621D86EE7CEAF70B02E2B2B1C25C061A
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(stb_truetype noto_sans noto_sans_license)

    # UPnP (noxsdl port_mapper.cpp): miniupnpc (BSD), static, to open a hosted game's port on the
    # router.
    set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(miniupnpc
        URL https://miniupnp.tuxfamily.org/files/miniupnpc-2.3.3.tar.gz
        URL_HASH SHA256=D52A0AFA614AD6C088CC9DDFF1AE7D29C8C595AC5FDD321170A05F41E634BD1A
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(miniupnpc)

    # The HD movies (noxsdl hd_video.cpp): FFmpeg's LGPL shared build for Windows (BtbN's
    # FFmpeg-Builds, release 8.1.3, with libdav1d for AV1), its headers and import libraries; the
    # DLLs are delay-loaded from HDVideo\ and ship in the HD movies' download, not with noxsdl.
    #
    # The configure never depends on this download: the pin is a month-end autobuild (BtbN keeps
    # the last build of each month for years and prunes only the dailies), and if it can't be had
    # anyway (gone, offline, wrong hash) noxsdl is built without FFmpeg and plays the VQA movies,
    # as it does when the DLLs are missing at run time. FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL may
    # point at an unpacked win64-lgpl-shared 8.1 build instead (nothing is downloaded then).
    # ffmpeg_lgpl_SOURCE_DIR, set only when a build is there, is what noxsdl's CMakeLists tests.
    option(NOXSDL_HD_VIDEO "noxsdl: play HDVideo\\*.mkv through FFmpeg (Windows; skipped if the build can't be fetched)" ON)
    set(ffmpeg_lgpl_SOURCE_DIR "")
    if(NOXSDL_HD_VIDEO AND WIN32 AND MSVC)
        set(nox_ffmpeg_url "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1.zip")
        set(nox_ffmpeg_sha256 "3E47BDA1607740550141E37C0E49D1E5182B34699F15ADFD137EE266D346811A")
        nox_fetch_ffmpeg("${nox_ffmpeg_url}" "${nox_ffmpeg_sha256}" ffmpeg_lgpl_SOURCE_DIR)
        if(NOT ffmpeg_lgpl_SOURCE_DIR)
            message(WARNING "noxsdl HD video: no FFmpeg build, so noxsdl is built without it and plays the VQA movies "
                            "(set FETCHCONTENT_SOURCE_DIR_FFMPEG_LGPL to an unpacked win64-lgpl-shared 8.1 build to have it)")
        endif()
    endif()
endif()

if(NOX_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
        URL_HASH SHA256=0957CAE5821B17CE07F0833AAA52B5137643A8382203221F363A8303C109AF34
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
