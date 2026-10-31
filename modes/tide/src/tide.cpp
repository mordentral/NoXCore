// Undead Tide's director (tide.hpp describes the run as a whole). In this file, in order: the
// tables' answers (data.luau's, read at set-up: tide_data.hpp), the director's script and what it
// may ask of the director (its owner functions), the lobby and arena maps, the
// MSG_NOXCPP_MODE_F5 codec (tidemsg), then the director's frame: tick runs the lobby (runLobby) or
// the arena (runArena), which runs the tide, each player's weapons, the soul gems, picks, chests,
// pickups and the revive shrine on modes/common's blocks and the script's rules.
#include "noxcore/server/tide.hpp"
#include "noxcore/util/x87.hpp"

#include "noxcore/mapgen/arena_lua.hpp"
#include "noxcore/mapgen/biome.hpp"
#include "noxcore/modes/mode_frame.hpp"
#include "noxcore/modes/script/gen_runtime.hpp"
#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/modes/script/scripted_mode.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/record_class.hpp"
#include "noxcore/server/tide_records.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include "gather.hpp"
#include "horde.hpp"
#include "lua_lists.hpp"
#include "mode_script.hpp"
#include "mode_shared.hpp"
#include "ready_timers.hpp"
#include "record_file.hpp"
#include "watchers.hpp"
#include "tide_data.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace nox::server {

namespace oc = things::ObjectClass;

namespace {

// The tables (the auto-weapons, the passives, the pickups, the difficulties, each biome's
// creatures, the lobby's signs) are the script's (script/server/data.luau), read once at set-up
// (tide_data.hpp). So are its words: what it tells players, templates filled in as they're told.
using tide_data::data;
using modes::script::fillText;
using modes::detail::pushList;
using modes::detail::pushNumbers;

const tide_data::Text& words()
{
    return data().text;
}

/// A number as the words have it.
std::string num(long long n)
{
    return std::to_string(n);
}

const tide_data::Hardness& hardness(TideDifficulty d)
{
    const auto& all = data().difficulties;
    return all[std::min<std::size_t>(static_cast<std::size_t>(d), all.size() - 1)];
}

/// A run of the records as the script's words read it (told.record): how it ended, its time,
/// stage, difficulty, biome and date, the kills, the party (each its name, class by its name:
/// record_class.hpp, and level).
void pushRecord(lua_State* L, const TideRecord& r)
{
    lua_createtable(L, 0, 8);
    lua_pushnumber(L, r.outcome);
    lua_setfield(L, -2, "outcome");
    lua_pushnumber(L, r.seconds);
    lua_setfield(L, -2, "seconds");
    lua_pushnumber(L, r.stage);
    lua_setfield(L, -2, "stage");
    lua_pushlstring(L, r.difficulty.data(), r.difficulty.size());
    lua_setfield(L, -2, "difficulty");
    lua_pushlstring(L, r.biome.data(), r.biome.size());
    lua_setfield(L, -2, "biome");
    lua_pushlstring(L, r.date.data(), r.date.size());
    lua_setfield(L, -2, "date");
    lua_pushnumber(L, r.kills());
    lua_setfield(L, -2, "kills");
    pushList(L, r.party, [L](const TideRecord::Member& m) {
        const std::string_view className = recordClassName(m.cls);
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, m.name.data(), m.name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, className.data(), className.size());
        lua_setfield(L, -2, "class");
        lua_pushnumber(L, m.level);
        lua_setfield(L, -2, "level");
    });
    lua_setfield(L, -2, "party");
}

/// The Tide's script (script/server/main.luau, built into the program) as a script mode its
/// director owns (mode_script.hpp). Its budget a built-in script's (LuauVm::Limits::builtIn);
/// `safepoints` (tests) a count instead.
std::unique_ptr<modes::script::ScriptedMode> makeScript(const things::ThingDb* things, const std::function<void(modes::script::ScriptedMode&)>& owner = {},
                                                        std::uint64_t safepoints = 0)
{
    modes::script::LuauVm::Limits limits = modes::script::LuauVm::Limits::builtIn();
    if (safepoints != 0) {
        limits.safepointsPerCall = safepoints;
    }
    return detail::makeBuiltinScript("tide", "Undead Tide", things, limits, owner);
}

// The rules' names in the script (TideDirector::Rule's order).
constexpr const char* kRuleNames[] = {"xpFor", "cooldown", "setOptions", "beginStage", "spawn", "spawnBoss", "gainXp", "offerPicks", "pick", "stageDamage", "advance", "chestOpened", "record", "filed", "riseShrine", "bringBack", "onChat", "setUp", "shoot", "shape", "forget", "fell", "comeBack", "sign", "board", "lobbyLoaded", "guardianAt", "prepare", "stageBack", "coming", "setsDifficulty", "rises", "minute", "taken", "pickedForYou", "ended", "keptSafe", "gemTaken", "pickupTaken", "gemReach", "passiveHealth", "charm", "bodyDue", "runOver"};

// Whether a map path's file name, without its extension, is `name` (any case).
using detail::isMap;

/// A line for MSG_TEXT_MESSAGE, at most 250 characters.
using detail::widen;

using detail::tellPlayer;

// A player's unit alive and in play: not dead, not observing (status 1), not watching.
bool inPlay(const ServerObject& u)
{
    return u.player && !u.gone() && u.health != 0 && !(u.flags & ObjFlag::Dead) && (u.player->statusFlags & 1u) == 0 && !u.player->observing;
}

/// The frame's takers of gems and pickups (a list kept from frame to frame).
std::vector<Gatherables::Taker>& takersScratch()
{
    thread_local std::vector<Gatherables::Taker> takers;
    return takers;
}

constexpr std::uint32_t kFps = 30;

} // namespace

// An Undead Tide map's name: "UTD_" (any case) and something after it.
bool isTideMapName(std::string_view name)
{
    return detail::hasMapPrefix(name, "UTD_");
}

namespace {

const tide_data::Weapon& spec(TideWeapon w)
{
    const auto& weapons = data().weapons;
    return weapons[std::min<std::size_t>(static_cast<std::size_t>(w), weapons.size() - 1)];
}

/// A passive's top level (the script's: Multishot's 3, the rest 5).
int passiveMax(TidePassive p)
{
    const auto& all = data().passives;
    const auto i = static_cast<std::size_t>(p);
    return i < all.size() ? all[i].most : 0;
}

/// A weapon's top level (the script's: 5).
int tideMaxLevel()
{
    return data().weaponMost;
}

} // namespace

std::string_view tideWeaponName(TideWeapon w)
{
    return spec(w).name;
}

std::uint8_t tideWeaponClasses(TideWeapon w)
{
    return spec(w).classes;
}

TideIcon tideWeaponIcon(TideWeapon w)
{
    return spec(w).icon;
}

std::string_view tideDifficultyName(TideDifficulty d)
{
    // (One past the four: the first's name, as ever.)
    const auto& all = data().difficulties;
    const auto i = static_cast<std::size_t>(d);
    return i < all.size() ? all[i].name : all.front().name;
}

std::string_view tideDifficultyAbout(TideDifficulty d)
{
    return hardness(d).about;
}

std::string_view tidePassiveName(TidePassive p)
{
    const auto& all = data().passives;
    const auto i = static_cast<std::size_t>(p);
    return i < all.size() ? std::string_view(all[i].name) : std::string_view("?");
}

std::string_view tideWeaponText(TideWeapon w)
{
    return spec(w).text;
}

std::string_view tidePassiveText(TidePassive p)
{
    const auto& all = data().passives;
    const auto i = static_cast<std::size_t>(p);
    return i < all.size() ? std::string_view(all[i].text) : std::string_view();
}

void loadTideData()
{
    (void)data();
}

struct TideDirector::ScriptRules {
    modes::script::ScriptedMode::Rule rules[std::size(kRuleNames)];
};

TideDirector::TideDirector(const things::ThingDb& things, Options options)
    : things_(things)
    , options_(std::move(options))
    , runSeed_(options_.seed)
    , rngState_(options_.seed | 1u)
    , script_(makeScript(&things, [this](modes::script::ScriptedMode& s) { addOwnerFunctions(s); }, options_.scriptSafepoints))
    , rules_(std::make_unique<ScriptRules>())
{
    loadTideData();
    for (std::size_t i = 0; i < std::size(kRuleNames); ++i) {
        rules_->rules[i] = script_->rule(kRuleNames[i]);
    }
    // The run's settings the script's rules read.
    ruleWith(Rule::SetOptions, 0, [&](lua_State* L) {
        lua_pushnumber(L, options_.perView);
        lua_pushnumber(L, options_.maxCreatures);
        lua_pushnumber(L, options_.holdPerPlayer);
        lua_pushboolean(L, options_.dawnWins);
        lua_pushnumber(L, options_.graceSeconds);
        return 5;
    });
    difficulty_ = options_.difficulty;
    // The weapons' and the tide's blocks, their ways and rules the script's (setUp), the script
    // asked as a weapon is ready (shoot) or a new weapon's shape is wanted (shape).
    timers_ = std::make_unique<ReadyTimers>(things_);
    timers_->onFire([this](const ReadyTimers::Shot& s) {
        const auto& [unit, r] = (*firing_)[s.holder];
        // (The frame it's ready next; declined, why: the test, its range, walls tested, reach,
        // kept list, least, enchantments (each plus 1: 0 none), a second test and its retry.)
        double out[11] = {static_cast<double>(s.readyAt)};
        rule(Rule::Shoot,
             {static_cast<double>(firingFrame_), static_cast<double>(boss_), static_cast<double>(unit->extent), unit->x, unit->y, unit->radius,
              static_cast<double>(r->hiddenUntil), static_cast<double>(r->passives[static_cast<std::size_t>(TidePassive::Multishot)]),
              static_cast<double>(r->passives[static_cast<std::size_t>(TidePassive::Haste)]), static_cast<double>(s.kind), static_cast<double>(s.level),
              static_cast<double>(s.readyAt), static_cast<double>(s.have), static_cast<double>(s.total)},
             out);
        ReadyTimers::Result res;
        res.readyAt = static_cast<std::uint32_t>(out[0]);
        const auto testOf = [](double v) {
            const int test = static_cast<int>(v);
            if (test < 0 || test > static_cast<int>(ReadyTimers::Wait::Test::AllUnder)) {
                throw std::runtime_error("the Tide's rule shoot: no such wait");
            }
            return static_cast<ReadyTimers::Wait::Test>(test);
        };
        res.wait.test = testOf(out[1]);
        res.wait.also = testOf(out[9]);
        res.wait.alsoRetry = static_cast<std::uint32_t>(out[10]);
        res.wait.range = static_cast<float>(out[2]);
        res.wait.tested = static_cast<std::uint16_t>(out[3]);
        res.wait.reach = static_cast<float>(out[4]);
        res.wait.key = static_cast<std::uint32_t>(out[5]);
        res.wait.least = static_cast<int>(out[6]);
        res.wait.enchant = static_cast<int>(out[7]) - 1;
        res.wait.enchant2 = static_cast<int>(out[8]) - 1;
        res.wait.retry = res.readyAt - firingFrame_;
        return res;
    });
    timers_->onShape([this](std::uint32_t kind, int level, int bonus) {
        double out[4] = {};
        rule(Rule::Shape, {static_cast<double>(kind), static_cast<double>(level), static_cast<double>(bonus)}, out);
        ReadyTimers::Shape s;
        s.orbit.count = static_cast<int>(out[0]);
        s.orbit.radius = static_cast<float>(out[1]);
        s.orbit.step = static_cast<float>(out[2]);
        s.cap = static_cast<int>(out[3]);
        return s;
    });
    horde_ = std::make_unique<Horde>();
    // A creature of the tide slain, and the guardian left far behind: the script's (fall.luau's
    // fell and comeBack), asked as each happens.
    horde_->onFell([this](GameHost& host, const Horde::Member& f, ServerObject& o) {
        const std::uint32_t frame = host.server().clock().frame();
        const HandScope hand(*this, &host, nullptr, nullptr, frame);
        rule(Rule::Fell, {static_cast<double>(o.extent), static_cast<double>(f.worth), static_cast<double>(f.rank), static_cast<double>(seconds(frame) / 60)}, {});
    });
    horde_->onFar([this](GameHost&, Horde::Member&, ServerObject& o, ServerObject& nearest) {
        rule(Rule::ComeBack, {static_cast<double>(o.extent), static_cast<double>(nearest.extent)}, {});
    });
    // The soul gems (drawn in) and the pickups (touched), their figures data.luau's, what's taken
    // the script's (pickups.luau's gemTaken and taken).
    gemField_ = std::make_unique<Gatherables>();
    pickupField_ = std::make_unique<Gatherables>();
    {
        const tide_data::Data::Gems& g = data().gems;
        Gatherables::Rules r;
        r.way = Gatherables::Way::Drift;
        r.take = g.take;
        r.life = g.life;
        r.most = g.most;
        r.base = g.base;
        r.jitter = g.jitter;
        r.pull = g.pull;
        r.near = g.near;
        gemField_->setRules(r);
        const tide_data::Data::PickupField& p = data().pickupField;
        Gatherables::Rules q;
        q.way = Gatherables::Way::Touch;
        q.take = p.touch;
        q.life = p.life;
        q.most = p.most;
        q.dropHeld = true;
        pickupField_->setRules(q);
    }
    gemField_->onTaken([this](const Gatherables::Item& g, std::size_t taker, float, float) {
        const auto& [unit, r] = (*gathering_)[taker];
        const HandScope hand(*this, gatherHost_, unit, r, gatherHost_->server().clock().frame());
        rule(Rule::GemTaken, {static_cast<double>(unit->extent), static_cast<double>(g.value)}, {});
    });
    pickupField_->onTaken([this](const Gatherables::Item& p, std::size_t taker, float x, float y) {
        const auto& [unit, r] = (*gathering_)[taker];
        ++pickupsTaken_;
        const HandScope hand(*this, gatherHost_, unit, r, gatherHost_->server().clock().frame());
        rule(Rule::PickupTaken, {static_cast<double>(p.value), static_cast<double>(unit->extent), x, y}, {});
    });
    rule(Rule::SetUp, {}, {});
    // When the night's guardian comes (the script's: run.luau's guardianAt).
    double at = 0;
    rule(Rule::GuardianAt, {static_cast<double>(options_.bossSeconds), options_.dawnWins ? 1.0 : 0.0, static_cast<double>(options_.winSeconds)},
         {&at, 1});
    bossAt_ = static_cast<std::uint32_t>(at);
}

TideDirector::~TideDirector() = default;

