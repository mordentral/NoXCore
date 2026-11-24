// A mode's own maps (mode_maps.hpp).
#include "noxcore/server/mode_maps.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <system_error>

namespace nox::server {

namespace fs = std::filesystem;

namespace {

/// The entry of `folder` named `want` ignoring case that is a real file inside `root`; empty: none.
fs::path realFileIn(const fs::path& folder, const std::string& want, const fs::path& root)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (!util::equalsNoCase(pathUtf8(e.path().filename()), want)) {
            continue;
        }
        const fs::file_status st = e.symlink_status(ec);
        if (ec || isLinkStatus(st) || !fs::is_regular_file(st) || !resolvesInside(e.path(), root)) {
            return {};
        }
        return e.path();
    }
    return {};
}

} // namespace

std::shared_ptr<const ModeMaps> ModeMaps::index(const fs::path& dir, std::vector<std::string>* problems)
{
    auto out = std::make_shared<ModeMaps>();
    const auto problem = [&](const std::string& name, const std::string& why) {
        if (problems) {
            problems->push_back("map " + name + " left out: " + why);
        }
    };
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    const fs::path root = fs::canonical(dir, ec);
    if (ec) {
        return out;
    }
    std::vector<Entry> found;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name;
        try {
            name = pathUtf8(e.path().filename());
        } catch (const std::exception&) {
            continue; // (a name that can't be made text)
        }
        const fs::file_status st = e.symlink_status(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!fs::is_directory(st) && !isLinkStatus(st)) {
            continue; // (a loose file: not a map's folder)
        }
        // A map's name names a folder and its file anywhere a map is found by name, and exits
        // name it up to a '.' or ':'; a link could lead out of the mod's folder.
        if (!isPlainFileName(name) || name.find('.') != std::string::npos) {
            problem(name, "not a plain map name");
            continue;
        }
        if (isLinkStatus(st) || !resolvesInside(e.path(), root)) {
            problem(name, "a link, not a folder of the mod's own");
            continue;
        }
        const fs::path map = realFileIn(e.path(), name + ".map", root);
        if (map.empty()) {
            problem(name, "no " + name + ".map of its own in it");
            continue;
        }
        if (found.size() == kMaxMaps) {
            problem(name, "more than " + std::to_string(kMaxMaps) + " maps");
            continue;
        }
        found.push_back({name, map, realFileIn(e.path(), name + ".nxz", root)});
    }
    std::ranges::sort(found, [](const Entry& a, const Entry& b) { return util::compareNoCase(a.name, b.name) < 0; });
    // (Two folders of one name in two cases, on a file system that keeps them apart: the first.)
    const auto dup = std::ranges::unique(found, [](const Entry& a, const Entry& b) { return util::equalsNoCase(a.name, b.name); });
    for (auto it = dup.begin(); it != dup.end(); ++it) {
        problem(it->name, "another folder has its name");
    }
    found.erase(dup.begin(), dup.end());
    out->keys_.reserve(found.size());
    for (const Entry& f : found) {
        out->keys_.push_back(util::lowered(f.name));
    }
    out->entries_ = std::move(found);
    return out;
}

bool isSoloMapName(std::string_view name) noexcept
{
    if (name.size() <= 3 || name[3] < '0' || name[3] > '9') {
        return false;
    }
    const std::string_view prefix = name.substr(0, 3);
    return util::equalsNoCase(prefix, "wiz") || util::equalsNoCase(prefix, "war") || util::equalsNoCase(prefix, "con");
}

const ModeMaps::Entry* ModeMaps::find(std::string_view name) const
{
    if (entries_.empty()) {
        return nullptr;
    }
    const std::string key = util::lowered(name);
    const auto it = std::ranges::lower_bound(keys_, key);
    if (it == keys_.end() || *it != key) {
        return nullptr;
    }
    return &entries_[static_cast<std::size_t>(it - keys_.begin())];
}

} // namespace nox::server
