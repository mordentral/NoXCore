// Which of a mode's maps is up: a map's name read against the mode's own (isMap), and those
// answers kept for the world's map name (WhichMap: a director asks at every tick, and the name
// only changes as a map loads).
#pragma once

#include "noxcore/util/text.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace nox::server::detail {

/// Whether a map's name ("DEL_LOB.map", or a path) is `name` without its extension, case aside.
inline bool isMap(std::string_view mapName, std::string_view name)
{
    const std::string stem = std::filesystem::path(std::string(mapName)).stem().string();
    return util::equalsNoCase(stem, name);
}

/// A director's two maps (its lobby, its level) against the world's map name: isMap's answers,
/// worked out again only as the name changes.
class WhichMap {
public:
    /// `mapName` (the world's) read against `lobby` and `level`; the answers kept for that name.
    void read(const std::string& mapName, std::string_view lobby, std::string_view level)
    {
        if (!known_ || mapName != name_) {
            known_ = true;
            name_ = mapName;
            lobby_ = isMap(mapName, lobby);
            level_ = isMap(mapName, level);
        }
    }
    bool lobby() const noexcept { return lobby_; }
    bool level() const noexcept { return level_; }

private:
    bool known_ = false;
    std::string name_;
    bool lobby_ = false;
    bool level_ = false;
};

} // namespace nox::server::detail
