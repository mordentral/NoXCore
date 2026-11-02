// A script mode's abilities and their toolbox (mod_abilities.hpp).
#include "mod_abilities.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/world.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>

namespace nox::modes::script {

bool ModAbilities::reaches(const server::GameHost& host, std::uint8_t peer)
{
    return host.players().contains(peer) && !host.refused(peer) && (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 &&
           host.clientLevel(peer) >= uimsg::kAbilityLevel;
}

bool ModAbilities::define(Def d)
{
    if (const int i = defIndex(d.id); i >= 0) {
        defs_[static_cast<std::size_t>(i)] = std::move(d);
        for (auto& [peer, p] : players_) {
            if (std::ranges::any_of(p.slots, [&](const Slot& s) { return s.def == i; })) {
                p.dirty = true;
            }
        }
        return true;
    }
    if (defs_.size() >= kMaxDefs) {
        return false;
    }
    defs_.push_back(std::move(d));
    return true;
}

int ModAbilities::defIndex(std::string_view id) const
{
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ModAbilities::Player* ModAbilities::player(std::uint8_t peer)
{
    const auto it = players_.find(peer);
    return it == players_.end() ? nullptr : &it->second;
}

ModAbilities::Slot* ModAbilities::slotOf(std::uint8_t peer, int def)
{
    Player* p = player(peer);
    if (!p) {
        return nullptr;
    }
    for (Slot& s : p->slots) {
        if (s.def == def) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<int> ModAbilities::grant(std::uint8_t peer, int def)
{
    Player& p = players_[peer];
    for (std::size_t i = 0; i < p.slots.size(); ++i) {
        if (p.slots[i].def == def) {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < p.slots.size(); ++i) {
        if (p.slots[i].def < 0) {
            p.slots[i] = {def, 0};
            p.dirty = true;
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

bool ModAbilities::revoke(std::uint8_t peer, int def)
{
    Player* p = player(peer);
    if (!p) {
        return false;
    }
    for (std::size_t i = 0; i < p->slots.size(); ++i) {
        if (p->slots[i].def == def) {
            p->slots[i] = {};
            if (p->holding == static_cast<int>(i)) {
                p->holding = -1;
            }
            p->dirty = true;
            return true;
        }
    }
    return false;
}

std::optional<ModAbilities::Asked> ModAbilities::heard(std::uint8_t peer, const uimsg::AbilityUse& use, std::uint32_t frame)
{
    Player* p = player(peer);
    if (!p) {
        return std::nullopt;
    }
    // (kUsesPerSecond a second at most: a modified client can't hold the server's thread with them.)
    if (frame - p->usesFrom >= 30) {
        p->usesFrom = frame;
        p->uses = 0;
    }
    if (++p->uses > kUsesPerSecond) {
        return std::nullopt;
    }
    const Slot& s = p->slots[use.slot];
    if (s.def < 0) {
        return std::nullopt;
    }
    if (use.release) {
        if (p->holding != static_cast<int>(use.slot)) {
            return std::nullopt; // (nothing of it held: a press refused, or ended already)
        }
        return Asked{use.slot, s.def, true};
    }
    if (s.left > 0 || p->holding >= 0) {
        return std::nullopt; // (its cooldown runs; or one's held)
    }
    return Asked{use.slot, s.def, false};
}

void ModAbilities::pressed(std::uint8_t peer, int slot)
{
    Player* p = player(peer);
    if (!p) {
        return;
    }
    Slot& s = p->slots[static_cast<std::size_t>(slot)];
    const Def& d = def(s.def);
    s.left = d.cooldown;
    if (d.target == uimsg::AbilityTarget::Hold) {
        p->holding = slot;
        p->holdLeft = d.hold;
    }
    p->dirty = true;
}

void ModAbilities::letGo(std::uint8_t peer)
{
    if (Player* p = player(peer)) {
        p->holding = -1;
    }
}

std::vector<std::pair<std::uint8_t, int>> ModAbilities::step(const std::function<bool(std::uint8_t peer)>& inPlay)
{
    std::vector<std::pair<std::uint8_t, int>> ended;
    for (auto& [peer, p] : players_) {
        for (Slot& s : p.slots) {
            if (s.left > 0) {
                --s.left; // (the client counts its own down from what it was sent)
            }
        }
        if (p.holding >= 0 && (--p.holdLeft <= 0 || !inPlay(peer))) {
            ended.emplace_back(peer, p.slots[static_cast<std::size_t>(p.holding)].def);
            p.holding = -1;
        }
    }
    return ended;
}

void ModAbilities::flush(server::GameHost& host)
{
    server::World& world = host.server().world();
    for (auto& [peer, p] : players_) {
        if (!p.dirty) {
            continue;
        }
        p.dirty = false;
        if (!reaches(host, peer)) {
            continue;
        }
        std::vector<uimsg::AbilitySlot> bar;
        for (std::size_t i = 0; i < p.slots.size(); ++i) {
            const Slot& s = p.slots[i];
            if (s.def < 0) {
                continue;
            }
            const Def& d = def(s.def);
            bar.push_back({static_cast<std::uint8_t>(i), d.target, static_cast<std::uint16_t>(d.cooldown),
                           static_cast<std::uint16_t>(std::clamp(s.left, 0, 0xFFFF)), d.id, d.title, d.icon});
        }
        // (In order with the frame's other messages to it.)
        world.broadcasts().push_back({uimsg::abilityBarMessage(bar), true, host.players().at(peer).extent});
    }
}

namespace {

int stockAbility(lua_State* L, int idx);

/// The peer of the player whose unit is at `idx` (not a held-out one); an error for anything else.
std::uint8_t peerArg(lua_State* L, int idx)
{
    const server::ServerObject& u = toolbox::playerUnitArg(L, idx);
    if (const server::GameHost* host = toolbox::hostOf(L)) {
        for (const auto& [peer, p] : host->players()) {
            if (p.extent == u.extent) {
                return peer;
            }
        }
    }
    luaL_argerror(L, idx, "a player in the game");
    return 0;
}

int defArg(lua_State* L, int idx)
{
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    const int i = toolbox::partsOf(L).abilities.defIndex(std::string_view(s, len));
    luaL_argcheck(L, i >= 0, idx, "no ability of that id (nox.defineAbility first)");
    return i;
}

std::string textField(lua_State* L, int t, const char* key, std::string fallback)
{
    lua_rawgetfield(L, t, key);
    if (!lua_isnil(L, -1)) {
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        luaL_argcheck(L, s && len <= uimsg::kMaxAbilityText, 2, (std::string(key) + ": text of 64 bytes at most").c_str());
        fallback.assign(s, len);
    }
    lua_pop(L, 1);
    return fallback;
}

int framesField(lua_State* L, int t, const char* key, int fallback, int most)
{
    lua_rawgetfield(L, t, key);
    if (!lua_isnil(L, -1)) {
        const double v = lua_tonumber(L, -1);
        luaL_argcheck(L, lua_isnumber(L, -1) && v >= 0 && v <= most && v == std::floor(v), 2,
                      (std::string(key) + ": whole frames 0-" + std::to_string(most)).c_str());
        fallback = static_cast<int>(v);
    }
    lua_pop(L, 1);
    return fallback;
}

int noxDefineAbility(lua_State* L)
{
    // nox.defineAbility(id, { title, icon, cooldown = frames, target = "point" | "none" | "hold",
    // hold = frames }): an ability the mode can grant (its strip button), or that one changed.
    size_t len = 0;
    const char* id = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len >= 1 && len <= uimsg::kMaxAbilityText, 1, "an id of 1-64 bytes");
    luaL_argcheck(L, stockAbility(L, 1) == 0, 1, "a stock ability's name (nox.abilityCooldown's for the warrior's own)");
    luaL_checktype(L, 2, LUA_TTABLE);
    ModAbilities::Def d;
    d.id.assign(id, len);
    d.title = textField(L, 2, "title", d.id);
    d.icon = textField(L, 2, "icon", "");
    d.cooldown = framesField(L, 2, "cooldown", 0, ModAbilities::kMaxCooldown);
    d.hold = std::max(1, framesField(L, 2, "hold", 60, ModAbilities::kMaxHold));
    const std::string target = textField(L, 2, "target", "point");
    luaL_argcheck(L, target == "point" || target == "none" || target == "hold", 2, "target: \"point\", \"none\" or \"hold\"");
    d.target = target == "none" ? uimsg::AbilityTarget::None : target == "hold" ? uimsg::AbilityTarget::Hold : uimsg::AbilityTarget::Point;
    if (!toolbox::partsOf(L).abilities.define(std::move(d))) {
        luaL_error(L, "nox.defineAbility: %d abilities at most", static_cast<int>(ModAbilities::kMaxDefs));
    }
    return 0;
}

int noxGrantAbility(lua_State* L)
{
    // nox.grantAbility(unit, id) -> its button (1-4); nil when the strip's full, or the player's
    // client can't show a strip (stock, older than client level 8, no ModeUi: nothing granted).
    const std::uint8_t peer = peerArg(L, 1);
    const int def = defArg(L, 2);
    const server::GameHost* host = toolbox::hostOf(L);
    if (!host || !ModAbilities::reaches(*host, peer)) {
        lua_pushnil(L);
        return 1;
    }
    if (const auto slot = toolbox::partsOf(L).abilities.grant(peer, def)) {
        lua_pushnumber(L, *slot + 1);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int noxRevokeAbility(lua_State* L)
{
    // nox.revokeAbility(unit, id) -> whether it had it (a held use of it ends with no word).
    const std::uint8_t peer = peerArg(L, 1);
    const int def = defArg(L, 2);
    lua_pushboolean(L, toolbox::partsOf(L).abilities.revoke(peer, def));
    return 1;
}

/// A warrior's stock ability by its name (onAbility's): 1-5; 0: not one.
int stockAbility(lua_State* L, int idx)
{
    static constexpr std::string_view kNames[6] = {"", "BerserkerCharge", "WarCry", "Harpoon", "TreadLightly", "EyeOfTheWolf"};
    size_t len = 0;
    const char* s = lua_tolstring(L, idx, &len);
    if (!s) {
        return 0;
    }
    for (int i = 1; i < 6; ++i) {
        if (kNames[i] == std::string_view(s, len)) {
            return i;
        }
    }
    return 0;
}

/// The warrior's unit at `idx` (a player who uses abilities: a warrior, or a multiclass one).
server::ServerObject& warriorArg(lua_State* L, int idx)
{
    server::ServerObject& u = toolbox::playerUnitArg(L, idx);
    luaL_argcheck(L, u.player && u.player->usesAbilities(), idx, "a warrior (a player who uses abilities)");
    return u;
}

int noxAbilityCooldown(lua_State* L)
{
    // nox.abilityCooldown(unit, id) -> frames till it can be used again (0: ready, or not granted);
    // a stock ability by its name ("BerserkerCharge", "WarCry", "Harpoon", "TreadLightly",
    // "EyeOfTheWolf") is the warrior's own cool-down.
    if (const int stock = stockAbility(L, 2)) {
        const server::ServerObject& u = warriorArg(L, 1);
        lua_pushnumber(L, u.player->abilityCooldown[static_cast<std::size_t>(stock)]);
        return 1;
    }
    const std::uint8_t peer = peerArg(L, 1);
    const int def = defArg(L, 2);
    const ModAbilities::Slot* s = toolbox::partsOf(L).abilities.slotOf(peer, def);
    lua_pushnumber(L, s ? s->left : 0);
    return 1;
}

int noxSetAbilityCooldown(lua_State* L)
{
    // nox.setAbilityCooldown(unit, id, frames): its cooldown now (0: ready); whether it's granted.
    // A stock ability's (by its name): the warrior's own cool-down, its client told as the game
    // tells it (MSG_REPORT_ABILITY_STATE: ready at 0, else not ready; the bar's shading runs the
    // game's own delay from then); its records under way left alone.
    toolbox::refuseInsideAsked(L, "nox.setAbilityCooldown");
    const double frames = luaL_checknumber(L, 3);
    luaL_argcheck(L, frames >= 0 && frames <= ModAbilities::kMaxCooldown && frames == std::floor(frames), 3, "whole frames 0-18000");
    if (const int stock = stockAbility(L, 2)) {
        server::ServerObject& u = warriorArg(L, 1);
        u.player->abilityCooldown[static_cast<std::size_t>(stock)] = static_cast<int>(frames);
        toolbox::worldOf(L).broadcasts().push_back(
            {{0xCE, static_cast<std::uint8_t>(stock), static_cast<std::uint8_t>(frames == 0 ? 1 : 0)}, true, u.extent});
        lua_pushboolean(L, true);
        return 1;
    }
    const std::uint8_t peer = peerArg(L, 1);
    const int def = defArg(L, 2);
    ModAbilities& a = toolbox::partsOf(L).abilities;
    ModAbilities::Slot* s = a.slotOf(peer, def);
    if (s) {
        s->left = static_cast<int>(frames);
        a.player(peer)->dirty = true;
    }
    lua_pushboolean(L, s != nullptr);
    return 1;
}

} // namespace

void addAbilityToolbox(lua_State* L)
{
    static constexpr std::pair<const char*, lua_CFunction> kFns[] = {
        {"defineAbility", noxDefineAbility}, {"grantAbility", noxGrantAbility}, {"revokeAbility", noxRevokeAbility},
        {"abilityCooldown", noxAbilityCooldown}, {"setAbilityCooldown", noxSetAbilityCooldown},
    };
    for (const auto& [name, fn] : kFns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
}

} // namespace nox::modes::script
