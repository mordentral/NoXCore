// servers.txt, the saved servers the browser always pings (server_list_files.hpp): read through
// noxcore's splitServerList, as the browser always has; edited line by line so comments, line
// endings and other lines stay as they were.
#include "server_list_files.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/lobby/listing.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace noxsdl {
namespace fs = std::filesystem;
namespace {

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// The form two lines are compared in: normalised, else as written (an old malformed line can
/// still be removed).
std::string key(std::string_view text)
{
    if (const auto normalized = normalizedServerAddress(text)) return *normalized;
    return std::string(trim(text));
}

/// The whole file (none: empty). Throws if it's there but can't be read.
std::string readText(const fs::path& path)
{
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    const auto bytes = nox::readFileBytes(path);
    return {bytes.begin(), bytes.end()};
}

/// Each line with its own line ending, for editing in place.
template<class F> void eachLine(const std::string& text, F fn)
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

/// Replaces the whole file: written beside it, then renamed over it (an interrupted write leaves
/// the old list), keeping the old file's permissions. A read-only list is left alone.
void replaceFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (exists && (fs::status(path).permissions() & (fs::perms::owner_write | fs::perms::group_write |
                                                     fs::perms::others_write)) == fs::perms::none) {
        throw std::runtime_error("Read-only server list: " + path.string());
    }
    fs::path temp = path;
    temp += ".new";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    try {
        if (!out) throw std::runtime_error("Cannot write " + temp.string());
        if (exists) fs::permissions(temp, fs::status(path).permissions());
        fs::rename(temp, path);
    } catch (...) {
        fs::remove(temp, ec);
        throw;
    }
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

std::vector<fs::path> serverListPaths(const fs::path& settingsDir, const fs::path& executableDir)
{
    std::vector<fs::path> paths{settingsDir / "servers.txt"};
    if (!executableDir.empty()) paths.push_back(executableDir / "servers.txt");
    return paths;
}

ServerListFiles::ServerListFiles(std::vector<fs::path> paths)
{
    for (const auto& path : paths) {
        std::error_code ec;
        auto resolved = fs::weakly_canonical(path, ec);
        if (ec) resolved = path;
        if (std::find(paths_.begin(), paths_.end(), resolved) == paths_.end()) paths_.push_back(resolved);
    }
}

std::vector<std::string> ServerListFiles::read(std::vector<std::string>* errors) const
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& path : paths_) {
        std::string text;
        try {
            text = readText(path);
        } catch (const std::exception& ex) {
            if (!errors) throw;
            errors->push_back(path.string() + ": " + ex.what());
            continue;
        }
        const auto lines = nox::lobby::splitServerList({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        for (const auto& line : lines) {
            auto k = key(line);
            if (seen.insert(k).second) out.push_back(std::move(k));
        }
    }
    return out;
}

bool ServerListFiles::add(std::string_view address) const
{
    const auto normalized = normalizedServerAddress(address);
    if (!normalized) throw std::runtime_error("Enter a hostname or IPv4 address, with an optional port (1-65535).");
    if (paths_.empty()) throw std::runtime_error("No server list is configured.");
    const auto saved = read();
    if (std::find(saved.begin(), saved.end(), *normalized) != saved.end()) return false;
    std::string text = readText(paths_.front());
    const std::string newline = text.find("\r\n") != text.npos ? "\r\n" : "\n";
    if (!text.empty() && text.back() != '\n') text += newline;
    text += *normalized + newline;
    replaceFile(paths_.front(), text);
    return true;
}

void ServerListFiles::remove(std::string_view address) const
{
    // Every file read and edited before any is written: one that can't be read changes none.
    const auto removed = key(address);
    std::vector<std::pair<fs::path, std::string>> edits;
    for (const auto& path : paths_) {
        const auto before = readText(path);
        std::string after;
        eachLine(before, [&](std::string_view line) {
            const auto a = addressOf(line);
            if (a.empty() || key(a) != removed) after += line;
            else if (const auto comment = line.find('#'); comment != line.npos) after += line.substr(comment);
        });
        if (after != before) edits.emplace_back(path, std::move(after));
    }
    for (const auto& [path, text] : edits) replaceFile(path, text);
}

} // namespace noxsdl
