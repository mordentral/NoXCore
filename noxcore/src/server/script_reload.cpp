// Hot reload of a running map script (script_reload.hpp): the map editor's only.
#include "noxcore/server/script_reload.hpp"

#include "noxcore/map/sections.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/server.hpp"

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <utility>

namespace nox::server {

/// The parts of Server and ScriptHost a reload swaps or remaps (both befriend this class alone).
class ScriptReloadAccess {
public:
    static script::ScriptFile& file(Server& s) { return s.scriptFile_; }
    static ScriptHost* host(Server& s) { return s.script_.get(); }
    static std::list<ScriptTimer>& timers(ScriptHost& h) { return h.timers_; }
    static std::vector<ScriptHost::QueuedBlock>& queued(ScriptHost& h) { return h.queued_; }
    static std::map<std::uint32_t, std::vector<std::uint32_t>>& objectVars(ScriptHost& h) { return h.objectVars_; }
    /// The new script's bytes where the server keeps the map's (nc.obj, and the map file a solo
    /// save writes from), so what it writes matches what runs.
    static void storeScriptBytes(Server& s)
    {
        s.lastScriptCode_ = s.scriptFile_.serialize();
        for (map::RawSection& section : s.mapFile_.sections) {
            if (section.name == map::ScriptObject::kName) {
                map::ScriptObject so = map::ScriptObject::parse(section.data);
                so.code = s.lastScriptCode_;
                section.data = so.serialize();
                break;
            }
        }
    }
};

namespace dev {

namespace {

using script::DebugInfo;
using script::DebugVar;
using script::ScriptFile;
using script::Type;
using Severity = ReloadNote::Severity;

/// The running VM's state, taken before its script goes.
struct VmState {
    std::vector<std::uint32_t> globals;
    std::vector<std::vector<std::uint32_t>> locals;
    std::vector<std::string> strings; ///< every string by id (the constants, then those made since)
    std::size_t constants = 0;
    std::array<std::uint32_t, script::Vm::kStackWords> stack{};
    std::size_t top = 0;
};

VmState takeState(script::Vm& vm)
{
    VmState s;
    const auto g = vm.globals();
    s.globals.assign(g.begin(), g.end());
    for (std::size_t i = 0; i < vm.file().functions.size(); ++i) {
        const auto l = vm.locals(i);
        s.locals.emplace_back(l.begin(), l.end());
    }
    for (std::size_t i = 0; i < vm.stringCount(); ++i) {
        s.strings.push_back(vm.string(static_cast<std::uint32_t>(i)));
    }
    s.constants = vm.constantCount();
    s.stack = vm.stackWords();
    s.top = vm.stackTop();
    return s;
}

/// String ids from the old script's VM to the new one's: a constant to the new constant with its
/// text (the n-th of that text for the n-th, else the first), a run-time string to its copy
/// (copied in order, so the ids move by the change in the constants' count). A constant the new
/// script no longer has is made a run-time string with its text, once.
class StringMap {
public:
    StringMap(const VmState& old, const ScriptFile& newFile, script::Vm& vm)
        : old_(old)
        , vm_(vm)
        , newConstants_(newFile.strings.size())
    {
        std::map<std::string_view, std::vector<std::uint32_t>> byText;
        for (std::size_t i = 0; i < newFile.strings.size(); ++i) {
            byText[newFile.strings[i]].push_back(static_cast<std::uint32_t>(i));
        }
        std::map<std::string_view, std::size_t> seen;
        constant_.resize(old.constants, kNone);
        for (std::size_t i = 0; i < old.constants && i < old.strings.size(); ++i) {
            const std::string_view text = old.strings[i];
            const std::size_t n = seen[text]++;
            if (const auto it = byText.find(text); it != byText.end()) {
                constant_[i] = n < it->second.size() ? it->second[n] : it->second.front();
            }
        }
        // The run-time strings, in order.
        for (std::size_t i = old.constants; i < old.strings.size(); ++i) {
            vm_.addString(old.strings[i]);
        }
    }

    /// A string value's id in the new VM.
    std::uint32_t operator()(std::uint32_t id)
    {
        std::uint32_t to = id;
        if (id < old_.constants) {
            if (constant_[id] == kNone) {
                constant_[id] = vm_.addString(old_.strings[id]);
                ++orphans_;
            }
            to = constant_[id];
        } else if (id < old_.strings.size()) {
            to = static_cast<std::uint32_t>(newConstants_ + (id - old_.constants));
        }
        if (to != id) {
            ++remapped_;
        }
        return to;
    }