std::size_t TideDirector::pickups() const noexcept
{
    return pickupField_->size();
}

std::size_t TideDirector::creatures() const noexcept
{
    return horde_->size();
}

// The auto-weapons (the blocks': ready_timers.hpp): each player in play's in the party's order, each
// weapon in its own, as each was fired; then the rings of those no longer in play or no longer
// holding the ring put away.
void TideDirector::fireWeapons(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    firing_ = &alive;
    firingFrame_ = frame;
    for (std::size_t i = 0; i < alive.size(); ++i) {
        const auto& [unit, r] = alive[i];
        timers_->evaluate(host, frame, i, *unit, std::span<Runner::Weapon>(r->weapons), r->passives[static_cast<std::size_t>(TidePassive::Multishot)],
                          boss_);
    }
    firing_ = nullptr;
    if (timers_->satellites().empty()) {
        return;
    }
    timers_->satellites().tidy(world, [&](std::uint32_t centre) {
        const ServerObject* u = world.byExtent(centre);
        const auto holder = std::ranges::find_if(alive, [&](const auto& e) { return e.first->extent == centre; });
        const bool holds = holder != alive.end() && std::ranges::any_of(holder->second->weapons, [&](const Runner::Weapon& w) {
            return timers_->way(static_cast<std::uint32_t>(w.kind)) == ReadyTimers::Way::Orbit;
        });
        return u && inPlay(*u) && holds;
    });
}
// The spawn rules (spawns.luau): the clock's turn (the frame the next is due back), or the night's
// guardian (its id, if it found a spot, and its name back), asked with the run as it stands and the
// players in play.
void TideDirector::scriptSpawn(GameHost& host, const std::vector<ServerObject*>& alive, bool guardian)
{
    const std::uint32_t frame = host.server().clock().frame();
    const Rule which = guardian ? Rule::SpawnBoss : Rule::Spawn;
    if (guardian) {
        bossCame_ = true;
    }
    ruleWith(
        which, guardian ? 2 : 1,
        [&](lua_State* L) {
            lua_pushnumber(L, frame);
            lua_pushnumber(L, seconds(frame));
            lua_pushnumber(L, static_cast<double>(difficulty_));
            lua_pushnumber(L, stage_);
            lua_pushnumber(L, partySize_);
            lua_pushlstring(L, biome_.data(), biome_.size());
            if (guardian) {
                if (alive.empty()) {
                    lua_pushnil(L);
                } else {
                    lua_pushnumber(L, alive.front()->extent);
                }
            } else {
                pushNumbers(L, alive, [](const ServerObject* u) { return u->extent; });
            }
            return 7;
        },
        [&](lua_State* L) {
            if (guardian) {
                if (!lua_isnil(L, -2)) {
                    boss_ = static_cast<std::uint32_t>(lua_tonumber(L, -2));
                }
                std::size_t len = 0;
                const char* name = lua_tolstring(L, -1, &len);
                bossName_.assign(name ? name : "", name ? len : 0);
            } else {
                nextSpawn_ = static_cast<std::uint32_t>(lua_tonumber(L, -1));
            }
        });
}
void TideDirector::addOwnerFunctions(modes::script::ScriptedMode& s)
{
    // (Each on the runner in hand: the player whose rule is running, named by its unit's id.)
    const auto inHand = [this](lua_State* L) {
        const double id = luaL_checknumber(L, 1);
        if (!ruleHost_ || !ruleUnit_ || !ruleRunner_ || static_cast<double>(ruleUnit_->extent) != id) {
            luaL_error(L, "owner: not the player whose rule is running");
        }
    };
    // Its weapons don't show it (showAttacker) for so many frames.
    s.addOwnerFunction("hide", [this, inHand](lua_State* L) {
        inHand(L);
        ruleRunner_->hiddenUntil = ruleFrame_ + static_cast<std::uint32_t>(luaL_checknumber(L, 2));
        return 0;
    });
    // Every weapon ready to fire again at once.
    s.addOwnerFunction("readyAll", [this, inHand](lua_State* L) {
        inHand(L);
        for (Runner::Weapon& w : ruleRunner_->weapons) {
            w.readyAt = ruleFrame_;
        }
        return 0;
    });
    // Every gem on the ground: its worth the player's XP, the gem gone.
    s.addOwnerFunction("drawGems", [this, inHand](lua_State* L) {
        inHand(L);
        World& world = ruleHost_->server().world();
        int worth = 0;
        for (const Gatherables::Item& g : gemField_->items()) {
            if (ServerObject* go = world.byExtent(g.extent); go && !go->gone() && !go->pendingDelete) {
                worth += g.value;
                deleteObject(world, *go);
            }
        }
        gemField_->items().clear();
        if (worth > 0) {
            gainXp(*ruleHost_, *ruleUnit_, *ruleRunner_, worth);
        }
        return 0;
    });
    // The runner in hand's XP: its XP toward the next level, hundredths carried, level, levels
    // banked, passives (by TidePassive, from 1); set back as they end.
    s.addOwnerFunction("xp", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        const Runner& r = *ruleRunner_;
        lua_pushnumber(L, r.xp);
        lua_pushnumber(L, r.xpCarry);
        lua_pushnumber(L, r.level);
        lua_pushnumber(L, r.banked);
        pushNumbers(L, r.passives);
        return 5;
    });
    s.addOwnerFunction("setXp", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        Runner& r = *ruleRunner_;
        r.xp = static_cast<int>(luaL_checknumber(L, 1));
        r.xpCarry = static_cast<int>(luaL_checknumber(L, 2));
        r.level = static_cast<int>(luaL_checknumber(L, 3));
        r.banked = static_cast<int>(luaL_checknumber(L, 4));
        return 0;
    });
    // The runner in hand's kit: its weapons ({kind, level} in its order) and passives.
    s.addOwnerFunction("kit", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        const Runner& r = *ruleRunner_;
        lua_createtable(L, static_cast<int>(r.weapons.size()), 0);
        for (std::size_t i = 0; i < r.weapons.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, static_cast<double>(r.weapons[i].kind));
            lua_setfield(L, -2, "kind");
            lua_pushnumber(L, r.weapons[i].level);
            lua_setfield(L, -2, "level");
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        pushNumbers(L, r.passives);
        return 2;
    });
    // A weapon of the runner in hand's raised to a level, or added (at level 1, ready at once).
    s.addOwnerFunction("setWeapon", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        const auto kind = static_cast<TideWeapon>(static_cast<int>(luaL_checknumber(L, 1)));
        const auto level = static_cast<std::uint8_t>(luaL_checknumber(L, 2));
        auto& weapons = ruleRunner_->weapons;
        auto w = std::ranges::find_if(weapons, [&](const Runner::Weapon& x) { return x.kind == kind; });
        if (w == weapons.end()) {
            weapons.push_back({kind, 1, 0});
        } else {
            w->level = level;
        }
        return 0;
    });
    // A passive of the runner in hand's set, applied at once to its unit (if it has one now).
    s.addOwnerFunction("setPassive", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        const auto which = static_cast<std::size_t>(luaL_checknumber(L, 1));
        luaL_argcheck(L, which < kTidePassives, 1, "a passive");
        ruleRunner_->passives[which] = static_cast<std::uint8_t>(luaL_checknumber(L, 2));
        if (ruleUnit_ && ruleHost_) {
            applyPassives(ruleHost_->server().world(), *ruleUnit_, *ruleRunner_);
        }
        return 0;
    });
    // A level-up's glow, put away a while on.
    s.addOwnerFunction("addGlow", [this](lua_State* L) {
        glows_.emplace_back(static_cast<std::uint32_t>(luaL_checknumber(L, 1)), ruleFrame_);
        return 0;
    });
    // An elite's chest's reward: a level banked for each runner of that unit; their names.
    s.addOwnerFunction("rewardChest", [this](lua_State* L) {
        const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        lua_newtable(L);
        int n = 0;
        for (auto& [key, r] : runners_) {
            if (r.unit == id) {
                ++r.banked;
                lua_pushlstring(L, r.name.data(), r.name.size());
                lua_rawseti(L, -2, ++n);
            }
        }
        return 1;
    });
    // A fallen runner (by its unit) back in the run: out no more, no lives (its quest lives none).
    s.addOwnerFunction("revive", [this](lua_State* L) {
        const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        for (auto& [key, r] : runners_) {
            if (r.out && r.unit == id) {
                r.out = false;
                r.lives = 0;
                if (ServerObject* u = scriptHost_ ? scriptHost_->server().world().byExtent(id) : nullptr; u && u->player) {
                    u->player->questLives = 0;
                }
                ++revived_;
                return 0;
            }
        }
        luaL_error(L, "owner.revive: no fallen runner of that unit");
        return 0;
    });
    // The difficulty (the next run's in the lobby, the run's own in one), and set (in the lobby
    // alone: false once the run has begun); the open questions at the gate told it.
    s.addOwnerFunction("difficulty", [this](lua_State* L) {
        lua_pushnumber(L, static_cast<double>(difficulty_));
        return 1;
    });
    s.addOwnerFunction("setDifficulty", [this](lua_State* L) {
        const int d = static_cast<int>(luaL_checknumber(L, 1));
        luaL_argcheck(L, d >= 0 && d < static_cast<int>(kTideDifficulties), 1, "a difficulty");
        lua_pushboolean(L, setDifficulty(static_cast<TideDifficulty>(d)));
        return 1;
    });
    s.addOwnerFunction("refreshAsks", [this](lua_State* L) {
        if (!ruleHost_) {
            luaL_error(L, "owner: no host in hand");
        }
        refreshAsks(*ruleHost_);
        return 0;
    });
    // The runner in hand's level, XP and kills; its offer's size; a pick from it by the player at
    // a peer (as pick: taken, told).
    s.addOwnerFunction("me", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        lua_pushnumber(L, ruleRunner_->level);
        lua_pushnumber(L, ruleRunner_->xp);
        lua_pushnumber(L, ruleRunner_->kills);
        return 3;
    });
    s.addOwnerFunction("offerSize", [this](lua_State* L) {
        lua_pushnumber(L, ruleRunner_ ? static_cast<double>(ruleRunner_->offer.size()) : 0.0);
        return 1;
    });
    s.addOwnerFunction("pickOffer", [this](lua_State* L) {
        const auto peer = static_cast<std::uint8_t>(luaL_checknumber(L, 1));
        const auto n = static_cast<std::size_t>(luaL_checknumber(L, 2));
        if (!ruleHost_ || !ruleHost_->players().contains(peer)) {
            luaL_error(L, "owner: no such player");
        }
        lua_pushboolean(L, pick(*ruleHost_, runnerKey(ruleHost_->players().at(peer)), n));
        return 1;
    });
    // The director's random stream (its other draws share it).
    s.addOwnerFunction("roll", [this](lua_State* L) {
        lua_pushnumber(L, roll());
        return 1;
    });
    // The tide's live creatures as the horde keeps them (its steering and reaping walk them).
    s.addOwnerFunction("foeCount", [this](lua_State* L) {
        lua_pushnumber(L, static_cast<double>(creatures()));
        return 1;
    });
    // A creature the tide made: its worth, its first steer, its most health, its rank (0 the
    // tide's, 1 an elite, 2 the guardian: brought back when left far behind).
    s.addOwnerFunction("addFoe", [this](lua_State* L) {
        Horde::Member m{static_cast<std::uint32_t>(luaL_checknumber(L, 1)), static_cast<std::uint8_t>(luaL_checknumber(L, 2)),
                        static_cast<std::uint32_t>(luaL_checknumber(L, 3)), static_cast<std::uint16_t>(luaL_checknumber(L, 4)),
                        static_cast<std::uint8_t>(luaL_checknumber(L, 5))};
        m.comesBack = m.rank == 2;
        horde_->add(m);
        return 0;
    });
    // A creature's most health as the tide holds it (an elite's, made so).
    s.addOwnerFunction("setFoeMost", [this](lua_State* L) {
        const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        const auto most = static_cast<std::uint16_t>(luaL_checknumber(L, 2));
        if (Horde::Member* m = horde_->find(id)) {
            m->most = most;
        }
        return 0;
    });
    // The horde's rules (horde.hpp): how often each creature looks, how far is too far, how near is
    // near enough.
    s.addOwnerFunction("horde", [this](lua_State* L) {
        Horde::Rules r;
        r.period = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        r.spread = static_cast<std::uint32_t>(luaL_checknumber(L, 2));
        r.cull = static_cast<float>(luaL_checknumber(L, 3));
        r.close = static_cast<float>(luaL_checknumber(L, 4));
        horde_->setRules(r);
        return 0;
    });
    // A weapon kind's way (ready_timers.hpp): 0 fire, 1 orbit, 2 keep; for keep its lists' group,
    // the group's most in all, the foe it waits for (in sight within a reach, the walls tested
    // for so many).
    s.addOwnerFunction("weaponWay", [this](lua_State* L) {
        ReadyTimers::Kind k;
        const int way = static_cast<int>(luaL_checknumber(L, 2));
        luaL_argcheck(L, way >= 0 && way <= 2, 2, "a way: 0 fire, 1 orbit, 2 keep");
        k.way = static_cast<ReadyTimers::Way>(way);
        k.group = static_cast<std::uint32_t>(luaL_optnumber(L, 3, 0));
        k.most = static_cast<int>(luaL_optnumber(L, 4, 0));
        k.gate = static_cast<float>(luaL_optnumber(L, 5, 0));
        k.tested = static_cast<std::uint16_t>(luaL_optnumber(L, 6, 0));
        timers_->setKind(static_cast<std::uint32_t>(luaL_checknumber(L, 1)), k);
        return 0;
    });
    // A kept list key's rule (kept.hpp): 0 present, 1 living; the type (thing.bin's name, "": any);
    // its group.
    s.addOwnerFunction("keptRule", [this](lua_State* L) {
        KeptLists::Rule r;
        const int keep = static_cast<int>(luaL_checknumber(L, 2));
        luaL_argcheck(L, keep == 0 || keep == 1, 2, "0 present, 1 living");
        r.keep = static_cast<KeptLists::Keep>(keep);
        const char* type = luaL_checkstring(L, 3);
        if (*type != 0) {
            const things::ThingDef* t = things_.findThing(type);
            luaL_argcheck(L, t != nullptr, 3, "a thing.bin type");
            r.type = static_cast<std::uint16_t>(t - things_.things.data() + 1);
        }
        r.group = static_cast<std::uint32_t>(luaL_checknumber(L, 4));
        timers_->kept().setRule(static_cast<std::uint32_t>(luaL_checknumber(L, 1)), r);
        return 0;
    });
    // A thing kept on a holder's list (by the holder's unit and the list's key); a list settled and
    // counted.
    s.addOwnerFunction("keep", [this](lua_State* L) {
        timers_->kept().add(static_cast<std::uint32_t>(luaL_checknumber(L, 1)), static_cast<std::uint32_t>(luaL_checknumber(L, 2)),
                            static_cast<std::uint32_t>(luaL_checknumber(L, 3)));
        return 0;
    });
    s.addOwnerFunction("kept", [this](lua_State* L) {
        if (!scriptHost_) {
            luaL_error(L, "owner.kept: no host");
        }
        lua_pushnumber(L, timers_->kept().settle(scriptHost_->server().world(), static_cast<std::uint32_t>(luaL_checknumber(L, 1)),
                                                 static_cast<std::uint32_t>(luaL_checknumber(L, 2))));
        return 1;
    });
    // The fall's (fall.luau's fell): the player whose doing a creature's death was (its hurtBy, a
    // missile kept a frame as the dying, up its owners to a player; nil none), credited with it.
    s.addOwnerFunction("slayer", [this](lua_State* L) {
        World& world = scriptHost_->server().world();
        ServerObject* o = world.byExtent(static_cast<std::uint32_t>(luaL_checknumber(L, 1)));
        ServerObject* by = o && o->hurtBy ? world.byExtent(o->hurtBy) : nullptr;
        if (o && !by && o->hurtBy) {
            by = world.dying(o->hurtBy);
        }
        if (by && !by->player && by->owner) {
            if (ServerObject* owner = world.byExtent(by->owner)) {
                by = owner;
            }
        }
        Fight fight = scriptHost_->server().fight();
        const ServerObject* p = by ? fight.parentPlayer(by) : nullptr;
        if (p && p->player) {
            lua_pushnumber(L, p->extent);
        } else {
            lua_pushnil(L);
        }
        return 1;
    });
    s.addOwnerFunction("addKill", [this](lua_State* L) {
        const auto unit = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        for (auto& [name, r] : runners_) {
            if (r.unit == unit) {
                ++r.kills;
            }
        }
        return 0;
    });
    // The soul gems on the ground (the director's, drawn to the players each frame): the first
    // within `reach` of (x, y) (its place in the list, id, worth, type; nil none); one's thing and
    // worth set; one added.
    s.addOwnerFunction("gemNear", [this](lua_State* L) {
        World& world = scriptHost_->server().world();
        const auto x = static_cast<float>(luaL_checknumber(L, 1));
        const auto y = static_cast<float>(luaL_checknumber(L, 2));
        const auto reach = static_cast<float>(luaL_checknumber(L, 3));
        for (std::size_t i = 0; i < gemField_->items().size(); ++i) {
            const ServerObject* go = world.byExtent(gemField_->items()[i].extent);
            if (go && !go->gone() && !go->pendingDelete && (go->x - x) * (go->x - x) + (go->y - y) * (go->y - y) < reach * reach) {
                lua_pushnumber(L, static_cast<double>(i) + 1);
                lua_pushnumber(L, go->extent);
                lua_pushnumber(L, gemField_->items()[i].value);
                lua_pushlstring(L, go->typeName.data(), go->typeName.size());
                return 4;
            }
        }
        lua_pushnil(L);
        return 1;
    });
    s.addOwnerFunction("setGem", [this](lua_State* L) {
        const auto i = static_cast<std::size_t>(luaL_checknumber(L, 1));
        luaL_argcheck(L, i >= 1 && i <= gemField_->items().size(), 1, "a gem's place");
        gemField_->items()[i - 1].extent = static_cast<std::uint32_t>(luaL_checknumber(L, 2));
        gemField_->items()[i - 1].value = static_cast<std::uint8_t>(luaL_checknumber(L, 3));
        return 0;
    });
    s.addOwnerFunction("addGem", [this](lua_State* L) {
        gemField_->items().push_back({static_cast<std::uint32_t>(luaL_checknumber(L, 1)), static_cast<std::uint8_t>(luaL_checknumber(L, 2)), ruleFrame_});
        return 0;
    });
    // An elite slain (counted), its chest (opened, a free upgrade: runChests); the guardian slain
    // (the stage won); a pickup laid (dropPickup); whether a class in the run may wear an armour;
    // armour charmed (warding or repelling); a body kept a while.
    s.addOwnerFunction("eliteSlain", [this](lua_State* L) {
        ++elitesSlain_;
        if (!lua_isnoneornil(L, 1)) {
            if (const ServerObject* c = scriptHost_->server().world().byExtent(static_cast<std::uint32_t>(luaL_checknumber(L, 1)))) {
                chests_.push_back({c->extent, c->x, c->y});
            }
        }
        return 0;
    });
    s.addOwnerFunction("guardianSlain", [this](lua_State*) {
        bossSlain_ = true;
        boss_ = 0;
        return 0;
    });
    s.addOwnerFunction("dropPickup", [this](lua_State* L) {
        lua_pushboolean(L, dropPickup(*scriptHost_, static_cast<int>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                                      static_cast<float>(luaL_checknumber(L, 3))));
        return 1;
    });
    s.addOwnerFunction("wearable", [this](lua_State* L) {
        const ServerObject* o = scriptHost_->server().world().byExtent(static_cast<std::uint32_t>(luaL_checknumber(L, 1)));
        lua_pushboolean(L, o && armourForParty(*scriptHost_, o->typeName));
        return 1;
    });
    s.addOwnerFunction("charm", [this](lua_State* L) {
        charms_[static_cast<std::uint32_t>(luaL_checknumber(L, 1))] = {lua_toboolean(L, 2) ? Ward::Warding : Ward::Repulsion, 0};
        return 0;
    });
    // Whether a thing is kept on its holder's death (KEPT_ON_DEATH: a quest's own things).
    s.addOwnerFunction("keptOnDeath", [this](lua_State* L) {
        const ServerObject* o = scriptHost_ ? scriptHost_->server().world().byExtent(static_cast<std::uint32_t>(luaL_checknumber(L, 1))) : nullptr;
        lua_pushboolean(L, o && (o->flags & ObjFlag::KeptOnDeath));
        return 1;
    });
    s.addOwnerFunction("addBody", [this](lua_State* L) {
        bodies_.emplace_back(static_cast<std::uint32_t>(luaL_checknumber(L, 1)), ruleFrame_);
        return 0;
    });
    // A line to everyone as the engine sends one (World::textToAll, 250 at most).
    s.addOwnerFunction("announce", [this](lua_State* L) {
        GameHost* host = ruleHost_ ? ruleHost_ : scriptHost_;
        if (!host) {
            luaL_error(L, "owner.announce: no host");
        }
        std::size_t len = 0;
        const char* text = luaL_checklstring(L, 1, &len);
        host->server().world().textToAll(widen(std::string_view(text, len)));
        return 0;
    });
    // The runner in hand's lives as last seen; a unit's quest lives set (told as the host reports
    // them); a fallen player got up (Plr_Respawn); a runner back in the run (out no more, no lives).
    s.addOwnerFunction("lives", [this](lua_State* L) {
        if (!ruleRunner_) {
            luaL_error(L, "owner: no runner in hand");
        }
        lua_pushnumber(L, ruleRunner_->lives);
        return 1;
    });
    s.addOwnerFunction("setQuestLives", [this](lua_State* L) {
        ServerObject* u = ruleHost_ ? ruleHost_->server().world().byExtent(static_cast<std::uint32_t>(luaL_checknumber(L, 1))) : nullptr;
        luaL_argcheck(L, u && u->player, 1, "a player's unit");
        u->player->questLives = static_cast<std::uint32_t>(luaL_checknumber(L, 2));
        return 0;
    });
    s.addOwnerFunction("respawn", [this](lua_State* L) {
        ServerObject* u = ruleHost_ ? ruleHost_->server().world().byExtent(static_cast<std::uint32_t>(luaL_checknumber(L, 1))) : nullptr;
        luaL_argcheck(L, u && u->player, 1, "a player's unit");
        ruleHost_->server().fight().respawn(*u);
        return 0;
    });
    s.addOwnerFunction("backInRun", [this](lua_State* L) {
        const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        for (auto& [key, r] : runners_) {
            if (r.unit == id) {
                r.out = false;
                r.lives = 0;
                if (ServerObject* u = ruleHost_ ? ruleHost_->server().world().byExtent(id) : nullptr; u && u->player) {
                    u->player->questLives = 0;
                }
            }
        }
        return 0;
    });
    // The records' best `n` runs (each as told.record reads one: pushRecord), and the line that
    // points to the full leaderboard.
    s.addOwnerFunction("topRecords", [this](lua_State* L) {
        const auto n = static_cast<std::size_t>(luaL_checknumber(L, 1));
        const std::vector<TideRecord> best = options_.records ? options_.records->top(n) : std::vector<TideRecord>{};
        pushList(L, best, [L](const TideRecord& r) { pushRecord(L, r); });
        lua_pushlstring(L, kFullLeaderboard.data(), kFullLeaderboard.size());
        return 2;
    });
    // An enchantment's bit by its name (ENCHANT_...), for the waits.
    s.addOwnerFunction("enchantBit", [](lua_State* L) {
        const std::string_view name = luaL_checkstring(L, 1);
        for (int e = 0; e < 32; ++e) {
            if (kEnchantNames[e] == name) {
                lua_pushnumber(L, e);
                return 1;
            }
        }
        luaL_argerror(L, 1, "an enchantment's name (ENCHANT_...)");
        return 0;
    });
    // A spell's number by its name (SPELL_...), asked once and kept.
    s.addOwnerFunction("spellNumber", [](lua_State* L) {
        const auto id = things::spellId(luaL_checkstring(L, 1));
        luaL_argcheck(L, id.has_value(), 1, "a spell's name (SPELL_...)");
        lua_pushnumber(L, *id);
        return 1;
    });
    // A unit's orbit group (satellites.hpp): its angle now; things handed to it from this frame
    // (the list of ids, its life and the frames before it harms, the type its things are, the
    // harm about each: reach, full, damage, type).
    s.addOwnerFunction("orbitAngle", [this](lua_State* L) {
        const Satellites::Group* g = timers_->satellites().find(static_cast<std::uint32_t>(luaL_checknumber(L, 1)));
        lua_pushnumber(L, g ? g->angle : 0.0f);
        return 1;
    });
    s.addOwnerFunction("orbit", [this](lua_State* L) {
        const auto centre = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        std::vector<std::uint32_t> things;
        const int n = lua_objlen(L, 2);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, 2, i);
            things.push_back(static_cast<std::uint32_t>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
        Satellites::Harm harm;
        harm.reach = static_cast<float>(luaL_checknumber(L, 6));
        harm.full = static_cast<float>(luaL_checknumber(L, 7));
        harm.damage = static_cast<int>(luaL_checknumber(L, 8));
        harm.type = static_cast<int>(luaL_checknumber(L, 9));
        timers_->satellites().start(centre, std::move(things), firingFrame_, static_cast<std::uint32_t>(luaL_checknumber(L, 3)),
                                    static_cast<std::uint32_t>(luaL_checknumber(L, 4)), static_cast<std::uint16_t>(luaL_checknumber(L, 5)), harm);
        return 0;
    });
    // XP as soul gems give it.
    s.addOwnerFunction("gainXp", [this, inHand](lua_State* L) {
        inHand(L);
        gainXp(*ruleHost_, *ruleUnit_, *ruleRunner_, static_cast<int>(luaL_checknumber(L, 2)));
        return 0;
    });
}
void TideDirector::detach(GameHost& host)
{
    if (scriptHost_ == &host) {
        script_->detach(host);
        scriptHost_ = nullptr;
    }
}

