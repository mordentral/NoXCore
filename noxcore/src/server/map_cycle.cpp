// Loads and walks mapcycle.txt the way the exe does (map_cycle.hpp).
#include "noxcore/server/map_cycle.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace nox::server {

namespace {

struct Section {
    const char* header;
    std::uint32_t mode;
};

// The file's section headers and the game-mode flag each one lists maps for.
constexpr Section kSections[6] = {
    {"[ELIMINATION]", 0x400},      {"[DEATHMATCH]", 0x100}, {"[CAPTURE THE FLAG]", 0x20},
    {"[KING OF THE REALM]", 0x10}, {"[FLAGBALL]", 0x40},    {"[QUEST]", 0x1000},
};

using util::equalsNoCase;

/// a header's section, or -1.
int sectionOf(const std::string& line)
{
    for (int i = 0; i < 6; ++i) {
        if (equalsNoCase(line, kSections[i].header)) {
            return i;
        }
    }
    return -1;
}

} // namespace

bool MapCycle::load(const std::filesystem::path& file, const std::function<std::uint32_t(const std::string&)>& modesOf)
{
    for (auto& s : sections_) {
        s.clear();
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string line;
    const auto readLine = [&] {
        if (!std::getline(in, line, '\n')) {
            return false;
        }
        if (const auto cr = line.find('\r'); cr != std::string::npos) {
            line.resize(cr);
        }
        return true;
    };
    // Lines before the first header go to [DEATHMATCH] (section 1).
    int section = 1;
    // The first line: a header, or kept aside (a list of its own the cycle doesn't use).
    if (readLine()) {
        if (const int s = sectionOf(line); s >= 0) {
            section = s;
        }
    }
    while (readLine()) {
        if (const int s = sectionOf(line); s >= 0) {
            section = s;
            continue;
        }
        auto& list = sections_[static_cast<std::size_t>(section)];
        if (list.size() >= 25 || line.empty()) {
            continue;
        }
        const std::string name = line.substr(0, line.find('.')); // (strtok ".\n")
        if (!name.empty() && (modesOf(name) & kSections[section].mode) != 0) {
            list.push_back(name);
        }
    }
    return true;
}

std::optional<std::string> MapCycle::take(std::uint32_t gameFlags)
{
    std::size_t s = 0;
    // The first section whose mode the game has; none matches: [ELIMINATION] (section 0).
    for (std::size_t i = 0; i < 6; ++i) {
        if (kSections[i].mode & gameFlags) {
            s = i;
            break;
        }
    }
    const auto& list = sections_[s];
    if (list.empty()) {
        return std::nullopt;
    }
    const int count = static_cast<int>(list.size());
    int at = at_[s];
    if (count <= at) { // (the exe's <: past the list's end when it shrank to where it was)
        at = 0;
    }
    at_[s] = (at + 1) % count;
    return list[static_cast<std::size_t>(at)];
}

std::optional<std::string> MapCycle::next(std::uint32_t gameFlags, const std::string& current)
{
    auto name = take(gameFlags);
    if (name && equalsNoCase(*name, current)) {
        name = take(gameFlags);
    }
    return name;
}

} // namespace nox::server