    std::uint32_t remapped() const { return remapped_; }
    std::uint32_t orphans() const { return orphans_; }

private:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    const VmState& old_;
    script::Vm& vm_;
    std::size_t newConstants_;
    std::vector<std::uint32_t> constant_;
    std::uint32_t remapped_ = 0;
    std::uint32_t orphans_ = 0;
};

std::string typeText(const DebugVar& v)
{
    std::string t(script::typeName(v.type));
    if (v.size > 1) {
        t += "[" + std::to_string(v.size) + "]";
    }
    return t;
}

void note(ReloadReport& r, Severity s, std::string message, script::SourceLoc loc = {})
{
    r.notes.push_back({s, std::move(message), loc});
}

/// A variable's words copied from the old state into the new, string values remapped.
void copyWords(std::span<const std::uint32_t> from, std::size_t fromAt, std::span<std::uint32_t> to, std::size_t toAt, std::uint32_t size,
               bool strings, StringMap& map)
{
    for (std::uint32_t k = 0; k < size; ++k) {
        if (fromAt + k >= from.size() || toAt + k >= to.size()) {
            return;
        }
        const std::uint32_t w = from[fromAt + k];
        to[toAt + k] = strings ? map(w) : w;
    }
}

/// The k-th symbol named `name` among `symbols` (the same name may be declared in sibling
/// blocks).
const DebugVar* nthNamed(const std::vector<DebugVar>& symbols, const std::string& name, std::size_t k)
{
    for (const DebugVar& v : symbols) {
        if (v.name == name && k-- == 0) {
            return &v;
        }
    }
    return nullptr;
}

/// An old function index's new one, by name (nothing: the function is gone).
std::optional<std::uint32_t> newIndex(const ScriptFile& oldFile, const ScriptFile& newFile, std::uint32_t index)
{
    if (index >= oldFile.functions.size()) {
        return index; // (an index no function had: left as it was)
    }
    const auto f = newFile.find(oldFile.functions[index].name);
    return f ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*f)) : std::nullopt;
}

/// Every object's object variables (ScriptHost's words by extent) laid out again for the new
/// script: each variable matched by (name, type, size) keeps its words on every object (string
/// values remapped); a new one, or one whose type or size changed, starts at 0 (its initial
/// value: there are no initialisers); a removed one is dropped. Words no named variable covers
/// (the pad, a store past an array's end) aren't carried.
void migrateObjectVars(std::map<std::uint32_t, std::vector<std::uint32_t>>& objects, const ScriptFile& oldFile, const ScriptFile& newFile,
                       const DebugInfo& oldDebug, const DebugInfo& newDebug, StringMap& strings, ReloadReport& r)
{
    struct Move {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
        std::uint32_t size = 0;
        bool string = false;
    };
    std::vector<Move> moves;
    std::map<std::string, const DebugVar*> old;
    for (const DebugVar& v : oldDebug.objectVars) {
        if (!v.name.empty()) {
            old.emplace(v.name, &v);
        }
    }
    const auto count = std::to_string(objects.size()) + (objects.size() == 1 ? " object" : " objects");
    std::set<std::string> present;
    for (const DebugVar& v : newDebug.objectVars) {
        if (v.name.empty()) {
            continue; // (the pad)
        }
        present.insert(v.name);
        const auto it = old.find(v.name);
        if (it == old.end()) {
            ++r.objectVarsNew;
            continue;
        }
        if (it->second->type != v.type || it->second->size != v.size) {
            ++r.objectVarsReset;
            note(r, Severity::Info,
                 "object." + v.name + " changed from " + typeText(*it->second) + " to " + typeText(v) + ": 0 again on " + count, v.loc);
            continue;
        }
        moves.push_back({oldFile.functions[0].symbolOffsets[it->second->symbol], newFile.functions[0].symbolOffsets[v.symbol], v.size,
                         v.type == Type::String});
        ++r.objectVarsKept;
    }
    for (const auto& [name, v] : old) {
        if (!present.contains(name)) {
            ++r.objectVarsDropped;
            note(r, Severity::Info, "object." + name + " was removed: its values on " + count + " are dropped");
        }
    }
    r.objectsWithVars = static_cast<std::uint32_t>(objects.size());
    const std::size_t words = newFile.functions.empty() ? 0 : newFile.functions[0].localWords;
    for (auto& [extent, values] : objects) {
        std::vector<std::uint32_t> next(words, 0u);
        for (const Move& m : moves) {
            copyWords(values, m.from, next, m.to, m.size, m.string, strings);
        }
        values = std::move(next);
    }
}

std::string objectText(const ServerObject& o)
{
    return std::string(o.typeName) + " " + std::to_string(o.extent);
}

/// The monsters' dialog functions remapped by name (cleared where the function went).
void remapDialogs(Server& server, const ScriptFile& oldFile, const ScriptFile& newFile, ReloadReport& r)
{
    for (auto& [extent, o] : server.world().objects()) {
        if (!o.monster) {
            continue;
        }
        for (std::int32_t* index : {&o.monster->dialogStart, &o.monster->dialogEnd}) {
            if (*index < 0) {
                continue;
            }
            const auto old = static_cast<std::uint32_t>(*index);
            if (const auto to = newIndex(oldFile, newFile, old)) {
                if (*to != old) {
                    *index = static_cast<std::int32_t>(*to);
                    ++r.dialogsRemapped;
                }
            } else {
                note(r, Severity::Warning,
                     objectText(o) + "'s dialog " + (index == &o.monster->dialogStart ? "start" : "end") + " function " +
                         oldFile.functions[old].name + " was deleted: its dialog is cleared");
                *index = -1;
                ++r.dialogsCleared;
            }
        }
    }
}

/// Every handler (a name) on the map's objects and polygons that names a function the new script
/// doesn't have: reported once per name.
void reportHandlers(Server& server, const ScriptFile& newFile, ReloadReport& r)
{
    std::map<std::string, std::pair<std::uint32_t, std::string>> missing; // name: count, first owner
    const auto check = [&](const ServerObject::ScriptBlock& b, const std::string& owner) {
        if (b.function.empty() || newFile.find(b.function.view())) {
            return;
        }
        auto& m = missing[std::string(b.function.view())];
        if (m.first++ == 0) {
            m.second = owner;
        }
    };
    for (const auto& [extent, o] : server.world().objects()) {
        const std::string owner = objectText(o);
        check(o.onPickup, owner);
        if (o.trigger) {
            check(o.trigger->collided, owner);
            check(o.trigger->pressed, owner);
            check(o.trigger->released, owner);
        }
        if (o.pit) {
            check(o.pit->stepped, owner);
        }
        if (o.monster) {
            const auto& m = *o.monster;
            for (const auto* b : {&m.onEnemySighted, &m.onDeath, &m.onLookingForEnemy, &m.onChangeFocus, &m.onIsHit, &m.onRetreat,
                                  &m.onCollision, &m.onHearEnemy, &m.onEndOfWaypoint, &m.onLostEnemy}) {
                check(*b, owner);
            }
        }
        if (o.generator) {
            for (const auto* b : {&o.generator->onHurt, &o.generator->onDie, &o.generator->onSpawn, &o.generator->onCollide}) {
                check(*b, owner);
            }
        }
    }
    std::size_t index = 1;
    for (const MapPolygon& p : server.world().polygons()) {
        const std::string owner = "polygon " + std::to_string(index++);
        check(p.onEnter, owner);
        check(p.onLeave, owner);
    }
    for (const auto& [name, m] : missing) {
        r.handlersMissing += m.first;
        note(r, Severity::Warning,
             "the handler function " + name + " (named by " + m.second + (m.first > 1 ? " and " + std::to_string(m.first - 1) + " more" : "") +
                 ") isn't in the script: those calls are skipped");
    }
}

} // namespace

