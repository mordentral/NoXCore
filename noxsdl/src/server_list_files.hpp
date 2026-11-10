#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

// Syntax only: saving an address must not depend on DNS or a running server.
std::optional<std::string> normalizedServerAddress(std::string_view text);
std::vector<std::filesystem::path> serverListPaths(const std::filesystem::path& gameDir,
                                                 const std::filesystem::path& executableDir);

// Reads fresh contents for each edit, retaining comments and unrelated lines. The game-data
// file is first (the destination for additions); other files remain independent sources.
class ServerListFiles {
public:
    explicit ServerListFiles(std::vector<std::filesystem::path> paths = {});
    std::vector<std::string> read() const;
    bool add(std::string_view address) const; // false: already saved
    void remove(const std::vector<std::string>& addresses) const;
private:
    std::vector<std::filesystem::path> paths_;
};

} // namespace noxsdl
