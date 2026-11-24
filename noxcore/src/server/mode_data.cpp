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
    if (isLinkStatus(dirStatus) || !fs::is_directory(dirStatus)) {
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
        if (isLinkStatus(st) || !fs::is_regular_file(st) || !resolvesInside(e.path(), root)) {
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

HostTables hostTables(const ModeData* modeData, bool solo, const fs::path& gameDir, const InstallTables* install)
{
    HostTables out;
    std::optional<things::DataTables> mod;
    if (modeData) {
        mod = modeData->tables(solo, gameDir); // (throws: a file that won't read, an override that won't apply)
        out.merged = mod->merged;
    }
    const bool modThings = mod && mod->things;
    // (Each the mod's; else, without the install's given, its file's; else in a mod's game a copy
    // of the install's, so every table the game reads is its own.)
    if (modThings) {
        out.things = std::move(*mod->things);
    } else if (!install) {
        out.things = things::loadThingDb(gameDir / "thing.bin");
    } else if (mod) {
        out.things = *install->things;
    }
    if (mod && mod->modifiers) {
        out.modifiers = std::move(*mod->modifiers);
    } else if (!install) {
        if (fs::exists(resolveNoCase(gameDir / "modifier.bin"))) {
            out.modifiers = things::loadModifierDb(gameDir / "modifier.bin");
        }
    } else if (mod && install->modifiers) {
        out.modifiers = *install->modifiers;
    }
    if (mod && mod->gameData) {
        out.gameData = std::move(*mod->gameData);
    } else if (!install) {
        // (No game runs without the game's numbers: a missing gamedata.bin stops it as a missing
        // thing.bin does.)
        out.gameData = things::loadGameData(gameDir / "gamedata.bin");
    } else if (mod) {
        out.gameData = *install->gameData;
    }
    if (mod && mod->monsters) {
        out.monsters = std::move(*mod->monsters); // (read for this game's numbers: `solo`)
    } else if (fs::exists(resolveNoCase(gameDir / "monster.bin"))) {
        out.monsters = things::loadMonsterDb(gameDir / "monster.bin", solo);
    }
    if (out.monsters) {
        out.monsters->keepThingTypes(out.things ? *out.things : *install->things);
    }
    if (mod && mod->soundSets) {
        out.soundSets = std::move(*mod->soundSets);
    } else if (fs::exists(resolveNoCase(gameDir / "soundset.bin"))) {
        out.soundSets = things::loadSoundSets(gameDir / "soundset.bin");
    }
    if (modThings) {
        // (things::useAddedTypeBases' for a set with a thing table: its table moved out above.)
        things::setAddedTypeBases(std::move(mod->addedBases));
        out.addedBases = true;
    }
    return out;
}

} // namespace nox::server
