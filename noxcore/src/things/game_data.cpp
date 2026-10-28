// gamedata.bin reader and look-ups (format in game_data.hpp). The numbers come only from the
// install's file: every program that runs a game loads it at start-up, as it loads thing.bin.
#include "noxcore/things/game_data.hpp"
#include "noxcore/util/x87.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"

#include <cstdlib>
#include <string>

namespace nox::things {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

/// Words split as the game's tokeniser does (" ,\r\n\t").
std::vector<std::string_view> splitWords(std::string_view s)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (isSpace(s[i]) || s[i] == ',')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < s.size() && !isSpace(s[i]) && s[i] != ',') {
            ++i;
        }
        if (i > start) {
            out.push_back(s.substr(start, i - start));
        }
    }
    return out;
}

/// The names compare without case ("WarCryDuration" finds the file's
/// "WarcryDuration").
bool sameName(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c; };
        if (up(a[i]) != up(b[i])) {
            return false;
        }
    }
    return true;
}

char upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c;
}

} // namespace

std::size_t GameData::NoCaseHash::operator()(std::string_view s) const noexcept
{
    std::size_t h = 14695981039346656037ull; // (FNV-1a over the upper-cased name)
    for (const char c : s) {
        h = (h ^ static_cast<unsigned char>(upper(c))) * 1099511628211ull;
    }
    return h;
}

bool GameData::NoCaseEqual::operator()(std::string_view a, std::string_view b) const noexcept
{
    return sameName(a, b);
}

namespace {

/// picks' column for a game's flags: 1 if solo, + 2 if online.
std::size_t pickOf(std::uint32_t gameFlags)
{
    return ((gameFlags & (0x800u | 0x200000u)) != 0 ? 1u : 0u) | ((gameFlags & 0x2000u) != 0 ? 2u : 0u);
}

std::atomic<std::uint32_t> nextSerial{1};

} // namespace

void GameData::reindex()
{
    byName.clear();
    picks.clear();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto [it, added] = byName.try_emplace(entries[i].name);
        if (added) {
            it->second.slot = static_cast<std::uint32_t>(picks.size());
            picks.emplace_back();
        }
        it->second.entries.push_back(static_cast<std::uint32_t>(i));
    }
    // Each kind of game's answer: the last line standing (a solo game skips the ARENA lines, an
    // online one the SOLO ones).
    for (const auto& [name, named] : byName) {
        for (std::size_t pick = 0; pick < 4; ++pick) {
            const bool solo = (pick & 1u) != 0;
            const bool online = (pick & 2u) != 0;
            std::int32_t found = -1;
            for (auto i = named.entries.rbegin(); i != named.entries.rend() && found < 0; ++i) {
                const Section s = entries[*i].section;
                if (!((solo && s == Section::Arena) || (online && s == Section::Solo))) {
                    found = static_cast<std::int32_t>(*i);
                }
            }
            picks[named.slot][pick] = found;
        }
    }
    serial = nextSerial.fetch_add(1, std::memory_order_relaxed);
    indexed = entries.size();
}

// Without the index (entries were added after reindex) fall back to a full scan.
const std::vector<float>* GameData::arena(std::string_view name) const
{
    // (The last line not SOLO's.)
    if (indexed == entries.size()) {
        const auto it = byName.find(name);
        if (it == byName.end()) {
            return nullptr;
        }
        for (auto i = it->second.entries.rbegin(); i != it->second.entries.rend(); ++i) {
            if (entries[*i].section != Section::Solo) {
                return &entries[*i].values;
            }
        }
        return nullptr;
    }
    const std::vector<float>* found = nullptr;
    for (const auto& e : entries) {
        if (e.section != Section::Solo && sameName(e.name, name)) {
            found = &e.values;
        }
    }
    return found;
}

float GameData::arena(std::string_view name, std::size_t index) const
{
    const auto* values = arena(name);
    return values && index < values->size() ? (*values)[index] : 0.0f;
}

const std::vector<float>* GameData::find(std::string_view name, std::uint32_t gameFlags) const
{
    const bool solo = (gameFlags & (0x800u | 0x200000u)) != 0;
    const bool online = (gameFlags & 0x2000u) != 0;
    if (indexed == entries.size()) { // (the last line standing)
        const auto it = byName.find(name);
        if (it == byName.end()) {
            return nullptr;
        }
        for (auto i = it->second.entries.rbegin(); i != it->second.entries.rend(); ++i) {
            const Entry& e = entries[*i];
            if (!((solo && e.section == Section::Arena) || (online && e.section == Section::Solo))) {
                return &e.values;
            }
        }
        return nullptr;
    }
    const std::vector<float>* found = nullptr;
    for (const auto& e : entries) {
        if (!sameName(e.name, name) || (solo && e.section == Section::Arena) || (online && e.section == Section::Solo)) {
            continue;
        }
        found = &e.values;
    }
    return found;
}

