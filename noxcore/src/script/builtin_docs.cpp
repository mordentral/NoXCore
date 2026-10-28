// The builtins' documentation (builtin_docs.hpp): builtin_docs.txt and builtin_usage.txt parsed
// and checked against the compiler's table, and the usage report made from the stock maps.
#include "noxcore/script/builtin_docs.hpp"

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/script/builtins.hpp"
#include "noxcore/script/decompiler.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/script/script_file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

// The two files' bytes (NoxEmbedFile.cmake, noxcore/CMakeLists.txt).
extern const unsigned char kBuiltinDocsText[];
extern const std::size_t kBuiltinDocsTextSize;
extern const unsigned char kBuiltinUsageText[];
extern const std::size_t kBuiltinUsageTextSize;

namespace nox::script {

namespace {

constexpr std::array<std::string_view, 23> kCategories{
    "walls",  "timers",   "objects", "waypoints", "groups",   "movement", "creatures", "inventory",
    "combat", "health",   "spells",  "enchants",  "owners",   "sound",    "messages",  "dialog",
    "quest",  "players",  "triggers", "math",     "strings",  "game",     "unused"};

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> splitLines(std::string_view text)
{
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return lines;
}

std::vector<std::string> splitList(std::string_view s, char sep)
{
    std::vector<std::string> out;
    while (true) {
        const std::size_t at = s.find(sep);
        const std::string_view part = trim(s.substr(0, at));
        if (!part.empty()) out.emplace_back(part);
        if (at == std::string_view::npos) break;
        s.remove_prefix(at + 1);
    }
    return out;
}

bool isIdent(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

struct ParsedProto {
    std::string returns;
    std::string name;
    std::vector<std::pair<std::string, std::string>> args; ///< type, name
    bool ok = false;
};

ParsedProto parseProto(std::string_view p)
{
    ParsedProto r;
    p = trim(p);
    const std::size_t open = p.find('(');
    const std::size_t close = p.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open || !trim(p.substr(close + 1)).empty()) {
        return r;
    }
    const std::string_view head = trim(p.substr(0, open));
    const std::size_t space = head.find(' ');
    if (space == std::string_view::npos) {
        return r;
    }
    r.returns = std::string(trim(head.substr(0, space)));
    r.name = std::string(trim(head.substr(space + 1)));
    const std::string_view inside = trim(p.substr(open + 1, close - open - 1));
    if (!inside.empty()) {
        for (const std::string& a : splitList(inside, ',')) {
            const std::size_t sp = a.find(' ');
            if (sp == std::string::npos) {
                r.args.emplace_back(a, std::string());
            } else {
                r.args.emplace_back(std::string(trim(std::string_view(a).substr(0, sp))), std::string(trim(std::string_view(a).substr(sp + 1))));
            }
        }
    }
    r.ok = true;
    return r;
}

std::string typePrototype(const std::string& returns, const std::string& name, const std::vector<std::string>& types)
{
    std::string s = returns + " " + name + "(";
    for (std::size_t i = 0; i < types.size(); ++i) {
        s += (i ? ", " : "") + types[i];
    }
    return s + ")";
}

/// The example: as written, without the blank lines around it.
std::string verbatim(const std::vector<std::string_view>& lines)
{
    std::size_t b = 0, e = lines.size();
    while (b < e && trim(lines[b]).empty()) ++b;
    while (e > b && trim(lines[e - 1]).empty()) --e;
    std::string out;
    for (std::size_t i = b; i < e; ++i) {
        out += std::string(lines[i]);
        if (i + 1 < e) out += '\n';
    }
    return out;
}

std::string joinLines(const std::vector<std::string_view>& lines)
{
    std::string text;
    for (const auto l : lines) {
        text += std::string(l) + "\n";
    }
    return joinParagraphs(text);
}

} // namespace

std::string joinParagraphs(std::string_view text)
{
    std::string out;
    bool inPara = false; // a line of text is open (soft-wrapped lines join it)
    bool gap = false;    // a blank line since the last text
    for (const auto raw : splitLines(text)) {
        const std::string_view line = trim(raw);
        if (line.empty()) {
            gap = !out.empty();
            inPara = false;
            continue;
        }
        const bool bullet = line.starts_with("- ");
        if (inPara && !bullet && !gap) {
            out += ' ';
        } else if (!out.empty()) {
            out += gap ? "\n\n" : "\n";
        }
        out += std::string(line);
        inPara = true;
        gap = false;
    }
    return out;
}

std::string stripParamNames(std::string_view prototype)
{
    const ParsedProto p = parseProto(prototype);
    if (!p.ok) {
        return std::string(prototype);
    }
    std::vector<std::string> types;
    for (const auto& [type, name] : p.args) types.push_back(type);
    return typePrototype(p.returns, p.name, types);
}

std::string builtinTypePrototype(std::uint32_t id)
{
    const auto& sig = builtinSignature(id);
    std::vector<std::string> types;
    for (const Type t : sig.args) types.emplace_back(typeName(t));
    return typePrototype(std::string(typeName(sig.returns)), std::string(sig.name), types);
}

BuiltinDocSet parseBuiltinDocs(std::string_view docsText, std::string_view usageText)
{
    BuiltinDocSet set;
    set.docs.resize(kBuiltinCount);
    const auto error = [&](std::size_t line, const std::string& what) {
        set.errors.push_back("builtin_docs.txt:" + std::to_string(line) + ": " + what);
    };

    // The entries, each finished when the next begins.
    struct Raw {
        std::size_t line = 0;
        std::string name;
        std::map<std::string, std::string> keys;
        std::map<std::string, std::vector<std::string_view>> sections;
    };
    std::vector<Raw> raws;
    std::string section;
    const auto lines = splitLines(docsText);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (line.starts_with("== ")) {
            raws.push_back({i + 1, std::string(trim(line.substr(3))), {}, {}});
            section.clear();
            continue;
        }
        if (raws.empty()) {
            if (!trim(line).empty() && !line.starts_with("#")) error(i + 1, "text before the first entry");
            continue;
        }
        Raw& r = raws.back();
        if (line.starts_with("[") && trim(line).ends_with("]")) {
            section = std::string(trim(line).substr(1, trim(line).size() - 2));
            static const std::set<std::string, std::less<>> known{"description", "params", "returns", "edge", "example"};
            if (!known.contains(section)) error(i + 1, "unknown section [" + section + "]");
            if (r.sections.contains(section)) error(i + 1, "[" + section + "] twice");
            r.sections[section];
            continue;
        }
        if (section.empty()) {
            if (trim(line).empty()) continue;
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                error(i + 1, "expected key: value");
                continue;
            }
            const std::string key(trim(line.substr(0, colon)));
            static const std::set<std::string, std::less<>> keys{"proto", "category", "related", "short"};
            if (!keys.contains(key)) error(i + 1, "unknown key " + key);
            r.keys[key] = std::string(trim(line.substr(colon + 1)));
            continue;
        }
        r.sections[section].push_back(line);
    }

    for (const Raw& r : raws) {
        const auto id = findBuiltin(r.name);
        if (!id || builtinSignature(*id).name != r.name) {
            error(r.line, "no builtin " + r.name);
            continue;
        }
        BuiltinDoc& d = set.docs[*id];
        if (!d.name.empty()) {
            error(r.line, r.name + " documented twice");
            continue;
        }
        d.id = *id;
        d.name = r.name;
        const auto key = [&](const char* k) {
            const auto it = r.keys.find(k);
            return it == r.keys.end() ? std::string() : it->second;
        };
        const auto sectionLines = [&](const char* s) {
            const auto it = r.sections.find(s);
            if (it == r.sections.end()) {
                error(r.line, r.name + " has no [" + std::string(s) + "]");
                return std::vector<std::string_view>{};
            }
            return it->second;
        };
        d.prototype = key("proto");
        d.category = key("category");
        d.related = splitList(key("related"), ',');
        d.shortText = key("short");
        d.description = joinLines(sectionLines("description"));
        d.returns = joinLines(sectionLines("returns"));
        d.edge = joinLines(sectionLines("edge"));
        d.example = verbatim(sectionLines("example"));

        const ParsedProto p = parseProto(d.prototype);
        if (!p.ok || p.name != d.name) {
            error(r.line, r.name + ": bad proto \"" + d.prototype + "\"");
        } else if (stripParamNames(d.prototype) != builtinTypePrototype(*id)) {
            error(r.line, r.name + ": proto \"" + d.prototype + "\" isn't the compiler's " + builtinTypePrototype(*id));
        }
        // The parameters: "- name: text", soft-wrapped lines joined.
        std::vector<std::pair<std::string, std::string>> params;
        for (const auto l : sectionLines("params")) {
            const std::string_view t = trim(l);
            if (t.empty()) continue;
            if (t.starts_with("- ")) {
                const std::size_t colon = t.find(':');
                if (colon == std::string_view::npos) {
                    error(r.line, r.name + ": parameter line without a colon");
                    continue;
                }
                params.emplace_back(std::string(trim(t.substr(2, colon - 2))), std::string(trim(t.substr(colon + 1))));
            } else if (!params.empty()) {
                params.back().second += " " + std::string(t);
            } else {
                error(r.line, r.name + ": [params] text before the first \"- name:\"");
            }
        }
        if (p.ok) {
            if (params.size() != p.args.size()) {
                error(r.line, r.name + ": " + std::to_string(params.size()) + " parameters described, the proto has " + std::to_string(p.args.size()));
            }
            for (std::size_t i = 0; i < p.args.size(); ++i) {
                const std::string& pname = p.args[i].second;
                if (pname.empty()) error(r.line, r.name + ": proto parameter " + std::to_string(i + 1) + " has no name");
                std::string text;
                if (i < params.size()) {
                    if (params[i].first != pname) error(r.line, r.name + ": parameter " + params[i].first + " isn't the proto's " + pname);
                    text = params[i].second;
                }
                d.params.push_back({p.args[i].first, pname, text});
            }
        }
        if (d.shortText.empty()) error(r.line, r.name + " has no short text");
        if (d.description.empty()) error(r.line, r.name + " has an empty [description]");
        if (d.returns.empty()) error(r.line, r.name + " has an empty [returns]");
        if (d.example.empty()) error(r.line, r.name + " has an empty [example]");
        if (std::ranges::find(kCategories, d.category) == kCategories.end()) error(r.line, r.name + ": unknown category \"" + d.category + "\"");
        for (const std::string& rel : d.related) {
            const auto rid = findBuiltin(rel);
            if (!rid || builtinSignature(*rid).name != rel || *rid == *id) error(r.line, r.name + ": related " + rel + " isn't another builtin");
        }
    }
    for (std::uint32_t id = 0; id < kBuiltinCount; ++id) {
        if (set.docs[id].name.empty()) {
            set.errors.push_back("builtin_docs.txt: no entry for " + std::string(builtinSignature(id).name));
        }
    }

    // The usage: "scripts N", then "Name<TAB>maps<TAB>calls<TAB>map,map,...".
    const auto ulines = splitLines(usageText);
    for (std::size_t i = 0; i < ulines.size(); ++i) {
        const std::string_view line = trim(ulines[i]);
        if (line.empty() || line.starts_with("#")) continue;
        if (line.starts_with("scripts ")) {
            set.usageScripts = std::atoi(std::string(line.substr(8)).c_str());
            continue;
        }
        const std::vector<std::string> cols = [&] {
            std::vector<std::string> c;
            std::string_view s = line;
            while (true) {
                const std::size_t tab = s.find('\t');
                c.emplace_back(s.substr(0, tab));
                if (tab == std::string_view::npos) break;
                s.remove_prefix(tab + 1);
            }
            return c;
        }();
        const auto id = cols.size() >= 3 ? findBuiltin(cols[0]) : std::nullopt;
        if (!id || builtinSignature(*id).name != cols[0]) {
            set.errors.push_back("builtin_usage.txt:" + std::to_string(i + 1) + ": bad line");
            continue;
        }
        BuiltinDoc& d = set.docs[*id];
        d.usageMaps = std::atoi(cols[1].c_str());
        d.usageCalls = std::atoi(cols[2].c_str());
        d.usageExamples = cols.size() >= 4 ? splitList(cols[3], ',') : std::vector<std::string>{};
    }
    return set;
}

