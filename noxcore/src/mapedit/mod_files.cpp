// A mod folder's files for a collaborative session (mod_files.hpp).
#include "noxcore/mapedit/mod_files.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/mapedit/session_proto.hpp"
#include "noxcore/modes/mode.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace nox::mapedit {

namespace {

fs::path u8path(std::string_view s)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string u8text(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

/// A name Windows keeps for a device (CON, PRN, AUX, NUL, COM0-9, LPT0-9), whatever follows its
/// first dot ("aux.luau" is the device on older Windows).
bool reservedName(std::string_view part)
{
    const std::string stem = util::lowered(part.substr(0, part.find('.')));
    static constexpr std::array<std::string_view, 4> kNames{"con", "prn", "aux", "nul"};
    if (std::ranges::find(kNames, stem) != kNames.end()) {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9';
}

} // namespace

bool modPathValid(std::string_view rel)
{
    // (Well-formed UTF-8 only: a name Windows can't map is refused before a path is ever made of it.)
    if (rel.empty() || rel.size() > kMaxModPath || rel.front() == '/' || rel.back() == '/' || !util::validUtf8(rel)) {
        return false;
    }
    std::size_t parts = 0;
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t slash = std::min(rel.find('/', start), rel.size());
        const std::string_view part = rel.substr(start, slash - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' || reservedName(part)) {
            return false;
        }
        for (const char c : part) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || std::string_view("<>:\"\\|?*").find(c) != std::string_view::npos) {
                return false;
            }
        }
        if (++parts > kMaxModDepth) {
            return false; // (deeper than a mod's folder is ever read: readModFolder)
        }
        if (parts == 1 && util::equalsNoCase(part, modes::kModSavesFolder)) {
            return false; // (its players' saves: never the mod's, never listed, sent, written or removed as its files)
        }
        start = slash + 1;
    }
    // (A file being written goes through "<name>.saving": never a mod file's own name.)
    return !util::lowered(rel).ends_with(".saving");
}

bool modPathsClash(std::string_view a, std::string_view b)
{
    if (a == b) {
        return false;
    }
    const std::string la = util::lowered(a);
    const std::string lb = util::lowered(b);
    // One file on a disk that ignores case, or a file where the other has a folder.
    return la == lb || lb.starts_with(la + "/") || la.starts_with(lb + "/");
}

std::optional<fs::path> modPathIn(const fs::path& folder, std::string_view rel)
{
    if (!modPathValid(rel)) {
        return std::nullopt;
    }
    try {
        const fs::path p = folder / u8path(rel);
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(folder, ec);
        if (ec) {
            return std::nullopt;
        }
        const fs::path real = fs::weakly_canonical(p, ec);
        if (ec) {
            return std::nullopt;
        }
        const auto [r, _] = std::mismatch(root.begin(), root.end(), real.begin(), real.end());
        if (r != root.end() || real == root) {
            return std::nullopt;
        }
        return p;
    } catch (const std::exception&) {
        return std::nullopt; // (a name the file system can't take: never thrown out to a frame)
    }
}

ModFolderFiles readModFolder(const fs::path& folder)
{
    ModFolderFiles out;
    try {
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) {
            out.error = "no folder " + u8text(folder);
            return out;
        }
        std::size_t total = 0;
        fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::directory_entry& e = *it;
            if (e.is_symlink(ec)) {
                it.disable_recursion_pending(); // (never followed out of the folder)
                continue;
            }
            if (e.is_directory(ec)) {
                if (it.depth() + 1 >= static_cast<int>(kMaxModDepth) ||
                    (it.depth() == 0 && util::equalsNoCase(e.path().filename().string(), modes::kModSavesFolder))) {
                    it.disable_recursion_pending(); // (no file in it could have a path modPathValid takes)
                }
                continue;
            }
            if (!e.is_regular_file(ec)) {
                continue;
            }
            std::string rel;
            try {
                rel = u8text(fs::relative(e.path(), folder, ec));
            } catch (const std::exception&) {
                continue; // (a name that can't be made text: left out)
            }
            if (ec || !modPathValid(rel)) {
                ec.clear();
                continue;
            }
            const std::uintmax_t size = e.file_size(ec);
            if (ec || size > session::kMaxModFile) {
                out.error = rel + " is over " + std::to_string(session::kMaxModFile >> 20) + " MB";
                return out;
            }
            if (out.files.size() >= session::kMaxModFiles || total + size > session::kMaxModBytes) {
                out.error = "the mod is over " + std::to_string(session::kMaxModFiles) + " files or " + std::to_string(session::kMaxModBytes >> 20) + " MB";
                return out;
            }
            if (std::ranges::any_of(out.files, [&](const auto& f) { return modPathsClash(f.first, rel); })) {
                continue; // (a second file of the same name in another case: the first kept)
            }
            std::ifstream in(e.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            total += bytes.size();
            out.files.emplace(rel, std::move(bytes));
        }
        if (ec) {
            out.error = ec.message();
        }
    } catch (const std::exception& ex) {
        out.error = ex.what();
    }
    return out;
}

bool writeModFile(const fs::path& folder, std::string_view rel, std::span<const std::uint8_t> bytes, std::string& error)
{
    try {
        std::error_code ec;
        fs::create_directories(folder, ec);
        const auto p = modPathIn(folder, rel);
        if (!p) {
            error = "\"" + std::string(rel) + "\" isn't a path inside the mod";
            return false;
        }
        fs::create_directories(p->parent_path(), ec);
        if (!modPathIn(folder, rel)) {
            error = "\"" + std::string(rel) + "\" leads out of the mod";
            return false; // (checked again with its folders made: nothing on the way leads out)
        }
        ReplaceFailure why;
        if (!tryReplaceFileBytes(*p, bytes, ".saving", &why)) {
            error = "can't write " + std::string(rel) + (why.moving ? ": " + why.error.message() : std::string());
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string(rel) + ": " + ex.what();
        return false;
    }
}

bool removeModFile(const fs::path& folder, std::string_view rel, std::string& error)
{
    try {
        const auto p = modPathIn(folder, rel);
        if (!p) {
            error = "\"" + std::string(rel) + "\" isn't a path inside the mod";
            return false;
        }
        std::error_code ec;
        if (fs::is_directory(*p, ec)) {
            error = std::string(rel) + " is a folder";
            return false;
        }
        fs::remove(*p, ec);
        if (ec) {
            error = ec.message();
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string(rel) + ": " + ex.what();
        return false;
    }
}

} // namespace nox::mapedit
