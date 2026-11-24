// Small file helpers shared by the loaders.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nox {

/// Whole file contents (`path` matched ignoring case where it isn't found as it is:
/// resolveNoCase). Throws std::system_error if it can't be read.
std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path);

/// Writes (replacing) a whole file. Throws std::system_error on failure.
void writeFileBytes(const std::filesystem::path& path, std::span<const std::uint8_t> data);

/// Writes a whole file by way of another beside it (`path` + `tempSuffix`: ".tmp", ".part"...),
/// written whole and then moved over `path`, so a failed write never leaves half a file; the
/// temporary file is removed when either step fails. Throws std::system_error (the write's, as
/// writeFileBytes) or std::filesystem::filesystem_error (the move's).
void replaceFileBytes(const std::filesystem::path& path, std::span<const std::uint8_t> data, std::string_view tempSuffix = ".tmp");
/// Why a tryReplaceFileBytes failed: in its write of the temporary file, or in its move over the
/// file (`error` the move's).
struct ReplaceFailure {
    bool moving = false;
    std::error_code error;
};
/// replaceFileBytes without a throw: false when it failed (`why`, when given, says where).
bool tryReplaceFileBytes(const std::filesystem::path& path, std::span<const std::uint8_t> data, std::string_view tempSuffix = ".tmp",
                         ReplaceFailure* why = nullptr) noexcept;

/// Whether `st` (a symlink_status, never followed) is a link of some kind: a symbolic link, or on
/// Windows a junction too. A folder a program reads as its own never follows one out.
bool isLinkStatus(const std::filesystem::file_status& st) noexcept;
/// Whether `path`, every link in it resolved (std::filesystem::canonical: it must be there), is
/// inside `root` (itself resolved already) and isn't `root` itself.
bool resolvesInside(const std::filesystem::path& path, const std::filesystem::path& root);

/// `dir / name` if it exists, else the entry of `dir` whose name matches ignoring case (the game
/// ran on a case-insensitive file system), else `dir / name`. `name` may be a relative path
/// whose parts are each matched that way.
std::filesystem::path findNoCase(const std::filesystem::path& dir, const std::filesystem::path& name);

/// Whether `name` is a single file or folder name, safe to append to a folder: not empty, not "."
/// or "..", without '/', '\\', ':' or a control character, not ending in a dot or a space, and
/// not one of Windows' device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, any case, with or
/// without an extension). A name from the network (a map a server names) must be one before it
/// becomes part of a path: else "..\x" or "C:\x" would read or write outside the folder, "NUL"
/// a device, "Estate." another name's file.
bool isPlainFileName(std::string_view name) noexcept;

/// `path` as UTF-8 text. (path::string() is the system's code page on Windows, and throws for a
/// name it can't hold: a mod folder named in another script.)
std::string pathUtf8(const std::filesystem::path& path);

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