bool TideDirector::setDifficulty(TideDifficulty d)
{
    if (state_ != State::Lobby) {
        return false;
    }
    difficulty_ = d;
    return true;
}

void TideDirector::closeAsks(GameHost& host)
{
    for (const auto& [peer, asked] : askedAtGate_) {
        host.sendToIdentified(peer, tidemsg::askMessage(tidemsg::kAskClosed));
    }
    askedAtGate_.clear();
}

void TideDirector::refreshAsks(GameHost& host)
{
    for (const auto& [peer, asked] : askedAtGate_) {
        host.sendToIdentified(peer, tidemsg::askMessage(static_cast<std::uint8_t>(difficulty_)));
    }
}

std::string TideDirector::runnerKey(const HostedPlayer& p)
{
    std::string account;
    for (const std::uint8_t c : p.accept.account) {
        if (c == 0) {
            break;
        }
        account += static_cast<char>(c);
    }
    return account.empty() ? p.name : account + "/" + p.name;
}

TideDirector::Runner* TideDirector::findRunner(const std::string& keyOrName)
{
    if (const auto it = runners_.find(keyOrName); it != runners_.end()) {
        return &it->second;
    }
    for (auto& [key, r] : runners_) {
        if (r.name == keyOrName) {
            return &r;
        }
    }
    return nullptr;
}

TideDirector::HandScope::HandScope(TideDirector& d, GameHost* host, ServerObject* unit, Runner* runner, std::uint32_t frame)
    : d_(d), host_(d.ruleHost_), unit_(d.ruleUnit_), runner_(d.ruleRunner_), frame_(d.ruleFrame_)
{
    d.ruleHost_ = host;
    d.ruleUnit_ = unit;
    d.ruleRunner_ = runner;
    d.ruleFrame_ = frame;
}

TideDirector::HandScope::~HandScope()
{
    d_.ruleHost_ = host_;
    d_.ruleUnit_ = unit_;
    d_.ruleRunner_ = runner_;
    d_.ruleFrame_ = frame_;
}

std::uint32_t TideDirector::roll()
{
    // (Its own stream: the game's random numbers stay the game's.)
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return rngState_;
}

int TideDirector::sweep(const std::filesystem::path& mapsDir)
{
    return detail::sweepMaps(mapsDir, isTideMapName);
}

int TideDirector::sweepOwn() const
{
    return detail::sweepMapsNamed(options_.mapsDir, {options_.mapName, options_.lobbyName});
}