std::string ReloadReport::summary() const
{
    if (!applied) {
        return "not reloaded";
    }
    std::string s = "globals " + std::to_string(globalsKept) + " kept, " + std::to_string(globalsNew) + " new (" +
                    std::to_string(initialisersRun) + " initialised), " + std::to_string(globalsDropped) + " dropped; locals " +
                    std::to_string(localsKept) + " kept";
    if (localsReset) {
        s += ", " + std::to_string(localsReset) + " reset";
    }
    s += "; timers " + std::to_string(timersRemapped) + " remapped";
    if (timersCancelled) {
        s += ", " + std::to_string(timersCancelled) + " cancelled";
    }
    if (dialogsRemapped || dialogsCleared) {
        s += "; dialogs " + std::to_string(dialogsRemapped) + " remapped, " + std::to_string(dialogsCleared) + " cleared";
    }
    if (objectVarsKept || objectVarsNew || objectVarsReset || objectVarsDropped) {
        s += "; object variables " + std::to_string(objectVarsKept) + " kept, " + std::to_string(objectVarsNew) + " new";
        if (objectVarsReset) {
            s += ", " + std::to_string(objectVarsReset) + " reset";
        }
        if (objectVarsDropped) {
            s += ", " + std::to_string(objectVarsDropped) + " dropped";
        }
        s += " (on " + std::to_string(objectsWithVars) + " objects)";
    }
    s += "; " + std::to_string(stringsRemapped) + " string values remapped";
    if (handlersMissing) {
        s += "; " + std::to_string(handlersMissing) + " handlers name missing functions";
    }
    return s;
}

