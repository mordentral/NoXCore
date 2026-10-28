// A mod's folder's files as a collaborative session carries them (session_proto.hpp's ModProject
// and ModFile): the path rules every side checks, the folder read whole on the host, and a file
// written or removed only inside a given folder (the host's mods\<id> at a publish, a member's own
// copy of the session's mod). Nothing here throws: a name the file system can't take is refused.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::mapedit {

/// A mod file's path at most: bytes, and parts (a file 8 deep: the deepest readModFolder reads).
inline constexpr std::size_t kMaxModPath = 128;
inline constexpr std::size_t kMaxModDepth = 8;

/// Whether `rel` is a mod file's path: well-formed UTF-8, '/' separated parts (at most
/// kMaxModDepth), none empty, "." or "..", none ending in '.' or a space, none a name Windows
/// keeps for a device (CON, PRN, AUX, NUL, COM0-9, LPT0-9, whatever follows the first dot); no
/// control characters, 0x7F, drive, '\\' or characters Windows refuses in names; not ending
/// ".saving" (a file being written); at most kMaxModPath bytes.
bool modPathValid(std::string_view rel);

/// Whether two different paths would be one file, or a file and a folder, on some disk: the same
/// but for case, or one a folder the other is inside.
bool modPathsClash(std::string_view a, std::string_view b);

/// `rel` under `folder`; none: not a valid path, or one that leads out of the folder (through a
/// link or junction inside it too).
std::optional<std::filesystem::path> modPathIn(const std::filesystem::path& folder, std::string_view rel);

/// A mod folder's files, by path: every regular file whose path modPathValid takes (no links
/// followed; of two that clash, the first), within the session's caps (session::kMaxModFiles,
/// kMaxModFile, kMaxModBytes). An empty `error`: read whole.
struct ModFolderFiles {
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::string error;
};
ModFolderFiles readModFolder(const std::filesystem::path& folder);

/// One file written (its folders made, through a temporary file) or removed (an emptied folder
/// left), inside `folder` only; false with why.
bool writeModFile(const std::filesystem::path& folder, std::string_view rel, std::span<const std::uint8_t> bytes, std::string& error);
bool removeModFile(const std::filesystem::path& folder, std::string_view rel, std::string& error);

} // namespace nox::mapedit
