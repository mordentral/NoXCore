// A mod's own game data (data_files.hpp).
#include "noxcore/things/data_files.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/content/text_table.hpp"
#include "noxcore/content/thing_patch.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/io/zip.hpp"
#include "noxcore/util/text.hpp"

#include <array>
#include <exception>
#include <map>
#include <stdexcept>

namespace nox::things {

namespace {

struct FileInfo {
    std::string_view name;
    std::size_t maxBytes;
    bool client;
};

constexpr std::array<FileInfo, kDataFileCount> kFiles{{
    {"thing.bin", 8u << 20, true},
    {"modifier.bin", 1u << 20, true},
    {"gamedata.bin", 1u << 20, true},
    {"monster.bin", 1u << 20, false},
    {"soundset.bin", 1u << 20, false},
    {"thing.patch", 1u << 20, true},
    {"modifier.patch", 1u << 20, true},
    {"gamedata.patch", 1u << 20, true},
    {"monster.patch", 1u << 20, false},
    {"soundset.patch", 1u << 20, false},
}};

const FileInfo& info(DataFile file) noexcept
{
    const auto i = static_cast<std::size_t>(file);
    return kFiles[i < kFiles.size() ? i : 0];
}

/// The text table a game file is (the four text ones).
content::TableFormat tableFormat(DataFile table)
{
    switch (table) {
    case DataFile::Modifiers: return content::TableFormat::Modifier;
    case DataFile::GameData: return content::TableFormat::GameData;
    case DataFile::Monsters: return content::TableFormat::Monster;
    default: return content::TableFormat::SoundSet;
    }
}

/// The check of a merged thing.bin: every type's name, lines, DRAW and pictures (content packs'
/// hash over them all), folded to 32 bits, never 0.
std::uint32_t thingsCheck(const ThingDb& db)
{
    const std::uint64_t h = content::newThingTypesHash(db, 0);
    const auto v = static_cast<std::uint32_t>(h ^ (h >> 32));
    return v == 0 ? 1u : v;
}

/// The table read from a game file's decrypted text into its slot of `out`; "" or why not.
std::string readText(DataFile table, std::string_view text, bool solo, DataTables& out)
{
    switch (table) {
    case DataFile::Modifiers: {
        ModifierDb db = parseModifierDb(text);
        if (db.weapons.empty() && db.armor.empty() && db.modifiers.empty()) {
            return "no records";
        }
        out.modifiers = std::move(db);
        return {};
    }
    case DataFile::GameData: {
        GameData data = parseGameData(text);
        if (data.entries.empty()) {
            return "no records";
        }
        out.gameData = std::move(data);
        return {};
    }
    case DataFile::Monsters: {
        MonsterDb db = parseMonsterDb(text, solo);
        if (db.monsters.empty()) {
            return "no records";
        }
        out.monsters = std::move(db);
        return {};
    }
    case DataFile::SoundSets: {
        SoundSets sets = parseSoundSets(text);
        if (sets.sets.empty()) {
            return "no records";
        }
        out.soundSets = std::move(sets);
        return {};
    }
    default:
        return "not a text table";
    }
}

/// The thing.bin read into `out`; "" or why not.
std::string readThings(ThingDb db, DataTables& out)
{
    if (db.things.empty()) {
        return "no object types";
    }
    if (db.things.size() > content::kMaxThingTypes) {
        return "more than " + std::to_string(content::kMaxThingTypes) + " object types";
    }
    out.things = std::move(db);
    return {};
}

/// A whole file read into `out` (no content pack); "" or why not.
std::string readWhole(DataFile file, std::vector<std::uint8_t> bytes, bool solo, DataTables& out)
{
    try {
        if (file == DataFile::Things) {
            return readThings(thingDbFromFile(std::move(bytes), false), out);
        }
        crypto::cipherForKey(content::tableKey(tableFormat(file))).decrypt(bytes);
        return readText(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), solo, out);
    } catch (const std::exception& ex) {
        return ex.what();
    }
}

/// An override read alone (its structure); "" or its first problem.
std::string checkPatch(DataFile file, std::span<const std::uint8_t> bytes)
{
    const std::string text(bytes.begin(), bytes.end());
    try {
        if (dataTableOf(file) == DataFile::Things) {
            std::vector<std::string> problems;
            const content::ThingPatch patch = content::parseThingPatch(text, &problems);
            if (!problems.empty()) {
                return problems.front();
            }
            if (patch.records.empty()) {
                return "no records";
            }
            return {};
        }
        const content::TableFormat format = tableFormat(dataTableOf(file));
        const content::TextTable patch = content::TextTable::parsePatch(format, text);
        if (format == content::TableFormat::GameData) {
            // (gamedata.bin is one record, its entries the fields)
            return patch.records().empty() || patch.records().front().fields.empty() ? "no records" : "";
        }
        if (patch.records().empty()) {
            return "no records";
        }
        // Every record (and modifier.bin's every section) closed by its END, the last line one:
        // the readers take a file cut short as far as it goes, an override mustn't be.
        std::size_t ends = 0;
        std::size_t sections = 0;
        std::string last;
        std::size_t at = 0;
        while (at <= text.size()) {
            const std::size_t nl = std::min(text.find('\n', at), text.size());
            std::string_view line = util::trim(std::string_view(text).substr(at, nl - at));
            at = nl + 1;
            if (line.empty() || line.starts_with('#') || line.starts_with("//")) {
                continue;
            }
            last = std::string(line);
            ends += util::equalsNoCase(line, "END") ? 1 : 0;
            if (format == content::TableFormat::Modifier) {
                for (const std::string_view s : {"WEAPON_DEFINITIONS", "ARMOR_DEFINITIONS", "EFFECTIVENESS", "MATERIAL", "ENCHANTMENT"}) {
                    sections += util::equalsNoCase(line, s) ? 1 : 0;
                }
            }
        }
        if (ends < patch.records().size() + sections || !util::equalsNoCase(last, "END")) {
            return "a record without its END";
        }
        return {};
    } catch (const std::exception& ex) {
        return ex.what();
    }
}

/// The install's file of `table`, its override applied, into `out` with its check; "" or why not.
std::string mergeDataFile(DataFile table, std::span<const std::uint8_t> patchBytes, const std::filesystem::path& installDir, bool solo,
                          std::uint32_t expected, DataTables& out)
{
    const std::string name(dataFileName(table));
    std::vector<std::uint8_t> base;
    try {
        base = readFileBytes(resolveNoCase(installDir / name));
    } catch (const std::exception&) {
        return "the install's " + name + " can't be read";
    }
    const std::string text(patchBytes.begin(), patchBytes.end());
    try {
        std::uint32_t check = 0;
        if (table == DataFile::Things) {
            ThingDb db = thingDbFromFile(std::move(base), false);
            std::vector<std::string> problems;
            const content::ThingPatch patch = content::parseThingPatch(text, &problems);
            if (!problems.empty()) {
                return problems.front();
            }
            const content::ThingPatchResult r = content::applyThingPatch(db, patch, true);
            if (!r.problems.empty()) {
                return r.problems.front();
            }
            // Each new type's stock type: its LIKE, or that one's when it names a type added before it.
            std::map<std::string, std::string, std::less<>> added;
            for (const auto& rec : patch.records) {
                if (!rec.isNew()) {
                    continue;
                }
                const auto earlier = added.find(util::lowered(rec.like));
                const std::string stock = earlier != added.end() ? earlier->second : rec.like;
                added[util::lowered(rec.name)] = stock;
                out.addedBases.emplace_back(rec.name, stock);
            }
            check = thingsCheck(db);
            if (std::string why = readThings(std::move(db), out); !why.empty()) {
                return why;
            }
        } else {
            const content::TableFormat format = tableFormat(table);
            content::TextTable merged = content::TextTable::load(format, base);
            const content::TextTable patch = content::TextTable::parsePatch(format, text);
            const content::PatchResult r = content::applyPatch(merged, patch);
            if (!r.problems.empty()) {
                return r.problems.front();
            }
            const std::string& plain = merged.text();
            check = io::crc32(std::span(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()));
            check = check == 0 ? 1u : check;
            if (std::string why = readText(table, plain, solo, out); !why.empty()) {
                return why;
            }
        }
        if (expected != 0 && expected != check) {
            return "it gives another " + name + " over this install's than over the host's (the two installs' " + name + " differ)";
        }
        out.merged[static_cast<std::size_t>(table)] = check;
        return {};
    } catch (const std::exception& ex) {
        return ex.what();
    }
}

} // namespace