bool debugMatches(const ScriptFile& file, const DebugInfo& debug)
{
    if (file.functions.size() < 2) {
        return debug.functions.empty() && debug.globals.empty();
    }
    if (debug.functions.size() != file.functions.size() - 2 || debug.globals.size() != file.functions[1].symbolSizes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < debug.globals.size(); ++i) {
        if (debug.globals[i].symbol != i || debug.globals[i].size != file.functions[1].symbolSizes[i]) {
            return false;
        }
    }
    // Function 0's stored symbols (after the loader's placeholder) are the object variables.
    const auto& objectSizes = file.functions[0].symbolSizes;
    if (debug.objectVars.size() + 1 != std::max<std::size_t>(objectSizes.size(), 1)) {
        return false;
    }
    for (std::size_t i = 0; i < debug.objectVars.size(); ++i) {
        if (debug.objectVars[i].symbol != i + 1 || debug.objectVars[i].size != objectSizes[i + 1]) {
            return false;
        }
    }
    for (std::size_t k = 0; k < debug.functions.size(); ++k) {
        const auto& d = debug.functions[k];
        const auto& f = file.functions[k + 2];
        if (d.index != k + 2 || d.name != f.name || d.symbols.size() != f.symbolSizes.size()) {
            return false;
        }
        for (std::size_t i = 0; i < d.symbols.size(); ++i) {
            if (d.symbols[i].symbol != i || d.symbols[i].size != f.symbolSizes[i]) {
                return false;
            }
        }
    }
    return true;
}

