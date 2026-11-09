// Writable settings and saves, separate from installed game assets.
#pragma once

#include <filesystem>

namespace noxsdl {

/// Imports missing settings and, when requested, saves once. Throws on filesystem errors.
void prepareUserData(const std::filesystem::path& assets, const std::filesystem::path& user,
                     bool importSaves);

} // namespace noxsdl
