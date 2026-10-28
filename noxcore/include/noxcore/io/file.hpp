// Small file helpers shared by the loaders.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace nox {

/// Whole file contents (`path` matched ignoring case where it isn't found as it is:
/// resolveNoCase). Throws std::system_error if it can't be read.
std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path);

/// Writes (replacing) a whole file. Throws std::system_error on failure.
void writeFileBytes(const std::filesystem::path& path, std::span<const std::uint8_t> data);

/// `dir / name` if it exists, else the entry of `dir` whose name matches ignoring case (the game
/// ran on a case-insensitive file system), else `dir / name`. `name` may be a relative path
/// whose parts are each matched that way.
std::filesystem::path findNoCase(const std::filesystem::path& dir, const std::filesystem::path& name);

/// Whether `name` is a single file or folder name, safe to append to a folder: not empty, not "."
/// or "..", without '/', '\\', ':' or a control character. A name from the network (a map a
/// server names) must be one before it becomes part of a path: else "..\x" or "C:\x" would
/// read or write outside the folder.
bool isPlainFileName(std::string_view name) noexcept;

/// The game's data names its files in any case (Con01A.map in maps/con01a, inputcfg.wnd for
/// InputCfg.wnd), as the case-insensitive file systems it ran on let it. On Windows `path` as it
/// is; elsewhere `path` if it exists, else the path whose missing parts (from the last existing
/// folder on) are the entries matching them ignoring case, else `path` unchanged.
#ifdef _WIN32
inline const std::filesystem::path& resolveNoCase(const std::filesystem::path& path)
{
    return path;
}
#else
std::filesystem::path resolveNoCase(const std::filesystem::path& path);
#endif

} // namespace nox
