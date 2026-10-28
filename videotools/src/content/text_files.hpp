// The host's text files as the Content Editor checks them: a rule set (.rul), the map cycle
// (mapcycle.txt), the message of the day (motd.txt) and a window file (.wnd). Each is read by the
// NoX Core's own reader, so what the check says is what the game does with the file.
#pragma once

#include "content/game_files.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace videotools::content {

enum class TextKind { Rules, MapCycle, Motd, Window, ModManifest, Other };

/// Which kind a file is, by its name (.rul, mapcycle.txt, motd.txt, .wnd, a mod's mode.json).
TextKind textKindOf(const std::filesystem::path& file);
const char* textKindName(TextKind kind);

struct TextReport {
    std::vector<std::string> problems;                       ///< what the game won't take, or drops
    std::vector<std::pair<std::string, std::string>> facts;  ///< what it reads (label, what)
};

/// Reads `text` as the game would a file of that kind (`file`: where it is, for what it names
/// beside it: a mode's scripts).
TextReport checkText(TextKind kind, std::string_view text, const GameFiles& files, const std::filesystem::path& file = {});

} // namespace videotools::content