namespace {

/// The Tide's script for callers with no director (the maps' parameters, xpFor, a record's line):
/// one a thread, with the generator's biomes (owner.biomes: their names and weights).
modes::script::ScriptedMode& staticScript()
{
    thread_local const std::unique_ptr<modes::script::ScriptedMode> script = makeScript(nullptr, [](modes::script::ScriptedMode& s) {
        s.addOwnerFunction("biomes", [](lua_State* L) {
            const auto& all = mapgen::biomes();
            lua_createtable(L, static_cast<int>(all.size()), 0);
            for (std::size_t i = 0; i < all.size(); ++i) {
                lua_createtable(L, 0, 2);
                lua_pushlstring(L, all[i].name.data(), all[i].name.size());
                lua_setfield(L, -2, "name");
                lua_pushnumber(L, all[i].weight);
                lua_setfield(L, -2, "weight");
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            return 1;
        });
    });
    return *script;
}

/// A map's parameters from the script's table on the stack's top (params.luau's): the fields it has
/// set, each of its own kind (arena_lua.hpp), the rest the generator's own.
void readParams(lua_State* L, mapgen::ArenaParams& p)
{
    using F = mapgen::ArenaField;
    mapgen::readArenaParams(L, -1, p,
                            F::Title | F::Author | F::GameFlags | F::MinPlayers | F::MaxPlayers | F::CenterX | F::CenterY | F::Radius | F::Seed |
                                F::Biome | F::FloorName | F::WallSet | F::Ambient | F::PlayerStarts | F::Items | F::Signs | F::Centre |
                                F::QuestExit,
                            mapgen::ArenaRead::Typed);
}

/// A failure of the maps' parameters' rule (prepare's door catches it with the generator's).
struct ParamsFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A map's parameters by the script's rule `name` (params.luau's arena or lobby).
mapgen::ArenaParams scriptParams(const char* name, const std::function<int(lua_State*)>& push)
{
    mapgen::ArenaParams p;
    modes::script::ScriptedMode& script = staticScript();
    if (!script.call(name, 1, push, [&](lua_State* L) { readParams(L, p); })) {
        throw ParamsFailed(std::string("the Tide's rule ") + name + " failed: " + (script.errors().empty() ? std::string() : script.errors().back()));
    }
    return p;
}

} // namespace

mapgen::ArenaParams TideDirector::arenaParams(std::uint32_t seed, std::string_view biome, int radius)
{
    // (The script's: params.luau's arena.)
    return scriptParams("arenaParams", [&](lua_State* L) {
        lua_pushnumber(L, seed);
        lua_pushlstring(L, biome.data(), biome.size());
        lua_pushnumber(L, radius);
        return 3;
    });
}

mapgen::ArenaParams TideDirector::lobbyParams()
{
    // (The script's: params.luau's lobby.)
    return scriptParams("lobbyParams", [](lua_State*) { return 0; });
}

std::string TideRecords::describe(const TideRecord& r)
{
    // (The script's words, told.record, as the lobby's board tells them: the thread's own script,
    // so any caller, with a director or none, gets the same line.)
    std::string line;
    modes::script::ScriptedMode& script = staticScript();
    if (!script.call("recordLine", 1, [&](lua_State* L) { pushRecord(L, r); return 1; },
                     [&](lua_State* L) {
                         std::size_t n = 0;
                         const char* s = lua_tolstring(L, -1, &n);
                         line.assign(s ? s : "", s ? n : 0);
                     })) {
        throw std::runtime_error("the Tide's rule recordLine failed: " + (script.errors().empty() ? std::string() : script.errors().back()));
    }
    return line;
}

std::filesystem::path TideDirector::writeLobby()
{
    return mapgen::writeMapFiles(options_.mapsDir, options_.lobbyName, mapgen::buildArena(things_, lobbyParams()), things_);
}

std::string TideDirector::listedName(std::optional<std::uint32_t> seconds)
{
    // (Whole minutes: the name goes to every client as it changes, MSG_GAME_SETTINGS.)
    return seconds ? fillText(words().listedRun, {{"minutes", num(*seconds / 60)}}) : words().listedLobby;
}

std::uint32_t TideDirector::seconds(std::uint32_t frame) const
{
    return state_ == State::Running && startFrame_ != 0 && frame > startFrame_ ? (frame - startFrame_) / kFps : 0;
}

int TideDirector::xpFor(int level)
{
    // (A director's own calls ask its own script, xpNeed: this is for callers with none, each
    // thread a script of its own.)
    modes::script::ScriptedMode* script = &staticScript();
    double xp = 0;
    const bool ok = script->call("xpFor", 1, [&](lua_State* L) {
        lua_pushnumber(L, level);
        return 1;
    }, [&](lua_State* L) { xp = lua_tonumber(L, -1); });
    if (!ok) {
        throw std::runtime_error("the Tide's rule xpFor failed: " + (script->errors().empty() ? std::string() : script->errors().back()));
    }
    return static_cast<int>(xp);
}

int TideDirector::xpNeed(int level) const
{
    double xp = 0;
    rule(Rule::XpFor, {static_cast<double>(level)}, {&xp, 1});
    return static_cast<int>(xp);
}

void TideDirector::failed(Rule which) const
{
    throw std::runtime_error(std::string("the Tide's rule ") + kRuleNames[static_cast<std::size_t>(which)] + " failed: " +
                             (script_->errors().empty() ? std::string() : script_->errors().back()));
}

void TideDirector::ruleWith(Rule which, int nresults, const std::function<int(lua_State*)>& push, const std::function<void(lua_State*)>& read) const
{
    if (!script_->call(rules_->rules[static_cast<std::size_t>(which)], nresults, push, read)) {
        failed(which);
    }
}

void TideDirector::rule(Rule which, std::initializer_list<double> args, std::span<double> out) const
{
    const int n = static_cast<int>(out.size());
    const bool ok = script_->call(rules_->rules[static_cast<std::size_t>(which)], n,
                                  [&](lua_State* L) {
                                      for (const double a : args) {
                                          lua_pushnumber(L, a);
                                      }
                                      return static_cast<int>(args.size());
                                  },
                                  [&](lua_State* L) {
                                      for (int i = 0; i < n; ++i) {
                                          out[static_cast<std::size_t>(i)] = lua_tonumber(L, i - n);
                                      }
                                  });
    if (!ok) {
        failed(which);
    }
}

// The map change's door: the generator's failure (GenerationFailed) or the arena's parameters'
// rule's stops here, never out of the host's map change (bug hunt 3): told as a script failure,
// false, so the map loads as last written; that arena begun as a new one (a run's or the next
// stage's state set up on it, its land the last one's). A refusal (FormatError: an unknown biome)
// goes on as before.
bool TideDirector::prepare(const std::filesystem::path& next, const World& world)
{
    if (!isMap(next.string(), options_.mapName)) {
        return false;
    }
    try {
        return prepareArena(next, world);
    } catch (const modes::script::GenerationFailed& e) {
        scriptFailed("a map change (the arena's build)", e);
    } catch (const ParamsFailed& e) {
        scriptFailed("a map change (the arena's parameters)", e);
    }
    ++written_;
    return false;
}

bool TideDirector::prepareArena(const std::filesystem::path&, const World&)
{
    // Each arena written its own seed: the run's mixed with the count written so far.
    std::uint32_t seed = runSeed_ * 2654435761u + written_ * 40503u + 1u;
    mapgen::ArenaParams p = arenaParams(seed, options_.biome, options_.arenaRadius);
    // The next stage's land another than the last (when the biome's drawn).
    for (int tries = 0; carry_ && options_.biome.empty() && p.biome == biome_ && tries < 32; ++tries) {
        seed = seed * 2654435761u + 7u;
        p = arenaParams(seed, options_.biome, options_.arenaRadius);
    }
    if (options_.arenaHook) {
        options_.arenaHook(p);
    }
    mapgen::writeMapFiles(options_.mapsDir, options_.mapName, mapgen::buildArena(things_, p), things_);
    biome_ = p.biome; // (the land written: a build that failed leaves the last one's)
    ++written_;
    return true;
}

void TideDirector::beginRun(GameHost& host)
{
    const util::RoundingScope rounding(host.server().exactMaths());
    if (state_ != State::Lobby) {
        return;
    }
    closeAsks(host);
    rule(Rule::Coming, {static_cast<double>(difficulty_)}, {}); // (told.luau's coming)
    detail::lockGame(host, savedClosed_, savedAllowed_); // (the game closed to all but those in it now)
    state_ = State::Starting;
    host.command("startSoloQuest"); // (OTQuest.rul's line: the quest begun with everyone in)
}

namespace tidemsg {

namespace {

// Little-endian, as every Nox message.
void put16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

// A u8 length, then the bytes (cut to kNameMost).
void putName(std::vector<std::uint8_t>& out, std::string_view s)
{
    const std::size_t n = std::min<std::size_t>(s.size(), kNameMost);
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

/// {op, sub, u16 the rest's length, the rest} (mode_frame.hpp)
std::vector<std::uint8_t> framed(std::uint8_t sub, const std::vector<std::uint8_t>& rest)
{
    return modes::framed(kOp, sub, rest);
}

// Reads a sub's rest; past its end `ok` goes false and every read after gives 0.
struct Reader {
    std::span<const std::uint8_t> d;
    std::size_t at = 0;
    bool ok = true;
    std::uint8_t u8()
    {
        if (at + 1 > d.size()) {
            ok = false;
            return 0;
        }
        return d[at++];
    }
    std::uint16_t u16()
    {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
    std::string name()
    {
        const std::size_t n = u8();
        if (!ok || at + n > d.size()) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(d.data() + at), n);
        at += n;
        return s;
    }
};

} // namespace

// {op, kState, u16 length 1, u8 in run}
std::vector<std::uint8_t> stateMessage(bool inRun)
{
    return {kOp, kState, 1, 0, static_cast<std::uint8_t>(inRun ? 1 : 0)};
}

std::vector<std::uint8_t> hudMessage(const Hud& h)
{
    std::vector<std::uint8_t> r;
    put16(r, h.level);
    put16(r, h.xp);
    put16(r, h.need);
    put16(r, h.seconds);
    put16(r, h.dawn);
    put16(r, h.bossAt);
    r.push_back(h.boss);
    put16(r, h.kills);
    r.push_back(static_cast<std::uint8_t>(h.weapons.size()));
    for (const Hud::Weapon& w : h.weapons) {
        r.push_back(w.kind);
        r.push_back(w.level);
        put16(r, w.cooldown);
        put16(r, w.readyIn);
    }
    r.push_back(static_cast<std::uint8_t>(h.passives.size()));
    r.insert(r.end(), h.passives.begin(), h.passives.end());
    r.push_back(h.stage);
    return framed(kHud, r);
}

std::vector<std::uint8_t> offerMessage(const Offer& o)
{
    std::vector<std::uint8_t> r{o.level, o.seconds, static_cast<std::uint8_t>(o.picks.size())};
    for (const TidePick& p : o.picks) {
        r.push_back(static_cast<std::uint8_t>(p.kind));
        r.push_back(p.which);
        r.push_back(p.level);
    }
    return framed(kOffer, r);
}

std::vector<std::uint8_t> resultsMessage(const Results& res)
{
    std::vector<std::uint8_t> r{static_cast<std::uint8_t>(res.won ? 1 : 0), res.boss};
    put16(r, res.seconds);
    put16(r, res.elites);
    putName(r, res.bossName);
    r.push_back(static_cast<std::uint8_t>(std::min(res.players.size(), kResultsMost)));
    for (std::size_t i = 0; i < res.players.size() && i < kResultsMost; ++i) {
        const Results::Player& p = res.players[i];
        putName(r, p.name);
        put16(r, p.level);
        put16(r, p.kills);
        r.push_back(static_cast<std::uint8_t>(p.weapons.size()));
        for (const auto& [kind, level] : p.weapons) {
            r.push_back(kind);
            r.push_back(level);
        }
    }
    r.push_back(res.stage);
    return framed(kResults, r);
}

std::vector<std::uint8_t> askMessage(std::uint8_t current)
{
    return framed(kAsk, {current});
}

std::vector<std::uint8_t> chooseMessage(std::uint8_t difficulty)
{
    return framed(kChoose, {difficulty});
}

std::vector<std::uint8_t> pickMessage(std::uint8_t index, std::uint16_t level)
{
    std::vector<std::uint8_t> r{index};
    put16(r, level);
    return framed(kPick, r);
}

std::optional<Hud> parseHud(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Hud h;
    h.level = in.u16();
    h.xp = in.u16();
    h.need = in.u16();
    h.seconds = in.u16();
    h.dawn = in.u16();
    h.bossAt = in.u16();
    h.boss = in.u8();
    h.kills = in.u16();
    const std::size_t n = in.u8();
    for (std::size_t i = 0; i < n && in.ok; ++i) {
        Hud::Weapon w;
        w.kind = in.u8();
        w.level = in.u8();
        w.cooldown = in.u16();
        w.readyIn = in.u16();
        if (w.kind >= kTideWeapons) {
            return std::nullopt;
        }
        h.weapons.push_back(w);
    }
    const std::size_t np = in.u8();
    for (std::size_t i = 0; i < np && in.ok; ++i) {
        h.passives.push_back(in.u8());
    }
    if (in.ok && in.at < in.d.size()) { // (an older server's ends before it: stage 1)
        h.stage = std::max<std::uint8_t>(1, in.u8());
    }
    if (!in.ok) {
        return std::nullopt;
    }
    return h;
}

std::optional<Offer> parseOffer(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Offer o;
    o.level = in.u8();
    o.seconds = in.u8();
    const std::size_t n = in.u8();
    for (std::size_t i = 0; i < n && in.ok; ++i) {
        TidePick p;
        const std::uint8_t kind = in.u8();
        p.which = in.u8();
        p.level = in.u8();
        if (kind > static_cast<std::uint8_t>(TidePick::Kind::Heal) ||
            (kind == static_cast<std::uint8_t>(TidePick::Kind::Weapon) && p.which >= kTideWeapons) ||
            (kind == static_cast<std::uint8_t>(TidePick::Kind::Passive) && p.which >= kTidePassives)) {
            return std::nullopt;
        }
        p.kind = static_cast<TidePick::Kind>(kind);
        o.picks.push_back(p);
    }
    if (!in.ok) {
        return std::nullopt;
    }
    return o;
}

std::optional<Results> parseResults(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Results r;
    r.won = in.u8() != 0;
    r.boss = in.u8();
    r.seconds = in.u16();
    r.elites = in.u16();
    r.bossName = in.name();
    const std::size_t n = in.u8();
    for (std::size_t i = 0; i < n && in.ok; ++i) {
        Results::Player p;
        p.name = in.name();
        p.level = in.u16();
        p.kills = in.u16();
        const std::size_t nw = in.u8();
        for (std::size_t k = 0; k < nw && in.ok; ++k) {
            const std::uint8_t kind = in.u8();
            const std::uint8_t level = in.u8();
            if (kind >= kTideWeapons) {
                return std::nullopt;
            }
            p.weapons.emplace_back(kind, level);
        }
        r.players.push_back(std::move(p));
    }
    if (in.ok && in.at < in.d.size()) { // (an older server's ends before it: stage 1)
        r.stage = std::max<std::uint8_t>(1, in.u8());
    }
    if (!in.ok) {
        return std::nullopt;
    }
    return r;
}

std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m)
{
    return modes::unframed(kOp, m);
}

} // namespace tidemsg

void TideDirector::tellState(GameHost& host)
{
    // Each noxsdl player told whether it's in a run's arena, as that changes.
    const bool inRun = state_ == State::Running;
    std::erase_if(told_, [&](const auto& e) { return !host.players().contains(e.first); });
    for (const auto& [peer, p] : host.players()) {
        if (!p.ready || !host.identified(peer)) {
            continue;
        }
        const auto it = told_.find(peer);
        if (it == told_.end() || it->second != std::pair{p.extent, inRun}) {
            if (host.sendToIdentified(peer, tidemsg::stateMessage(inRun))) {
                told_[peer] = {p.extent, inRun};
                if (inRun) {
                    // (one just in, or back: its open offer told again)
                    if (const auto r = runners_.find(runnerKey(p)); r != runners_.end() && !r->second.offer.empty()) {
                        tellOffer(host, r->first, r->second);
                    }
                }
            }
        }
    }
    if (inRun) {
        tellHuds(host);
    }
}

std::optional<std::uint8_t> TideDirector::identifiedPeer(GameHost& host, const std::string& name) const
{
    for (const auto& [peer, p] : host.players()) {
        if (runnerKey(p) == name && host.identified(peer)) {
            return peer;
        }
    }
    return std::nullopt;
}

void TideDirector::tellHuds(GameHost& host)
{
    // A third of a second apart: the clock, the XP bar and the weapons' waits (noxsdl runs the
    // waits down between). Once in the frame, however often the host's loop ticks.
    const std::uint32_t frame = host.server().clock().frame();
    if (frame % 10 != 0 || frame == hudFrame_) {
        return;
    }
    hudFrame_ = frame;
    const std::uint32_t secs = endedAt_ != 0 ? endSeconds_ : seconds(frame);
    for (const auto& [peer, p] : host.players()) {
        if (!p.ready || !host.identified(peer)) {
            continue;
        }
        const auto it = runners_.find(runnerKey(p));
        if (it == runners_.end()) {
            continue;
        }
        const Runner& r = it->second;
        tidemsg::Hud h;
        h.level = static_cast<std::uint16_t>(std::min(r.level, 0xFFFF));
        h.xp = static_cast<std::uint16_t>(std::clamp(r.xp, 0, 0xFFFF));
        h.need = static_cast<std::uint16_t>(std::clamp(xpNeed(r.level), 1, 0xFFFF));
        h.seconds = static_cast<std::uint16_t>(std::min<std::uint32_t>(secs, 0xFFFF));
        h.dawn = static_cast<std::uint16_t>(std::min<std::uint32_t>(options_.winSeconds, 0xFFFF));
        h.bossAt = static_cast<std::uint16_t>(std::min<std::uint32_t>(bossSeconds(), 0xFFFF));
        h.boss = !bossCame_ ? 0 : bossSlain_ ? 2 : 1;
        h.kills = static_cast<std::uint16_t>(std::min(r.kills, 0xFFFF));
        h.stage = static_cast<std::uint8_t>(std::clamp(stage_, 1, 255));
        for (const Runner::Weapon& w : r.weapons) {
            const std::uint32_t wait = w.kind == TideWeapon::ForceOfNature ? 0 : cooldown(r, w);
            const std::uint32_t in = w.readyAt > frame && wait != 0 ? std::min(w.readyAt - frame, wait) : 0;
            h.weapons.push_back({static_cast<std::uint8_t>(w.kind), w.level, static_cast<std::uint16_t>(wait), static_cast<std::uint16_t>(in)});
        }
        h.passives.assign(r.passives.begin(), r.passives.end());
        host.sendToIdentified(peer, tidemsg::hudMessage(h));
    }
}

void TideDirector::tellOffer(GameHost& host, const std::string& name, const Runner& r)
{
    const auto peer = identifiedPeer(host, name);
    if (!peer) {
        return;
    }
    tidemsg::Offer o;
    if (!r.offer.empty()) {
        const std::uint32_t frame = host.server().clock().frame();
        const std::uint32_t gone = frame - r.offeredAt;
        o.level = static_cast<std::uint8_t>(std::clamp(r.level - r.banked + 1, 1, 255));
        o.seconds = static_cast<std::uint8_t>(gone < data().timers.offer ? (data().timers.offer - gone + kFps - 1) / kFps : 0);
        o.picks = r.offer;
    }
    host.sendToIdentified(*peer, tidemsg::offerMessage(o));
}

tidemsg::Results TideDirector::results() const
{
    tidemsg::Results res;
    res.won = won_;
    res.boss = !bossCame_ ? 0 : bossSlain_ ? 2 : 1;
    res.seconds = static_cast<std::uint16_t>(std::min<std::uint32_t>(endedAt_ != 0 ? stagesSeconds_ + endSeconds_ : 0, 0xFFFF)); // (every stage's)
    res.stage = static_cast<std::uint8_t>(std::clamp(stage_, 1, 255));
    res.elites = static_cast<std::uint16_t>(std::min(elitesSlain_, 0xFFFF));
    res.bossName = bossName_;
    for (const auto& [key, r] : runners_) {
        tidemsg::Results::Player p;
        p.name = r.name;
        p.level = static_cast<std::uint16_t>(std::min(r.level, 0xFFFF));
        p.kills = static_cast<std::uint16_t>(std::min(r.kills, 0xFFFF));
        for (const Runner::Weapon& w : r.weapons) {
            p.weapons.emplace_back(static_cast<std::uint8_t>(w.kind), w.level);
        }
        res.players.push_back(std::move(p));
    }
    // The best dozen (highest level, then most slain): one reliable message holds them;
    // the chat lines name everyone.
    std::ranges::stable_sort(res.players, [](const auto& a, const auto& b) { return a.level != b.level ? a.level > b.level : a.kills > b.kills; });
    if (res.players.size() > tidemsg::kResultsMost) {
        res.players.resize(tidemsg::kResultsMost);
    }
    return res;
}

// A noxsdl's message: the gate's difficulty answer (kChoose) in the lobby, or a level-up pick
// (kPick: {u8 index, u16 the offer's level}) in a run.
void TideDirector::message(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m)
{
    const auto v = tidemsg::parse(m);
    if (v && v->first == tidemsg::kChoose && !v->second.empty()) {
        // The gate's question answered: the difficulty set; the run begun when it's still at the
        // gate (asked, not walked off).
        if (state_ != State::Lobby || v->second[0] >= kTideDifficulties || !host.players().contains(peer)) {
            return;
        }
        setDifficulty(static_cast<TideDifficulty>(v->second[0]));
        // (Told: told.luau's setsDifficulty.)
        const std::string& name = host.players().at(peer).name;
        const HandScope hand(*this, &host, nullptr, nullptr, host.server().clock().frame());
        ruleWith(Rule::SetsDifficulty, 0, [&](lua_State* L) {
            lua_pushlstring(L, name.data(), name.size());
            lua_pushnumber(L, static_cast<double>(difficulty_));
            return 2;
        });
        if (askedAtGate_.contains(peer)) {
            beginRun(host);
        } else {
            refreshAsks(host);
        }
        return;
    }
    if (!v || v->first != tidemsg::kPick || v->second.size() < 3 || state_ != State::Running || endedAt_ != 0) {
        return;
    }
    const auto who = host.players().find(peer);
    if (who == host.players().end()) {
        return;
    }
    const auto it = runners_.find(runnerKey(who->second));
    if (it == runners_.end() || it->second.offer.empty()) {
        return;
    }
    // (for the offer open now: a pick sent as the last was taken for it is dropped)
    const int level = v->second[1] | (v->second[2] << 8);
    if (level != it->second.level - it->second.banked + 1) {
        return;
    }
    pick(host, it->first, v->second[0]);
}

// Which map is up decides the state: the lobby (a run just over is cleared away here, a new seed
// drawn) or the arena (Running from its first frame). runLobby / runArena once a frame.
bool TideDirector::tick(GameHost& host)
{
    const State was = state_;
    try {
        return tickFrame(host);
    } catch (const std::exception& e) {
        firing_ = nullptr;
        scriptFailed("its frame", e);
        return locked() != (was != State::Lobby);
    }
}

bool TideDirector::onChat(GameHost& host, std::uint8_t peer, std::u16string_view text)
{
    try {
        return chatLine(host, peer, text);
    } catch (const std::exception& e) {
        scriptFailed("a chat line", e);
        return false;
    }
}

void TideDirector::onMessage(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m)
{
    try {
        message(host, peer, m);
    } catch (const std::exception& e) {
        scriptFailed("a noxsdl message", e);
    }
}

void TideDirector::scriptFailed(std::string_view where, const std::exception& e)
{
    ++scriptFailures_;
    const std::string line = "SCRIPT FAILURE: Undead Tide's script failed in " + std::string(where) + ", its work there dropped: " + e.what();
    if (options_.log) {
        options_.log(line);
    } else {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

bool TideDirector::tickFrame(GameHost& host)
{
    const util::RoundingScope rounding(host.server().exactMaths()); // (the server's rounding: x87.hpp)
    // The script attached to the host at the first tick (its float maths then the host's rounding),
    // and ticked with it.
    if (scriptHost_ != &host) {
        script_->attach(host);
        scriptHost_ = &host;
    }
    script_->tick(host, 0);
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    const State was = state_;
    whichMap_.read(world.mapName, options_.lobbyName, options_.mapName);
    if (whichMap_.lobby()) {
        if (state_ == State::Running) {
            detail::unlockGame(host, savedClosed_, savedAllowed_); // (back from a run)
            state_ = State::Lobby;
            difficulty_ = options_.difficulty; // (the next party chooses its own)
            stage_ = 1;
            stagesSeconds_ = 0;
            advanceAt_ = 0;
            carry_ = false;
            bringBack_.clear();
            askedAtGate_.clear();
            ++runs_;
            runSeed_ = (options_.seed ^ (runs_ * 0x9E3779B9u)) * 2246822519u + runs_;
            runners_.clear();
            gemField_->items().clear();
            horde_->clear();
            timers_->clear();
            rule(Rule::Forget, {0}, {}); // (the script's own of the weapons)
            bodies_.clear();
            glows_.clear();
        }
        if (frame != lastFrame_) {
            lastFrame_ = frame;
            runLobby(host);
        }
        world.noBareHands = false;
        host.setServerName(words().listedLobby); // (listedName(std::nullopt), not copied at every tick)
    } else if (whichMap_.level()) {
        state_ = State::Running;
        if (host.players().empty()) {
            world.exitTo = options_.lobbyName + ".map"; // (everyone gone: back to the lobby)
        }
        // Nothing to fight with by hand, not a punch either (World::noBareHands).
        world.noBareHands = true;
        if (frame != lastFrame_) {
            lastFrame_ = frame;
            runArena(host);
        }
        // (The rings are no DeathBalls, plain sprites: nothing for DeathBallUpdate's cancel to
        // meet.)
    }
    tellState(host);
    return locked() != (was != State::Lobby);
}

// The lobby's frame: its signs speak their rule lines to whoever stands near (each at most every
// 5 s), the board tells the records, and a player on the gate begins the run.
// The lobby's zones made afresh on its things as it loads (the old zones' players forgotten, as
// the board's near and the signs' waits were): each sign speaks to the first player near it at
// most every signEvery frames (the script's sign); the board tells whoever comes near it (the
// script's board, which keeps the 20 s per player; one refused is told as its wait ends while it
// stays); the gate begins the run (a NoX Core player asked the difficulty first).
void TideDirector::watchLobby()
{
    if (!watch_) {
        watch_ = std::make_unique<Watchers>();
    }
    if (lobbyGroup_ < 0) {
        lobbyGroup_ = watch_->group();
    }
    watch_->reset(lobbyGroup_);
    const tide_data::Data::Lobby& near = data().lobby;
    for (std::size_t i = 0; i < signs_.size() && i < data().rules.size(); ++i) {
        watch_->zone(lobbyGroup_, signs_[i], 0, 0, near.signNear, 0, Watchers::Mode::AnyCooldown, near.signEvery, [this, i](const Watchers::Event&) {
            rule(Rule::Sign, {static_cast<double>(i) + 1, static_cast<double>(signs_[i])}, {});
        });
    }
    // (The board's near kept by unit, the gate's by peer, as the old frame kept them: two players
    // of one name each their own.)
    const std::uint32_t board = watch_->zone(lobbyGroup_, board_, 0, 0, near.boardNear, Watchers::ByUnit, Watchers::Mode::EachEnter, 0, [this](const Watchers::Event& e) {
        if (e.kind == Watchers::Event::Kind::Enter) {
            if (!runBoard(*lobbyHost_, *e.unit)) {
                boardWaiting_.insert(e.unit->extent);
            }
        } else if (e.kind == Watchers::Event::Kind::Leave) {
            boardWaiting_.erase(e.unit->extent);
        }
    });
    if (board_ == 0) {
        watch_->retarget(board, 0);
    }
    const std::uint32_t gate = watch_->zone(lobbyGroup_, portal_, 0, 0, near.gateNear, Watchers::NotBusy | Watchers::ByPeer, Watchers::Mode::EachEnter, 0, [this](const Watchers::Event& e) {
        GameHost& host = *lobbyHost_;
        if (e.kind == Watchers::Event::Kind::Enter) {
            atGate_.insert(e.peer);
            // A stock player on the gate begins the run; a NoX Core player is asked the difficulty
            // first (kAsk), its answer beginning it (onMessage), or its wait's end.
            if (!host.identified(e.peer)) {
                beginNow_ = true;
            } else if (!askedAtGate_.contains(e.peer) && state_ == State::Lobby) {
                askedAtGate_[e.peer] = {e.unit->extent, host.server().clock().frame()};
                host.sendToIdentified(e.peer, tidemsg::askMessage(static_cast<std::uint8_t>(difficulty_)));
            }
        } else if (e.kind == Watchers::Event::Kind::Leave) {
            atGate_.erase(e.peer);
            if (const auto it = askedAtGate_.find(e.peer); it != askedAtGate_.end()) {
                host.sendToIdentified(e.peer, tidemsg::askMessage(tidemsg::kAskClosed));
                askedAtGate_.erase(it);
            }
        }
    });
    if (portal_ == 0) {
        watch_->retarget(gate, 0);
    }
    boardWaiting_.clear();
    atGate_.clear();
    rule(Rule::LobbyLoaded, {}, {});
}

namespace {

/// The lobby's thing types (data.luau's `things`), interned once.
const detail::ThingTypes& lobbyTypes()
{
    static const detail::ThingTypes types = detail::thingTypes(data().things);
    return types;
}

} // namespace

void TideDirector::runLobby(GameHost& host)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    lobbyHost_ = &host;
    const detail::ThingTypes& types = lobbyTypes();
    const ServerObject* portal = portal_ ? world.byExtent(portal_) : nullptr;
    if (!portal || portal->gone() || portal->typeName != types.portal || lobbyGroup_ < 0) {
        // West to east: the i-th sign from the left speaks the i-th rule.
        detail::LobbyThings found = detail::lobbyThings(world, types);
        signs_ = std::move(found.signs);
        portal_ = found.portal;
        board_ = found.board;
        watchLobby();
    }
    beginNow_ = false;
    watch_->evaluate(lobbyGroup_, host, frame);
    // Those near the board whose wait was on, told as it ends; a NoX Core player asked at the
    // gate and not answering, begun as its wait ends.
    for (const auto& [peer, p] : host.players()) {
        ServerObject* unit = world.byExtent(p.extent);
        if (!unit || !unit->player || !p.ready) {
            continue;
        }
        if (boardWaiting_.contains(unit->extent) && runBoard(host, *unit)) {
            boardWaiting_.erase(unit->extent);
        }
        if (const auto it = askedAtGate_.find(peer); it != askedAtGate_.end() && atGate_.contains(peer) && data().lobby.askWait < frame - it->second.at) {
            beginNow_ = true;
        }
    }
    std::erase_if(askedAtGate_, [&](const auto& e) { return !host.players().contains(e.first); });
    std::erase_if(atGate_, [&](std::uint8_t peer) { return !host.players().contains(peer); }); // (as the gate zone forgets it)
    if (beginNow_) {
        beginRun(host);
    }
}

// A weapon's wait between shots in frames: the script's rule (rules.luau's cooldown), asked with the
// weapon, its level and the runner's Haste.
std::uint32_t TideDirector::cooldown(const Runner& r, const Runner::Weapon& w) const
{
    double frames = 0;
    rule(Rule::Cooldown, {static_cast<double>(w.kind), static_cast<double>(w.level), static_cast<double>(r.passives[static_cast<std::size_t>(TidePassive::Haste)])},
         {&frames, 1});
    return static_cast<std::uint32_t>(frames);
}

void TideDirector::prepareUnit(World& world, ServerObject& unit, Runner& r)
{
    PlayerControl& pc = *unit.player;
    // The script's (run.luau's prepare, on the runner in hand: its book emptied, its lives, its class's
    // weapon to start, its word on how it plays); its health before Vitality kept.
    const HandScope hand(*this, scriptHost_, &unit, &r, world.frameNow);
    rule(Rule::Prepare, {static_cast<double>(unit.extent), static_cast<double>(pc.classId), static_cast<double>(stage_)}, {});
    if (r.baseHealth == 0 || stage_ == 1) { // (a later stage's unit may have Vitality's in it already)
        r.baseHealth = unit.maxHealth;
    }
}

void TideDirector::disarm(GameHost& host, ServerObject& unit)
{
    World& world = host.server().world();
    std::optional<Fight> fight;
    for (const std::uint32_t e : keysOf(unit.inventory)) {
        ServerObject* i = world.byExtent(e);
        if (!i || i->pendingDelete || !(i->classes & data().disarm)) { // (data.luau's disarm)
            continue;
        }
        if (!fight) {
            fight.emplace(host.server().fight());
        }
        Items{world, things_, host.options().modifiers, fight->rng, fight->frame}.removeFromInventory(unit, *i);
        i->pendingDelete = true;
    }
}

// runArena's first frame of a new arena (seen_ behind written_): the run's or the stage's state
// reset, the scenery walked through, the land announced.
void TideDirector::beginArena(World& world, std::uint32_t frame)
{
    // A new run's arena, or the next stage's (carry_: the party's runners kept, their levels,
    // weapons and kills; those out brought back; the clock anew).
    seen_ = written_;
    const bool carried = std::exchange(carry_, false);
    advanceAt_ = 0;
    if (!carried) {
        stage_ = 1;
        stagesSeconds_ = 0;
    }
    startFrame_ = frame == 0 ? 1 : frame;
    // The tide's clock begun anew (the script's: the first spawn after the quiet, its waves'
    // and specials' own clocks).
    double first = 0;
    rule(Rule::BeginStage, {static_cast<double>(startFrame_), static_cast<double>(difficulty_), static_cast<double>(stage_)}, {&first, 1});
    nextSpawn_ = static_cast<std::uint32_t>(first);
    lastToldMinute_ = 0;
    won_ = false;
    endedAt_ = 0;
    bringBack_.clear();
    if (carried) {
        for (auto& [key, r] : runners_) {
            r.offeredAt = startFrame_; // (an offer open as the land changed: its full time again)
            if (r.out) {
                bringBack_.push_back(key);
            }
        }
        for (auto& [item, charm] : charms_) {
            charm.nextAt = 0; // (the load took the shield its warding gave: at once again)
        }
    } else {
        runners_.clear();
        elitesSlain_ = 0;
        pickupsTaken_ = 0;
        revived_ = 0;
        charms_.clear(); // (carried, the armour comes along under its own extent, its charm too:
                         // runCharms drops one whose item is gone)
    }
    prepared_.clear();
    gemField_->items().clear();
    bodies_.clear();
    glows_.clear();
    chests_.clear();
    openedChests_.clear();
    horde_->clear();
    timers_->clear();
    rule(Rule::Forget, {1}, {}); // (all the script's weapons have out)
    pickupField_->items().clear();
    boss_ = 0;
    bossCame_ = false;
    bossSlain_ = false;
    bossName_.clear();
    shrine_ = 0;
    shrineDueAt_ = 0;
    // The arena's scenery (trees, rocks, tombs, furniture: immobile or obstacles, none of the
    // game's working things) walked through: nothing to snag on while the tide comes (the
    // project's choice, 2026-10-12). Once a load, not each frame. Only what has no collide of its own
    // (none, DefaultCollide, NoCollide, a rock's AudioEventCollide): a pad, hole, elevator,
    // chest or trap keeps its touch on any map.
    // (Which: data.luau's scenery.)
    const tide_data::Data::Scenery& scenery = data().scenery;
    const auto plain = [&](const ServerObject& o) {
        return std::ranges::find(scenery.collides, o.collideFn) != scenery.collides.end() ||
               (!scenery.collidePrefix.empty() && o.collideFn.starts_with(scenery.collidePrefix));
    };
    sceneryWalked_ = 0;
    for (auto& [extent, o] : world.objects()) {
        if (!o.gone() && (o.classes & scenery.classes) && !(o.classes & scenery.working) && !(o.flags & ObjFlag::NoCollide) && plain(o)) {
            o.flags |= ObjFlag::NoCollide;
            ++sceneryWalked_;
        }
    }
    // The land, the difficulty and the goal told (told.luau's rises).
    ruleWith(Rule::Rises, 0, [&](lua_State* L) {
        lua_pushnumber(L, stage_);
        lua_pushlstring(L, biome_.data(), biome_.size());
        lua_pushnumber(L, static_cast<double>(difficulty_));
        lua_pushboolean(L, options_.dawnWins);
        lua_pushnumber(L, options_.winSeconds);
        lua_pushnumber(L, bossSeconds());
        return 6;
    });
}

// The arena's frame. A new arena's first frame resets the run's state (or a stage's, the runners
// carried); then each frame: the party's runners, the fallen put to watching, the end or the next
// stage, the damage curve, the guardian, the weapons, the tide and every pickup and pick.
void TideDirector::runArena(GameHost& host)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    if (seen_ != written_) {
        beginArena(world, frame);
    }
    // The party, each its runner.
    std::vector<std::pair<ServerObject*, Runner*>> party;
    std::vector<std::string> names; ///< (as shown)
    std::vector<std::string> keys;  ///< (runnerKey)
    for (const auto& [peer, p] : host.players()) {
        ServerObject* unit = world.byExtent(p.extent);
        if (!unit || !unit->player || !p.ready) {
            continue;
        }
        keys.push_back(runnerKey(p));
        Runner& r = runners_[keys.back()];
        r.name = p.name;
        r.cls = std::min<std::uint8_t>(unit->player->classId, 2);
        r.multiclass = unit->player->multiclass;
        r.noxCore = host.identified(peer);
        r.unit = unit->extent;
        party.emplace_back(unit, &r);
        names.push_back(p.name);
    }
    const std::uint32_t secs = seconds(frame);
    // (Its name built anew only as the minute changes: it's set each frame.)
    if (listedMinute_ != secs / 60 || listed_.empty()) {
        listedMinute_ = secs / 60;
        listed_ = listedName(secs);
    }
    host.setServerName(listed_);
    // The quest's rejoin after a load (its watchers back in play) made the load's one-off here: its
    // clock put away once past, else it takes the run's watchers back every frame and they're put
    // to watching again (status reports to everyone each frame). The shrine and
    // the stages bring the fallen back.
    if (world.questRejoinUntil != 0 && world.questRejoinUntil < frame) {
        world.questRejoinUntil = 0;
    }
    std::optional<Fight> fightOpt;
    const auto fight = [&]() -> Fight& {
        if (!fightOpt) {
            fightOpt.emplace(host.server().fight());
        }
        return *fightOpt;
    };
    for (std::size_t i = 0; i < party.size(); ++i) {
        auto& [unit, r] = party[i];
        // A new stage: one out as the last ended is back (2026-10-12), its game over
        // done with, no extra life; one still lying dead got up first (as its client's Continue).
        if (const auto back = std::ranges::find(bringBack_, keys[i]); back != bringBack_.end()) {
            // (The script's: run.luau's stageBack, 1 once it's back.)
            const HandScope hand(*this, &host, unit, r, frame);
            double done = 0;
            ruleWith(Rule::StageBack, 1,
                     [&](lua_State* L) {
                         lua_pushnumber(L, unit->extent);
                         lua_pushlstring(L, names[i].data(), names[i].size());
                         return 2;
                     },
                     [&](lua_State* L) { done = lua_tonumber(L, -1); });
            if (done != 0) {
                bringBack_.erase(back);
            }
        }
        if ((unit->player->statusFlags & 1u) == 0 && std::ranges::find(prepared_, unit->extent) == prepared_.end()) {
            prepared_.push_back(unit->extent);
            prepareUnit(world, *unit, *r);
        }
        disarm(host, *unit);
        if (std::ranges::find(prepared_, unit->extent) != prepared_.end()) {
            r->lives = unit->player->questLives; // (as it stands: given back should it come back)
        }
        // Out of lives (the quest's game over): out of the run, watching. One that took the game
        // over's OK and came back is put to watching again (a stock quest goes on with fresh lives).
        if (unit->player->questGameOverAt != 0 && !r->out) {
            r->out = true;
            {
                // (Told: told.luau's taken.)
                const HandScope hand(*this, &host, unit, r, frame);
                ruleWith(Rule::Taken, 0, [&](lua_State* L) {
                    lua_pushlstring(L, names[i].data(), names[i].size());
                    return 1;
                });
            }
            if (shrineDueAt_ == 0 && shrine_ == 0) {
                shrineDueAt_ = frame + options_.reviveSeconds * kFps; // (a revive shrine a while on)
            }
        }
        if (r->out && inPlay(*unit)) {
            goObserver(fight(), *unit, false, true);
        }
    }
    // The levels' glows put away; the slain's bodies cleared (a zombie that got up joins the tide
    // again).
    std::erase_if(glows_, [&](const auto& g) {
        if (frame - g.second < data().timers.glow) {
            return false;
        }
        if (ServerObject* o = world.byExtent(g.first)) {
            o->pendingDelete = true;
        }
        return true;
    });
    std::erase_if(bodies_, [&](const auto& b) {
        if (frame - b.second < data().timers.body) {
            return false;
        }
        if (ServerObject* o = world.byExtent(b.first); o && !o->gone() && !o->pendingDelete) {
            // (The script's: arena.luau's bodyDue.)
            rule(Rule::BodyDue, {static_cast<double>(o->extent), o->monster ? 1.0 : 0.0, (o->flags & ObjFlag::Dead) ? 0.0 : 1.0,
                                 static_cast<double>(o->maxHealth), static_cast<double>(frame)},
                 {});
        }
        return true;
    });
    if (endedAt_ != 0) {
        // The run's over: a while to read the results, then back to the lobby with nothing.
        if (endedAt_ != 1 && data().timers.ended < frame - endedAt_) {
            // What the party found goes with the run (the script's: arena.luau's runOver).
            ruleWith(Rule::RunOver, 0, [&](lua_State* L) {
                pushNumbers(L, party, [](const auto& e) { return e.first->extent; });
                return 1;
            });
            world.exitTo = options_.lobbyName + ".map";
            endedAt_ = 1;
        }
        return;
    }
    std::vector<std::pair<ServerObject*, Runner*>> alive;
    std::vector<ServerObject*> aliveUnits;
    for (auto& [unit, r] : party) {
        if (!r->out && inPlay(*unit)) {
            alive.emplace_back(unit, r);
            aliveUnits.push_back(unit);
        }
    }
    // The next stage asked for: nothing more here till it's in.
    if (advanceAt_ == 1) {
        return;
    }
    // The end: everyone out (lost); with dawnWins, dawn or the guardian's fall (won). Else the
    // guardian's fall carries the party on to the next stage a while on (advance).
    if (!party.empty() && std::ranges::all_of(party, [](const auto& e) { return e.second->out; })) {
        endRun(host, false);
        return;
    }
    if (options_.dawnWins && ((options_.winSeconds <= secs) || bossSlain_)) {
        endRun(host, true);
        return;
    }
    if (bossSlain_ && advanceAt_ == 0) {
        advance(host);
    }
    if (advanceAt_ > 1 && advanceAt_ <= frame) {
        // The wait over: the next stage's land written and loaded (prepare, carry_), everyone in
        // the quest along.
        carry_ = true;
        ++stage_;
        stagesSeconds_ += secs;
        startFrame_ = frame;
        advanceAt_ = 1;
        world.exitTo = options_.mapName + ".map";
        return;
    }
    partySize_ = std::max<int>(1, static_cast<int>(aliveUnits.size()));
    // The damage players take, the run's own curve (the quest's multiplier, PlayerDamage's
    // World::questDamage, set each frame over the quest's own): 15% at the start, the whole of
    // it by dawn, the climb steepest late (gentle first minutes; a network game's creatures hit
    // hard: a zombie 20 every 10-30 frames); a fifth more each stage past the first.
    double damage = 1;
    rule(Rule::StageDamage, {static_cast<double>(secs), static_cast<double>(options_.winSeconds), static_cast<double>(stage_)}, {&damage, 1});
    world.questDamage = static_cast<float>(damage);
    // The guardian lost without falling (gone some other way): it comes again, so the run can
    // always be won.
    if (bossCame_ && boss_ != 0 && !bossSlain_) {
        if (const ServerObject* b = world.byExtent(boss_); !b || b->gone() || b->pendingDelete) {
            boss_ = 0;
            bossCame_ = false;
        }
    }
    if (!bossCame_ && bossSeconds() <= secs && !aliveUnits.empty() && options_.holdPerPlayer == 0) {
        scriptSpawn(host, aliveUnits, true);
    }
    const std::uint32_t minute = secs / 60;
    if (minute > lastToldMinute_ && advanceAt_ == 0) {
        lastToldMinute_ = minute;
        // (Told: told.luau's minute.)
        const HandScope hand(*this, &host, nullptr, nullptr, frame);
        rule(Rule::Minute, {static_cast<double>(minute), static_cast<double>(secs), static_cast<double>(options_.winSeconds), bossSlain_ ? 1.0 : 0.0}, {});
    }
    // The auto-weapons (the blocks' and the script's: fireWeapons).
    fireWeapons(host, alive);
    if (nextSpawn_ <= frame && !aliveUnits.empty() && advanceAt_ == 0) { // (none once the guardian's slain)
        scriptSpawn(host, aliveUnits, false);
    }
    // The tide steered at the players and its slain reaped (the horde's), the gems on the ground at
    // most 150 (the oldest go).
    horde_->steer(host, frame, aliveUnits);
    horde_->reap(host);
    gemField_->cap(world);
    runChests(host);
    runCharms(host, alive);
    runShrine(host, alive);
    runGems(host, alive);
    runPickups(host, alive);
    // Picks: one open for each with a level to spend; an offer left unanswered taken for it.
    for (std::size_t i = 0; i < party.size(); ++i) {
        auto& [unit, r] = party[i];
        if (!r->offer.empty() && data().timers.offer < frame - r->offeredAt) {
            rule(Rule::PickedForYou, {static_cast<double>(unit->extent)}, {}); // (told.luau's pickedForYou)
            pick(host, keys[i], 0);
        }
        if (r->offer.empty() && r->banked > 0 && !r->out) {
            offerPicks(host, *unit, *r);
        }
        if (frame % data().timers.passives == 0 && inPlay(*unit)) {
            applyPassives(world, *unit, *r);
        }
        // The mana tube as the XP bar (nothing in a run spends mana): empty as a level begins,
        // full at the next, for the stock client with no window of the run's (decided
        // 2026-10-12). Told as any mana change is (MSG_REPORT_MANA). A warrior's client draws no
        // mana tube: it has none.
        if (PlayerControl& pc = *unit->player; pc.maxMana > 0) {
            const long long need = std::max(1, xpNeed(r->level));
            pc.mana = static_cast<std::uint16_t>(std::clamp<long long>(static_cast<long long>(pc.maxMana) * std::max(0, r->xp) / need, 0, pc.maxMana));
        }
    }
}

// The run over (won or lost): told in chat, filed in the records, the results sent to noxsdl
// players and every open offer closed; the survivors made invulnerable while they read them.
// runArena sends everyone back to the lobby 10 s later.
void TideDirector::endRun(GameHost& host, bool won)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    const std::uint32_t secs = seconds(frame);
    endedAt_ = frame;
    endSeconds_ = secs;
    won_ = won;
    // How it ended and the results: the script's (told.luau's ended).
    const HandScope hand(*this, &host, nullptr, nullptr, frame);
    ruleWith(Rule::Ended, 0, [&](lua_State* L) {
        lua_pushboolean(L, won);
        lua_pushboolean(L, bossSlain_);
        lua_pushboolean(L, bossCame_);
        lua_pushlstring(L, bossName_.data(), bossName_.size());
        lua_pushnumber(L, secs);
        lua_pushnumber(L, stage_);
        lua_pushnumber(L, stagesSeconds_ + secs);
        lua_pushnumber(L, elitesSlain_);
        lua_createtable(L, static_cast<int>(runners_.size()), 0);
        int i = 0;
        for (const auto& [key, r] : runners_) {
            lua_createtable(L, 0, 4);
            lua_pushlstring(L, r.name.data(), r.name.size());
            lua_setfield(L, -2, "name");
            lua_pushnumber(L, r.level);
            lua_setfield(L, -2, "level");
            lua_pushnumber(L, r.kills);
            lua_setfield(L, -2, "kills");
            lua_createtable(L, static_cast<int>(r.weapons.size()), 0);
            for (std::size_t k = 0; k < r.weapons.size(); ++k) {
                lua_createtable(L, 0, 2);
                lua_pushnumber(L, static_cast<double>(r.weapons[k].kind));
                lua_setfield(L, -2, "kind");
                lua_pushnumber(L, r.weapons[k].level);
                lua_setfield(L, -2, "level");
                lua_rawseti(L, -2, static_cast<int>(k) + 1);
            }
            lua_setfield(L, -2, "kit");
            lua_rawseti(L, -2, ++i);
        }
        return 9;
    });
    fileRecord(host);
    // noxsdl players: the results' window, their offers closed (and everyone's here: no pick after
    // the end).
    for (auto& [name, r] : runners_) {
        r.offer.clear();
        r.banked = 0;
    }
    const std::vector<std::uint8_t> told = tidemsg::resultsMessage(results());
    for (const auto& [peer, p] : host.players()) {
        if (host.identified(peer)) {
            host.sendToIdentified(peer, told);
            host.sendToIdentified(peer, tidemsg::offerMessage({}));
        }
    }
    // The survivors kept safe while the results are read (the script's: told.luau's keptSafe); the
    // circling balls put away.
    {
        std::vector<std::uint32_t> safe;
        for (const auto& [peer, p] : host.players()) {
            if (ServerObject* u = world.byExtent(p.extent); u && inPlay(*u)) {
                safe.push_back(u->extent);
            }
        }
        ruleWith(Rule::KeptSafe, 0, [&](lua_State* L) {
            pushNumbers(L, safe);
            return 1;
        });
    }
    timers_->satellites().putAway(world);
}

// The guardian slain (with dawnWins off): the next stage's load set stageDelaySeconds on
// (advanceAt_), the players standing made invulnerable till then.
void TideDirector::advance(GameHost& host)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    // The script's (run.luau's advance), asked with the players in play: when the next stage is due.
    std::vector<double> units;
    for (const auto& [peer, p] : host.players()) {
        if (ServerObject* u = world.byExtent(p.extent); u && inPlay(*u)) {
            units.push_back(u->extent);
        }
    }
    ruleWith(
        Rule::Advance, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, frame);
            lua_pushnumber(L, options_.stageDelaySeconds);
            lua_pushnumber(L, stage_);
            lua_pushlstring(L, bossName_.data(), bossName_.size());
            pushNumbers(L, units);
            return 5;
        },
        [&](lua_State* L) { advanceAt_ = static_cast<std::uint32_t>(lua_tonumber(L, -1)); });
}

