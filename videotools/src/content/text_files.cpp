// The host's text files, checked by NoX Core's own readers (text_files.hpp).
#include "content/text_files.hpp"

#include "noxcore/gui/window_file.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/server/map_cycle.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/rules.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::content {

namespace {

using nox::util::equalsNoCase;
using nox::util::lowered;

/// A scratch folder for a reader that takes files (removed when it goes).
struct Scratch {
    fs::path dir;
    explicit Scratch(const char* name) : dir(fs::temp_directory_path() / name)
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const fs::path& rel, std::string_view text) const
    {
        const fs::path p = dir / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
        return p;
    }
};

/// Up to `most` names, then how many more.
std::string listOf(const std::vector<std::string>& names, std::size_t most = 8)
{
    if (names.empty()) {
        return "none";
    }
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < most; ++i) {
        out += (i ? ", " : "") + names[i];
    }
    if (names.size() > most) {
        out += std::format(" and {} more", names.size() - most);
    }
    return out;
}

// The modes a rule set has sections for, in the order the game's menus list them.
constexpr std::uint32_t kRuleModes[] = {0x400, 0x100, 0x20, 0x10, 0x40, 0x1000};

TextReport checkRules(std::string_view text, const GameFiles& files)
{
    TextReport r;
    const auto* things = files.things();
    if (!things) {
        r.problems.push_back("thing.bin isn't loaded: the spells can't be checked");
        return r;
    }
    // readRules reads a map folder's file: this text as a rule set of a scratch map's.
    const Scratch s("noxcontent_rules");
    s.write(fs::path("maps") / "NOXCHECK" / "check.rul", text);
    std::vector<std::string> sections{*nox::server::ruleSection(0x17F0)}; // ([COMMON]: every mode's)
    for (const std::uint32_t mode : kRuleModes) {
        const auto header = nox::server::ruleSection(mode);
        if (!header) {
            continue;
        }
        sections.push_back(*header);
        const std::string name = header->substr(1, header->size() - 2); // ("[DEATHMATCH]" -> DEATHMATCH)
        const nox::server::RuleSet rules = nox::server::readRules(s.dir / "maps", "NOXCHECK", "check.rul", 2, mode, *things);
        std::vector<std::string> spells, weapons, armour;
        for (std::uint32_t id = 1; id < 160; ++id) {
            if (const std::string_view n = nox::things::spellName(id); !n.empty() && things->findSpell(id) && !rules.spellAllowed(int(id))) {
                spells.emplace_back(n);
            }
        }
        for (const auto& w : nox::server::ruleWeapons()) {
            if (!(rules.weapons & w.bit)) weapons.emplace_back(w.thing);
        }
        for (const auto& a : nox::server::ruleArmour()) {
            if (!(rules.armor & a.bit)) armour.emplace_back(a.thing);
        }
        r.facts.emplace_back(name, std::format("spells off: {}; weapons off: {}; armour off: {}", listOf(spells), listOf(weapons), listOf(armour)));
    }
    // A section the game has no mode for is skipped whole.
    std::string_view rest = text;
    int lineNo = 0;
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        const std::string_view line = nox::util::trim(rest.substr(0, nl));
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        ++lineNo;
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            if (std::ranges::none_of(sections, [&](const std::string& known) { return equalsNoCase(known, line); })) {
                r.problems.push_back(std::format("line {}: {} isn't a section the game knows: the lines after it still count for the section before "
                                                 "it (the sections: {})",
                                                 lineNo, line, listOf(sections, 10)));
            }
        }
    }
    return r;
}

/// A map's modes as the host finds them (maps\<name>\<name>.map, its type list as game flags;
/// 0 when it isn't there).
std::uint32_t mapModes(const GameFiles& files, const std::string& name, std::map<std::string, std::uint32_t>& cache)
{
    const std::string key = lowered(name);
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    std::uint32_t modes = 0;
    for (const fs::path& m : files.maps()) {
        if (equalsNoCase(m.stem().string(), name) && equalsNoCase(m.extension().string(), ".map")) {
            try {
                const auto file = nox::map::loadMapFile(m);
                modes = file.find(nox::map::MapInfo::kName) ? nox::server::mapToGameFlags(nox::map::decodeSection<nox::map::MapInfo>(file).gameFlags) : 0;
            } catch (const std::exception&) {
                modes = 0;
            }
            break;
        }
    }
    cache[key] = modes;
    return modes;
}

TextReport checkMapCycle(std::string_view text, const GameFiles& files)
{
    TextReport r;
    static constexpr std::pair<const char*, std::uint32_t> kSections[6] = {
        {"[ELIMINATION]", 0x400}, {"[DEATHMATCH]", 0x100}, {"[CAPTURE THE FLAG]", 0x20},
        {"[KING OF THE REALM]", 0x10}, {"[FLAGBALL]", 0x40}, {"[QUEST]", 0x1000},
    };
    std::map<std::string, std::uint32_t> cache;
    const auto modesOf = [&](const std::string& name) { return mapModes(files, name, cache); };
    const Scratch s("noxcontent_mapcycle");
    nox::server::MapCycle cycle;
    cycle.load(s.write("mapcycle.txt", text), modesOf);
    for (std::size_t i = 0; i < 6; ++i) {
        r.facts.emplace_back(kSections[i].first, listOf(cycle.sections()[i], 25));
    }
    // What it drops, line by line (as MapCycle::load walks it: lines before a header are the deathmatch list's,
    // the first line is a header or a list of its own).
    int section = 1;
    int lineNo = 0;
    std::array<int, 6> count{};
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        std::string line(rest.substr(0, nl));
        rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
        ++lineNo;
        if (const auto cr = line.find('\r'); cr != std::string::npos) {
            line.resize(cr);
        }
        const auto header = std::ranges::find_if(kSections, [&](const auto& h) { return equalsNoCase(line, h.first); });
        if (header != std::end(kSections)) {
            section = int(header - std::begin(kSections));
            continue;
        }
        if (lineNo == 1) {
            continue; // (kept aside by the reader)
        }
        const std::string name = line.substr(0, line.find('.'));
        if (line.empty() || name.empty()) {
            continue;
        }
        if (count[std::size_t(section)] >= 25) {
            r.problems.push_back(std::format("line {}: {} is past the 25 maps a section holds: dropped", lineNo, name));
            continue;
        }
        const std::uint32_t modes = modesOf(name);
        if (modes == 0) {
            r.problems.push_back(std::format("line {}: there's no map {} (maps\\{}\\{}.map): dropped", lineNo, name, name, name));
        } else if (!(modes & kSections[std::size_t(section)].second)) {
            r.problems.push_back(std::format("line {}: {} isn't a {} map: dropped", lineNo, name, kSections[std::size_t(section)].first));
        } else {
            ++count[std::size_t(section)];
        }
    }
    if (!text.empty() && text.find('[') == std::string_view::npos) {
        r.problems.push_back("no section headers: every map is read as a deathmatch one (and the first line is left out)");
    }
    return r;
}

