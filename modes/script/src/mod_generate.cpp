// A mod's maps, generated (mod_generate.hpp).
#include "mod_generate.hpp"

#include "mod_parts.hpp"
#include "mod_store.hpp"
#include "toolbox.hpp"
#include "value_codec.hpp"

#include "gen_bindings.hpp"
#include "map_writer.hpp"
#include "mode_shared.hpp"

#include "noxcore/mapgen/arena_lua.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/x87.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace nox::modes::script {

namespace fs = std::filesystem;

namespace {

/// Builds let go by games that ended (noxsdl's leaving a game mustn't wait), waited for at the
/// program's end (Generation::finishReleased).
std::mutex gReleasedMutex;
std::vector<std::future<Generation::Built>>& released()
{
    static auto* all = new std::vector<std::future<Generation::Built>>(); // (never freed: workers at exit)
    return *all;
}

void letGo(std::future<Generation::Built> f)
{
    if (!f.valid()) {
        return;
    }
    const std::lock_guard lock(gReleasedMutex);
    auto& all = released();
    std::erase_if(all, [](std::future<Generation::Built>& g) { return g.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    all.push_back(std::move(f));
}

/// The build itself (any thread: it reads only what it's given).
Generation::Built build(const things::ThingDb* things, std::shared_ptr<const GenModules> modules, Generation::Request r)
{
    Generation::Built out;
    // (NoX Core's own generator: to nearest wherever it runs, as buildArena's.)
    const util::RoundingScope nearest(false);
    try {
        if (!things) {
            throw std::runtime_error("no thing.bin");
        }
        if (r.arena) {
            mapgen::ArenaSpots spots;
            r.arenaParams.seed = r.seed;
            const map::MapFile map = mapgen::buildArena(*things, r.arenaParams, &spots);
            out.files = mapgen::encodeMapFiles(map, *things);
            out.spots = std::move(spots);
        } else {
            GenVm vm(Generation::limits(), std::move(modules));
            mapgen::detail::MapWriter writer(*things);
            std::string encodeError;
            vm.call(r.module, r.fn,
                    [&](lua_State* L) {
                        if (!decodeValue(L, r.params, ModStore::kLimits)) {
                            lua_pushnil(L);
                        }
                        if (lua_istable(L, -1)) {
                            lua_pushnumber(L, r.seed);
                            lua_setfield(L, -2, "seed");
                        }
                        mapgen::detail::pushMapWriter(L, writer);
                        return 2;
                    },
                    1,
                    [&](lua_State* L) {
                        if (!lua_isnil(L, -1) && !encodeValue(L, -1, out.result, encodeError, ModStore::kLimits)) {
                            out.result.clear();
                        }
                    });
            if (!encodeError.empty()) {
                throw std::runtime_error("its result can't be kept: " + encodeError);
            }
            const map::MapFile map = writer.finish({.title = r.title, .author = r.author, .gameFlags = r.gameFlags, .minPlayers = r.minPlayers,
                                                    .maxPlayers = r.maxPlayers});
            out.files = mapgen::encodeMapFiles(map, *things);
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    } catch (...) {
        out.error = "the build failed";
    }
    return out;
}

void setNumber(lua_State* L, const char* name, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, name);
}

void pushPoint(lua_State* L, const mapgen::ArenaSpots::Point& p)
{
    lua_createtable(L, 0, 2);
    setNumber(L, "x", p.x);
    setNumber(L, "y", p.y);
}

/// The arena builder's spots as a table: {end, centre = {...}, gates = {{type, at, sign}...},
/// features = {...}}.
void pushSpots(lua_State* L, const mapgen::ArenaSpots& s)
{
    lua_createtable(L, 0, 4);
    pushPoint(L, s.end);
    lua_setfield(L, -2, "end");
    lua_createtable(L, static_cast<int>(s.centre.size()), 0);
    for (std::size_t i = 0; i < s.centre.size(); ++i) {
        pushPoint(L, s.centre[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "centre");
    lua_createtable(L, static_cast<int>(s.gates.size()), 0);
    for (std::size_t i = 0; i < s.gates.size(); ++i) {
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, s.gates[i].type.data(), s.gates[i].type.size());
        lua_setfield(L, -2, "type");
        pushPoint(L, s.gates[i].at);
        lua_setfield(L, -2, "at");
        pushPoint(L, s.gates[i].sign);
        lua_setfield(L, -2, "sign");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "gates");
    lua_createtable(L, static_cast<int>(s.features.size()), 0);
    for (std::size_t i = 0; i < s.features.size(); ++i) {
        lua_pushlstring(L, s.features[i].data(), s.features[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "features");
}

std::string upper(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string stringField(lua_State* L, int t, const char* key, std::string fallback, std::size_t maxLength)
{
    lua_rawgetfield(L, t, key);
    if (!lua_isnil(L, -1)) {
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        if (!s || lua_type(L, -1) != LUA_TSTRING || n == 0 || n > maxLength) {
            luaL_error(L, "nox.generate: %s is text of 1-%d letters", key, static_cast<int>(maxLength));
        }
        fallback.assign(s, n);
    }
    lua_pop(L, 1);
    return fallback;
}

double numberField(lua_State* L, int t, const char* key, double fallback, double lo, double hi)
{
    lua_rawgetfield(L, t, key);
    double v = fallback;
    if (!lua_isnil(L, -1)) {
        v = lua_tonumber(L, -1);
        if (!lua_isnumber(L, -1) || !(v >= lo && v <= hi) || v != std::floor(v)) {
            luaL_error(L, "nox.generate: %s is a whole number, %.0f-%.0f", key, lo, hi);
        }
    }
    lua_pop(L, 1);
    return v;
}

} // namespace

std::string mapPrefixProblem(std::string_view prefix)
{
    if (prefix.empty()) {
        return {};
    }
    if (prefix.size() < 2 || prefix.size() > 3 || !std::ranges::all_of(prefix, [](char c) { return c >= 'A' && c <= 'Z'; })) {
        return "\"mapPrefix\" is 2-3 of A-Z";
    }
    return {};
}

void Generation::setUp(std::string prefix, int slot, fs::path mapsDir, const things::ThingDb* things)
{
    prefix_ = std::move(prefix);
    slot_ = slot;
    mapsDir_ = std::move(mapsDir);
    things_ = things;
}

int Generation::sweepOwn() const
{
    if (prefix_.empty() || mapsDir_.empty()) {
        return 0;
    }
    return server::detail::sweepMaps(mapsDir_, [&](const std::string& name) { return ownsName(name); });
}

std::optional<std::string> Generation::nameOf(std::string_view tag) const
{
    const bool ok = !tag.empty() && tag.size() <= 3 && tag[0] >= 'a' && tag[0] <= 'z' &&
                    std::ranges::all_of(tag, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
    if (!ok || prefix_.empty()) {
        return std::nullopt;
    }
    std::string name = prefix_ + "_" + std::to_string(slot_) + upper(tag);
    if (name.size() > 8) {
        return std::nullopt;
    }
    return name;
}

bool Generation::ownsName(std::string_view name) const
{
    if (prefix_.empty()) {
        return false;
    }
    const std::string mine = prefix_ + "_" + std::to_string(slot_);
    return name.size() > mine.size() && util::equalsNoCase(name.substr(0, mine.size()), mine) && name[mine.size()] >= 'A' &&
           ((name[mine.size()] <= 'Z') || (name[mine.size()] >= 'a' && name[mine.size()] <= 'z'));
}

Generation::Request Generation::requestArg(lua_State* L, int idx) const
{
    if (prefix_.empty()) {
        luaL_error(L, "nox.generate: the mod has no \"mapPrefix\" in its mode.json");
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    const int t = lua_absindex(L, idx);
    Request r;
    r.tag = stringField(L, t, "map", "", 3);
    const auto name = nameOf(r.tag);
    if (!name) {
        luaL_error(L, "nox.generate: map is a tag of 1-3 of a-z and 0-9, a letter first (its name %s_<slot><TAG>, 8 letters at most)",
                   prefix_.c_str());
    }
    r.name = *name;
    r.seed = static_cast<std::uint32_t>(numberField(L, t, "seed", 1, 0, 4294967295.0));
    lua_rawgetfield(L, t, "arena");
    if (lua_istable(L, -1)) {
        r.arena = true;
        mapgen::readArenaParams(L, lua_gettop(L), r.arenaParams, mapgen::ArenaField::All, mapgen::ArenaRead::Typed);
        lua_pop(L, 1);
        return r;
    }
    lua_pop(L, 1);
    r.module = stringField(L, t, "module", "", 32);
    r.fn = stringField(L, t, "fn", "build", 32);
    if (r.module.empty()) {
        luaL_error(L, "nox.generate: module (a gen/ script's name) or arena (the arena builder's parameters)");
    }
    lua_rawgetfield(L, t, "params");
    if (!lua_isnil(L, -1)) {
        std::string error;
        if (!encodeValue(L, -1, r.params, error, ModStore::kLimits)) {
            luaL_error(L, "nox.generate: params: %s", error.c_str());
        }
    }
    lua_pop(L, 1);
    r.title = stringField(L, t, "title", toolbox::manifestOf(L).info.name, 64);
    r.author = toolbox::manifestOf(L).info.name;
    r.gameFlags = static_cast<std::uint32_t>(numberField(L, t, "gameFlags", 4, 0, 4294967295.0));
    r.minPlayers = static_cast<std::uint8_t>(numberField(L, t, "minPlayers", 1, 1, 32));
    r.maxPlayers = static_cast<std::uint8_t>(numberField(L, t, "maxPlayers", 6, r.minPlayers, 32));
    return r;
}

Generation::Built Generation::buildNow(const Request& r) const
{
    return build(things_, modules_, r);
}

Generation::Built Generation::runNow(const Request& r, std::string_view currentMap)
{
    std::erase_if(outcomes_, [&](const auto& o) { return o.first == r.tag; });
    Job j{.request = r};
    j.built = buildNow(r);
    j.done = true;
    write(j, currentMap);
    return std::move(j.built);
}

void Generation::pushResult(lua_State* L, const Built& b)
{
    if (!b.ok) {
        lua_pushlstring(L, b.error.data(), b.error.size());
    } else if (b.spots) {
        pushSpots(L, *b.spots);
    } else if (b.result.empty() || !decodeValue(L, b.result, ModStore::kLimits)) {
        lua_pushnil(L);
    }
}

bool Generation::start(Request r, std::string& error)
{
    if (jobs_.size() >= kMaxQueued) {
        error = "busy (" + std::to_string(kMaxQueued) + " builds asked for already)";
        return false;
    }
    if (std::ranges::any_of(jobs_, [&](const Job& j) { return j.request.tag == r.tag; })) {
        error = "building " + r.tag + " already";
        return false;
    }
    std::erase_if(outcomes_, [&](const auto& o) { return o.first == r.tag; });
    jobs_.push_back({.request = std::move(r)});
    startWaiting();
    return true;
}

void Generation::startWaiting()
{
    std::size_t running = static_cast<std::size_t>(std::ranges::count_if(jobs_, [](const Job& j) { return j.running; }));
    for (Job& j : jobs_) {
        if (running >= kRunning) {
            break;
        }
        if (!j.running && !j.done) {
            j.future = std::async(std::launch::async, build, things_, modules_, j.request);
            j.running = true;
            ++running;
        }
    }
}

std::optional<std::string_view> Generation::state(std::string_view tag) const
{
    if (std::ranges::any_of(jobs_, [&](const Job& j) { return j.request.tag == tag; })) {
        return "building";
    }
    for (const auto& [t, ok] : outcomes_) {
        if (t == tag) {
            return ok ? "ready" : "failed";
        }
    }
    return std::nullopt;
}

bool Generation::cancel(std::string_view tag)
{
    const auto it = std::ranges::find_if(jobs_, [&](const Job& j) { return j.request.tag == tag; });
    if (it == jobs_.end()) {
        return false;
    }
    letGo(std::move(it->future));
    jobs_.erase(it);
    startWaiting();
    return true;
}

void Generation::write(Job& job, std::string_view currentMap)
{
    Built& b = job.built;
    if (b.ok && util::equalsNoCase(job.request.name, currentMap)) {
        b.ok = false;
        b.error = job.request.name + " is in use (the map being played): not written over";
    }
    if (b.ok) {
        try {
            mapgen::writeEncodedMapFiles(mapsDir_, job.request.name, b.files);
        } catch (const std::exception& e) {
            b.ok = false;
            b.error = std::string("its files couldn't be written: ") + e.what();
        }
    }
    b.files = {}; // (the bytes written: let go)
    outcomes_.emplace_back(job.request.tag, b.ok);
    if (outcomes_.size() > 64) {
        outcomes_.erase(outcomes_.begin());
    }
}

void Generation::told(std::string tag, Built built)
{
    told_.emplace_back(std::move(tag), std::move(built));
}

void Generation::waitFor(std::string_view name, std::string_view currentMap)
{
    const auto it = std::ranges::find_if(jobs_, [&](const Job& j) { return util::equalsNoCase(j.request.name, name); });
    if (it == jobs_.end()) {
        return;
    }
    it->built = it->running ? it->future.get() : buildNow(it->request);
    it->done = true;
    write(*it, currentMap);
    told(it->request.tag, std::move(it->built));
    jobs_.erase(it);
    startWaiting();
}

void Generation::poll(lua_State* L, std::string_view currentMap)
{
    for (Job& j : jobs_) {
        if (j.running && !j.done && j.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            j.built = j.future.get();
            j.done = true;
            write(j, currentMap);
            told(j.request.tag, std::move(j.built));
        }
    }
    std::erase_if(jobs_, [](const Job& j) { return j.done; });
    startWaiting();
    if (told_.empty()) {
        return;
    }
    auto now = std::exchange(told_, {});
    for (auto& [tag, b] : now) {
        if (!toolbox::beginHook(L, "onGenerated")) {
            if (!b.ok) {
                toolbox::report(L, "nox.generate " + tag + ": " + b.error);
            }
            continue;
        }
        lua_pushlstring(L, tag.data(), tag.size());
        lua_pushboolean(L, b.ok);
        pushResult(L, b);
        toolbox::callHook(L, "onGenerated", 3, 0);
    }
}

void Generation::release()
{
    for (Job& j : jobs_) {
        letGo(std::move(j.future));
    }
    jobs_.clear();
    told_.clear();
}

void Generation::stop()
{
    for (Job& j : jobs_) {
        if (j.future.valid()) {
            j.future.wait();
        }
    }
    jobs_.clear();
    told_.clear();
}

void Generation::finishReleased()
{
    std::vector<std::future<Built>> all;
    {
        const std::lock_guard lock(gReleasedMutex);
        all.swap(released());
    }
    for (auto& f : all) {
        if (f.valid()) {
            f.wait();
        }
    }
}

} // namespace nox::modes::script
