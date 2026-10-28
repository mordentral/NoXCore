// The built-in modes' Luau sources, built into the program (a
// built-in mode needs no script files on disk). Each mode's folder keeps them under script/
// (modes/tide/script/server/data.luau), its mode.cmake names them (nox_mode_script), and
// modes/script/embed_scripts.cmake writes them into builtin_scripts.cpp at build time.
#pragma once

#include <optional>
#include <span>
#include <string_view>

namespace nox::modes::script {

struct BuiltinScript {
    std::string_view mode; ///< the mode's id ("tide")
    std::string_view path; ///< its path in the mode's folder ("server/data.luau")
    std::string_view text; ///< the source, as the file had it
};

/// Every built-in mode's scripts.
std::span<const BuiltinScript> builtinScripts();

/// One of a built-in mode's scripts by its path; none: not built in.
std::optional<std::string_view> builtinScript(std::string_view mode, std::string_view path);

} // namespace nox::modes::script
