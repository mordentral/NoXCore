// The map editor's script on the document's side (script_doc.hpp).
#include "noxcore/mapedit/script_doc.hpp"

#include "noxcore/script/decompiler.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <variant>

namespace nox::mapedit {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string jsonQuote(std::string_view s)
{
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 15];
            } else {
                out += ch; // (UTF-8 kept as it is)
            }
        }
    }
    return out + "\"";
}

void utf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// A JSON string's value (its text with the quotes), or nothing for text that isn't one.
std::optional<std::string> jsonUnquote(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) {
        ++i;
    }
    if (i >= s.size() || s[i] != '"') {
        return std::nullopt;
    }
    ++i;
    std::string out;
    const auto hex4 = [&](std::size_t at) -> std::optional<std::uint32_t> {
        if (at + 4 > s.size()) {
            return std::nullopt;
        }
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = s[at + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    };
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') {
            return out;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= s.size()) {
            return std::nullopt;
        }
        const char e = s[i++];
        switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            auto cp = hex4(i);
            if (!cp) {
                return std::nullopt;
            }
            i += 4;
            if (*cp >= 0xD800 && *cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                if (const auto lo = hex4(i + 2); lo && *lo >= 0xDC00 && *lo < 0xE000) {
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
                    i += 6;
                }
            }
            utf8(out, *cp);
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void addMonsterScripts(std::vector<std::pair<std::string, map::ScriptHandler*>>& out, map::xfer::MonsterScripts& s)
{
    out.emplace_back("Enemy sighted", &s.enemySighted);
    out.emplace_back("Death", &s.death);
    out.emplace_back("Looking for enemy", &s.lookingForEnemy);
    out.emplace_back("Change focus", &s.changeFocus);
    out.emplace_back("Is hit", &s.isHit);
    out.emplace_back("Retreat", &s.retreat);
    out.emplace_back("Collision", &s.collision);
    out.emplace_back("Hear enemy", &s.hearEnemy);
    out.emplace_back("End of waypoint", &s.endOfWaypoint);
    out.emplace_back("Lost enemy", &s.lostEnemy);
}

std::string stripNul(std::string s)
{
    while (!s.empty() && s.back() == '\0') {
        s.pop_back();
    }
    return s;
}

} // namespace

std::optional<std::string> sidecarScriptSource(const Sidecar& side)
{
    const auto it = side.other.find(std::string(kSidecarScriptSource));
    return it == side.other.end() ? std::nullopt : jsonUnquote(it->second);
}

std::optional<script::DebugInfo> sidecarScriptDebug(const Sidecar& side)
{
    const auto it = side.other.find(std::string(kSidecarScriptDebug));
    return it == side.other.end() ? std::nullopt : script::DebugInfo::fromJson(it->second);
}

void storeScriptInSidecar(Sidecar& side, const std::optional<std::string>& source, const script::DebugInfo* debug)
{
    if (source) {
        side.other[std::string(kSidecarScriptSource)] = jsonQuote(*source);
    } else {
        side.other.erase(std::string(kSidecarScriptSource));
    }
    if (debug) {
        side.other[std::string(kSidecarScriptDebug)] = debug->toJson();
    } else {
        side.other.erase(std::string(kSidecarScriptDebug));
    }
}

std::string HandlerRef::where(const MapDocument& doc) const
{
    if (owner == Owner::Polygon) {
        std::string name = polygon < doc.polygons.polygons.size() ? doc.polygons.polygons[polygon].name : std::string();
        return "polygon " + std::to_string(polygon + 1) + (name.empty() ? "" : " (" + name + ")") + ": " + field;
    }
    const auto it = doc.objects.find(object);
    if (it == doc.objects.end()) {
        return "object: " + field;
    }
    return it->second.type + " (extent " + std::to_string(it->second.header.extent) + "): " + field;
}

std::vector<std::pair<std::string, map::ScriptHandler*>> objectHandlers(DocObject& o)
{
    std::vector<std::pair<std::string, map::ScriptHandler*>> out;
    if (o.header.details && o.header.details->onPickup) {
        out.emplace_back("Pickup", &*o.header.details->onPickup);
    }
    std::visit(Overloaded{
                   [&](map::xfer::Trigger& t) {
                       out.emplace_back("Pressed", &t.onPressed);
                       out.emplace_back("Released", &t.onReleased);
                       if (o.xferVersion > 0x1E) {
                           out.emplace_back("Collided", &t.onCollided);
                       }
                   },
                   [&](map::xfer::Hole& h) { out.emplace_back("Stepped on", &h.script); },
                   [&](map::xfer::Monster& m) { addMonsterScripts(out, m.scripts); },
                   [&](map::xfer::Npc& n) { addMonsterScripts(out, n.scripts); },
                   [&](map::xfer::MonsterGenerator& g) {
                       // (the server reads them as hurt, die, collide, spawn: World::load)
                       out.emplace_back("Hurt", &g.scripts[0]);
                       out.emplace_back("Die", &g.scripts[1]);
                       out.emplace_back("Collide", &g.scripts[2]);
                       out.emplace_back("Spawn", &g.scripts[3]);
                   },
                   [](auto&) {},
               },
               o.data);
    return out;
}