// This run in the records (what it holds the script's); a top-ten place told.
void TideDirector::fileRecord(GameHost&)
{
    if (!options_.records || runners_.empty()) {
        return;
    }
    // What the record holds is the script's (run.luau's record); the file and the class byte
    // the records' own.
    TideRecord rec;
    const std::string date = detail::recordDate();
    const auto field = [](lua_State* L, const char* name) {
        lua_rawgetfield(L, -1, name);
        const double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    };
    const auto text = [](lua_State* L, const char* name) {
        lua_rawgetfield(L, -1, name);
        std::size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        std::string out(s ? s : "", s ? len : 0);
        lua_pop(L, 1);
        return out;
    };
    ruleWith(
        Rule::Record, 1,
        [&](lua_State* L) {
            lua_pushboolean(L, won_);
            lua_pushboolean(L, bossSlain_);
            lua_pushnumber(L, stagesSeconds_ + endSeconds_);
            lua_pushnumber(L, stage_);
            lua_pushnumber(L, options_.winSeconds);
            lua_pushlstring(L, biome_.data(), biome_.size());
            lua_pushlstring(L, date.data(), date.size());
            lua_pushnumber(L, static_cast<double>(difficulty_));
            lua_createtable(L, static_cast<int>(runners_.size()), 0);
            int i = 0;
            for (const auto& [key, r] : runners_) {
                lua_createtable(L, 0, 6);
                lua_pushlstring(L, r.name.data(), r.name.size());
                lua_setfield(L, -2, "name");
                lua_pushnumber(L, r.cls);
                lua_setfield(L, -2, "class");
                lua_pushboolean(L, r.multiclass);
                lua_setfield(L, -2, "multiclass");
                lua_pushboolean(L, r.noxCore);
                lua_setfield(L, -2, "noxCore");
                lua_pushnumber(L, r.level);
                lua_setfield(L, -2, "level");
                lua_pushnumber(L, r.kills);
                lua_setfield(L, -2, "kills");
                lua_rawseti(L, -2, ++i);
            }
            return 9;
        },
        [&](lua_State* L) {
            rec.outcome = static_cast<int>(field(L, "outcome"));
            rec.seconds = static_cast<int>(field(L, "seconds"));
            rec.stage = static_cast<int>(field(L, "stage"));
            rec.minutes = static_cast<int>(field(L, "minutes"));
            rec.biome = text(L, "biome");
            rec.date = text(L, "date");
            rec.difficulty = text(L, "difficulty");
            lua_rawgetfield(L, -1, "party");
            const int n = static_cast<int>(lua_objlen(L, -1));
            for (int k = 1; k <= n; ++k) {
                lua_rawgeti(L, -1, k);
                lua_rawgetfield(L, -1, "multiclass");
                const bool multiclass = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_rawgetfield(L, -1, "noxCore");
                const bool noxCore = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                rec.party.push_back({text(L, "name"), recordClass(static_cast<std::uint8_t>(field(L, "class")), multiclass, noxCore),
                                     static_cast<int>(field(L, "level")), static_cast<int>(field(L, "kills"))});
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        });
    const int place = options_.records->add(std::move(rec));
    ruleWith(Rule::Filed, 0, [&](lua_State* L) {
        lua_pushnumber(L, place);
        return 1;
    });
}

bool TideDirector::runBoard(GameHost& host, const ServerObject& unit)
{
    // Whoever walks up to the board is told the table: the script's (lobby.luau's board, its wait per
    // player its own). Whether it was told.
    double told = 0;
    rule(Rule::Board, {static_cast<double>(unit.extent), static_cast<double>(host.server().clock().frame())}, {&told, 1});
    return told != 0;
}

std::uint32_t TideDirector::bossSeconds() const
{
    return bossAt_; // (run.luau's guardianAt, asked once)
}

void TideDirector::runChests(GameHost& host)
{
    // An elite's chest opened (ChestCollide's touch: the chest itself marked dead, open): a free
    // upgrade to the player nearest it then. The open ones put away 20 s on.
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    std::erase_if(chests_, [&](const Chest& c) {
        const ServerObject* o = world.byExtent(c.extent);
        if (o && !o->gone() && !o->pendingDelete && !(o->flags & ObjFlag::Dead)) {
            return false;
        }
        // Who gets it is the script's (run.luau's chestOpened), asked with the players in play.
        std::vector<double> near;
        for (auto& [name, r] : runners_) {
            ServerObject* u = world.byExtent(r.unit);
            if (u && !r.out && inPlay(*u)) {
                near.push_back(u->extent);
            }
        }
        ruleWith(Rule::ChestOpened, 0, [&](lua_State* L) {
            lua_pushnumber(L, c.x);
            lua_pushnumber(L, c.y);
            pushNumbers(L, near);
            return 3;
        });
        if (o && !o->gone() && !o->pendingDelete) {
            openedChests_.emplace_back(c.extent, frame);
        }
        return true;
    });
    std::erase_if(openedChests_, [&](const auto& e) {
        if (frame - e.second < data().timers.openChest) {
            return false;
        }
        if (ServerObject* o = world.byExtent(e.first)) {
            deleteObject(world, *o);
        }
        return true;
    });
}

void TideDirector::runCharms(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive)
{
    // Charmed armour worn by a player in play, looked at every timers.charms frames: its turn the
    // script's as it comes due (arena.luau's charm: Warding's shield, Repulsion's push).
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    if (frame % data().timers.charms != 0) {
        return;
    }
    for (auto it = charms_.begin(); it != charms_.end();) {
        ServerObject* item = world.byExtent(it->first);
        if (!item || item->gone() || item->pendingDelete) {
            it = charms_.erase(it);
            continue;
        }
        Charm& charm = it->second;
        ++it;
        if (!(item->flags & ObjFlag::Equipped) || item->holder == 0 || frame < charm.nextAt) {
            continue;
        }
        ServerObject* wearer = nullptr;
        for (const auto& [u, r] : alive) {
            if (u->extent == item->holder) {
                wearer = u;
            }
        }
        if (!wearer) {
            continue;
        }
        // Its turn is the script's (arena.luau's charm): the frame of its next.
        double next = 0;
        rule(Rule::Charm,
             {static_cast<double>(item->extent), static_cast<double>(wearer->extent), charm.ward == Ward::Warding ? 1.0 : 0.0, static_cast<double>(frame)},
             {&next, 1});
        charm.nextAt = static_cast<std::uint32_t>(next);
    }
}

void TideDirector::runShrine(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    ServerObject* shrine = shrine_ ? world.byExtent(shrine_) : nullptr;
    if (shrine && (shrine->gone() || shrine->pendingDelete)) {
        shrine = nullptr;
    }
    if (!shrine) {
        shrine_ = 0;
    }
    // The fallen still in the game (one who fell and left has nobody to bring back).
    std::vector<Runner*> fallen;
    for (auto& [key, r] : runners_) {
        if (r.out && std::ranges::any_of(host.players(), [&](const auto& e) { return e.second.extent == r.unit; })) {
            fallen.push_back(&r);
        }
    }
    if (fallen.empty()) {
        shrineDueAt_ = 0;
        if (shrine) {
            deleteObject(world, *shrine); // (nobody left to bring back: put away)
            shrine_ = 0;
        }
        return;
    }
    if (alive.empty()) {
        return;
    }
    if (!shrine) {
        // One rises a while after a fall, near someone still standing (out of the tide's thick:
        // 260-400 away, on open ground).
        if (shrineDueAt_ == 0) {
            shrineDueAt_ = frame + options_.reviveSeconds * kFps;
        }
        if (frame < shrineDueAt_) {
            return;
        }
        // Where it rises is the script's (shrines.luau's rise), asked with the players in play.
        ruleWith(
            Rule::RiseShrine, 1,
            [&](lua_State* L) {
                pushNumbers(L, alive, [](const auto& e) { return e.first->extent; });
                return 1;
            },
            [&](lua_State* L) {
                if (!lua_isnil(L, -1)) {
                    shrine_ = static_cast<std::uint32_t>(lua_tonumber(L, -1));
                }
            });
        return;
    }
    // Touched by one in play: every fallen one who's watching (its game over answered) back
    // beside it, no extra life, whole and safe a moment; the shrine spent.
    const float reach = data().shrineReach;
    const bool touched = std::ranges::any_of(alive, [&](const auto& e) {
        const ServerObject& u = *e.first;
        return (u.x - shrine->x) * (u.x - shrine->x) + (u.y - shrine->y) * (u.y - shrine->y) < reach * reach;
    });
    if (!touched) {
        return;
    }
    // Bringing them back is the script's (shrines.luau's bringBack): the fallen ready for it (their
    // game over answered, watching, not lying dead).
    std::vector<Runner*> ready;
    for (Runner* f : fallen) {
        const ServerObject* u = world.byExtent(f->unit);
        if (u && u->player && u->player->questGameOverAt == 0 && !(u->flags & ObjFlag::Dead) && (u->player->statusFlags & 1u)) {
            ready.push_back(f);
        }
    }
    int back = 0;
    ruleWith(
        Rule::BringBack, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, shrine->extent);
            lua_createtable(L, static_cast<int>(ready.size()), 0);
            for (std::size_t i = 0; i < ready.size(); ++i) {
                lua_createtable(L, 0, 2);
                lua_pushnumber(L, ready[i]->unit);
                lua_setfield(L, -2, "unit");
                lua_pushlstring(L, ready[i]->name.data(), ready[i]->name.size());
                lua_setfield(L, -2, "name");
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            return 2;
        },
        [&](lua_State* L) { back = static_cast<int>(lua_tonumber(L, -1)); });
    if (back == 0) {
        return; // (the fallen still at their game over: it waits)
    }
    shrine_ = 0;
    // Some fallen still at their game over: another shrine after the usual wait.
    shrineDueAt_ = back < static_cast<int>(fallen.size()) ? frame + options_.reviveSeconds * kFps : 0;
}

