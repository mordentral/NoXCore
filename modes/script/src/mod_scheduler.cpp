// The script's timers (mod_scheduler.hpp).
#include "mod_scheduler.hpp"

#include "toolbox.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>

namespace nox::modes::script {

int Scheduler::add(lua_State* L, std::uint32_t now, bool repeat)
{
    const double frames = luaL_checknumber(L, 1);
    luaL_argcheck(L, frames >= 1 && frames <= kMaxFrames && frames == std::floor(frames), 1, "a whole number of frames, 1 to a day's");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (live_.size() >= kMaxPending) {
        luaL_error(L, "%d timers are waiting already (cancel some)", static_cast<int>(kMaxPending));
    }
    // {fn, args...}, held by the registry till it's run (or dropped).
    const int top = lua_gettop(L);
    const int nargs = top - 2;
    lua_createtable(L, nargs + 1, 0);
    for (int i = 2; i <= top; ++i) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i - 1);
    }
    Timer t;
    t.ref = lua_ref(L, -1);
    lua_pop(L, 1);
    t.nargs = nargs;
    t.every = repeat ? static_cast<std::uint32_t>(frames) : 0;
    t.at = now + static_cast<std::uint32_t>(frames);
    t.order = nextOrder_++;
    std::uint32_t id = nextId_++;
    while (id == 0 || live_.contains(id)) { // (after four billion: past the ones still waiting)
        id = nextId_++;
    }
    due_.push({t.at, t.order, id});
    live_.emplace(id, t);
    lua_pushnumber(L, id);
    return 1;
}

bool Scheduler::cancel(lua_State* L, std::uint32_t id)
{
    const auto it = live_.find(id);
    if (it == live_.end()) {
        return false;
    }
    lua_unref(L, it->second.ref);
    live_.erase(it);
    if (due_.size() > 2 * kMaxPending + 64) {
        compact(); // (made and cancelled over and over: the heap kept to the live ones)
    }
    return true;
}

void Scheduler::compact()
{
    std::vector<Due> keep;
    keep.reserve(live_.size());
    for (const auto& [id, t] : live_) {
        keep.push_back({t.at, t.order, id});
    }
    due_ = decltype(due_)(std::greater<>{}, std::move(keep));
}

void Scheduler::run(lua_State* L, std::uint32_t frame)
{
    for (int runs = 0; runs < kRunsPerFrame && !due_.empty() && due_.top().frame <= frame;) {
        const Due d = due_.top();
        due_.pop();
        const auto it = live_.find(d.id);
        if (it == live_.end() || it->second.at != d.frame || it->second.order != d.order) {
            continue; // (cancelled: its entry stale)
        }
        const Timer t = it->second;
        if (t.every == 0) {
            live_.erase(it); // (gone before it runs: cancelling itself is a no-op)
        } else {
            it->second.at = d.frame + t.every; // (its own beat: no drift)
            due_.push({it->second.at, t.order, d.id});
        }
        ++runs;
        if (!lua_checkstack(L, t.nargs + 2)) {
            toolbox::report(L, "a timer's arguments don't fit the stack (not run)");
            if (t.every == 0) {
                lua_unref(L, t.ref);
            }
            continue;
        }
        lua_getref(L, t.ref);
        for (int i = 1; i <= t.nargs + 1; ++i) {
            lua_rawgeti(L, -i, i); // (the table moves down as each is pushed)
        }
        lua_remove(L, -(t.nargs + 2));
        toolbox::callHook(L, t.every ? "nox.every's function" : "nox.after's function", t.nargs, 0);
        if (t.every == 0) {
            lua_unref(L, t.ref);
        }
    }
}

void Scheduler::clear(lua_State* L)
{
    for (const auto& [id, t] : live_) {
        lua_unref(L, t.ref);
    }
    live_.clear();
    due_ = {};
}

} // namespace nox::modes::script