std::vector<HandlerRef> handlerRefs(const MapDocument& doc)
{
    std::vector<HandlerRef> out;
    for (const auto& [id, object] : doc.objects) {
        DocObject copy = object; // (objectHandlers hands out pointers into what it's given)
        for (const auto& [label, h] : objectHandlers(copy)) {
            HandlerRef r;
            r.owner = HandlerRef::Owner::Object;
            r.object = id;
            r.field = label;
            r.function = h->function;
            out.push_back(std::move(r));
        }
    }
    for (std::size_t i = 0; i < doc.polygons.polygons.size(); ++i) {
        const auto& p = doc.polygons.polygons[i];
        out.push_back({HandlerRef::Owner::Polygon, 0, i, "Enter", p.playerEnter.function});
        out.push_back({HandlerRef::Owner::Polygon, 0, i, "Leave", p.monsterEnter.function});
    }
    return out;
}

std::vector<std::string> handlerNames(const MapDocument& doc)
{
    std::set<std::string> names;
    for (const auto& r : handlerRefs(doc)) {
        if (!r.function.empty()) {
            names.insert(r.function);
        }
    }
    return {names.begin(), names.end()};
}

DecompiledSource decompileDocumentScript(const MapDocument& doc, const script::DebugInfo* names)
{
    DecompiledSource out;
    if (doc.script.code.empty()) {
        return out;
    }
    try {
        const script::ScriptFile file = script::ScriptFile::parse(doc.script.code);
        if (file.functions.empty()) {
            return out;
        }
        script::DecompileOptions o;
        o.names = names;
        auto d = script::decompile(file, o);
        out.source = std::move(d.source);
        out.identical = d.identical;
        out.notes = std::move(d.notes);
    } catch (const std::exception& ex) {
        out.notes.push_back(std::string("the map's script can't be read: ") + ex.what());
    }
    return out;
}

std::optional<std::string> documentNeedsNoxCore(const MapDocument& doc)
{
    if (doc.script.code.empty()) {
        return std::nullopt;
    }
    try {
        if (script::usesObjectVariables(script::ScriptFile::parse(doc.script.code))) {
            return "the script uses object variables, a NoX Core-only feature: the original game crashes when it runs, so the map "
                   "needs a NoX Core server";
        }
    } catch (const std::exception&) {
        // (A script the game can't parse: the validator says so.)
    }
    return std::nullopt;
}

std::optional<std::string> sidecarNeedsNoxCore(const Sidecar& side)
{
    const auto it = side.other.find(std::string(kSidecarNeedsNoxCore));
    if (it == side.other.end()) {
        return std::nullopt;
    }
    return jsonUnquote(it->second).value_or("needs a NoX Core server");
}

void markNoxCoreNeeds(Sidecar& side, const MapDocument& doc)
{
    if (const auto why = documentNeedsNoxCore(doc)) {
        side.other[std::string(kSidecarNeedsNoxCore)] = jsonQuote(*why);
    } else {
        side.other.erase(std::string(kSidecarNeedsNoxCore));
    }
}

std::optional<std::string> scriptReadOnlyReason(const MapDocument& doc, bool allowObjectVariables)
{
    if (allowObjectVariables || !documentNeedsNoxCore(doc)) {
        return std::nullopt;
    }
    return "This map's script uses object variables, a NoX Core-only feature: the original game crashes on them, and the map "
           "needs a NoX Core server. They are off in Editor Settings, so the source is shown read-only and the map's script is "
           "kept as it is. Turn on \"Allow NoX Core-only script features (object variables)\" to edit and compile it.";
}