std::size_t TideDirector::pickupKinds()
{
    return data().pickups.size();
}

std::uint8_t TideDirector::partyClasses() const
{
    std::uint8_t mask = 0;
    for (const auto& [key, r] : runners_) {
        mask |= static_cast<std::uint8_t>(1u << std::min<std::uint8_t>(r.cls, 2));
    }
    return mask == 0 ? 7 : mask;
}

bool TideDirector::armourForParty(const GameHost& host, std::string_view type) const
{
    const things::EquipDef* def = host.options().modifiers ? host.options().modifiers->findArmor(type) : nullptr;
    return !def || !def->classUse || (*def->classUse & partyClasses()) != 0;
}

bool TideDirector::dropPickup(GameHost& host, int kind, float x, float y)
{
    if (kind < 0 || static_cast<std::size_t>(kind) >= data().pickups.size()) {
        return false;
    }
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    ServerObject* o = world.createAt(things_, data().pickups[static_cast<std::size_t>(kind)].type, x, y, frame);
    if (!o) {
        return false;
    }
    o->flags |= ObjFlag::NoCollide; // (no touch takes it: runPickups uses it)
    // Nor a hand: its PICKUP the stock trap's, which gives a thing only to its owner (none here):
    // a stock client's pickup request refused with the stock sound. And no creature's food: Meat
    // made no Food, so the hurt tide doesn't eat it.
    o->pickup = std::string("TrapPickup");
    o->classes &= ~oc::Food;
    pickupField_->items().push_back({o->extent, static_cast<std::uint8_t>(kind), frame});
    // At most 25 on the ground (the oldest go).
    pickupField_->cap(world);
    return true;
}

