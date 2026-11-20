// A mode's own game data (mode_data.hpp).
#include "noxcore/server/mode_data.hpp"

#include "noxcore/crypto/sha256.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/io/zip.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

namespace nox::server {

namespace fs = std::filesystem;

namespace {

/// A symbolic link, or on Windows a junction: never followed out of the folder.
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

/// FNV-1a over each file's kind, size, CRC and SHA-256 (never 0).
std::uint32_t setHash(const std::vector<things::DataFileBytes>& files)
{
    std::uint32_t h = 2166136261u;
    const auto mix = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 16777619u;
        }
    };
    for (const auto& f : files) {
        mix(static_cast<std::uint32_t>(f.kind));
        mix(static_cast<std::uint32_t>(f.bytes.size()));
        mix(f.crc);
        for (const std::uint8_t b : f.digest) {
            h = (h ^ b) * 16777619u;
        }
    }
    return h == 0 ? 1u : h;
}

} // namespace

std::shared_ptr<const ModeData> ModeData::fromFiles(std::vector<things::DataFileBytes> files, std::string& why)
{
    why.clear();
    std::ranges::sort(files, {}, &things::DataFileBytes::kind);
    std::size_t total = 0;
    // Each table once, whichever form: a whole file's kind and its override's aren't neighbours
    // once sorted (thing.bin 0, modifier.bin 1 ... thing.patch 5), so each table's first is kept.
    std::array<const things::DataFileBytes*, things::kDataTableCount> table{};
    for (auto& f : files) {
        const std::string name(things::dataFileName(f.kind));
        const things::DataFileBytes*& first = table[static_cast<std::size_t>(things::dataTableOf(f.kind))];
        if (first) {
            why = "data\\" + name + ": " + std::string(things::dataFileName(first->kind)) + " is there too (a file's whole form or its override, not both)";
            return nullptr;
        }
        first = &f;
        if (std::string bad = things::checkDataFile(f.kind, f.bytes); !bad.empty()) {
            why = "data\\" + name + ": " + bad;
            return nullptr;
        }
        total += f.bytes.size();
        f.crc = io::crc32(f.bytes);
        f.digest = crypto::sha256(f.bytes);
    }
    if (total > things::kMaxDataFilesBytes) {
        why = "data\\: over " + std::to_string(things::kMaxDataFilesBytes >> 20) + " MB of files";
        return nullptr;
    }
    if (files.empty()) {
        return nullptr;
    }
    auto out = std::make_shared<ModeData>();
    out->files_ = std::move(files);
    out->hash_ = setHash(out->files_);
    return out;
}

std::shared_ptr<const ModeData> ModeData::read(const fs::path& dir, std::string& why, std::vector<std::string>* notes)
{
    why.clear();
    std::error_code ec;
    const fs::file_status dirStatus = fs::symlink_status(dir, ec);
    if (ec || !fs::exists(dirStatus)) {
        return nullptr; // (no data\: the install's files, as any mod's)
    }
    if (isLink(dirStatus) || !fs::is_directory(dirStatus)) {
        why = "data\\ isn't a folder of the mod's own";
        return nullptr;
    }
    const fs::path root = fs::canonical(dir, ec);
    if (ec) {
        why = "data\\ can't be read";
        return nullptr;
    }
    std::vector<things::DataFileBytes> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name;
        try {
            name = pathUtf8(e.path().filename());
        } catch (const std::exception&) {
            name = "?";
        }
        const fs::file_status st = e.symlink_status(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto kind = things::dataFileNamed(name);
        if (!kind) {
            if (notes) {
                notes->push_back("data\\" + name + " left out: not one of the game's data files (thing.bin, modifier.bin, "
                                 "gamedata.bin, monster.bin, soundset.bin)");
            }
            continue;
        }
        if (isLink(st) || !fs::is_regular_file(st) || !resolvesInside(e.path(), root)) {
            why = "data\\" + name + " isn't a file of the mod's own (a link, or a folder)";
            return nullptr;
        }
        const std::uintmax_t size = fs::file_size(e.path(), ec);
        if (ec) {
            why = "data\\" + name + " can't be read";
            return nullptr;
        }
        if (size > things::dataFileMaxBytes(*kind)) {
            why = "data\\" + name + ": over " + std::to_string(things::dataFileMaxBytes(*kind) >> 20) + " MB";
            return nullptr;
        }
        things::DataFileBytes f;
        f.kind = *kind;
        try {
            f.bytes = readFileBytes(e.path());
        } catch (const std::exception&) {
            why = "data\\" + name + " can't be read";
            return nullptr;
        }
        files.push_back(std::move(f));
    }
    return fromFiles(std::move(files), why);
}

const things::DataFileBytes* ModeData::find(things::DataFile kind) const
{
    for (const auto& f : files_) {
        if (f.kind == kind) {
            return &f;
        }
    }
    return nullptr;
}

std::size_t ModeData::bytes() const noexcept
{
    std::size_t n = 0;
    for (const auto& f : files_) {
        n += f.bytes.size();
    }
    return n;
}

things::DataTables ModeData::tables(bool solo, const fs::path& installDir) const
{
    return things::readDataTables(files_, solo, installDir);
}

} // namespace nox::server
