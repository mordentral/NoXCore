// The host's map cycle (mapcycle.txt in the game folder), as GAME.EXE reads and walks it:
//
//   - loadMapCycle: section headers [ELIMINATION], [DEATHMATCH],
//     [CAPTURE THE FLAG], [KING OF THE REALM], [FLAGBALL], [QUEST] (each a mode: 0x400, 0x100,
//     0x20, 0x10, 0x40, 0x1000); every other line a map, its name up to the first '.', kept when
//     the map exists and its types include the section's mode, at most 25 a section. Lines before
//     the first header go in [DEATHMATCH]. Reloaded at each map change; where each section is up
//     to stays.
//   - getSomeMapName: the section of the game's mode (the first whose mode it has;
//     none: [ELIMINATION]), its next map round the list. The host asks again when that's the
//     map it's on (case aside).
// A dedicated server walks it always (engine flag 0x40000); without the file the map stays.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nox::server {

class MapCycle {
public:
    /// Reads `file`; `modesOf` gives a map's modes (game flag bits) from its name, 0 when it isn't
    /// there. False without the file.
    bool load(const std::filesystem::path& file, const std::function<std::uint32_t(const std::string&)>& modesOf);
    /// The next map for a game of `gameFlags`, not `current` if it can help it; none when its
    /// section is empty.
    std::optional<std::string> next(std::uint32_t gameFlags, const std::string& current);

    const std::array<std::vector<std::string>, 6>& sections() const noexcept { return sections_; }

private:
    std::optional<std::string> take(std::uint32_t gameFlags);

    std::array<std::vector<std::string>, 6> sections_;
    std::array<int, 6> at_{};
};

} // namespace nox::server