std::string_view embeddedBuiltinDocsText()
{
    return {reinterpret_cast<const char*>(kBuiltinDocsText), kBuiltinDocsTextSize};
}

std::string_view embeddedBuiltinUsageText()
{
    return {reinterpret_cast<const char*>(kBuiltinUsageText), kBuiltinUsageTextSize};
}

const BuiltinDocSet& builtinDocs()
{
    static const BuiltinDocSet set = parseBuiltinDocs(embeddedBuiltinDocsText(), embeddedBuiltinUsageText());
    return set;
}

const BuiltinDoc* builtinDoc(std::uint32_t id)
{
    const auto& docs = builtinDocs().docs;
    return id < docs.size() && !docs[id].name.empty() ? &docs[id] : nullptr;
}

// --- The usage report --------------------------------------------------------------------------

namespace {

/// Each builtin's calls in a NoxScript source: a builtin's name followed by "(", outside comments
/// and string literals.
std::map<std::uint32_t, int> sourceCalls(std::string_view src)
{
    std::map<std::uint32_t, int> calls;
    for (std::size_t i = 0; i < src.size();) {
        const char c = src[i];
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            i = std::min(src.find('\n', i), src.size());
        } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            const std::size_t end = src.find("*/", i + 2);
            i = end == std::string_view::npos ? src.size() : end + 2;
        } else if (c == '"') {
            for (++i; i < src.size() && src[i] != '"'; ++i) {
                if (src[i] == '\\') ++i;
            }
            ++i;
        } else if (isIdent(c) && !std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t b = i;
            while (i < src.size() && isIdent(src[i])) ++i;
            std::size_t j = i;
            while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) ++j;
            if (j < src.size() && src[j] == '(') {
                if (const auto id = findBuiltin(src.substr(b, i - b))) ++calls[*id];
            }
        } else {
            ++i;
        }
    }
    return calls;
}

