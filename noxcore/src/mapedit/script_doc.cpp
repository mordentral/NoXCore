// The map editor's script on the document's side (script_doc.hpp).
#include "noxcore/mapedit/script_doc.hpp"
#include "noxcore/mapedit/op_label.hpp"

#include "noxcore/script/decompiler.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <set>
#include <type_traits>
#include <variant>

namespace nox::mapedit {

namespace {

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

/// Each script handler `o` has, in the order the editor lists them: f(label, handler), the handler
/// as `o` gives it (const for a const object: nothing copied to read them).
template <typename Obj, typename F>
void eachHandler(Obj& o, F&& f)
{
    if (o.header.details && o.header.details->onPickup) {
        f(NOX_TRC("editor.script.handler.pickup", "Pickup"), *o.header.details->onPickup);
    }
    const auto monster = [&](auto& s) {
        f(NOX_TRC("editor.script.handler.enemy_sighted", "Enemy sighted"), s.enemySighted);
        f(NOX_TRC("editor.script.handler.death", "Death"), s.death);
        f(NOX_TRC("editor.script.handler.looking_for_enemy", "Looking for enemy"), s.lookingForEnemy);
        f(NOX_TRC("editor.script.handler.change_focus", "Change focus"), s.changeFocus);
        f(NOX_TRC("editor.script.handler.is_hit", "Is hit"), s.isHit);
        f(NOX_TRC("editor.script.handler.retreat", "Retreat"), s.retreat);
        f(NOX_TRC("editor.script.handler.collision", "Collision"), s.collision);
        f(NOX_TRC("editor.script.handler.hear_enemy", "Hear enemy"), s.hearEnemy);
        f(NOX_TRC("editor.script.handler.end_of_waypoint", "End of waypoint"), s.endOfWaypoint);
        f(NOX_TRC("editor.script.handler.lost_enemy", "Lost enemy"), s.lostEnemy);
    };
    std::visit(
        [&](auto& d) {
            using T = std::remove_cvref_t<decltype(d)>;
            if constexpr (std::is_same_v<T, map::xfer::Trigger>) {
                f(NOX_TRC("editor.script.handler.pressed", "Pressed"), d.onPressed);
                f(NOX_TRC("editor.script.handler.released", "Released"), d.onReleased);
                if (o.xferVersion > 0x1E) {
                    f(NOX_TRC("editor.script.handler.collided", "Collided"), d.onCollided);
                }
            } else if constexpr (std::is_same_v<T, map::xfer::Hole>) {
                f(NOX_TRC("editor.script.handler.stepped_on", "Stepped on"), d.script);
            } else if constexpr (std::is_same_v<T, map::xfer::Monster> || std::is_same_v<T, map::xfer::Npc>) {
                monster(d.scripts);
            } else if constexpr (std::is_same_v<T, map::xfer::MonsterGenerator>) {
                // (the server reads them as hurt, die, collide, spawn: World::load)
                f(NOX_TRC("editor.script.handler.hurt", "Hurt"), d.scripts[0]);
                f(NOX_TRC("editor.script.handler.die", "Die"), d.scripts[1]);
                f(NOX_TRC("editor.script.handler.collide", "Collide"), d.scripts[2]);
                f(NOX_TRC("editor.script.handler.spawn", "Spawn"), d.scripts[3]);
            }
        },
        o.data);
}

std::string stripNul(std::string s)
{
    while (!s.empty() && s.back() == '\0') {
        s.pop_back();
    }
    return s;
}

} // namespace

std::optional<std::string> sidecarScriptSource(const Sidecar& side, std::optional<script::CallOrderMigration>* migration)
{
    if (migration) {
        migration->reset();
    }
    const auto it = side.other.find(std::string(kSidecarScriptSource));
    if (it == side.other.end()) {
        return std::nullopt;
    }
    auto source = jsonUnquote(it->second);
    if (!source) {
        return std::nullopt;
    }
    int version = 1;
    if (const auto v = side.other.find(std::string(kSidecarScriptSourceVersion)); v != side.other.end()) {
        const std::string& raw = v->second;
        if (std::from_chars(raw.data(), raw.data() + raw.size(), version).ec != std::errc{}) {
            version = 1;
        }
    }
    if (version >= kScriptSourceVersion) {
        return source;
    }
    auto m = script::migrateCallOrder(*source);
    source = m.source;
    if (migration) {
        *migration = std::move(m);
    }
    return source;
}

std::vector<SourceNote> callOrderMigrationNotes(const script::CallOrderMigration& m)
{
    using Outcome = script::CallOrderMigration::Outcome;
    std::vector<SourceNote> out;
    switch (m.outcome) {
    case Outcome::Unchanged:
        break;
    case Outcome::Same:
        out.push_back({false, l10n::format(NOX_TRP("editor.script.migrated.same",
                                                   "the saved source was written for the old argument order (a call's first argument went to the "
                                                   "function's last parameter): {n} call of the script's own functions now has its arguments "
                                                   "turned round to read in parameter order; it compiles to the same bytes",
                                                   "the saved source was written for the old argument order (a call's first argument went to the "
                                                   "function's last parameter): {n} calls of the script's own functions now have their arguments "
                                                   "turned round to read in parameter order; they compile to the same bytes",
                                                   m.calls),
                                           {{"n", m.calls}})});
        break;
    case Outcome::Unchecked:
        out.push_back({true, l10n::format(NOX_TRP("editor.script.migrated.unchecked",
                                                  "the saved source was written for the old argument order (a call's first argument went to the "
                                                  "function's last parameter) and doesn't compile, so the change can't be checked: {n} call of "
                                                  "the script's own functions now has its arguments turned round to read in parameter order; "
                                                  "look it over",
                                                  "the saved source was written for the old argument order (a call's first argument went to the "
                                                  "function's last parameter) and doesn't compile, so the change can't be checked: {n} calls of "
                                                  "the script's own functions now have their arguments turned round to read in parameter order; "
                                                  "look them over",
                                                  m.calls),
                                          {{"n", m.calls}})});
        break;
    case Outcome::TypeErrors:
        out.push_back({true, l10n::format(NOX_TRP("editor.script.migrated.type_errors",
                                                  "the saved source was written for the old argument order (a call's first argument went to the "
                                                  "function's last parameter): {n} call of the script's own functions now has its arguments "
                                                  "turned round to read as the map runs it, and it doesn't compile: the old order put a value into "
                                                  "a parameter of another type. Fix it (see the errors); until then the map's script stays as it is",
                                                  "the saved source was written for the old argument order (a call's first argument went to the "
                                                  "function's last parameter): {n} calls of the script's own functions now have their arguments "
                                                  "turned round to read as the map runs them, and some don't compile: the old order put a value into "
                                                  "a parameter of another type. Fix them (see the errors); until then the map's script stays as it is",
                                                  m.calls),
                                          {{"n", m.calls}})});
        break;
    case Outcome::Failed:
        out.push_back({true, NOX_TR("editor.script.migrated.failed",
                                    "the saved source was written for the old argument order (a call's first argument went to the "
                                    "function's last parameter), and it couldn't be turned round to give the same compiled script: it "
                                    "is shown as saved, so its calls of the script's own functions with two or more arguments now bind "
                                    "the other way round; check them before compiling")});
        break;
    }
    if (m.skipped > 0) {
        out.push_back({true, l10n::format(NOX_TRP("editor.script.migrated.skipped",
                                                  "{n} call of the script's own functions with two or more arguments, in code that doesn't "
                                                  "parse, was left as it was saved (for the old argument order): it now binds the other way round",
                                                  "{n} calls of the script's own functions with two or more arguments, in code that doesn't "
                                                  "parse, were left as they were saved (for the old argument order): they now bind the other way round",
                                                  m.skipped),
                                          {{"n", m.skipped}})});
    }
    if (m.unparsed) {
        // (The parser skips the code after each syntax error: calls there can't be found or counted.)
        out.push_back({true, NOX_TR("editor.script.migrated.unparsed",
                                    "the saved source was written for the old argument order (a call's first argument went to the "
                                    "function's last parameter) and has syntax errors: the code the parser skips after each one isn't "
                                    "read, so a call there of the script's own functions with two or more arguments is left as it was "
                                    "saved and now binds the other way round; check the code around each syntax error")});
    }
    return out;
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
        side.other[std::string(kSidecarScriptSourceVersion)] = std::to_string(kScriptSourceVersion);
    } else {
        side.other.erase(std::string(kSidecarScriptSource));
        side.other.erase(std::string(kSidecarScriptSourceVersion));
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
        return name.empty() ? l10n::format(NOX_TR("editor.script.where.polygon", "polygon {n}: {field}"), {{"n", polygon + 1}, {"field", field}})
                            : l10n::format(NOX_TR("editor.script.where.polygon_named", "polygon {n} ({name}): {field}"), {{"n", polygon + 1}, {"name", name}, {"field", field}});
    }
    const auto it = doc.objects.find(object);
    if (it == doc.objects.end()) {
        return l10n::format(NOX_TR("editor.script.where.object_gone", "object: {field}"), {{"field", field}});
    }
    return l10n::format(NOX_TR("editor.script.where.object", "{type} (extent {extent}): {field}"),
                        {{"type", it->second.type}, {"extent", it->second.header.extent}, {"field", field}});
}

