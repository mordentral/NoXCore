// A mode's own maps (mode_maps.hpp).
#include "noxcore/server/mode_maps.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <system_error>

namespace nox::server {

namespace fs = std::filesystem;

namespace {

/// Whether `st` is a link of some kind (a symbolic link; on Windows a junction too): never
/// followed out of the folder.
bool isLink(const fs::file_status& st)
{
#ifdef _MSC_VER
    if (st.type() == fs::file_type::junction) {
        return true;
    }
#endif
    return fs::is_symlink(st);
}

/// Whether `p`, every link in it resolved, is inside `root` (itself resolved) and not `root`.
bool resolvesInside(const fs::path& p, const fs::path& root)
{
    std::error_code ec;
    const fs::path real = fs::canonical(p, ec);
    if (ec) {
        return false;
    }
    const auto [r, _] = std::mismatch(root.begin(), root.end(), real.begin(), real.end());
    return r == root.end() && real != root;
}

/// The entry of `folder` named `want` ignoring case that is a real file inside `root`; empty: none.
fs::path realFileIn(const fs::path& folder, const std::string& want, const fs::path& root)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (!util::equalsNoCase(pathUtf8(e.path().filename()), want)) {
            continue;
        }
        const fs::file_status st = e.symlink_status(ec);
        if (ec || isLink(st) || !fs::is_regular_file(st) || !resolvesInside(e.path(), root)) {
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
        if (!fs::is_directory(st) && !isLink(st)) {
            continue; // (a loose file: not a map's folder)
        }
        // A map's name names a folder and its file anywhere a map is found by name, and exits
        // name it up to a '.' or ':'; a link could lead out of the mod's folder.
        if (!isPlainFileName(name) || name.find('.') != std::string::npos) {
            problem(name, "not a plain map name");
            continue;
        }
        if (isLink(st) || !resolvesInside(e.path(), root)) {
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