TextReport checkMotd(std::string_view text)
{
    TextReport r;
    const auto lines = std::ranges::count(text, '\n') + (text.empty() || text.back() == '\n' ? 0 : 1);
    r.facts.emplace_back("Sent", "whole, to each player who joins a network game (not a quest), when the host sends it");
    r.facts.emplace_back("Size", std::format("{} bytes, {} lines", text.size(), lines));
    if (const auto z = text.find('\0'); z != std::string_view::npos) {
        r.problems.push_back(std::format("a zero byte at {}: the players' games stop reading there", z));
    }
    return r;
}

void windowFacts(const nox::gui::WindowDef& w, const GameFiles& files, TextReport& r, int& count)
{
    ++count;
    if (w.type == nox::gui::WindowType::Unknown) {
        r.problems.push_back(std::format("window {}: type \"{}\" isn't one the game makes", w.id, w.typeName));
    }
    const auto* strings = files.strings();
    for (const std::string* label : {&w.text, &w.tooltip}) {
        if (!label->empty() && strings && !strings->find(*label)) {
            r.problems.push_back(std::format("window {}: nox.csf has no {} (the game shows the label itself)", w.id, *label));
        }
    }
    for (const auto& c : w.children) {
        windowFacts(c, files, r, count);
    }
}

TextReport checkWindow(std::string_view text, const GameFiles& files)
{
    TextReport r;
    try {
        const nox::gui::WindowDef root = nox::gui::parseWindowFile(text);
        int count = 0;
        windowFacts(root, files, r, count);
        r.facts.emplace_back("Windows", std::format("{} (the top one {}, {} x {})", count, root.id, root.width, root.height));
    } catch (const std::exception& ex) {
        r.problems.push_back(std::string("the game can't read it: ") + ex.what());
    }
    return r;
}

TextReport checkManifest(std::string_view text, const fs::path& dir)
{
    TextReport r;
    const auto read = nox::modes::script::readManifest(text);
    if (!read.manifest) {
        r.problems.push_back("NoX Core won't list this mode: " + read.error);
        return r;
    }
    const auto& m = *read.manifest;
    r.facts.emplace_back("Mode", std::format("{} (\"{}\")", m.info.name, m.info.id));
    if (!m.info.description.empty()) {
        r.facts.emplace_back("Description", m.info.description);
    }
    r.facts.emplace_back("Players", std::format("{} to {}", m.info.minPlayers, m.info.maxPlayers));
    r.facts.emplace_back("Stock players", m.info.stockClients ? "can join (GAME.EXE)" : "can't: NoX Core players only");
    r.facts.emplace_back("Server script", m.serverScript);
    r.facts.emplace_back("UI script", m.uiScript.empty() ? std::string("none (text only)") : m.uiScript);
    if (!dir.empty()) {
        for (const std::string* script : {&m.serverScript, &m.uiScript}) {
            std::error_code ec;
            if (!script->empty() && !fs::exists(dir / fs::path(*script), ec)) {
                r.problems.push_back(std::format("{} isn't in the mod's folder", *script));
            }
        }
    }
    return r;
}

} // namespace

TextKind textKindOf(const fs::path& file)
{
    const std::string name = lowered(file.filename().string());
    const std::string ext = lowered(file.extension().string());
    if (ext == ".rul") return TextKind::Rules;
    if (ext == ".wnd") return TextKind::Window;
    if (name == "mapcycle.txt") return TextKind::MapCycle;
    if (name == "motd.txt") return TextKind::Motd;
    if (name == "mode.json") return TextKind::ModManifest;
    return TextKind::Other;
}

const char* textKindName(TextKind kind)
{
    switch (kind) {
    case TextKind::Rules: return "rule set";
    case TextKind::MapCycle: return "map cycle";
    case TextKind::Motd: return "message of the day";
    case TextKind::Window: return "window file";
    case TextKind::ModManifest: return "game mode's manifest";
    case TextKind::Other: return "text";
    }
    return "text";
}

TextReport checkText(TextKind kind, std::string_view text, const GameFiles& files, const fs::path& file)
{
    switch (kind) {
    case TextKind::Rules: return checkRules(text, files);
    case TextKind::MapCycle: return checkMapCycle(text, files);
    case TextKind::Motd: return checkMotd(text);
    case TextKind::Window: return checkWindow(text, files);
    case TextKind::ModManifest: return checkManifest(text, file.parent_path());
    case TextKind::Other: break;
    }
    return {};
}

} // namespace videotools::content