std::vector<std::pair<std::string, map::ScriptHandler*>> objectHandlers(DocObject& o)
{
    std::vector<std::pair<std::string, map::ScriptHandler*>> out;
    eachHandler(o, [&](const char* label, map::ScriptHandler& h) { out.emplace_back(label, &h); });
    return out;
}

std::vector<HandlerRef> handlerRefs(const MapDocument& doc)
{
    std::vector<HandlerRef> out;
    for (const auto& [id, object] : doc.objects) {
        // (Read where they are: no copy of the object.)
        eachHandler(object, [&](const char* label, const map::ScriptHandler& h) {
            HandlerRef r;
            r.owner = HandlerRef::Owner::Object;
            r.object = id;
            r.field = label;
            r.function = h.function;
            out.push_back(std::move(r));
        });
    }
    for (std::size_t i = 0; i < doc.polygons.polygons.size(); ++i) {
        const auto& p = doc.polygons.polygons[i];
        out.push_back({HandlerRef::Owner::Polygon, 0, i, NOX_TR("editor.script.handler.enter", "Enter"), p.playerEnter.function});
        out.push_back({HandlerRef::Owner::Polygon, 0, i, NOX_TR("editor.script.handler.leave", "Leave"), p.monsterEnter.function});
    }
    return out;
}

