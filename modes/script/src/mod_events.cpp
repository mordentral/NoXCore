// The engine's events to a script's hooks (mod_events.hpp).
#include "mod_events.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/things/spell_ids.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <string>
#include <utility>

namespace nox::modes::script {

namespace {

void pushId(lua_State* L, std::uint32_t id)
{
    id ? lua_pushnumber(L, id) : lua_pushnil(L);
}

void setNumber(lua_State* L, const char* name, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, name);
}

/// {id, type, x, y}: a thing's facts as they were when the engine told them.
void pushFacts(lua_State* L, std::uint32_t id, const server::Name& type, float x, float y)
{
    lua_createtable(L, 0, 5);
    setNumber(L, "id", id);
    lua_pushlstring(L, type.data(), type.size());
    lua_setfield(L, -2, "type");
    setNumber(L, "x", x);
    setNumber(L, "y", y);
}

} // namespace

void Events::look(lua_State* L)
{
    const auto hasFn = [&](const char* name) {
        lua_rawgetfield(L, -1, name);
        const bool f = lua_isfunction(L, -1);
        lua_pop(L, 1);
        return f;
    };
    has_ = {.onObjectKilled = hasFn("onObjectKilled"), .onDestroyed = hasFn("onDestroyed"), .onDamage = hasFn("onDamage"),
            .onDamaged = hasFn("onDamaged"), .onPickup = hasFn("onPickup"), .onDrop = hasFn("onDrop"), .onEquip = hasFn("onEquip"),
            .onSpellCast = hasFn("onSpellCast"), .onExit = hasFn("onExit"), .castCost = hasFn("castCost")};
}

bool Events::room()
{
    if (queue_.size() < kPerFrame) {
        return true;
    }
    ++overflow_;
    return false;
}

void Events::killed(const server::ServerObject& victim, const server::ServerObject* by)
{
    if (has_.onObjectKilled && room()) {
        queue_.push_back({.kind = Kind::Killed, .a = victim.extent, .b = by ? by->extent : 0, .x = victim.x, .y = victim.y, .type = victim.typeName});
    }
}

void Events::destroyed(const server::ServerObject& o)
{
    if (has_.onDestroyed && room()) {
        queue_.push_back({.kind = Kind::Destroyed, .a = o.extent, .x = o.x, .y = o.y, .type = o.typeName});
    }
}

void Events::damaged(const server::ServerObject& target, int amount)
{
    if (has_.onDamaged && room()) {
        queue_.push_back({.kind = Kind::Damaged, .a = target.extent, .n = amount});
    }
}

void Events::pickedUp(const server::ServerObject& taker, const server::ServerObject& item, float x, float y)
{
    if (has_.onPickup && room()) {
        queue_.push_back({.kind = Kind::PickedUp, .a = taker.extent, .b = item.extent, .x = x, .y = y});
    }
}

void Events::dropped(const server::ServerObject& holder, const server::ServerObject& item)
{
    if (has_.onDrop && room()) {
        queue_.push_back({.kind = Kind::Dropped, .a = holder.extent, .b = item.extent});
    }
}

void Events::equipped(const server::ServerObject& unit, const server::ServerObject& item, bool worn)
{
    if (has_.onEquip && room()) {
        queue_.push_back({.kind = Kind::Equipped, .a = unit.extent, .b = item.extent, .n = worn ? 1 : 0});
    }
}

void Events::spellCast(const server::ServerObject& caster, std::uint32_t spell, const server::ServerObject* target, float x, float y,
                       std::span<const std::uint32_t> made)
{
    if (has_.onSpellCast && room()) {
        queue_.push_back({.kind = Kind::SpellCast, .a = caster.extent, .b = target ? target->extent : 0, .x = x, .y = y,
                          .n = static_cast<std::int32_t>(spell), .madeFrom = static_cast<std::uint32_t>(made_.size()),
                          .madeCount = static_cast<std::uint32_t>(made.size())});
        made_.insert(made_.end(), made.begin(), made.end());
    }
}

bool Events::ask(lua_State* L, const char* hook, int nargs, int nresults)
{
    Parts& parts = toolbox::partsOf(L);
    const char* outer = std::exchange(parts.asked, hook);
    const bool ok = toolbox::callHook(L, hook, nargs, nresults);
    parts.asked = outer;
    return ok;
}

bool Events::damage(lua_State* L, const server::ServerObject& target, const server::ServerObject* source,
                    const server::ServerObject* weapon, int& amount, int type)
{
    if (!has_.onDamage || !toolbox::beginHook(L, "onDamage")) {
        return true;
    }
    lua_pushnumber(L, target.extent);
    pushId(L, source ? source->extent : 0);
    pushId(L, weapon ? weapon->extent : 0);
    lua_pushnumber(L, amount);
    lua_pushnumber(L, type);
    if (!ask(L, "onDamage", 5, 1)) {
        return true; // (a failing hook changes nothing)
    }
    bool keep = true;
    if (lua_isnumber(L, -1)) {
        const double n = lua_tonumber(L, -1);
        if (!(n >= 0 && n <= 1'000'000)) { // (NaN too)
            toolbox::report(L, "onDamage: an amount from 0 to 1000000 (the hit kept as it was)");
        } else {
            amount = static_cast<int>(std::floor(n));
            keep = amount != 0;
        }
    } else if (!lua_isnil(L, -1)) {
        toolbox::report(L, "onDamage: return a number or nil (the hit kept as it was)");
    }
    lua_pop(L, 1);
    return keep;
}