std::map<std::uint32_t, int> bytecodeCalls(const ScriptFile& s)
{
    std::map<std::uint32_t, int> calls;
    for (const auto& f : s.functions) {
        for (std::size_t at = 0; at < f.code.size(); at += 1 + operandCount(f.code[at])) {
            if (f.code[at] == static_cast<std::uint32_t>(Op::CallBuiltin) && at + 1 < f.code.size()) {
                ++calls[f.code[at + 1]];
            }
        }
    }
    return calls;
}


} // namespace

std::string builtinUsageReport(const std::filesystem::path& mapsDir, std::vector<std::string>* errors)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> maps;
    for (const auto& entry : fs::recursive_directory_iterator(mapsDir)) {
        if (entry.is_regular_file() && util::lowered(entry.path().extension().string()) == ".map") {
            maps.push_back(entry.path());
        }
    }
    std::ranges::sort(maps, [](const fs::path& a, const fs::path& b) { return util::lowered(a.stem().string()) < util::lowered(b.stem().string()); });
    int scripts = 0;
    std::vector<std::vector<std::pair<int, std::string>>> perMap(kBuiltinCount); // (calls, map) per builtin
    for (const fs::path& path : maps) {
        const std::string name = path.stem().string();
        ScriptFile s;
        try {
            const auto file = map::loadMapFile(path);
            if (!file.find(map::ScriptObject::kName)) continue;
            s = ScriptFile::parse(map::decodeSection<map::ScriptObject>(file).code);
        } catch (const std::exception& ex) {
            if (errors) errors->push_back(name + ": " + ex.what());
            continue;
        }
        if (s.functions.empty()) continue;
        ++scripts;
        const auto source = decompile(s).source;
        const auto calls = sourceCalls(source);
        if (calls != bytecodeCalls(s) && errors) {
            errors->push_back(name + ": the decompiled source's builtin calls aren't the script's");
        }
        for (const auto& [id, n] : calls) {
            if (id < kBuiltinCount) perMap[id].emplace_back(n, name);
        }
    }
    std::ostringstream out;
    out << "# How the stock maps call each builtin: name, maps, calls, and up to 5 of the maps (the most\n"
           "# calls first). Generated by `noxscript --usage <the game's maps folder> -o <this file>`: each\n"
           "# map's script decompiled and its calls counted. Don't edit it by hand.\n";
    out << "scripts " << scripts << "\n";
    for (std::uint32_t id = 0; id < kBuiltinCount; ++id) {
        auto& m = perMap[id];
        std::ranges::sort(m, [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : util::lowered(a.second) < util::lowered(b.second); });
        int calls = 0;
        for (const auto& entry : m) calls += entry.first;
        out << builtinSignature(id).name << '\t' << m.size() << '\t' << calls << '\t';
        for (std::size_t i = 0; i < m.size() && i < 5; ++i) {
            out << (i ? "," : "") << m[i].second;
        }
        out << '\n';
    }
    return out.str();
}

} // namespace nox::script