namespace {

/// The functions `refs` name, each once, sorted.
std::vector<std::string> namesOf(const std::vector<HandlerRef>& refs)
{
    std::set<std::string> names;
    for (const auto& r : refs) {
        if (!r.function.empty()) {
            names.insert(r.function);
        }
    }
    return {names.begin(), names.end()};
}

} // namespace

std::vector<std::string> handlerNames(const MapDocument& doc)
{
    return namesOf(handlerRefs(doc));
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
        out.migration = std::move(d.migration);
    } catch (const std::exception& ex) {
        out.notes.push_back(l10n::format(NOX_TR("editor.script.unreadable", "the map's script can't be read: {error}"), {{"error", ex.what()}}));
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
    return NOX_TR("editor.script.read_only_object_vars",
                  "This map's script uses object variables, a NoX Core-only feature: the original game crashes on them, and the map "
                  "needs a NoX Core server. They are off in Editor Settings, so the source is shown read-only and the map's script is "
                  "kept as it is. Turn on \"Allow NoX Core-only script features (object variables)\" to edit and compile it.");
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
    const auto take = [](Names& names, const std::string& several) {
        std::vector<ArgValue> out;
        for (auto& [bare, n] : names) {
            if (n.count > 1) {
                n.value.note += " (" + l10n::format(several, {{"n", n.count}}) + ")";
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
            char x[24], y[24];
            std::snprintf(x, sizeof x, "%.0f", static_cast<double>(w.x));
            std::snprintf(y, sizeof y, "%.0f", static_cast<double>(w.y));
            return l10n::format(NOX_TR("editor.script.values.waypoint_at", "waypoint {id} at {x}, {y}"), {{"id", w.id}, {"x", x}, {"y", y}});
        }, -1);
    }
    Names groups[3];
    for (const auto& g : doc.groups.groups) {
        // (A group of groups, kind 3, takes the kind of its first member's group: World::groupKind.
        // The editor makes groups of the three kinds; a kind 3 one is offered as an object group.)
        const int k = g.kind == map::Group::Kind::Waypoints ? 1 : g.kind == map::Group::Kind::Walls ? 2 : 0;
        add(groups[k], stripNul(g.name), [&] {
            const std::size_t size = g.kind == map::Group::Kind::Walls ? g.walls.size() : g.members.size();
            const std::string& what = g.kind == map::Group::Kind::Walls       ? NOX_TRP("editor.script.values.walls", "{n} wall", "{n} walls", size)
                                      : g.kind == map::Group::Kind::Waypoints ? NOX_TRP("editor.script.values.waypoints", "{n} waypoint", "{n} waypoints", size)
                                      : g.kind == map::Group::Kind::Unknown3  ? NOX_TRP("editor.script.values.groups", "{n} group", "{n} groups", size)
                                                                              : NOX_TRP("editor.script.values.objects", "{n} object", "{n} objects", size);
            return l10n::format(what, {{"n", size}});
        }, -1);
    }
    using script::ValueMatch;
    sets.set({ValueSetId::ObjectName, ValueMatch::MapScoped, take(objects, NOX_TR("editor.script.values.objects_share_name", "{n} objects have this name"))});
    sets.set({ValueSetId::WaypointName, ValueMatch::MapScoped, take(waypoints, NOX_TR("editor.script.values.waypoints_share_name", "{n} waypoints have this name"))});
    sets.set({ValueSetId::ObjectGroup, ValueMatch::MapScoped, take(groups[0], NOX_TR("editor.script.values.groups_share_name", "{n} groups have this name"))});
    sets.set({ValueSetId::WaypointGroup, ValueMatch::MapScoped, take(groups[1], NOX_TR("editor.script.values.groups_share_name", "{n} groups have this name"))});
    sets.set({ValueSetId::WallGroup, ValueMatch::MapScoped, take(groups[2], NOX_TR("editor.script.values.groups_share_name", "{n} groups have this name"))});
}

script::CompileResult compileForDocument(const MapDocument& doc, std::string_view source, bool allowObjectVariables,
                                         const script::ArgValueSets* gameValues)
{
    // The map's handlers, listed once for the external names and the check below.
    const std::vector<HandlerRef> refs = handlerRefs(doc);
    const std::vector<std::string> external = namesOf(refs);
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
    for (const auto& h : refs) {
        if (!h.function.empty() && !file.find(h.function)) {
            auto& m = missing[h.function];
            if (m.second++ == 0) {
                m.first = h.where(doc);
            }
        }
    }
    for (const auto& [name, m] : missing) {
        warn({}, m.second > 1 ? l10n::format(NOX_TR("editor.script.handler_undefined_more",
                                                    "{where} names the function {name}, which the script doesn't define (and {n} more handlers)"),
                                             {{"where", m.first}, {"name", name}, {"n", m.second - 1}})
                              : l10n::format(NOX_TR("editor.script.handler_undefined", "{where} names the function {name}, which the script doesn't define"),
                                             {{"where", m.first}, {"name", name}}));
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
    return Op{opLabel(oplabels::kCompileScript), {SetScript{std::move(so)}}};
}

} // namespace nox::mapedit
