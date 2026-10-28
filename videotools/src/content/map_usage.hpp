// Which maps use which object types: each map's ObjectTOC (the type names its objects use), read
// once from every map in the game folder (off the UI thread: a game's ~160 maps take a few seconds).
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace videotools::content {

class MapUsage {
public:
    /// Reads every map's type list (a map that can't be read is counted in failed()).
    static MapUsage scan(const std::vector<std::filesystem::path>& maps);

    /// The maps using a type (by name, any case): indexes into maps().
    const std::vector<std::size_t>* mapsUsing(std::string_view type) const;
    const std::vector<std::string>& maps() const { return names_; }
    std::size_t failed() const { return failed_; }

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, std::vector<std::size_t>> byType_; ///< lower-case type name -> maps
    std::size_t failed_ = 0;
};

} // namespace videotools::content
