// The maps a NoX Core mode ships with it (a mod's maps\<Name>\<Name>.map, beside its mode.json),
// read once when the mode is: a game of that mode finds its maps there before the game's maps
// folder (GameHost::Options::modeMaps, GameHost::findMap). No other game sees them, solo games
// included: only a host that hosts the mode hands its options the index (save the host console's
// load of a name no other place has, which looks in every installed mod's: VD-80). The one other index is
// noxsdl's ordinary network host's: the map editor's folder, read once when the host is made
// (LocalHost, HostSettings::authoredMapsDir), so a map authored there is hosted, listed and sent
// to guests by its name.
//
// A name is a map's folder name, matched ignoring case as the maps folder's are. What the index
// keeps is checked as it's read, never again per lookup: a plain name (no path, no device), a real
// folder and a real file (no symbolic link or junction), each resolving inside the folder indexed.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server {

class ModeMaps {
public:
    /// The most maps an index takes (the rest are left out, said so).
    static constexpr std::size_t kMaxMaps = 1024;

    struct Entry {
        std::string name;               ///< its folder's name, as on disk
        std::filesystem::path map;      ///< <Name>\<Name>.map (any case)
        std::filesystem::path nxz;      ///< <Name>\<Name>.nxz beside it (empty: none shipped)
    };

    /// The maps in `dir` (each <dir>\<Name>\<Name>.map); `problems` (when given) gets a line for each
    /// folder left out and why. No folder `dir`: an empty index.
    static std::shared_ptr<const ModeMaps> index(const std::filesystem::path& dir, std::vector<std::string>* problems = nullptr);

    /// The map named `name` (its folder's name; any case); null: not this index's.
    const Entry* find(std::string_view name) const;
    /// Its maps, by name (ignoring case).
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    bool empty() const noexcept { return entries_.empty(); }

private:
    std::vector<Entry> entries_; ///< (sorted by lower-case name: find's binary search)
    std::vector<std::string> keys_; ///< (each entry's name in lower case)
};

} // namespace nox::server