ReloadReport hotReload(Server& server, ScriptFile newFile, const DebugInfo& oldDebug, const DebugInfo& newDebug)
{
    using A = ScriptReloadAccess;
    ReloadReport r;
    ScriptHost* host = A::host(server);
    if (!host) {
        note(r, Severity::Warning, "no map script is running");
        return r;
    }
    if (!debugMatches(A::file(server), oldDebug)) {
        note(r, Severity::Warning, "the running script isn't the one its names came from: use Restart scripts or Restart map");
        return r;
    }
    if (!debugMatches(newFile, newDebug)) {
        note(r, Severity::Warning, "the new script and its debug info disagree (an internal error): nothing reloaded");
        return r;
    }
    // The old VM's state, then the new script in its place (the old file kept here for its
    // layout and names).
    const VmState old = takeState(host->vm());
    ScriptFile oldFile = std::exchange(A::file(server), std::move(newFile));
    const ScriptFile& file = A::file(server);
    host->vm() = script::Vm(file, *host);
    script::Vm& vm = host->vm();
    StringMap strings(old, file, vm);
    vm.setStack(old.stack, old.top);
    host->setLanguageGlobals();
    r.applied = true;

    // Globals by (name, type, size), their values kept; the rest new.
    std::map<std::string, const DebugVar*> oldGlobals;
    for (const DebugVar& g : oldDebug.globals) {
        oldGlobals.emplace(g.name, &g);
    }
    std::set<std::uint32_t> fresh;
    std::set<std::string> present;
    const auto globals = vm.globals();
    for (const DebugVar& g : newDebug.globals) {
        if (g.symbol < 4) {
            continue; // (the language's four, set above)
        }
        present.insert(g.name);
        const auto it = oldGlobals.find(g.name);
        if (it != oldGlobals.end() && it->second->type == g.type && it->second->size == g.size) {
            copyWords(old.globals, oldFile.functions[1].symbolOffsets[it->second->symbol], globals, file.functions[1].symbolOffsets[g.symbol],
                      g.size, g.type == Type::String, strings);
            ++r.globalsKept;
            continue;
        }
        fresh.insert(g.symbol);
        ++r.globalsNew;
        if (it != oldGlobals.end()) {
            note(r, Severity::Info, "the global " + g.name + " changed from " + typeText(*it->second) + " to " + typeText(g) + ": initialised afresh", g.loc);
        }
    }
    for (const DebugVar& g : oldDebug.globals) {
        if (g.symbol >= 4 && !present.contains(g.name)) {
            ++r.globalsDropped;
            note(r, Severity::Info, "the global " + g.name + " was removed: its value is dropped");
        }
    }
    // The new ones' initialisers alone, in GLOBAL's order.
    std::vector<script::DebugGlobalInit> inits;
    for (const auto& i : newDebug.globalInits) {
        if (fresh.contains(i.symbol)) {
            inits.push_back(i);
        }
    }
    std::ranges::sort(inits, [](const auto& a, const auto& b) { return a.begin < b.begin; });
    for (const auto& i : inits) {
        vm.callRange(1, i.begin, i.end);
        ++r.initialisersRun;
    }

    // Persistent locals by (function, name: its n-th declaration for the n-th), type and size.
    for (const auto& nf : newDebug.functions) {
        const script::DebugFunction* of = oldDebug.function(nf.name);
        if (!of) {
            continue;
        }
        const auto locals = vm.locals(nf.index);
        std::map<std::string, std::size_t> seen;
        for (const DebugVar& v : nf.symbols) {
            if (v.name.empty()) {
                continue; // (a switch's temporary: used only inside a call)
            }
            const DebugVar* ov = nthNamed(of->symbols, v.name, seen[v.name]++);
            if (!ov) {
                continue;
            }
            if (ov->type != v.type || ov->size != v.size) {
                ++r.localsReset;
                note(r, Severity::Info, nf.name + "'s local " + v.name + " changed from " + typeText(*ov) + " to " + typeText(v) + ": reset", v.loc);
                continue;
            }
            copyWords(old.locals[of->index], oldFile.functions[of->index].symbolOffsets[ov->symbol], locals,
                      file.functions[nf.index].symbolOffsets[v.symbol], v.size, v.type == Type::String, strings);
            ++r.localsKept;
        }
    }
    migrateObjectVars(A::objectVars(*host), oldFile, file, oldDebug, newDebug, strings, r);
    r.stringsRemapped = strings.remapped();
    if (strings.orphans() > 0) {
        note(r, Severity::Info, std::to_string(strings.orphans()) + " string values held text the new script no longer has: kept as run-time strings");
    }

    // Timers by their function's name.
    auto& timers = A::timers(*host);
    for (auto it = timers.begin(); it != timers.end();) {
        const auto to = newIndex(oldFile, file, it->function);
        if (!to) {
            const std::uint32_t due = it->frame > host->frame() ? it->frame - host->frame() : 0;
            note(r, Severity::Warning,
                 "timer " + std::to_string(it->id) + " (to call " + oldFile.functions[it->function].name + " in " + std::to_string(due) +
                     " frames) is cancelled: " + oldFile.functions[it->function].name + " was deleted");
            it = timers.erase(it);
            ++r.timersCancelled;
            continue;
        }
        if (*to != it->function) {
            it->function = *to;
            ++r.timersRemapped;
        }
        ++it;
    }
    remapDialogs(server, oldFile, file, r);
    reportHandlers(server, file, r);
    A::storeScriptBytes(server);
    return r;
}

ReloadReport restartScripts(Server& server, ScriptFile newFile)
{
    using A = ScriptReloadAccess;
    ReloadReport r;
    ScriptHost* host = A::host(server);
    if (!host) {
        note(r, Severity::Warning, "no map script is running");
        return r;
    }
    ScriptFile oldFile = std::exchange(A::file(server), std::move(newFile));
    const ScriptFile& file = A::file(server);
    host->vm() = script::Vm(file, *host);
    r.applied = true;
    r.timersCancelled = static_cast<std::uint32_t>(A::timers(*host).size());
    A::timers(*host).clear();
    A::queued(*host).clear();
    remapDialogs(server, oldFile, file, r);
    reportHandlers(server, file, r);
    A::storeScriptBytes(server);
    // As a map's load and start run them (on the world as it is).
    host->runGlobalInit();
    host->runEntryPoints("MapInitialize");
    host->runEntryPoints("MapEntry");
    note(r, Severity::Info, "scripts restarted: GLOBAL, MapInitialize and MapEntry ran again; " + std::to_string(r.timersCancelled) + " timers cleared");
    return r;
}

} // namespace dev
} // namespace nox::server
