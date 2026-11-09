// Configuration sources for Input Config's Reset and Defaults buttons.
#pragma once

#include "noxcore/config/nox_config.hpp"

namespace noxsdl {

/// Saved bindings come from settingsDir; factory defaults remain with the game assets.
/// Returns nullopt for a missing file; throws if the file cannot be read or parsed.
std::optional<nox::config::Config> readInputConfig(const std::filesystem::path& gameDir,
                                                 const std::filesystem::path& settingsDir, bool defaults);

} // namespace noxsdl