void addMapArgValues(script::ArgValueSets& sets, const MapDocument& doc, const things::ThingDb* things)
{
    using script::ArgValue;
    using script::ValueSetId;
    const auto thingIndex = [&](const std::string& type) -> std::int32_t {
        const things::ThingDef* t = things ? things->findThing(type) : nullptr;
        return t ? static_cast<std::int32_t>(t - things->things.data()) : -1;
    };
    // Each name as a script writes it: what follows its "Scope:" (a name without ':' matches that,
    // World::byScriptName / namesMatch), the whole scoped names its aliases; once, with the first
    // owner's note and how many share it.
    struct Named {
        ArgValue value;
        int count = 0;
    };
    using Names = std::map<std::string, Named>;
    const auto add = [](Names& names, const std::string& stored, auto note, std::int32_t thing) {
        const auto colon = stored.find(':');
        const std::string bare = colon == std::string::npos ? stored : stored.substr(colon + 1);
        if (bare.empty()) {
            return;
        }
        Named& n = names[bare];
        if (n.count++ == 0) {
            n.value.value = bare;
            n.value.note = note();
            n.value.thing = thing;
        }
        if (colon != std::string::npos && std::ranges::find(n.value.aliases, stored) == n.value.aliases.end()) {
            n.value.aliases.push_back(stored);
        }
    };
    const auto take = [](Names& names, const char* several) {
        std::vector<ArgValue> out;
        for (auto& [bare, n] : names) {
            if (n.count > 1) {
                n.value.note += " (" + std::to_string(n.count) + several + ")";
            }
            if (!n.value.aliases.empty()) {
                n.value.note += "; " + n.value.aliases.front() + (n.value.aliases.size() > 1 ? "..." : "");
            }
            out.push_back(std::move(n.value));
        }
        return out;
    };
    Names objects;
    for (const auto& [id, object] : doc.objects) {
        if (object.header.details && !object.header.details->scriptName.empty()) {
            add(objects, stripNul(object.header.details->scriptName), [&] { return object.type; }, thingIndex(object.type));
        }
    }
    Names waypoints;
    for (const auto& w : doc.waypoints.waypoints) {
        add(waypoints, stripNul(w.name), [&] {
            char where[64];
            std::snprintf(where, sizeof where, "waypoint %u at %.0f, %.0f", w.id, static_cast<double>(w.x), static_cast<double>(w.y));
            return std::string(where);
        }, -1);
    }
    Names groups[3];
    for (const auto& g : doc.groups.groups) {
        // (A group of groups, kind 3, takes the kind of its first member's group: World::groupKind.
        // The editor makes groups of the three kinds; a kind 3 one is offered as an object group.)
        const int k = g.kind == map::Group::Kind::Waypoints ? 1 : g.kind == map::Group::Kind::Walls ? 2 : 0;
        add(groups[k], stripNul(g.name), [&] {
            const std::size_t size = g.kind == map::Group::Kind::Walls ? g.walls.size() : g.members.size();
            const char* what = g.kind == map::Group::Kind::Walls ? " wall" : g.kind == map::Group::Kind::Waypoints ? " waypoint"
                               : g.kind == map::Group::Kind::Unknown3                                       ? " group"
                                                                                                             : " object";
            return std::to_string(size) + what + (size == 1 ? "" : "s");
        }, -1);
    }
    using script::ValueMatch;
    sets.set({ValueSetId::ObjectName, ValueMatch::MapScoped, take(objects, " objects have this name")});
    sets.set({ValueSetId::WaypointName, ValueMatch::MapScoped, take(waypoints, " waypoints have this name")});
    sets.set({ValueSetId::ObjectGroup, ValueMatch::MapScoped, take(groups[0], " groups have this name")});
    sets.set({ValueSetId::WaypointGroup, ValueMatch::MapScoped, take(groups[1], " groups have this name")});
    sets.set({ValueSetId::WallGroup, ValueMatch::MapScoped, take(groups[2], " groups have this name")});
}

script::CompileResult compileForDocument(const MapDocument& doc, std::string_view source, bool allowObjectVariables,
                                         const script::ArgValueSets* gameValues)
{
    const std::vector<std::string> external = handlerNames(doc);
    // The value sets: the game's (when given; else the built-in ones, which need no game data)
    // and the map's own names.
    static const script::ArgValueSets builtIn = script::ArgValueSets::build({});
    script::ArgValueSets values = gameValues ? *gameValues : builtIn;
    addMapArgValues(values, doc);
    script::CompileOptions o;
    o.lint = true;
    o.externalNames = &external;
    o.allowObjectVariables = allowObjectVariables;
    o.argValues = &values;
    script::CompileResult r = script::compile(source, o);
    if (!r.file) {
        return r;
    }
    const script::ScriptFile& file = *r.file;
    const auto warn = [&](script::SourceLoc loc, std::string message) {
        r.diagnostics.push_back({script::Diagnostic::Severity::Warning, loc, std::move(message)});
    };
    // Handlers naming functions the script doesn't define (each name once, its first owner).
    std::map<std::string, std::pair<std::string, int>> missing;
    for (const auto& h : handlerRefs(doc)) {
        if (!h.function.empty() && !file.find(h.function)) {
            auto& m = missing[h.function];
            if (m.second++ == 0) {
                m.first = h.where(doc);
            }
        }
    }
    for (const auto& [name, m] : missing) {
        warn({}, m.first + " names the function " + name + ", which the script doesn't define" +
                     (m.second > 1 ? " (and " + std::to_string(m.second - 1) + " more handlers)" : ""));
    }
    // (A lookup by a literal name nothing on the map has: the compiler's lint, CompileOptions::
    // argValues, at the literal.)
    std::stable_sort(r.diagnostics.begin(), r.diagnostics.end(), [](const script::Diagnostic& a, const script::Diagnostic& b) {
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.column < b.loc.column;
    });
    return r;
}

Op compiledScriptOp(const MapDocument& doc, const script::ScriptFile& file)
{
    map::ScriptObject so = doc.script;
    so.code = file.serialize();
    if (so.code == doc.script.code && !std::ranges::none_of(doc.sections, [](const std::string& s) { return s == map::ScriptObject::kName; })) {
        return {};
    }
    return Op{"Compile script", {SetScript{std::move(so)}}};
}

} // namespace nox::mapedit