// Each pickup lying out (pickupField's figures): gone after its life (or once something holds it); else
// used by the first player in play within its touch, what it does the script's (pickups.luau's taken).
void TideDirector::runPickups(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive)
{
    // The pickups' block (gather.hpp): the first player in play within the touch takes one.
    std::vector<Gatherables::Taker>& takers = takersScratch();
    takers.clear();
    for (const auto& [u, r] : alive) {
        takers.push_back({u, data().pickupField.touch});
    }
    gathering_ = &alive;
    gatherHost_ = &host;
    pickupField_->evaluate(host, host.server().clock().frame(), takers);
}

// Each gem on the ground (data.luau's gems): gone after its life; else drawn toward the nearest player in
// play within its reach, taken (its worth as XP: pickups.luau's gemTaken) near it.
void TideDirector::runGems(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive)
{
    // The gems' block (gather.hpp): each player's reach the script's, by its Magnet (asked once a
    // level: pickups.luau's gemReach).
    std::vector<Gatherables::Taker>& takers = takersScratch();
    takers.clear();
    for (const auto& [u, r] : alive) {
        const std::size_t magnet = r->passives[static_cast<std::size_t>(TidePassive::Magnet)];
        if (gemReach_.size() <= magnet) {
            gemReach_.resize(magnet + 1, -1.0f);
        }
        if (gemReach_[magnet] < 0) {
            double reach = 0;
            rule(Rule::GemReach, {static_cast<double>(magnet)}, {&reach, 1});
            gemReach_[magnet] = static_cast<float>(reach);
        }
        takers.push_back({u, gemReach_[magnet]});
    }
    gathering_ = &alive;
    gatherHost_ = &host;
    gemField_->evaluate(host, host.server().clock().frame(), takers);
}

