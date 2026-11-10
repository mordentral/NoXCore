// servers.txt: "host" or "host:port" per line ('#' starts a comment), the servers the browser
// always pings. Two files: the settings folder's (the game folder, but for the Mac fork:
// backend_hooks.hpp), where Add/Remove saves, and the one beside the program.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

/// "host:port" in one form (lower case, the default port written out); nullopt if it isn't a
/// hostname or IPv4 address with an optional port 1-65535. Syntax only: no DNS.
std::optional<std::string> normalizedServerAddress(std::string_view text);
/// The servers.txt files, the settings folder's first.
std::vector<std::filesystem::path> serverListPaths(const std::filesystem::path& settingsDir,
                                                 const std::filesystem::path& executableDir);

/// The files' saved servers, read afresh for each edit; edits keep comments and other lines.
class ServerListFiles {
public:
    explicit ServerListFiles(std::vector<std::filesystem::path> paths = {});
    /// The saved addresses (normalised, no repeats). With `errors`, a file that can't be read is
    /// reported there and skipped; without, it throws (an edit never works from half the list).
    std::vector<std::string> read(std::vector<std::string>* errors = nullptr) const;
    /// Saves an address in the first file; false if it's already saved. Throws on failure.
    bool add(std::string_view address) const;
    /// Takes the address out of every file (comments on its lines kept). Throws on failure.
    void remove(std::string_view address) const;

private:
    std::vector<std::filesystem::path> paths_;
};

} // namespace noxsdl
