// Checks a map document against what the game accepts when it loads a map (the section
// readers' version limits, string buffers, object records that must decode exactly, waypoint
// links that must resolve) and against what usually goes wrong in a map (a missing player start).
#pragma once

#include "noxcore/mapedit/document.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

enum class Severity {
    Error,   // the game (or our writer) would fail on it: saving is refused
    Warning, // loads, but probably not what was meant
};

struct Issue {
    Severity severity = Severity::Error;
    std::string where;   // section or object, e.g. "WayPoints #12", "object 431 (Door)"
    std::string message;
};

std::vector<Issue> validate(const MapDocument& doc, const things::ThingDb& things);

/// Why `name` can't be a map's name (it names the map's folder and files), or nothing if it can.
/// Any printable name a folder can hold is fine, '!' and '#' included (as the game takes them),
/// spaces aside, up to the game's 8 characters (its map scan reads no more, so a longer name's map
/// is never listed). Stock map names are refused, and the solo maps' prefix (wiz, war or con and a
/// digit), which the scan leaves out of the map lists. Maps already named otherwise still open;
/// only saving under such a name is refused.
std::optional<std::string> mapNameProblem(std::string_view name);

/// The name a copy of map `from` is offered under: "<from>_ed", or "ed_<start of from>" to keep
/// within the game's 8 characters (lower case).
std::string suggestedMapName(std::string_view from);

} // namespace nox::mapedit