// XP to a runner: Wisdom's share added, then each level reached banked for a pick (offerPicks),
// with a sound, a glow, 3 s of invulnerability and a fifth of its health back.
void TideDirector::gainXp(GameHost& host, ServerObject& unit, Runner& r, int xp)
{
    // The script's (levels.luau's gainXp), on the runner in hand.
    const HandScope hand(*this, &host, &unit, &r, host.server().clock().frame());
    rule(Rule::GainXp, {static_cast<double>(unit.extent), static_cast<double>(xp)}, {});
}

// A level-up's offer: three drawn at random from its weapons' next levels, the new weapons of its
// class (while it holds fewer than 6) and its passives' next levels; a heal when nothing's left.
void TideDirector::offerPicks(GameHost& host, ServerObject& unit, Runner& r)
{
    World& world = host.server().world();
    std::vector<TidePick> all;
    // The offer drawn by the script (levels.luau's offerPicks, on the runner in hand), with
    // its line for a stock client.
    const std::uint32_t frame = host.server().clock().frame();
    const HandScope hand(*this, &host, &unit, &r, frame);
    std::string line;
    ruleWith(
        Rule::OfferPicks, 2,
        [&](lua_State* L) {
            lua_pushnumber(L, unit.player ? unit.player->classId : 0);
            lua_pushnumber(L, r.level - r.banked + 1);
            return 2;
        },
        [&](lua_State* L) {
            const int n = static_cast<int>(lua_objlen(L, -2));
            for (int i = 1; i + 2 <= n; i += 3) {
                TidePick p;
                lua_rawgeti(L, -2, i);
                p.kind = static_cast<TidePick::Kind>(static_cast<int>(lua_tonumber(L, -1)));
                lua_rawgeti(L, -3, i + 1);
                p.which = static_cast<std::uint8_t>(lua_tonumber(L, -1));
                lua_rawgeti(L, -4, i + 2);
                p.level = static_cast<std::uint8_t>(lua_tonumber(L, -1));
                lua_pop(L, 3);
                all.push_back(p);
            }
            std::size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            line.assign(s ? s : "", s ? len : 0);
        });
    r.offer = all;
    r.offeredAt = frame;
    // A noxsdl player's in its window (kOffer), a stock player's in a line.
    for (const auto& [peer, p] : host.players()) {
        if (p.extent == unit.extent && host.identified(peer)) {
            tellOffer(host, runnerKey(p), r);
            return;
        }
    }
    tellPlayer(world, unit, line);
}

// A pick taken from the runner's offer: a weapon added or raised, a passive raised (applied at
// once), or a heal; one banked level spent; the noxsdl window closed.
bool TideDirector::pick(GameHost& host, const std::string& name, std::size_t index)
{
    Runner* found = findRunner(name);
    if (!found || index >= found->offer.size()) {
        return false;
    }
    Runner& r = *found;
    World& world = host.server().world();
    ServerObject* unit = world.byExtent(r.unit);
    const TidePick p = r.offer[index];
    r.offer.clear();
    r.banked = std::max(0, r.banked - 1);
    for (const auto& [key, x] : runners_) {
        if (&x == found) {
            tellOffer(host, key, r); // (its window closed)
        }
    }
    // What it does is the script's (levels.luau's pick, on the runner in hand); the line it's told.
    const HandScope hand(*this, &host, unit, &r, host.server().clock().frame());
    std::string told;
    ruleWith(
        Rule::Pick, 1,
        [&](lua_State* L) {
            if (unit) {
                lua_pushnumber(L, unit->extent);
            } else {
                lua_pushnil(L);
            }
            lua_pushnumber(L, static_cast<double>(p.kind));
            lua_pushnumber(L, p.which);
            lua_pushnumber(L, p.level);
            return 4;
        },
        [&](lua_State* L) {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            told.assign(s ? s : "", s ? len : 0);
        });
    if (unit) {
        tellPlayer(world, *unit, told);
    }
    return true;
}

bool TideDirector::giveWeapon(const std::string& name, TideWeapon kind, int level)
{
    Runner* r = findRunner(name);
    if (!r) {
        return false;
    }
    auto& weapons = r->weapons;
    const auto w = std::ranges::find_if(weapons, [&](const Runner::Weapon& x) { return x.kind == kind; });
    if (level <= 0) {
        if (w != weapons.end()) {
            weapons.erase(w);
        }
        return true;
    }
    const auto lv = static_cast<std::uint8_t>(std::clamp(level, 1, tideMaxLevel()));
    if (w != weapons.end()) {
        w->level = lv;
    } else {
        weapons.push_back({kind, lv, 0});
    }
    return true;
}

bool TideDirector::giveXp(GameHost& host, const std::string& name, int xp)
{
    Runner* r = findRunner(name);
    ServerObject* unit = r ? host.server().world().byExtent(r->unit) : nullptr;
    if (!unit || !unit->player || xp <= 0) {
        return false;
    }
    gainXp(host, *unit, *r, xp);
    return true;
}

bool TideDirector::givePassive(const std::string& name, TidePassive kind, int level)
{
    Runner* r = findRunner(name);
    if (!r) {
        return false;
    }
    r->passives[static_cast<std::size_t>(kind)] = static_cast<std::uint8_t>(std::clamp(level, 0, passiveMax(kind)));
    return true;
}

void TideDirector::applyPassives(World& world, ServerObject& unit, Runner& r)
{
    // Vitality's most health and Recovery's health back are the script's (arena.luau's
    // passiveHealth, asked once for each base health and levels).
    const int vit = r.passives[static_cast<std::size_t>(TidePassive::Vitality)];
    const int rec = r.passives[static_cast<std::size_t>(TidePassive::Recovery)];
    const std::uint64_t key = (static_cast<std::uint64_t>(r.baseHealth) << 16) | (static_cast<std::uint64_t>(vit) << 8) | static_cast<std::uint64_t>(rec);
    auto it = passiveHealth_.find(key);
    if (it == passiveHealth_.end()) {
        double out[2] = {};
        rule(Rule::PassiveHealth, {static_cast<double>(r.baseHealth), static_cast<double>(vit), static_cast<double>(rec)}, out);
        it = passiveHealth_.emplace(key, std::pair<std::uint16_t, int>{static_cast<std::uint16_t>(out[0]), static_cast<int>(out[1])}).first;
    }
    const auto [want, regen] = it->second;
    if (vit > 0 && r.baseHealth != 0 && unit.maxHealth != want) {
        const bool full = unit.health >= unit.maxHealth;
        if (unit.maxHealth != r.baseHealth && unit.maxHealth < r.baseHealth) {
            r.baseHealth = unit.maxHealth;
        }
        unit.maxHealth = want;
        if (full) {
            unit.health = want;
        }
        unit.player->healthSent = -1; // (MSG_REPORT_TOTAL_HEALTH: its most told)
        unit.touch();
    }
    if (regen > 0 && unit.health < unit.maxHealth) {
        adjustHealth(world, unit, regen);
    }
}

TideDirector::Runner* TideDirector::runnerOf(GameHost& host, std::uint8_t peer)
{
    const auto who = host.players().find(peer);
    if (who == host.players().end()) {
        return nullptr;
    }
    const auto it = runners_.find(runnerKey(who->second));
    return it == runners_.end() ? nullptr : &it->second;
}

bool TideDirector::chatLine(GameHost& host, std::uint8_t peer, std::u16string_view text)
{
    // What a line does is the script's (chat.luau's onChat): the line ASCII lower-cased (anything
    // else '?'), whether it's all ASCII, its length; the player's runner in hand while a run goes.
    std::string lower;
    bool ascii = true;
    for (const char16_t c : text) {
        lower += c < 0x80 ? static_cast<char>(std::tolower(static_cast<int>(c))) : '?';
        ascii = ascii && c < 0x80;
    }
    const auto who = host.players().find(peer);
    const bool inGame = who != host.players().end();
    ServerObject* unit = inGame ? host.server().world().byExtent(who->second.extent) : nullptr;
    Runner* r = state_ == State::Running ? runnerOf(host, peer) : nullptr;
    const HandScope hand(*this, &host, unit, r, host.server().clock().frame());
    bool taken = false;
    ruleWith(
        Rule::OnChat, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, peer);
            lua_pushboolean(L, inGame);
            if (inGame) {
                lua_pushlstring(L, who->second.name.data(), who->second.name.size());
            } else {
                lua_pushstring(L, "");
            }
            if (unit) {
                lua_pushnumber(L, unit->extent);
            } else {
                lua_pushnil(L);
            }
            lua_pushlstring(L, lower.data(), lower.size());
            lua_pushboolean(L, ascii);
            lua_pushnumber(L, static_cast<double>(text.size()));
            lua_pushboolean(L, r != nullptr);
            return 8;
        },
        [&](lua_State* L) { taken = lua_toboolean(L, -1) != 0; });
    return taken;
}

} // namespace nox::server
