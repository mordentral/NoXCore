// Internal publication operations. Regression checks inject filesystem errors at commit and cleanup.
#pragma once

#include "noxcore/map/map_save.hpp"

namespace nox::map::detail {

using ReplaceFile = void (*)(const std::filesystem::path&, const std::filesystem::path&);
using RemoveTree = std::uintmax_t (*)(const std::filesystem::path&, std::error_code&);
std::filesystem::path publishMap(const std::filesystem::path& mapsDir, std::string_view name, const EncodedMapFiles& files,
                                 std::string_view who, std::optional<std::string_view> sidecar,
                                 std::filesystem::path& retainedCleanup, ReplaceFile replace, RemoveTree cleanup);

} // namespace nox::map::detail