bool Events::exitTouched(lua_State* L, const server::ServerObject& exit, const server::ServerObject& player)
{
    if (!has_.onExit || !toolbox::beginHook(L, "onExit")) {
        return true;
    }
    lua_pushnumber(L, exit.extent);
    lua_pushnumber(L, player.extent);
    if (!ask(L, "onExit", 2, 1)) {
        return true;
    }
    const bool go = !(lua_isboolean(L, -1) && !lua_toboolean(L, -1));
    lua_pop(L, 1);
    return go;
}

bool Events::castCost(lua_State* L, const server::ServerObject& caster, std::uint32_t spell, int cost, bool trap, int& mana, int& health)
{
    if (!has_.castCost || !toolbox::beginHook(L, "castCost")) {
        return false;
    }
    lua_pushnumber(L, caster.extent);
    const std::string_view name = things::spellName(spell);
    if (name.empty()) {
        lua_pushnumber(L, spell);
    } else {
        lua_pushlstring(L, name.data(), name.size());
    }
    lua_pushnumber(L, cost);
    lua_pushstring(L, trap ? "trap" : "cast");
    if (!ask(L, "castCost", 4, 2)) {
        return false; // (a failing hook: the game's price)
    }
    // (Results: -2 the mana, -1 the health.)
    const auto whole = [](lua_State* L, int idx, int& out) {
        if (lua_isnil(L, idx)) {
            out = 0;
            return true;
        }
        if (!lua_isnumber(L, idx)) {
            return false;
        }
        const double n = lua_tonumber(L, idx);
        if (!(n >= 0 && n <= 65535)) { // (NaN too)
            return false;
        }
        out = static_cast<int>(std::floor(n));
        return true;
    };
    bool priced = false;
    if (!lua_isnil(L, -2)) {
        int m = 0, h = 0;
        if (whole(L, -2, m) && whole(L, -1, h)) {
            mana = m;
            health = h;
            priced = true;
        } else {
            toolbox::report(L, "castCost: return nil, or mana and health 0-65535 (the game's price kept)");
        }
    } else if (!lua_isnil(L, -1)) {
        toolbox::report(L, "castCost: return nil, or mana and health 0-65535 (the game's price kept)");
    }
    lua_pop(L, 2);
    return priced;
}

void Events::deliver(lua_State* L)
{
    if (overflow_ != 0) {
        toolbox::report(L, std::to_string(overflow_) + " events past " + std::to_string(kPerFrame) + " in a frame (not delivered)");
        overflow_ = 0;
    }
    if (queue_.empty()) {
        return;
    }
    // (Events the script's own calls set off now are the next frame's.)
    std::vector<Event> now;
    now.swap(queue_);
    std::vector<std::uint32_t> made;
    made.swap(made_);
    for (const Event& e : now) {
        switch (e.kind) {
        case Kind::Killed:
            if (toolbox::beginHook(L, "onObjectKilled")) {
                pushFacts(L, e.a, e.type, e.x, e.y);
                if (e.b) {
                    setNumber(L, "by", e.b);
                }
                toolbox::callHook(L, "onObjectKilled", 1, 0);
            }
            break;
        case Kind::Destroyed:
            if (toolbox::beginHook(L, "onDestroyed")) {
                pushFacts(L, e.a, e.type, e.x, e.y);
                toolbox::callHook(L, "onDestroyed", 1, 0);
            }
            break;
        case Kind::Damaged:
            if (toolbox::beginHook(L, "onDamaged")) {
                lua_pushnumber(L, e.a);
                lua_pushnumber(L, e.n);
                toolbox::callHook(L, "onDamaged", 2, 0);
            }
            break;
        case Kind::PickedUp:
            if (toolbox::beginHook(L, "onPickup")) {
                lua_pushnumber(L, e.a);
                lua_pushnumber(L, e.b);
                lua_pushnumber(L, e.x);
                lua_pushnumber(L, e.y);
                toolbox::callHook(L, "onPickup", 4, 0);
            }
            break;
        case Kind::Dropped:
            if (toolbox::beginHook(L, "onDrop")) {
                lua_pushnumber(L, e.a);
                lua_pushnumber(L, e.b);
                toolbox::callHook(L, "onDrop", 2, 0);
            }
            break;
        case Kind::Equipped:
            if (toolbox::beginHook(L, "onEquip")) {
                lua_pushnumber(L, e.a);
                lua_pushnumber(L, e.b);
                lua_pushboolean(L, e.n);
                toolbox::callHook(L, "onEquip", 3, 0);
            }
            break;
        case Kind::SpellCast:
            if (toolbox::beginHook(L, "onSpellCast")) {
                lua_pushnumber(L, e.a);
                const std::string_view name = things::spellName(static_cast<std::uint32_t>(e.n));
                if (name.empty()) {
                    lua_pushnumber(L, e.n);
                } else {
                    lua_pushlstring(L, name.data(), name.size());
                }
                pushId(L, e.b);
                lua_pushnumber(L, e.x);
                lua_pushnumber(L, e.y);
                // What the cast made, oldest first (nil: nothing); each may be gone by now.
                if (e.madeCount == 0) {
                    lua_pushnil(L);
                } else {
                    lua_createtable(L, static_cast<int>(e.madeCount), 0);
                    for (std::uint32_t i = 0; i < e.madeCount; ++i) {
                        lua_pushnumber(L, made[e.madeFrom + i]);
                        lua_rawseti(L, -2, static_cast<int>(i) + 1);
                    }
                }
                toolbox::callHook(L, "onSpellCast", 6, 0);
            }
            break;
        }
    }
}

void Events::clear() noexcept
{
    queue_.clear();
    made_.clear();
    overflow_ = 0;
}

} // namespace nox::modes::script
