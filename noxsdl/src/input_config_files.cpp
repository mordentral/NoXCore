// Input Config's saved bindings and factory defaults (input_config_files.hpp).
#include "input_config_files.hpp"

#include "noxcore/io/file.hpp"

namespace noxsdl {

std::optional<nox::config::Config> readInputConfig(const std::filesystem::path& gameDir,
                                                 const std::filesystem::path& settingsDir, bool defaults)
{
    const auto path = nox::findNoCase(defaults ? gameDir : settingsDir, defaults ? "default.cfg" : "nox.cfg");
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
    const auto bytes = nox::readFileBytes(path);
    return nox::config::parseConfig(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

} // namespace noxsdl