std::string_view dataFileName(DataFile file) noexcept
{
    return info(file).name;
}

std::optional<DataFile> dataFileNamed(std::string_view name) noexcept
{
    for (std::size_t i = 0; i < kFiles.size(); ++i) {
        if (util::equalsNoCase(name, kFiles[i].name)) {
            return static_cast<DataFile>(i);
        }
    }
    return std::nullopt;
}

bool isDataPatch(DataFile file) noexcept
{
    return static_cast<std::size_t>(file) >= kDataTableCount;
}

DataFile dataTableOf(DataFile file) noexcept
{
    return isDataPatch(file) ? static_cast<DataFile>(static_cast<std::size_t>(file) - kDataTableCount) : file;
}

std::size_t dataFileMaxBytes(DataFile file) noexcept
{
    return info(file).maxBytes;
}

bool clientReadsDataFile(DataFile file) noexcept
{
    return info(file).client;
}

std::string checkDataFile(DataFile file, std::span<const std::uint8_t> bytes)
{
    if (static_cast<std::size_t>(file) >= kDataFileCount) {
        return "not a game data file";
    }
    if (bytes.empty()) {
        return "empty";
    }
    if (bytes.size() > info(file).maxBytes) {
        return "over " + std::to_string(info(file).maxBytes >> 20) + " MB";
    }
    if (isDataPatch(file)) {
        return checkPatch(file, bytes);
    }
    if (bytes.size() % 8 != 0) {
        return "not whole cipher blocks (its size isn't a multiple of 8)";
    }
    DataTables scratch;
    return readWhole(file, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), false, scratch);
}

DataTables readDataTables(std::span<const DataFileBytes> files, bool solo, const std::filesystem::path& installDir, bool clientOnly)
{
    DataTables out;
    for (const DataFileBytes& f : files) {
        if (static_cast<std::size_t>(f.kind) >= kDataFileCount) {
            throw std::runtime_error("a game data file of no known kind");
        }
        if (clientOnly && !clientReadsDataFile(f.kind)) {
            continue;
        }
        std::string why = checkDataFile(f.kind, f.bytes);
        if (why.empty()) {
            why = isDataPatch(f.kind) ? mergeDataFile(dataTableOf(f.kind), f.bytes, installDir, solo, f.merged, out)
                                      : readWhole(f.kind, f.bytes, solo, out);
        }
        if (!why.empty()) {
            throw std::runtime_error(std::string(dataFileName(f.kind)) + ": " + why);
        }
    }
    return out;
}

void useAddedTypeBases(const DataTables& tables)
{
    if (tables.things) {
        setAddedTypeBases(tables.addedBases);
    }
}

} // namespace nox::things