const std::vector<float>* GameData::find(const GameDataKey& key, std::uint32_t gameFlags) const
{
    if (indexed != entries.size()) {
        return find(key.name(), gameFlags);
    }
    // The key's slot in this index: kept from the last look-up while it's this index's.
    std::uint64_t found = key.found_.load(std::memory_order_relaxed);
    if (static_cast<std::uint32_t>(found >> 32) != serial) {
        const auto it = byName.find(key.name());
        found = static_cast<std::uint64_t>(serial) << 32 | (it == byName.end() ? 0u : it->second.slot + 1u);
        key.found_.store(found, std::memory_order_relaxed);
    }
    const auto slot = static_cast<std::uint32_t>(found);
    if (slot == 0) {
        return nullptr;
    }
    const std::int32_t e = picks[slot - 1][pickOf(gameFlags)];
    return e < 0 ? nullptr : &entries[static_cast<std::size_t>(e)].values;
}

float gameNumber(const GameData* data, std::string_view name, std::uint32_t gameFlags, std::size_t index)
{
    // (Game_GetGamedataFloat / gamedataGetFloatTable: a name the table lacks, or an index past its
    // values, reads the constant 0.0.)
    if (!data) {
        return 0.0f;
    }
    const auto* v = data->find(name, gameFlags);
    return v && index < v->size() ? (*v)[index] : 0.0f;
}

float gameNumber(const GameData* data, const GameDataKey& key, std::uint32_t gameFlags, std::size_t index)
{
    if (!data) {
        return 0.0f;
    }
    const auto* v = data->find(key, gameFlags);
    return v && index < v->size() ? (*v)[index] : 0.0f;
}

GameData parseGameData(std::string_view text)
{
    const util::RoundingScope nearest(false); // (the numbers read to nearest whatever thread reads them: util/x87.hpp)
    // Read as the exe reads it, by its tokens rather than its lines: a word (parseString: a
    // run of non-space; "//" anywhere skips the rest of the line), '#' at a word's start skips
    // the rest of the line; a SOLO / ARENA word marks the next; then the name, one word (the
    // '='), and the values: everything up to the ';', across lines (XPTable's
    // run over several), split on " ,\r\n\t" (at most 255; "1.2f" reads as 1.2).
    GameData out;
    std::size_t pos = 0;
    const auto skipLine = [&] {
        const auto nl = text.find('\n', pos);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
    };
    // Next word; nullopt at the end of the text. "//" inside a word drops the rest of the line.
    const auto word = [&]() -> std::optional<std::string> {
        std::string w;
        for (;;) {
            // '\0' counts as a separator for splitWords but not here: it is part of a word.
            while (pos < text.size() && isSpace(text[pos]) && text[pos] != '\0') {
                ++pos;
            }
            if (pos >= text.size()) {
                return std::nullopt; // (the end: a word cut off by it is lost too)
            }
            w.clear();
            bool comment = false;
            while (pos < text.size() && !(isSpace(text[pos]) && text[pos] != '\0')) {
                if (text[pos] == '/' && !w.empty() && w.back() == '/') {
                    skipLine();
                    comment = true;
                    break;
                }
                w.push_back(text[pos++]);
            }
            if (comment) {
                continue;
            }
            if (pos >= text.size()) {
                return std::nullopt;
            }
            return w;
        }
    };
    for (;;) {
        GameData::Entry e;
        std::optional<std::string> name = word();
        // Leading '#' comments and SOLO / ARENA prefixes before the name.
        while (name && (name->front() == '#' || sameName(*name, "SOLO") || sameName(*name, "ARENA"))) {
            if (name->front() == '#') {
                skipLine();
                e.section = GameData::Section::Common;
            } else {
                e.section = sameName(*name, "SOLO") ? GameData::Section::Solo : GameData::Section::Arena;
            }
            name = word();
        }
        // The second word is the '=', which isn't checked.
        if (!name || !word()) {
            break;
        }
        e.name = std::move(*name);
        const auto semi = text.find(';', pos);
        const std::string_view values = text.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = semi == std::string_view::npos ? text.size() : semi + 1;
        for (const std::string_view v : splitWords(values)) {
            if (e.values.size() == 255) {
                break;
            }
            e.values.push_back(std::strtof(std::string(v).c_str(), nullptr));
        }
        out.entries.push_back(std::move(e));
    }
    out.reindex();
    return out;
}

GameData loadGameData(const std::filesystem::path& path)
{
    auto data = readFileBytes(path);
    crypto::cipherForKey(crypto::FileKey::GameDataBin).decrypt(data);
    content::applyContentPacks(content::TableFormat::GameData, data); // (the installed content packs' changes, once, as it loads; none installed: nothing)
    return parseGameData(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

} // namespace nox::things
