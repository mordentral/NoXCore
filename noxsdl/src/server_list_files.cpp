#include "server_list_files.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace noxsdl {
namespace fs = std::filesystem;
namespace {
std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string key(std::string_view text)
{
    if (const auto normalized = normalizedServerAddress(text)) return *normalized;
    return std::string(trim(text)); // old, malformed lines are still removable
}
std::string readText(const fs::path& path)
{
    if (!fs::exists(path)) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path.string());
    std::string text((std::istreambuf_iterator<char>(in)), {});
    if (in.bad()) throw std::runtime_error("Cannot read " + path.string());
    return text;
}
// Each callback receives a complete line, including its original line ending.
template<class F> void lines(const std::string& text, F fn)
{
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        const auto next = end == text.npos ? text.size() : end + 1;
        fn(std::string_view(text).substr(pos, next - pos));
        pos = next;
    }
}
std::string_view addressOf(std::string_view line) { return trim(line.substr(0, line.find('#'))); }
void checkWritable(const fs::path& path)
{
    if (fs::exists(path) && (fs::status(path).permissions() &
        (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) == fs::perms::none)
        throw std::runtime_error("Read-only server list: " + path.string());
}
void replaceFile(const fs::path& path, const std::string& before, const std::string& after)
{
    // A sibling temporary file keeps a failed write from truncating the working list.
    // Reserve a unique directory rather than opening a predictable temporary filename.
    fs::path tempDir;
    std::random_device random;
    for (int attempt = 0; attempt < 20; ++attempt) {
        tempDir = path.parent_path() / (".nox-servers-" + std::to_string(random()));
        if (fs::create_directory(tempDir)) break;
        tempDir.clear();
    }
    if (tempDir.empty()) throw std::runtime_error("Cannot create temporary file for " + path.string());
    const auto temp = tempDir / "servers.txt";
    const auto cleanup = [&] { std::error_code ec; fs::remove(temp, ec); fs::remove(tempDir, ec); };
    try {
        std::ofstream out(temp, std::ios::binary);
        out.write(after.data(), static_cast<std::streamsize>(after.size()));
        out.close();
        if (!out) throw std::runtime_error("Cannot write " + path.string());
        if (fs::exists(path)) {
            checkWritable(path);
            fs::permissions(temp, fs::status(path).permissions());
        }
        if (readText(path) != before) throw std::runtime_error("Server list changed; please try again: " + path.string());
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), path.string());
#else
        fs::rename(temp, path);
#endif
        cleanup();
    } catch (...) { cleanup(); throw; }
}
} // namespace

std::optional<std::string> normalizedServerAddress(std::string_view text)
{
    text = trim(text);
    if (text.empty() || text.size() > 253 + 6) return std::nullopt;
    unsigned port = 18590;
    if (const auto colon = text.find(':'); colon != text.npos) {
        const auto part = text.substr(colon + 1);
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), port);
        if (part.empty() || ec != std::errc{} || end != part.data() + part.size() || port == 0 || port > 65535)
            return std::nullopt;
        text = text.substr(0, colon);
    }
    if (text.empty() || text.size() > 253) return std::nullopt;
    std::string host(text);
    if (host.back() == '.') host.pop_back();
    if (host.empty() || host.back() == '.') return std::nullopt;
    const bool numeric = std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
    std::string canonical;
    std::size_t pos = 0;
    int parts = 0;
    unsigned first = 0;
    while (pos < host.size()) {
        const auto dot = host.find('.', pos);
        const auto label = std::string_view(host).substr(pos, dot == host.npos ? host.size() - pos : dot - pos);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return std::nullopt;
        if (numeric) {
            unsigned n = 0;
            const auto [end, ec] = std::from_chars(label.data(), label.data() + label.size(), n);
            if (ec != std::errc{} || end != label.data() + label.size() || n > 255) return std::nullopt;
            if (parts == 0) first = n;
            canonical += (parts ? "." : "") + std::to_string(n);
        } else {
            for (unsigned char c : label) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                  (c >= '0' && c <= '9') || c == '-')) return std::nullopt;
        }
        ++parts;
        if (dot == host.npos) break;
        pos = dot + 1;
    }
    if (numeric && (parts != 4 || first == 0 || first >= 224)) return std::nullopt;
    if (!numeric) {
        canonical = host;
        std::transform(canonical.begin(), canonical.end(), canonical.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    return canonical + ":" + std::to_string(port);
}

std::vector<fs::path> serverListPaths(const fs::path& gameDir, const fs::path& executableDir)
{
    std::vector<fs::path> paths{gameDir / "servers.txt"};
    if (!executableDir.empty()) paths.push_back(executableDir / "servers.txt");
    return paths;
}

ServerListFiles::ServerListFiles(std::vector<fs::path> paths)
{
    for (const auto& path : paths) {
        const auto resolved = fs::weakly_canonical(path);
        if (std::find(paths_.begin(), paths_.end(), resolved) == paths_.end()) paths_.push_back(resolved);
    }
}

std::vector<std::string> ServerListFiles::read() const
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& path : paths_) lines(readText(path), [&](std::string_view line) {
        const auto address = addressOf(line);
        if (address.empty()) return;
        const auto normalized = key(address);
        if (seen.insert(normalized).second) out.push_back(normalized);
    });
    return out;
}

bool ServerListFiles::add(std::string_view address) const
{
    const auto normalized = normalizedServerAddress(address);
    if (!normalized) throw std::runtime_error("Enter a hostname or IPv4 address, with an optional port (1-65535).");
    if (paths_.empty()) throw std::runtime_error("No server list is configured.");
    const auto saved = read();
    if (std::find(saved.begin(), saved.end(), *normalized) != saved.end()) return false;
    const auto before = readText(paths_.front());
    auto after = before;
    const std::string newline = before.find("\r\n") != before.npos ? "\r\n" : "\n";
    if (!after.empty() && after.back() != '\n') after += newline;
    after += *normalized + newline;
    replaceFile(paths_.front(), before, after);
    return true;
}

void ServerListFiles::remove(const std::vector<std::string>& addresses) const
{
    std::unordered_set<std::string> keys;
    for (const auto& address : addresses) keys.insert(key(address));
    struct Edit { fs::path path; std::string before, after; };
    std::vector<Edit> edits;
    for (const auto& path : paths_) {
        const auto before = readText(path);
        std::string after;
        lines(before, [&](std::string_view line) {
            const auto address = addressOf(line);
            if (address.empty() || !keys.contains(key(address))) after += line;
            else if (const auto comment = line.find('#'); comment != line.npos) after += line.substr(comment);
        });
        if (before != after) {
            checkWritable(path);
            edits.push_back({path, before, std::move(after)});
        }
    }
    // Read and check every affected source before changing any. Each replacement is atomic;
    // a later filesystem failure can still interrupt a multi-file edit.
    for (const auto& edit : edits) replaceFile(edit.path, edit.before, edit.after);
}
} // namespace noxsdl
