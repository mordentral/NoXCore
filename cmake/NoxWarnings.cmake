# nox_warnings: interface target carrying our warning flags. Link it PRIVATE to our own
# targets only, so third-party code (SDL, Catch2) builds with its own settings.
add_library(nox_warnings INTERFACE)

if(MSVC)
    target_compile_options(nox_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    # The portable C library calls (sscanf, getenv...) without C4996's advice to use the
    # Windows-only _s versions; GCC and Clang have no such warning either.
    target_compile_definitions(nox_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(NOX_WARNINGS_AS_ERRORS)
        target_compile_options(nox_warnings INTERFACE /WX)
    endif()
else()
    # (-Wmissing-field-initializers off: partial aggregate initialisers, the rest left to their
    # default member initialisers, are this code's idiom; MSVC's /W4 has no such warning.)
    target_compile_options(nox_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wno-missing-field-initializers)
    if(NOX_WARNINGS_AS_ERRORS)
        target_compile_options(nox_warnings INTERFACE -Werror)
    endif()
endif()
