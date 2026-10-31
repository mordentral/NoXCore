// Delving Mode's director (delve.hpp has the design): noxcpp's own endless run, not in the
// original game. It draws each act and level, writes the generated maps (built ahead on worker
// threads), and runs every level's frame on top of the classic quest (quest.cpp): the lobby and
// its lock, the gates and the party's choice, elites, campfires, altars and blessings, bonus
// objectives, barrels, bot guardians, and the records table.
#include "noxcore/server/delve.hpp"

#include "noxcore/bot/bot_host.hpp"
#include "noxcore/bot/net_bot.hpp"
#include "noxcore/mapgen/arena_lua.hpp"
#include "noxcore/mapgen/biome.hpp"
#include "noxcore/modes/script/gen_runtime.hpp"
#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/modes/script/scripted_mode.hpp"
#include "noxcore/net/udp_socket.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/delve_map.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/record_class.hpp"
#include "noxcore/server/rewards.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/util/x87.hpp"

#include "delve_data.hpp"
#include "delve_links.hpp"
#include "watchers.hpp"
#include "lua_lists.hpp"
#include "mode_script.hpp"
#include "mode_shared.hpp"
#include "record_file.hpp"

#include "lua.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace nox::server {

namespace {

// The tables (the creatures by tier, the guardians, the difficulties, the lobby's words, the
// rooms' and climates' words, the names given, the champion's kit) are the script's
// (script/server/data.luau), read once at set-up (delve_data.hpp).
using delve_data::data;
using modes::script::fillText;
using modes::detail::pushList;
using modes::detail::pushNumbers;
using modes::detail::pushStrings;

/// What the Delve tells players: the script's text table (its templates, fillText's).
const delve_data::Text& words()
{
    return data().text;
}

/// The thing types found on the Delve's maps as they load (data.luau's `things`), interned once.
const detail::ThingTypes& thingTypes()
{
    static const detail::ThingTypes types = detail::thingTypes(data().things);
    return types;
}

std::string num(long long n)
{
    return std::to_string(n);
}


/// A map's name ("DEL_LOB.map", or a path) is this one, case aside.
using detail::isMap;
using detail::spacedName;
using detail::tellPlayer;
using detail::within;

/// The script's tables read now, at a set-up (the director's constructor), so no frame is the
/// first to ask. Throws std::runtime_error when the built-in script isn't the data it should be.
void loadDelveData()
{
    (void)data();
}

/// A line for MSG_TEXT_MESSAGE: at most 250 characters.
using detail::widen;

/// owner.biomes() and owner.biome(name): the map generator's biomes, to the Delve's script.
void addBiomes(modes::script::ScriptedMode& script)
{
    // owner.biomes(): the map generator's biomes in its order (mapgen::biomes()), each its name,
    // the weight an act draws it by, whether its rooms are built halls, its chests.
    script.addOwnerFunction("biomes", [](lua_State* L) {
        const auto& all = mapgen::biomes();
        lua_createtable(L, static_cast<int>(std::size(all)), 0);
        int i = 0;
        for (const mapgen::Biome& b : all) {
            lua_createtable(L, 0, 4);
            lua_pushlstring(L, b.name.data(), b.name.size());
            lua_setfield(L, -2, "name");
            lua_pushnumber(L, b.weight);
            lua_setfield(L, -2, "weight");
            lua_pushboolean(L, b.style == mapgen::LayoutStyle::Built);
            lua_setfield(L, -2, "built");
            pushStrings(L, b.chests);
            lua_setfield(L, -2, "chests");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    });
    // owner.biome(name): one biome by its name, any case (mapgen::findBiome): its title and climate
    // ("Mild", "Freezing", "Scorching"); nothing: none such.
    script.addOwnerFunction("biome", [](lua_State* L) {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, 1, &n);
        const mapgen::Biome* b = s ? mapgen::findBiome(std::string_view(s, n)) : nullptr;
        if (!b) {
            return 0;
        }
        lua_pushlstring(L, b->title.data(), b->title.size());
        lua_pushstring(L, b->climate == mapgen::Climate::Freezing ? "Freezing" : b->climate == mapgen::Climate::Scorching ? "Scorching" : "Mild");
        return 2;
    });
}

/// The Delve's script (script/server/main.luau, built into the program) as a script mode its
/// director owns (mode_script.hpp), with the generator's biomes; `owners` the director's own
/// functions, `override` a test's files.
std::unique_ptr<modes::script::ScriptedMode> makeScript(const things::ThingDb* things, const std::function<void(modes::script::ScriptedMode&)>& owners,
                                                       std::function<std::optional<std::string>(std::string_view)> override)
{
    // (Its rules are the program's own, trusted: a built-in script's budget.)
    return detail::makeBuiltinScript("delve", "Delve", things, modes::script::LuauVm::Limits::builtIn(), [&](modes::script::ScriptedMode& script) {
        addBiomes(script);
        if (owners) {
            owners(script); // (the director's own: its state its rules need)
        }
    }, std::move(override));
}

} // namespace

/// A built-in rule's failure (a fault in the program's own script): said loudly as it's thrown,
/// never taken for a level that can't be built (build() and prepare() pass it on), and caught at
/// the director's doors (tick, prepare, a chat line, a message), so no tick is thrown out of.
struct DelveRuleFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The script and its rules, each looked up once (ScriptedMode::rule).
struct DelveRules {
    using Handle = modes::script::ScriptedMode::Rule;
    struct Rule {
        Handle handle;
        const char* name = "";
    };
    std::unique_ptr<modes::script::ScriptedMode> script;
    Rule more, toughens, health, elites, bossTraits, bossHealth, regenerate, champion;
    Rule roll, fork, base, level, setObjective, setBotBoss, lobbyParams;
    Rule beginLevel, carriers, keepers, barrels, bossDraw, hunt, championDraw, chat, difficultyAsked, blessingsAsked, record;
    Rule runSeed, choiceInto, ahead, plan, swift;
    Rule lobbyArrive, lobbySign, lobbyBoard, lobbyPortal, records;
    Rule describe, objectiveText;
    Rule levelBegun, gateSign, gateTaken, guardianFell, levelTold, partyFallen, makeElite, heat;
    Rule objectiveDone, purgeHalf, tooSlow, atCampfire, campfireUpgrade, atAltar, blessed;
    Rule shrine, blessingKept, unbless, sealBroken, barrelsBroken, clearBar, sendRevokes, wipe;

    [[noreturn]] void failed(const Rule& r) const
    {
        const std::string why = std::string("the Delve's rule ") + r.name + " failed: " + (script->errors().empty() ? std::string() : script->errors().back());
        std::fprintf(stderr, "DELVE SCRIPT FAILURE: %s\n", why.c_str());
        throw DelveRuleFailed(why);
    }

    /// (owners: the director's owner functions; none for a lent script.)
    explicit DelveRules(const things::ThingDb* things, const std::function<void(modes::script::ScriptedMode&)>& owners = {},
                        std::function<std::optional<std::string>(std::string_view)> override = {})
        : script(makeScript(things, owners, std::move(override)))
    {
        const auto find = [&](Rule& r, const char* name) {
            r = {script->rule(name), name};
            if (r.handle.index < 0 || !script->has(r.handle)) { // (a missing hook fails here, at set-up)
                throw std::runtime_error(std::string("the Delve's script has no rule ") + name);
            }
        };
        find(more, "more");
        find(toughens, "toughens");
        find(health, "health");
        find(elites, "elites");
        find(bossTraits, "bossTraits");
        find(bossHealth, "bossHealth");
        find(regenerate, "regenerate");
        find(champion, "champion");
        find(roll, "roll");
        find(fork, "fork");
        find(base, "base");
        find(level, "level");
        find(setObjective, "setObjective");
        find(setBotBoss, "setBotBoss");
        find(lobbyParams, "lobbyParams");
        find(beginLevel, "beginLevel");
        find(carriers, "carriers");
        find(keepers, "keepers");
        find(barrels, "barrels");
        find(bossDraw, "bossDraw");
        find(hunt, "hunt");
        find(championDraw, "championDraw");
        find(chat, "chat");
        find(difficultyAsked, "difficultyAsked");
        find(blessingsAsked, "blessingsAsked");
        find(record, "record");
        find(runSeed, "runSeed");
        find(choiceInto, "choiceInto");
        find(ahead, "ahead");
        find(plan, "plan");
        find(swift, "swift");
        find(lobbyArrive, "lobbyArrive");
        find(lobbySign, "lobbySign");
        find(lobbyBoard, "lobbyBoard");
        find(lobbyPortal, "lobbyPortal");
        find(records, "records");
        find(describe, "describe");
        find(objectiveText, "objectiveText");
        find(levelBegun, "levelBegun");
        find(gateSign, "gateSign");
        find(gateTaken, "gateTaken");
        find(guardianFell, "guardianFell");
        find(levelTold, "levelTold");
        find(partyFallen, "partyFallen");
        find(makeElite, "makeElite");
        find(heat, "heat");
        find(objectiveDone, "objectiveDone");
        find(purgeHalf, "purgeHalf");
        find(tooSlow, "tooSlow");
        find(atCampfire, "atCampfire");
        find(campfireUpgrade, "campfireUpgrade");
        find(atAltar, "atAltar");
        find(blessed, "blessed");
        find(shrine, "shrine");
        find(blessingKept, "blessingKept");
        find(unbless, "unbless");
        find(sealBroken, "sealBroken");
        find(barrelsBroken, "barrelsBroken");
        find(clearBar, "clearBar");
        find(sendRevokes, "sendRevokes");
        find(wipe, "wipe");
    }

    /// Rule `r` called: `push` pushes its arguments and says how many, `read` (given) takes its
    /// `nresults` results off the stack. Throws as call does.
    void callWith(const Rule& r, int nresults, const std::function<int(lua_State*)>& push, const std::function<void(lua_State*)>& read = {}) const
    {
        if (!script->call(r.handle, nresults, push, read)) {
            failed(r);
        }
    }

    /// Rule `r` called with `args`: its first `out.size()` results into `out` (a boolean as 1 or
    /// 0). Throws std::runtime_error when it fails (a fault in the built-in script: the tests run
    /// it).
    void call(const Rule& r, std::initializer_list<double> args, std::span<double> out) const
    {
        const int n = static_cast<int>(out.size());
        const bool ok = script->call(r.handle, n,
                                     [&](lua_State* L) {
                                         for (const double a : args) {
                                             lua_pushnumber(L, a);
                                         }
                                         return static_cast<int>(args.size());
                                     },
                                     [&](lua_State* L) {
                                         for (int i = 0; i < n; ++i) {
                                             const int at = i - n;
                                             out[static_cast<std::size_t>(i)] = lua_isboolean(L, at) ? (lua_toboolean(L, at) ? 1.0 : 0.0) : lua_tonumber(L, at);
                                         }
                                     });
        if (!ok) {
            failed(r);
        }
    }
    /// Rule `r`'s one result, a list (from 1) of numbers (a boolean as 1 or 0) or of strings.
    std::vector<double> numbers(const Rule& r, std::initializer_list<double> args) const
    {
        std::vector<double> out;
        callWith(r, 1, [&](lua_State* L) { return pushAll(L, args); },
                 [&](lua_State* L) {
                     for (int i = 1, n = lua_objlen(L, -1); i <= n; ++i) {
                         lua_rawgeti(L, -1, i);
                         out.push_back(lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? 1.0 : 0.0) : lua_tonumber(L, -1));
                         lua_pop(L, 1);
                     }
                 });
        return out;
    }
    std::vector<std::string> strings(const Rule& r, std::initializer_list<double> args) const
    {
        std::vector<std::string> out;
        callWith(r, 1, [&](lua_State* L) { return pushAll(L, args); },
                 [&](lua_State* L) {
                     for (int i = 1, n = lua_objlen(L, -1); i <= n; ++i) {
                         lua_rawgeti(L, -1, i);
                         std::size_t len = 0;
                         const char* s = lua_tolstring(L, -1, &len);
                         out.emplace_back(s ? s : "", s ? len : 0);
                         lua_pop(L, 1);
                     }
                 });
        return out;
    }
    std::vector<int> numbersInt(const Rule& r, std::initializer_list<double> args) const
    {
        std::vector<int> out;
        for (const double v : numbers(r, args)) {
            out.push_back(static_cast<int>(v));
        }
        return out;
    }
    static int pushAll(lua_State* L, std::initializer_list<double> args)
    {
        for (const double a : args) {
            lua_pushnumber(L, a);
        }
        return static_cast<int>(args.size());
    }
    /// Rule `r`'s one result, a string.
    std::string text(const Rule& r, const std::function<int(lua_State*)>& push) const
    {
        std::string out;
        callWith(r, 1, push, [&](lua_State* L) {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            out.assign(s ? s : "", s ? len : 0);
        });
        return out;
    }
    double one(const Rule& r, std::initializer_list<double> args) const
    {
        double v = 0;
        call(r, args, {&v, 1});
        return v;
    }

    /// A script of its own for a level built off the director (on a worker thread, or by a caller
    /// with no director): one lent from those made so far and given back as the lease ends, so a
    /// thread never makes one each time (Linux starts a thread for every build).
    class Lease {
    public:
        Lease()
        {
            std::lock_guard lock(poolMutex());
            if (!pool().empty()) {
                rules_ = std::move(pool().back());
                pool().pop_back();
            }
        }
        ~Lease()
        {
            if (rules_) {
                std::lock_guard lock(poolMutex());
                pool().push_back(std::move(rules_));
            }
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        const DelveRules& operator*()
        {
            if (!rules_) {
                rules_ = std::make_unique<DelveRules>(nullptr); // (made outside the pool's lock)
            }
            return *rules_;
        }

    private:
        static std::mutex& poolMutex()
        {
            // (Never destroyed: a build let go at the program's end, its future waited for in a
            // static's destructor, may still lend and give back.)
            static std::mutex* const m = new std::mutex;
            return *m;
        }
        static std::vector<std::unique_ptr<DelveRules>>& pool()
        {
            static auto* const p = new std::vector<std::unique_ptr<DelveRules>>; // (never destroyed: as poolMutex)
            return *p;
        }
        std::unique_ptr<DelveRules> rules_;
    };
};

bool isGeneratedMapName(std::string_view name)
{
    return detail::hasMapPrefix(name, "DEL_");
}

DelveDirector::DelveDirector(const things::ThingDb& things, Options options)
    : things_(things)
    , options_(std::move(options))
    , runSeed_(options_.seed)
    , difficulty_(options_.difficulty)
    , rules_(std::make_unique<DelveRules>(&things, [this](modes::script::ScriptedMode& s) { addOwnerFunctions(s); }, options_.scriptOverride))
{
    loadDelveData();
}

// The director's own functions its script's hooks call (the `owner` table):
// - owner.records(n): the first n records (each {depth, act, date, party = {{name, class}}}, a
//   class by its name: record_class.hpp), and the full board's line;
// - owner.begin(): the run begun from the lobby (lock: its party kept, the game closed);
// - owner.startSoloQuest(): the quest begun with everyone in (OTQuest.rul's line);
// - owner.run(): the run as the script's words read it (pushRun: told.luau's run table), and
//   owner.runKey(): what it's kept by (it changes only as a level's written, prepare, and with the
//   difficulty: the level's serial and the difficulty);
// - owner.difficulty() (the run's, from 0) and owner.setDifficulty(d) (set: setDifficulty; false
//   while a run is locked);
// - the campfire's: owner.spellTitle(spell) (the game's title, past ASCII '?', and thing.bin's
//   name; nothing: no such spell), owner.campfireOffer(unit) (its offer, {spell, level} each; nil
//   none), owner.setCampfireOffer(unit, peer, list) (kept, nil put away, and sent to a NoX Core
//   client: MSG_NOXCPP_MODE_F4's campfire, delve_map.cpp), owner.healed(unit) and owner.heal(unit)
//   (a fire's, an altar's or a shrine's done for it this level), owner.celebrate(unit) (the
//   level-up's sound, glow and sparks);
// - the altar's: owner.hasBlessingOffer(unit), owner.setBlessingOffer(unit, peer, list) (each
//   {blessing, held}; kept, nil put away, and sent to a NoX Core client), owner.blessingsHeld(name)
//   (how many of each, from 1; a player never blessed made one of none, as the director's map).
void DelveDirector::addOwnerFunctions(modes::script::ScriptedMode& s)
{
    s.addOwnerFunction("records", [this](lua_State* L) {
        const double n = lua_tonumber(L, 1);
        const auto table = options_.records && n > 0 ? options_.records->top(static_cast<std::size_t>(n)) : std::vector<DelveRecord>{};
        lua_createtable(L, static_cast<int>(table.size()), 0);
        for (std::size_t i = 0; i < table.size(); ++i) {
            const DelveRecord& r = table[i];
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, r.depth);
            lua_setfield(L, -2, "depth");
            lua_pushnumber(L, r.act);
            lua_setfield(L, -2, "act");
            lua_pushlstring(L, r.date.data(), r.date.size());
            lua_setfield(L, -2, "date");
            lua_createtable(L, static_cast<int>(r.party.size()), 0);
            for (std::size_t k = 0; k < r.party.size(); ++k) {
                const auto& [name, cls] = r.party[k];
                const std::string_view className = recordClassName(cls);
                lua_createtable(L, 0, 2);
                lua_pushlstring(L, name.data(), name.size());
                lua_setfield(L, -2, "name");
                lua_pushlstring(L, className.data(), className.size());
                lua_setfield(L, -2, "class");
                lua_rawseti(L, -2, static_cast<int>(k) + 1);
            }
            lua_setfield(L, -2, "party");
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        lua_pushlstring(L, kFullLeaderboard.data(), kFullLeaderboard.size());
        return 2;
    });
    s.addOwnerFunction("begin", [this](lua_State*) {
        if (lobbyHost_) {
            lock(*lobbyHost_);
            state_ = State::Starting;
        }
        return 0;
    });
    s.addOwnerFunction("run", [this](lua_State* L) {
        pushRun(L);
        return 1;
    });
    s.addOwnerFunction("runKey", [this](lua_State* L) {
        lua_pushnumber(L, static_cast<double>(levelSerial_) * 8 + static_cast<int>(difficulty_));
        return 1;
    });
    s.addOwnerFunction("difficulty", [this](lua_State* L) {
        lua_pushnumber(L, static_cast<int>(difficulty_));
        return 1;
    });
    s.addOwnerFunction("setDifficulty", [this](lua_State* L) {
        lua_pushboolean(L, setDifficulty(static_cast<DelveDifficulty>(static_cast<int>(lua_tonumber(L, 1)))));
        return 1;
    });
    s.addOwnerFunction("spellTitle", [this](lua_State* L) {
        const things::SpellDef* def = things_.findSpell(static_cast<std::uint32_t>(lua_tonumber(L, 1)));
        if (!def || !levelHost_) {
            return 0;
        }
        const std::u16string t = levelHost_->server().world().text(def->title);
        std::string s;
        for (const char16_t c : t) {
            s += c < 0x80 ? static_cast<char>(c) : '?';
        }
        lua_pushlstring(L, s.data(), s.size());
        lua_pushlstring(L, def->id.data(), def->id.size());
        return 2;
    });
    s.addOwnerFunction("campfireOffer", [this](lua_State* L) {
        const auto it = offers_.find(static_cast<std::uint32_t>(lua_tonumber(L, 1)));
        if (it == offers_.end()) {
            return 0;
        }
        lua_createtable(L, static_cast<int>(it->second.size()), 0);
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, it->second[i].spell);
            lua_setfield(L, -2, "spell");
            lua_pushnumber(L, it->second[i].level);
            lua_setfield(L, -2, "level");
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        return 1;
    });
    s.addOwnerFunction("setCampfireOffer", [this](lua_State* L) {
        const auto unit = static_cast<std::uint32_t>(lua_tonumber(L, 1));
        const auto peer = static_cast<std::uint8_t>(lua_tonumber(L, 2));
        std::vector<delvemap::Upgradable> list;
        if (lua_istable(L, 3)) {
            for (int i = 1, n = lua_objlen(L, 3); i <= n; ++i) {
                lua_rawgeti(L, 3, i);
                lua_getfield(L, -1, "spell");
                lua_getfield(L, -2, "level");
                list.push_back({static_cast<std::uint8_t>(lua_tonumber(L, -2)), static_cast<std::uint8_t>(lua_tonumber(L, -1))});
                lua_pop(L, 3);
            }
            offers_[ObjectRef{unit}] = list;
        } else {
            offers_.erase(ObjectRef{unit});
        }
        if (levelHost_) {
            levelHost_->sendToIdentified(peer, delvemap::campfireMessage(list));
        }
        return 0;
    });
    s.addOwnerFunction("healed", [this](lua_State* L) {
        lua_pushboolean(L, healed_.contains(static_cast<std::uint32_t>(lua_tonumber(L, 1))));
        return 1;
    });
    s.addOwnerFunction("heal", [this](lua_State* L) {
        healed_.emplace(static_cast<std::uint32_t>(lua_tonumber(L, 1)));
        return 0;
    });
    s.addOwnerFunction("celebrate", [this](lua_State* L) {
        if (levelHost_) {
            World& world = levelHost_->server().world();
            if (const ServerObject* unit = world.byExtent(static_cast<std::uint32_t>(lua_tonumber(L, 1)))) {
                celebrate(world, *unit, levelHost_->server().clock().frame());
            }
        }
        return 0;
    });
    s.addOwnerFunction("hasBlessingOffer", [this](lua_State* L) {
        lua_pushboolean(L, blessOffers_.contains(static_cast<std::uint32_t>(lua_tonumber(L, 1))));
        return 1;
    });
    s.addOwnerFunction("setBlessingOffer", [this](lua_State* L) {
        const auto unit = static_cast<std::uint32_t>(lua_tonumber(L, 1));
        const auto peer = static_cast<std::uint8_t>(lua_tonumber(L, 2));
        std::vector<delvemap::BlessingOffer> list;
        if (lua_istable(L, 3)) {
            for (int i = 1, n = lua_objlen(L, 3); i <= n; ++i) {
                lua_rawgeti(L, 3, i);
                lua_getfield(L, -1, "blessing");
                lua_getfield(L, -2, "held");
                list.push_back({static_cast<delvemap::Blessing>(static_cast<int>(lua_tonumber(L, -2))), static_cast<std::uint8_t>(lua_tonumber(L, -1))});
                lua_pop(L, 3);
            }
            blessOffers_[ObjectRef{unit}] = list;
        } else {
            blessOffers_.erase(ObjectRef{unit});
        }
        if (levelHost_) {
            levelHost_->sendToIdentified(peer, delvemap::blessingMessage(list));
        }
        return 0;
    });
    s.addOwnerFunction("blessingsHeld", [this](lua_State* L) {
        std::size_t n = 0;
        const char* name = lua_tolstring(L, 1, &n);
        const Blessed& b = blessed_[std::string(name ? name : "", name ? n : 0)];
        pushNumbers(L, b.held);
        return 1;
    });
    s.addOwnerFunction("startSoloQuest", [this](lua_State*) {
        if (lobbyHost_) {
            lobbyHost_->command("startSoloQuest");
        }
        return 0;
    });
}

/// (The director's mode hooks: a map's load told; every other hook the game's own.)
struct DelveDirector::Hooks : ModeHooks {
    explicit Hooks(DelveDirector& d) : director(d) {}
    void mapLoaded(Server&) override { director.lobbyTargeted_ = false; } // (told: the lobby's things found again at its next tick)
    DelveDirector& director;
};

void DelveDirector::detach(GameHost& host)
{
    if (hooksHost_ == &host) {
        if (host.server().modeHooks() == hooks_.get()) {
            host.server().setModeHooks(nullptr);
        }
        hooksHost_ = nullptr;
    }
    if (scriptHost_ == &host) {
        rules_->script->detach(host);
        scriptHost_ = nullptr;
    }
}

void DelveDirector::carryLinks(const KeyCarry& carry)
{
    ::nox::server::carryLinks(*this, carry); // (delve_links.hpp)
}

std::string_view delveDifficultyName(DelveDifficulty d)
{
    // (One past the four: the first's name, as ever.)
    const auto& all = data().difficulties;
    const auto i = static_cast<std::size_t>(d);
    return i < all.size() ? all[i].name : all.front().name;
}

bool DelveDirector::setDifficulty(DelveDifficulty d)
{
    if (locked()) {
        return false;
    }
    if (d != difficulty_) {
        difficulty_ = d;
        dropAhead(); // (the next run's first level built again for it: tick)
    }
    return true;
}

int DelveDirector::sweep(const std::filesystem::path& mapsDir)
{
    return detail::sweepMaps(mapsDir, isGeneratedMapName);
}

int DelveDirector::sweepOwn() const
{
    return detail::sweepMapsNamed(options_.mapsDir, {options_.mapName, options_.lobbyName});
}

namespace {

// A level as the script's levels.luau holds it, to and from Lua: its params (those fields a level
// of the Delve sets, of the same names) and what it holds.
void setNumber(lua_State* L, const char* field, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, field);
}

void setString(lua_State* L, const char* field, const std::string& v)
{
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, field);
}

std::string stringOf(lua_State* L, int at)
{
    std::size_t n = 0;
    const char* s = lua_tolstring(L, at, &n);
    return s ? std::string(s, n) : std::string();
}

/// Table `t`'s field: whether it's there (it's then on the stack, to pop).
bool field(lua_State* L, int t, const char* name)
{
    lua_getfield(L, t, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

template <typename T>
void readNumber(lua_State* L, int t, const char* name, T& out)
{
    if (field(L, t, name)) {
        out = lua_isboolean(L, -1) ? static_cast<T>(lua_toboolean(L, -1)) : static_cast<T>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

/// The list (from 1) of whole numbers at `at`.
std::vector<int> numbersAt(lua_State* L, int at)
{
    std::vector<int> out;
    for (int i = 1, n = lua_objlen(L, at); i <= n; ++i) {
        lua_rawgeti(L, at, i);
        out.push_back(static_cast<int>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    return out;
}

void readString(lua_State* L, int t, const char* name, std::string& out)
{
    if (field(L, t, name)) {
        out = stringOf(L, -1);
        lua_pop(L, 1);
    }
}

// The params' fields a level of the Delve sets (arena_lua.hpp), read back as they're there.
constexpr std::uint32_t kLevelFields = mapgen::ArenaField::GameFlags | mapgen::ArenaField::QuestExit | mapgen::ArenaField::Seed |
                                       mapgen::ArenaField::Segments | mapgen::ArenaField::Radius | mapgen::ArenaField::Biome |
                                       mapgen::ArenaField::Monsters | mapgen::ArenaField::Items | mapgen::ArenaField::CenterX |
                                       mapgen::ArenaField::CenterY | mapgen::ArenaField::Centre | mapgen::ArenaField::EndRadius |
                                       mapgen::ArenaField::Gates | mapgen::ArenaField::Depth | mapgen::ArenaField::Traps |
                                       mapgen::ArenaField::Puzzles | mapgen::ArenaField::Ambushes | mapgen::ArenaField::Doors |
                                       mapgen::ArenaField::Obelisks | mapgen::ArenaField::Chests;

void pushParams(lua_State* L, const mapgen::ArenaParams& p)
{
    mapgen::pushArenaParams(L, p, kLevelFields);
}

void readParams(lua_State* L, int t, mapgen::ArenaParams& p)
{
    mapgen::readArenaParams(L, t, p, kLevelFields, mapgen::ArenaRead::Present);
}

void pushLevel(lua_State* L, const DelveLevel& l)
{
    lua_createtable(L, 0, 9);
    setNumber(L, "room", static_cast<double>(l.room));
    setNumber(L, "mystery", l.mystery ? static_cast<double>(*l.mystery) : -1.0);
    setString(L, "boss", l.boss);
    setNumber(L, "bossHealthTimes", l.bossHealthTimes);
    setNumber(L, "bossDrop", l.bossDrop);
    setNumber(L, "objective", static_cast<double>(l.objective));
    setNumber(L, "objectiveSeconds", l.objectiveSeconds);
    setNumber(L, "botBoss", l.botBoss ? static_cast<double>(*l.botBoss) : -1.0);
    pushParams(L, l.params);
    lua_setfield(L, -2, "params");
}

void readLevel(lua_State* L, int t, DelveLevel& l)
{
    int n = 0;
    readNumber(L, t, "room", n);
    l.room = static_cast<DelveRoom>(n);
    n = -1;
    readNumber(L, t, "mystery", n);
    l.mystery = n < 0 ? std::nullopt : std::optional(static_cast<DelveMystery>(n));
    readString(L, t, "boss", l.boss);
    readNumber(L, t, "bossHealthTimes", l.bossHealthTimes);
    readNumber(L, t, "bossDrop", l.bossDrop);
    n = 0;
    readNumber(L, t, "objective", n);
    l.objective = static_cast<DelveObjective>(n);
    readNumber(L, t, "objectiveSeconds", l.objectiveSeconds);
    n = -1;
    readNumber(L, t, "botBoss", n);
    l.botBoss = n < 0 ? std::nullopt : std::optional(static_cast<std::uint8_t>(n));
    if (field(L, t, "params")) {
        readParams(L, lua_gettop(L), l.params);
        lua_pop(L, 1);
    }
}

/// A level's title: "Delve 3" (the script's words), its depth at least 1.
std::string levelTitle(int depth)
{
    return fillText(words().levelTitle, {{"depth", num(std::max(depth, 1))}});
}

} // namespace

mapgen::ArenaParams DelveDirector::levelFor(int depth, std::uint32_t seed)
{
    // (The script's rule, levels.base: a fight's length, monsters and potions, drawn from the
    // level's own std::mt19937 as the C++ drew them.)
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    mapgen::ArenaParams p;
    rules.callWith(
        rules.base, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, depth);
            lua_pushnumber(L, seed);
            return 2;
        },
        [&](lua_State* L) { readParams(L, lua_gettop(L), p); });
    p.title = levelTitle(depth);
    return p;
}

void DelveDirector::setObjective(DelveLevel& level, DelveObjective objective)
{
    // (The script's rule, levels.setObjective: Seals' seals first among the things placed,
    // Swift's time.)
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    rules.callWith(
        rules.setObjective, 1,
        [&](lua_State* L) {
            pushLevel(L, level);
            lua_pushnumber(L, static_cast<double>(objective));
            return 2;
        },
        [&](lua_State* L) { readLevel(L, lua_gettop(L), level); });
}

void DelveDirector::setBotBoss(DelveLevel& level, std::uint8_t playerClass)
{
    // (The script's rule, levels.setBotBoss: a duel, its spot free.)
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    rules.callWith(
        rules.setBotBoss, 1,
        [&](lua_State* L) {
            pushLevel(L, level);
            lua_pushnumber(L, playerClass);
            return 2;
        },
        [&](lua_State* L) { readLevel(L, lua_gettop(L), level); });
}

std::string_view delveRoomName(DelveRoom room)
{
    const auto& rooms = data().rooms;
    const auto i = static_cast<std::size_t>(room);
    return i < rooms.size() ? std::string_view(rooms[i].name) : std::string_view("?");
}

DelveRoom DelveAct::room(int row, int lane) const
{
    const auto& r = rows[static_cast<std::size_t>(std::clamp(row, 0, kLength - 1))];
    if (r.empty()) {
        return row >= kLength - 1 ? DelveRoom::Guardian : DelveRoom::Fight;
    }
    return r[std::min(static_cast<std::size_t>(std::max(lane, 0)), r.size() - 1)];
}

void DelveAct::fill(DelveRoom kind)
{
    for (int r = 0; r < kLength; ++r) {
        rows[static_cast<std::size_t>(r)].assign(static_cast<std::size_t>(width(r)), kind);
    }
}

namespace {

/// An act as the script's acts.roll gives it: its biome at `at`, its rows (from 1, each a list of
/// rooms by their numbers) at `at` + 1.
DelveAct actOf(lua_State* L, int at, int number)
{
    DelveAct a;
    a.number = number;
    std::size_t n = 0;
    const char* biome = lua_tolstring(L, at, &n);
    a.biome.assign(biome ? biome : "", biome ? n : 0);
    for (int r = 0; r < DelveAct::kLength; ++r) {
        lua_rawgeti(L, at + 1, r + 1);
        const int count = lua_objlen(L, -1);
        auto& row = a.rows[static_cast<std::size_t>(r)];
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            row.push_back(static_cast<DelveRoom>(static_cast<int>(lua_tonumber(L, -1))));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return a;
}

/// An act's rows (from 1, each a list of its rooms by number), pushed.
void pushRows(lua_State* L, const DelveAct& a)
{
    pushList(L, a.rows, [L](const std::vector<DelveRoom>& row) { pushNumbers(L, row, [](DelveRoom room) { return static_cast<int>(room); }); });
}

/// An act to the script (run.luau's, told.luau's): {number, biome, rows, path}.
void pushAct(lua_State* L, const DelveAct& a)
{
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, a.number);
    lua_setfield(L, -2, "number");
    lua_pushlstring(L, a.biome.data(), a.biome.size());
    lua_setfield(L, -2, "biome");
    pushRows(L, a);
    lua_setfield(L, -2, "rows");
    pushNumbers(L, a.path);
    lua_setfield(L, -2, "path");
}

/// A guardian's fork to the script: {its left act, its right}.
void pushFork(lua_State* L, const std::array<DelveAct, 2>& fork)
{
    lua_createtable(L, 2, 0);
    pushAct(L, fork[0]);
    lua_rawseti(L, -2, 1);
    pushAct(L, fork[1]);
    lua_rawseti(L, -2, 2);
}

} // namespace

DelveAct DelveAct::roll(int number, std::uint32_t seed, std::span<const std::string> notBiomes)
{
    // (The script's rule, acts.roll: an act's own std::mt19937 drawn as the C++ drew it.)
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    DelveAct a;
    rules.callWith(
        rules.roll, 2,
        [&](lua_State* L) {
            lua_pushnumber(L, number);
            lua_pushnumber(L, seed);
            for (const std::string& b : notBiomes) {
                lua_pushlstring(L, b.data(), b.size());
            }
            return 2 + static_cast<int>(notBiomes.size());
        },
        [&](lua_State* L) { a = actOf(L, lua_gettop(L) - 1, number); });
    return a;
}

std::array<DelveAct, 2> DelveAct::fork(int number, std::uint32_t seed, const std::string& notBiome)
{
    // (The script's rule, acts.fork: the left act, the right one drawn again while they match.)
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    std::array<DelveAct, 2> two;
    rules.callWith(
        rules.fork, 4,
        [&](lua_State* L) {
            lua_pushnumber(L, number);
            lua_pushnumber(L, seed);
            lua_pushlstring(L, notBiome.data(), notBiome.size());
            return 3;
        },
        [&](lua_State* L) {
            const int top = lua_gettop(L);
            two[0] = actOf(L, top - 3, number);
            two[1] = actOf(L, top - 1, number);
        });
    return two;
}

std::string DelveAct::describe() const
{
    // (The script's words, told.describe: the first room, then rows 2-4's by kind.)
    DelveRules::Lease lease;
    return (*lease).text((*lease).describe, [&](lua_State* L) {
        pushAct(L, *this);
        return 1;
    });
}
DelveLevel DelveDirector::levelFor(const DelveAct& act, int depth, std::uint32_t seed, const std::array<DelveAct, 2>* fork)
{
    // (The script's rule, levels.level: the room the act's way reaches and what it holds, drawn
    // from the level's own std::mt19937s as the C++ drew them.)
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    DelveLevel level;
    rules.callWith(
        rules.level, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, act.number);
            lua_pushlstring(L, act.biome.data(), act.biome.size());
            pushRows(L, act);
            pushNumbers(L, act.path);
            lua_pushnumber(L, depth);
            lua_pushnumber(L, seed);
            if (!fork) {
                return 6;
            }
            lua_pushlstring(L, (*fork)[0].biome.data(), (*fork)[0].biome.size());
            lua_pushlstring(L, (*fork)[1].biome.data(), (*fork)[1].biome.size());
            return 8;
        },
        [&](lua_State* L) { readLevel(L, lua_gettop(L), level); });
    level.params.title = levelTitle(depth);
    return level;
}

mapgen::ArenaParams DelveDirector::lobbyParams()
{
    // (The script's: levels.lobby, read back as the levels' are; the fields it doesn't set the
    // generator's own.)
    using F = mapgen::ArenaField;
    DelveRules::Lease lease;
    const DelveRules& rules = *lease;
    mapgen::ArenaParams p;
    rules.callWith(
        rules.lobbyParams, 1, [](lua_State*) { return 0; },
        [&](lua_State* L) {
            mapgen::readArenaParams(L, lua_gettop(L), p,
                                    F::Title | F::GameFlags | F::MinPlayers | F::MaxPlayers | F::Radius | F::FloorName | F::Ambient | F::PlayerStarts |
                                        F::Items | F::QuestExit | F::Signs | F::Centre | F::Seed,
                                    mapgen::ArenaRead::Present);
        });
    return p;
}

std::span<const std::string_view> DelveDirector::rules()
{
    return data().rules.views;
}

std::filesystem::path DelveDirector::writeLobby()
{
    return mapgen::writeMapFiles(options_.mapsDir, options_.lobbyName, mapgen::buildArena(things_, lobbyParams()), things_);
}

void DelveDirector::lock(GameHost& host)
{
    // (The run's party, for its record: names and classes as it begins, multiclass and the NoX
    // Core client noted: record_class.hpp.)
    runParty_.clear();
    runDepth_ = 0;
    toldBest_ = false;
    for (const auto& [peer, p] : host.players()) {
        ServerObject* unit = host.server().world().byExtent(p.extent);
        const bool player = unit && unit->player;
        runParty_.emplace_back(p.name, recordClass(player ? unit->player->classId : 0, player && unit->player->multiclass, host.identified(peer)));
        if (unit && unit->player) {
            unit->player->questGameOverAt = 0; // (a game over of an earlier run's)
        }
    }
    // A closed game, its allowed list the party.
    detail::lockGame(host, savedClosed_, savedAllowed_);
}

void DelveDirector::unlock(GameHost& host)
{
    detail::unlockGame(host, savedClosed_, savedAllowed_);
}

// The director's doors: a built-in rule's failure (DelveRuleFailed, said loudly as it's thrown)
// or the generator's (GenerationFailed, said loudly by the generator) stops here, never out of the
// host's tick or its map change: the map change then loads the files as they were last written,
// the game going on (bug hunt 3: out of the map change, it left a host's game half-changed).
namespace {

/// A door's call: `inside`'s answer, or `failed`'s when either failure is thrown out of it.
template <class Inside, class Failed>
auto atDoor(Inside&& inside, Failed&& failed) -> decltype(inside())
{
    try {
        return inside();
    } catch (const DelveRuleFailed&) {
        return failed();
    } catch (const modes::script::GenerationFailed&) {
        return failed();
    }
}

} // namespace

bool DelveDirector::tick(GameHost& host)
{
    const State was = state_;
    // (Failed, the rest of the tick lost, but its answer still true to the lock: a rule failing
    // after the portal's owner.begin has locked the game.)
    return atDoor([&] { return tickRun(host); }, [&] { return locked() != (was != State::Lobby); });
}

bool DelveDirector::prepare(const std::filesystem::path& next, const World& world)
{
    try {
        return atDoor([&] { return prepareRun(next, world); }, [] { return false; });
    } catch (const std::exception& e) {
        // (A level that couldn't be built on any of its seeds, or its files not written: said
        // loudly, the map change going on with the files as last written, as above;
        // a deliberate choice.)
        std::fprintf(stderr, "DELVE LEVEL FAILURE: depth %d not made (%s); the last map is loaded again\n", depth_, e.what());
        return false;
    }
}

bool DelveDirector::onChat(GameHost& host, std::uint8_t peer, std::u16string_view text)
{
    return atDoor([&] { return onChatRun(host, peer, text); }, [] { return false; });
}

void DelveDirector::onMessage(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m)
{
    atDoor([&] { onMessageRun(host, peer, m); }, [] {});
}

bool DelveDirector::tickRun(GameHost& host)
{
    const util::RoundingScope rounding(host.server().exactMaths()); // (the server's rounding: x87.hpp)
    // The script attached to the host at the first tick (its float maths then the host's rounding),
    // and ticked with it.
    if (scriptHost_ != &host) {
        rules_->script->attach(host);
        scriptHost_ = &host;
        lastFrame_ = ~0u; // (a new host's frames counted afresh)
    }
    // The director's mode hooks on the server (a map's load told), set at its first tick.
    if (hooksHost_ != &host) {
        if (!hooks_) {
            hooks_ = std::make_unique<Hooks>(*this);
        }
        host.server().setModeHooks(hooks_.get());
        hooksHost_ = &host;
    }
    rules_->script->tick(host, 0);
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    const State was = state_;
    // The bot guardian's sockets, on whatever map: at every call.
    pumpBotBoss();
    // The host's loop calls as often as every couple of milliseconds: the lobby's and the level's
    // work, and what's sent of them, once a frame (a beat, a timer, a sign each fire once in their
    // frame). The state follows the map at every call, so the lock's answer is this call's.
    const bool due = frame != lastFrame_;
    lastFrame_ = frame;
    whichMap_.read(world.mapName, options_.lobbyName, options_.mapName);
    // In the lobby: back from a run, or gathering a party.
    if (whichMap_.lobby()) {
        endBotBoss(host);
        if (state_ == State::Running) {
            unlock(host); // (back from a run)
            state_ = State::Lobby;
            for (const std::uint32_t z : signZones_) {
                watch_->rearm(z); // (the signs speak at once again)
            }
            lobbyTargeted_ = false;
            fileRecord(); // (the run's record: the deepest it went)
            unbless(host);
            ++objective_.version; // (the objective put away: sendObjective)
            // The next run a dungeon of its own: a seed of its own (the first run's the
            // server's), the last run's levels built ahead let go.
            ++runs_;
            runSeed_ = static_cast<std::uint32_t>(rules_->one(rules_->runSeed, {static_cast<double>(options_.seed), static_cast<double>(runs_)})); // (run.seed)
            dropAhead();
        }
        if (state_ == State::Lobby && !ahead_.contains({1, -1})) {
            // A run's first level built ahead while the party gathers (the rest of a run's let go).
            dropAhead();
            buildAhead(1);
        }
        if (state_ == State::Lobby && due) {
            // The signs, the board, the portal and the greeting: watched (watchLobby), each acted
            // on as it happens.
            watchLobby(host);
            lobbyHost_ = &host;
            lobbyFrame_ = frame;
            watch_->evaluate(lobbyGroup_, host, frame);
        }
        host.setServerName(listed(0));
    // In a level of the run.
    } else if (whichMap_.level()) {
        state_ = State::Running;
        lobbyTargeted_ = false; // (the lobby's things found again as it's back)
        if (std::ranges::all_of(host.players(), [&](const auto& e) { return isBotBoss(e.second.name); })) {
            world.exitTo = options_.lobbyName + ".map"; // (everyone gone, a bot boss aside: back to the lobby)
        }
        host.setServerName(listed(depth_));
        if (due) {
            runLevel(host);
        }
    }
    if (due) {
        sendMaps(host);
        sendObjective(host);
    }
    // The spell bars' revokes owed (the script's, nox.revoke), sent once a frame while any are owed
    // (a chat's between the frame's passes too): level.sendRevokes, how many still owed.
    if (revokesOwed_ && frame != revokedAt_) {
        revokedAt_ = frame;
        revokesOwed_ = rules_->one(rules_->sendRevokes, {}) > 0;
    }
    // True when the lock changed this tick.
    return locked() != (was != State::Lobby);
}

const std::string& DelveDirector::listed(int depth)
{
    // (Filled in again only as the depth changes: tick asks every time it runs.)
    if (!listed_ || listed_->first != depth) {
        listed_.emplace(depth, listedName(depth));
    }
    return listed_->second;
}

void DelveDirector::watchLobby(GameHost& host)
{
    World& world = host.server().world();
    if (!watch_) {
        watch_ = std::make_unique<Watchers>();
    }
    if (lobbyGroup_ < 0) {
        // (What's done as each fires: the script's hooks, lobby.luau.) A player come in: the
        // greeting, to one on the stock client.
        // (A unit destroyed, as its player leaves, is told nothing: the toolbox's lines go to units.)
        lobbyGroup_ = watch_->group({}, [this](const Watchers::Event& e) {
            if (e.unit->gone()) {
                return;
            }
            const double identified = lobbyHost_->identified(e.peer) ? 1.0 : 0.0;
            const double unit = e.unit->extent;
            rules_->call(rules_->lobbyArrive, {unit, identified}, {});
        });
        // The signs (west to east: rules()' order) speak to a player near, each at most every 5 s.
        for (std::size_t i = 0; i < data().rules.names.size(); ++i) {
            signZones_.push_back(watch_->zone(lobbyGroup_, 0, 0, 0, data().near.sign, 0, Watchers::Mode::AnyCooldown, data().every.sign,
                                              [this, i](const Watchers::Event&) {
                                                  if (lobbyHost_->server().world().byExtent(lobbySigns_[i])) {
                                                      rules_->call(rules_->lobbySign, {static_cast<double>(i) + 1, static_cast<double>(lobbySigns_[i])}, {});
                                                  }
                                              }));
            watch_->retarget(signZones_.back(), 0);
        }
        // The board: the deepest delves to whoever comes up to it (within 100: a big thing, the
        // starts ~65 off), as it comes, at most every 20 s each (the script's wait); not again
        // while it stands there.
        boardZone_ = watch_->zone(lobbyGroup_, 0, 0, 0, data().near.board, 0, Watchers::Mode::EachEnter, 0, [this](const Watchers::Event& e) {
            if (e.kind != Watchers::Event::Kind::Enter || e.unit->gone()) {
                return;
            }
            const std::string& name = e.player->name;
            rules_->callWith(rules_->lobbyBoard, 0, [&](lua_State* L) {
                lua_pushnumber(L, e.unit->extent);
                lua_pushlstring(L, name.data(), name.size());
                lua_pushnumber(L, lobbyFrame_);
                return 3;
            });
        });
        watch_->retarget(boardZone_, 0);
        // The portal: anyone not busy stepping into it begins the delve (its pass's last: the
        // only Any zone).
        portalZone_ = watch_->zone(lobbyGroup_, 0, 0, 0, data().near.portal, Watchers::NotBusy, Watchers::Mode::Any, 0,
                                   [this](const Watchers::Event&) { rules_->call(rules_->lobbyPortal, {}, {}); });
        watch_->retarget(portalZone_, 0);
    }
    if (lobbyTargeted_) {
        return;
    }
    // The lobby's things, found as it's loaded (the map's numbers for them).
    lobbyTargeted_ = true;
    const detail::LobbyThings found = detail::lobbyThings(world, thingTypes());
    lobbySigns_.assign(signZones_.size(), ObjectRef{});
    for (std::size_t i = 0; i < signZones_.size(); ++i) {
        lobbySigns_[i] = i < found.signs.size() ? found.signs[i] : 0;
        watch_->retarget(signZones_[i], lobbySigns_[i]);
    }
    watch_->retarget(boardZone_, found.board);
    watch_->retarget(portalZone_, found.portal);
}
void DelveDirector::watchLevel()
{
    if (!watch_) {
        watch_ = std::make_unique<Watchers>();
    }
    if (gateSignGroup_ < 0) {
        // (The party: the level's players, a bot guardian aside.)
        const auto party = [this](const HostedPlayer& p, const ServerObject&) { return !isBotBoss(p.name); };
        // (Their players gathered once a tick: the party, everyone ready.)
        partyRoster_ = watch_->roster(party);
        everyoneRoster_ = watch_->roster();
        gateSignGroup_ = watch_->group(party, {}, partyRoster_);
        shrineGroup_ = watch_->group(party, {}, partyRoster_);
        // (Every ready player: the campfire's, the altar's, the seals'.)
        campfireGroup_ = watch_->group({}, {}, everyoneRoster_);
        altarGroup_ = watch_->group({}, {}, everyoneRoster_);
        sealGroup_ = watch_->group({}, {}, everyoneRoster_);
        // The barrels and crates that hold something (till broken), the elites (till gone or
        // dead), the guardian (till it falls), the champion's or guardian's spot (till found).
        barrelList_ = watch_->list(Watchers::Missing | Watchers::OtherType | Watchers::Deleted | Watchers::Destroyed,
                                   [this](const WatchEvent& e) { broken_.push_back(e); });
        eliteList_ = watch_->list(Watchers::Missing | Watchers::NotMonster | Watchers::Dead | Watchers::Gone, [this](const WatchEvent& e) {
            std::erase_if(elites_, [&](const Elite& elite) { return elite.extent == e.id; });
        });
        guardianList_ = watch_->list(Watchers::Missing | Watchers::Gone | Watchers::Dead | Watchers::NoHealth, [this](const WatchEvent&) { guardianFell_ = true; });
        bossFinder_ = watch_->finder([this](const WatchEvent& e) { bossFound(*e.unit); });
        // The blessed (every ready player: each kept by name), the air (the party, every one), the
        // spell bars cleared once a run (the party), the gates taken (the party).
        blessGroup_ = watch_->group({}, {}, everyoneRoster_);
        watch_->keeping(blessGroup_, [this](const WatchEvent& e) { blessingKept(e); });
        airGroup_ = watch_->group(party, {}, partyRoster_);
        watch_->keeping(airGroup_, [this](const WatchEvent& e) { heat_.push_back(e); });
        barsGroup_ = watch_->group(party, {}, partyRoster_);
        watch_->firsts(barsGroup_, Watchers::NotObserving, [this](const WatchEvent& e) { clearBar(e); });
        exitGroup_ = watch_->group(party, {}, partyRoster_);
        // The level's conditions over the party; the timers.
        levelGroup_ = watch_->group(party, {}, partyRoster_);
        watch_->conditions(levelGroup_, {.anyone = [this](const WatchEvent& e) { startObjective(levelHost_->server().world(), *e.unit); },
                                         .afterFrames = data().every.levelLine,
                                         .after = [this](const WatchEvent&) { tellLevel(*levelParty_); },
                                         .allFilter = Watchers::GameOver,
                                         .allFrames = data().every.wipe,
                                         .all = [this](const WatchEvent& e) { partyFallen(e); }});
        eliteTimer_ = watch_->timer([this](const WatchEvent&) { regenerateElites(); });
        watch_->every(eliteTimer_, data().every.regenerate);
        purgeTimer_ = watch_->timer([this](const WatchEvent&) { countPurge(); });
        watch_->every(purgeTimer_, data().every.purge);
        swiftTimer_ = watch_->timer([this](const WatchEvent&) {
            objective_.state = 2;
            ++objective_.version;
            rules_->call(rules_->tooSlow, {}, {}); // (told: the script's hook, level.tooSlow)
        });
    }
}
std::string DelveDirector::listedName(int depth)
{
    return depth <= 0 ? words().listedLobby : fillText(words().listedLevel, {{"depth", num(depth)}});
}

bool DelveDirector::prepareRun(const std::filesystem::path& next, const World& world)
{
    if (!isMap(next.string(), options_.mapName)) {
        return false;
    }
    const int depth = world.quest() ? world.questStage + 1 : 1;
    runDepth_ = std::max(runDepth_, depth);
    // Swift's done as the party leaves through a gate in time (the map changes as the last goes
    // through, before a level's frame sees it): its gold paid as the next level opens.
    // (The script's rule, run.swift.)
    swiftGold_ = static_cast<int>(rules_->one(
        rules_->swift, {state_ == State::Running ? 1.0 : 0.0, static_cast<double>(level_.objective), objective_.started ? 1.0 : 0.0,
                        static_cast<double>(objective_.state), static_cast<double>(frameNow_), static_cast<double>(objective_.deadline),
                        static_cast<double>(depth), static_cast<double>(depth_)}));
    // The way the party went, and the run as that makes it.
    const int choice = choiceInto(depth, world);
    Plan run = plan(depth, choice);
    depth_ = depth;
    deepest_ = std::max(deepest_, depth_);
    if (run.fresh) {
        watchLevel();
        watch_->forgetAll(barsGroup_); // (each one's bars cleared again)
        history_.clear();
    }
    if (run.finished) {
        history_.push_back(*run.finished);
    }
    act_ = std::move(run.act);
    fork_ = std::move(run.fork);
    lastChoice_ = run.lastChoice;
    // Its level: built ahead while the last was played (buildAhead), else built now. (The
    // generator's the long part of a level change: off the game's thread, the change only
    // writes the files.)
    Built built;
    const auto ahead = ahead_.find({depth_, choice});
    bool found = false;
    if (ahead != ahead_.end()) {
        // (Out of the list before it's waited for: whatever its build threw, no spent future is
        // left behind for dropAhead: bug hunt 3.)
        std::future<Built> f = std::move(ahead->second);
        ahead_.erase(ahead);
        try {
            built = f.get(); // (still being made: waits only for what's left)
            found = true;
            ++aheadUsed_;
        } catch (const modes::script::GenerationFailed&) {
            throw; // (the generator broke: never built again)
        } catch (const DelveRuleFailed&) {
            throw; // (said as it failed: not a level to build again)
        } catch (const std::exception&) {
        }
    }
    if (!found) {
        built = build(things_, runSeed_, plan(depth_, -2)); // (the run as it now stands)
        ++builtOnTheSpot_;
    }
    dropAhead();
    level_ = std::move(built.level);
    spots_ = std::move(built.spots);
    mapgen::writeEncodedMapFiles(options_.mapsDir, options_.mapName, built.files);
    buildAhead(depth_ + 1);
    ++levelSerial_;
    told_ = false;
    boss_ = 0;
    bossDown_ = false;
    gatesOpen_ = false;
    gates_.clear();
    gateSigns_.clear();
    firstGate_ = -1;
    watchLevel();
    watch_->reset(exitGroup_); // (the gates taken: the new level's)
    // (The gates' signs: the new level's, their zones made at its first frame when it has gates;
    // none on one without, where the last level's signs' numbers are this map's other things.)
    watch_->reset(gateSignGroup_);
    exitGates_ = ~std::size_t{0};
    airKept_ = false;
    healed_.clear();
    shrineWatched_ = false; // (the level's zones made again as it's played)
    bossSought_ = false;
    guardianWatched_ = 0;
    campfireWatched_ = false;
    altarWatched_ = false;
    return true;
}

namespace {

DelveAct actAt(lua_State* L, int t)
{
    DelveAct a;
    readNumber(L, t, "number", a.number);
    readString(L, t, "biome", a.biome);
    if (field(L, t, "rows")) {
        const int rows = lua_gettop(L);
        for (int r = 0; r < DelveAct::kLength; ++r) {
            lua_rawgeti(L, rows, r + 1);
            for (const int k : numbersAt(L, lua_gettop(L))) {
                a.rows[static_cast<std::size_t>(r)].push_back(static_cast<DelveRoom>(k));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (field(L, t, "path")) {
        for (const int lane : numbersAt(L, lua_gettop(L))) {
            a.path.push_back(static_cast<std::uint8_t>(lane));
        }
        lua_pop(L, 1);
    }
    return a;
}

} // namespace

int DelveDirector::choiceInto(int depth, const World& world) const
{
    // (The script's rule, run.choiceInto; -3: the gate most of the party took, counted here.)
    const int into = static_cast<int>(rules_->one(rules_->choiceInto, {static_cast<double>(depth), static_cast<double>(act_.number),
                                                                       static_cast<double>(act_.path.size()), fork_ ? 1.0 : 0.0,
                                                                       static_cast<double>(gates_.size())}));
    return into == -3 ? choose(world) : into;
}

DelveDirector::Plan DelveDirector::plan(int depth, int choice) const
{
    // (The script's rule, run.plan: the act, its fork, the way taken; drawn from the run's seed.)
    Plan p;
    p.depth = std::max(depth, 1);
    rules_->callWith(
        rules_->plan, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, depth);
            lua_pushnumber(L, choice);
            lua_pushnumber(L, runSeed_);
            pushAct(L, act_);
            if (fork_) {
                pushFork(L, *fork_);
            } else {
                lua_pushnil(L);
            }
            lua_pushnumber(L, lastChoice_);
            lua_pushlstring(L, options_.biome.data(), options_.biome.size());
            lua_pushnumber(L, options_.room ? static_cast<double>(*options_.room) : -1.0);
            return 8;
        },
        [&](lua_State* L) {
            const int t = lua_gettop(L);
            if (field(L, t, "act")) {
                p.act = actAt(L, lua_gettop(L));
                lua_pop(L, 1);
            }
            if (field(L, t, "fork")) {
                const int f = lua_gettop(L);
                std::array<DelveAct, 2> two;
                for (int i = 0; i < 2; ++i) {
                    lua_rawgeti(L, f, i + 1);
                    two[static_cast<std::size_t>(i)] = actAt(L, lua_gettop(L));
                    lua_pop(L, 1);
                }
                p.fork = std::move(two);
                lua_pop(L, 1);
            }
            readNumber(L, t, "lastChoice", p.lastChoice);
            if (field(L, t, "finished")) {
                DelveAct left = actAt(L, lua_gettop(L));
                lua_pop(L, 1);
                int out = -1;
                readNumber(L, t, "finishedChoice", out);
                p.finished = std::pair(std::move(left), out);
            }
            readNumber(L, t, "fresh", p.fresh);
        });
    p.objective = options_.objective;
    p.botBoss = options_.botBoss;
    p.difficulty = difficulty_;
    return p;
}
NOX_NOINLINE DelveDirector::Built DelveDirector::build(const things::ThingDb& things, std::uint32_t seed, const Plan& plan)
{
    // NoX Core's own level, figured to nearest so a seed makes the same level on every path and
    // platform: on the spot under the server's rounding, or on a worker thread (which on Linux
    // inherits its creator's rounding, on Windows starts at nearest).
    const util::RoundingScope nearest(false);
    // A level that can't be built (a layout without a wall for a gate, say) is drawn again.
    Built b;
    map::MapFile map;
    // Up to 9 attempts, each on another seed; the last one's failure is thrown.
    for (std::uint32_t attempt = 0;; ++attempt) {
        b.level = levelFor(plan.act, plan.depth, seed + attempt * 7919u, plan.fork ? &*plan.fork : nullptr);
        if (plan.objective) {
            setObjective(b.level, *plan.objective); // (a test's)
        }
        if (plan.botBoss && b.level.room == DelveRoom::Guardian) {
            setBotBoss(b.level, *plan.botBoss); // (a test's)
        }
        // The difficulty's more monsters: more of the level's own kinds (the script's rule and draw,
        // draws.more: which of them, from 1, copied on).
        if (auto& monsters = b.level.params.monsters; !monsters.empty()) {
            DelveRules::Lease lease;
            const DelveRules& rules = *lease;
            const std::size_t had = monsters.size();
            for (const int i : rules.numbersInt(rules.more, {static_cast<double>(plan.difficulty), static_cast<double>(seed), static_cast<double>(plan.depth),
                                                             static_cast<double>(attempt), static_cast<double>(had)})) {
                monsters.push_back(monsters[static_cast<std::size_t>(std::clamp<int>(i, 1, static_cast<int>(had))) - 1]);
            }
        }
        try {
            map = mapgen::buildArena(things, b.level.params, &b.spots);
            break;
        } catch (const modes::script::GenerationFailed&) {
            throw; // (the generator broke, not a level that can't be built: never drawn again)
        } catch (const std::exception&) {
            if (attempt == 8) {
                throw;
            }
        }
    }
    b.files = mapgen::encodeMapFiles(map, things);
    return b;
}

void DelveDirector::buildAhead(int depth)
{
    // The ways out of the level before it: a gate each (its two rows' rooms, or a guardian's two
    // acts), else the one way on (the fifth row's to the guardian; a run's first level).
    // (The script's rule, run.ahead.)
    const std::vector<int> choices = rules_->numbersInt(rules_->ahead, {static_cast<double>(depth), fork_ ? 1.0 : 0.0});
    for (const int c : choices) {
        if (ahead_.contains({depth, c})) {
            continue;
        }
        // Built on a worker thread from a copy of the plan (build() only reads the thing database).
        ahead_.emplace(std::pair(depth, c),
                       std::async(std::launch::async, [&things = things_, seed = runSeed_, p = plan(depth, c)] { return build(things, seed, p); }));
    }
}

std::function<bool(bool)> DelveDirector::releaseBuilds()
{
    // Every unfinished build's future moved out (a std::async future's end waits for its thread):
    // the caller's call waits for them instead, when it can.
    auto builds = std::make_shared<std::vector<std::future<Built>>>(std::move(dropped_));
    dropped_.clear();
    for (auto& [key, f] : ahead_) {
        builds->push_back(std::move(f));
    }
    ahead_.clear();
    const auto settle = [](std::vector<std::future<Built>>& v) {
        std::erase_if(v, [](const std::future<Built>& f) { return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    };
    settle(*builds);
    return [builds, settle](bool wait) {
        if (wait) {
            builds->clear(); // (each future's end waits for its thread)
        } else {
            settle(*builds);
        }
        return !builds->empty();
    };
}

void DelveDirector::dropAhead()
{
    // What's left of the last level's ahead goes (still being made: left to finish here, the
    // future's end waiting for its thread), and those finished since let go.
    for (auto& [key, f] : ahead_) {
        dropped_.push_back(std::move(f));
    }
    ahead_.clear();
    std::erase_if(dropped_, [](const std::future<Built>& f) { return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
}

void DelveDirector::sendMaps(GameHost& host)
{
    // The run's map (MSG_NOXCPP_MODE_F4) to each noxsdl player that hasn't had this level's yet:
    // at its join and with each level made. In the lobby, the map put away for those that had one.
    const bool running = state_ == State::Running && depth_ > 0;
    std::erase_if(mapSent_, [&](const auto& e) { return !host.players().contains(e.first); });
    // The records table (kRecords), in the lobby as in a run: at a join and as it changes.
    std::erase_if(recordsSent_, [&](const auto& e) { return !host.players().contains(e.first); });
    if (options_.records) {
        // The players' sent versions are kept as the table's version plus one, so a fresh entry (0) always sends.
        const std::uint64_t version = options_.records->version() + 1; // (0 never: none sent yet)
        std::optional<std::vector<std::vector<std::uint8_t>>> messages; // (kRecords, then kRecordsMore past what fits)
        for (const auto& [peer, p] : host.players()) {
            if (!p.ready || recordsSent_[peer] == version) {
                continue;
            }
            if (!messages) {
                std::vector<delvemap::Record> table;
                for (const DelveRecord& r : options_.records->top(data().recordsSent)) {
                    table.push_back({static_cast<std::uint16_t>(r.depth), static_cast<std::uint8_t>(r.act), r.date, r.difficulty, r.party});
                }
                messages = delvemap::recordsMessages(table);
            }
            for (const auto& m : *messages) {
                host.sendToIdentified(peer, m);
            }
            recordsSent_[peer] = version; // (a stock player's too: it's never sent one)
        }
    }
    // Out of a run: each player that was sent a map is told to put it away (serial 0: none held).
    if (!running) {
        for (auto& [peer, serial] : mapSent_) {
            if (serial != 0) {
                host.sendToIdentified(peer, delvemap::endMessage());
                serial = 0;
            }
        }
        return;
    }
    // One act's rooms, row by row, for the message; `taken` is the room picked in it (0xFF: none yet).
    const auto toMap = [](const DelveAct& a, int taken) {
        delvemap::Act m;
        m.biome = a.biome;
        for (std::size_t r = 0; r < m.rows.size(); ++r) {
            for (const DelveRoom room : a.rows[r]) {
                m.rows[r].push_back(static_cast<std::uint8_t>(room));
            }
        }
        m.path = a.path;
        m.taken = taken < 0 ? 0xFF : static_cast<std::uint8_t>(taken);
        return m;
    };
    // One message built for all who need it: the depth, the room in the act, past acts, this act and any fork.
    std::optional<std::vector<std::uint8_t>> message;
    for (const auto& [peer, p] : host.players()) {
        if (!p.ready || mapSent_[peer] == levelSerial_) {
            continue;
        }
        if (!message) {
            delvemap::Run run;
            run.depth = static_cast<std::uint16_t>(depth_);
            run.room = static_cast<std::uint8_t>((depth_ - 1) % DelveAct::kLength);  // 0-based room within the current act
            run.mystery = level_.mystery ? static_cast<std::uint8_t>(*level_.mystery) : 0xFF;
            for (const auto& [a, taken] : history_) {
                run.past.push_back(toMap(a, taken));
            }
            run.act = toMap(act_, -1);
            if (fork_) {
                run.fork = std::array{toMap((*fork_)[0], -1), toMap((*fork_)[1], -1)};
            }
            message = delvemap::runMessage(run);
        }
        host.sendToIdentified(peer, *message);
        mapSent_[peer] = levelSerial_; // (a stock player's too: it's never sent one)
    }
}

int DelveDirector::choose(const World& world) const
{
    // Count the players out by each gate.
    std::array<int, 2> votes{};
    for (const auto& [extent, o] : world.objects()) {
        if (!o.player || o.gone() || o.player->exitGate == 0) {
            continue;
        }
        for (std::size_t i = 0; i < gates_.size() && i < votes.size(); ++i) {
            votes[i] += o.player->exitGate == gates_[i] ? 1 : 0;
        }
    }
    if (votes[0] != votes[1]) {
        return votes[0] > votes[1] ? 0 : 1;
    }
    return firstGate_ >= 0 ? firstGate_ : 0;
}

// The run as the script's words read it (told.luau): {depth, room, mystery (-1: none known),
// gates (whether the level has its two), biome (the level's), difficulty, act, fork (or none)}.
void DelveDirector::pushRun(lua_State* L) const
{
    lua_createtable(L, 0, 8);
    lua_pushnumber(L, depth_);
    lua_setfield(L, -2, "depth");
    lua_pushnumber(L, static_cast<int>(level_.room));
    lua_setfield(L, -2, "room");
    lua_pushnumber(L, level_.mystery ? static_cast<int>(*level_.mystery) : -1);
    lua_setfield(L, -2, "mystery");
    lua_pushboolean(L, !level_.params.gates.empty());
    lua_setfield(L, -2, "gates");
    lua_pushlstring(L, level_.params.biome.data(), level_.params.biome.size());
    lua_setfield(L, -2, "biome");
    lua_pushnumber(L, static_cast<int>(difficulty_));
    lua_setfield(L, -2, "difficulty");
    pushAct(L, act_);
    lua_setfield(L, -2, "act");
    if (fork_) {
        pushFork(L, *fork_);
        lua_setfield(L, -2, "fork");
    }
}


// runLevel's first frame of a level: its loot, the shopkeepers' names, the barrels, the
// difficulty's health, the elites and the objective.
void DelveDirector::beginLevel(GameHost& host, World& world, std::uint32_t frame)
{
    levelSeen_ = levelSerial_;
    levelFrame_ = frame;
    watch_->rearmConditions(levelGroup_, frame); // (the objective, the level's line, the fall: the level's)
    // (The script's hook, level.begun: the gates' words worked out now, a map change's work, so the
    // guardian's fall only sends them.)
    rules_->call(rules_->levelBegun, {static_cast<double>(spots_.gates.size())}, {});
    // The level's loot (the quest's reward tables, giveRewards: a fresh RewardMarker each):
    // every chest filled with two things a stage up (a quest chest's markers go a stage up,
    // questRewardPass); about a third of the monsters given one each, which they drop when
    // they die. (Gathered first: what's made joins the world's objects.)
    // (The level's draws: the script's, draws.luau, one stream asked in this order.)
    const auto run = static_cast<double>(runSeed_);
    const auto serial = static_cast<double>(levelSerial_);
    rules_->callWith(rules_->beginLevel, 0, [&](lua_State* L) {
        lua_pushnumber(L, run);
        lua_pushnumber(L, serial);
        return 2;
    });
    std::vector<std::uint32_t> chests;
    std::vector<std::uint32_t> maybe; // (the monsters a third of which carry loot)
    for (const auto& [extent, o] : world.objects()) {
        if (o.gone() || o.holder != 0) {
            continue;
        }
        const things::ThingDef* def = things_.findThing(o.typeName);
        if (def && def->attribute("INIT").value_or("") == "ChestInit" && o.inventory.empty()) {
            chests.push_back(extent);
        } else if (o.monster && !o.shopkeeper && !(o.flags & ObjFlag::Dead)) {
            maybe.push_back(extent);
        }
    }
    std::vector<std::uint32_t> carriers;
    const std::vector<double> carry = rules_->numbers(rules_->carriers, {static_cast<double>(maybe.size())});
    for (std::size_t i = 0; i < maybe.size() && i < carry.size(); ++i) {
        if (carry[i] != 0) {
            carriers.push_back(maybe[i]);
        }
    }
    Fight loot = host.server().fight();
    const auto stage = static_cast<std::uint32_t>(std::max(1, world.questStage));
    for (const std::uint32_t e : chests) {
        if (ServerObject* c = world.byExtent(e)) {
            giveRewards(loot, *c, stage + 1, 2);
        }
    }
    for (const std::uint32_t e : carriers) {
        if (ServerObject* m = world.byExtent(e)) {
            giveRewards(loot, *m, stage, 1);
        }
    }
    // A shopkeeper a name: the shop's window titles it "NPC:" and its script name's last
    // part (sendStartShop), and the string table has no "NPC:Shopkeeper": one of the
    // script's names.
    std::vector<ServerObject*> keepers;
    for (auto& [extent, o] : world.objects()) {
        if (o.shopkeeper && !o.gone() && o.scriptName.empty()) {
            keepers.push_back(&o);
        }
    }
    if (!keepers.empty()) {
        const std::vector<std::string> names = rules_->strings(rules_->keepers, {static_cast<double>(keepers.size())});
        for (std::size_t i = 0; i < keepers.size() && i < names.size(); ++i) {
            keepers[i]->scriptName = fillText(words().shopkeeper, {{"name", names[i]}});
        }
    }
    // The breakable barrels and crates (the script's; their DIE CreateObjectDie, a stock barrel's:
    // nothing inside), each something to give as it breaks: low loot one in three, a small
    // creature one in seven, a gem one in sixty, else nothing (runBarrels).
    const std::vector<std::string_view>& breakables = data().breakables.views;
    barrels_.clear();
    watchLevel();
    watch_->unwatchAll(barrelList_);
    std::vector<Barrel> found;
    for (const auto& [extent, o] : world.objects()) {
        if (o.gone() || o.holder != 0 || std::ranges::find(breakables, std::string_view(o.typeName)) == breakables.end()) {
            continue;
        }
        found.push_back({ObjectRef{extent}, o.typeName, o.x, o.y, Barrel::Holds::Nothing});
    }
    if (!found.empty()) {
        const std::vector<double> holds = rules_->numbers(rules_->barrels, {static_cast<double>(found.size())});
        for (std::size_t i = 0; i < found.size() && i < holds.size(); ++i) {
            found[i].holds = static_cast<Barrel::Holds>(static_cast<int>(holds[i]));
            if (found[i].holds != Barrel::Holds::Nothing) {
                // (Watched till it breaks: a list, its place kept.)
                watch_->watch(barrelList_, found[i].extent, found[i].type, found[i].x, found[i].y);
                barrels_.emplace(found[i].extent, std::move(found[i]));
            }
        }
    }
    // The difficulty's tougher monsters (the script's rules, toughens and health; kept so:
    // keptHealth_).
    keptHealth_.clear();
    const auto difficulty = static_cast<double>(difficulty_);
    if (rules_->one(rules_->toughens, {difficulty}) != 0) {
        for (auto& [extent, o] : world.objects()) {
            if (o.monster && !o.shopkeeper && !o.gone() && !(o.flags & ObjFlag::Dead) && o.maxHealth != 0) {
                o.maxHealth = o.health = static_cast<std::uint16_t>(rules_->one(rules_->health, {difficulty, static_cast<double>(o.maxHealth)}));
                o.touch();
                keepHealth(o);
            }
        }
    }
    // A fight's elites (the script's rule, elites): from the difficulty's depth (Adventurer's 4)
    // one, another every few levels deeper, at most a few, each of a trait or two and something
    // on it.
    elites_.clear();
    watch_->unwatchAll(eliteList_);
    eliteLines_.clear();
    {
        std::vector<std::uint32_t> foes;
        for (const auto& [extent, o] : world.objects()) {
            if (o.monster && !o.shopkeeper && !o.gone() && !(o.flags & ObjFlag::Dead) && o.holder == 0) {
                foes.push_back(extent);
            }
        }
        // (Which of the foes, from 1, and each one's traits.)
        std::vector<std::pair<std::size_t, std::vector<int>>> picked;
        rules_->callWith(
            rules_->elites, 1,
            [&](lua_State* L) {
                lua_pushnumber(L, static_cast<double>(foes.size()));
                lua_pushnumber(L, difficulty);
                lua_pushnumber(L, depth_);
                lua_pushnumber(L, level_.room == DelveRoom::Fight ? 1.0 : 0.0);
                return 4;
            },
            [&](lua_State* L) {
                const int t = lua_gettop(L);
                for (int i = 1, n = lua_objlen(L, t); i <= n; ++i) {
                    lua_rawgeti(L, t, i);
                    const int e = lua_gettop(L);
                    std::size_t foe = 0;
                    readNumber(L, e, "foe", foe);
                    lua_getfield(L, e, "traits");
                    picked.emplace_back(foe, numbersAt(L, lua_gettop(L)));
                    lua_pop(L, 2);
                }
            });
        for (const auto& [foe, traits] : picked) {
            if (ServerObject* m = foe >= 1 && foe <= foes.size() ? world.byExtent(foes[foe - 1]) : nullptr) {
                makeElite(*m, traits);
                giveRewards(loot, *m, stage, 1);
            }
        }
    }
    // The bonus objective: set up with the first of the party in (startObjective).
    objective_ = {.version = objective_.version + 1};
    endBotBoss(host); // (the last level's champion, carried here by the map change)
}

void DelveDirector::runLevel(GameHost& host)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    levelHost_ = &host;
    watchLevel();
    // The level's first frame: its loot, the shopkeepers' names, the barrels, the difficulty's
    // health, the elites and the objective, set up once.
    if (levelSeen_ != levelSerial_) {
        beginLevel(host, world, frame);
    }
    Party party;
    for (const auto& [peer, p] : host.players()) {
        ServerObject* unit = world.byExtent(p.extent);
        if (unit && unit->player && p.ready && !isBotBoss(p.name)) {
            party.emplace_back(&p, unit);
        }
    }
    frameNow_ = frame;
    watch_->gatherRosters(host); // (the watchers' players this tick: the party's as above)
    runBotBoss(host);
    // The level's conditions over the party (levelGroup_): the objective started with the first
    // one in; the level told, two seconds in with someone there to hear it (and past the server's
    // deepest delve on record, that once a run); the whole party fallen (each at its quest's game
    // over, its lives gone): told, and three seconds on the run's over, back to the lobby with
    // nothing (partyFallen). (One who goes on alone, its game over answered, keeps the run going.)
    levelParty_ = &party;
    watch_->evaluateConditions(levelGroup_, host, frame);
    if (std::exchange(wiped_, false)) {
        return;
    }
    // The champion or guardian: found by its spot, made as strong as its room says.
    findBoss(world);
    runGates(world, frame);
    clearBars();
    runAir(host, frame);
    runShrine();
    // A barrel broken: what it held, where it broke. The elites' regenerating.
    runBarrels(host);
    runElites(host);
    runObjective(host);
    // The campfire: a spell strengthened (levels heal anyway, between them). The altar's
    // blessings, and those taken kept on their players.
    runCampfire(host);
    runAltar(host);
    runBlessings(host);
}

void DelveDirector::partyFallen(const WatchEvent& e)
{
    if (e.kind == WatchEvent::Kind::Begin) {
        rules_->call(rules_->partyFallen, {}, {}); // (told: the script's hook, level.fallen)
        return;
    }
    wipeParty(*levelHost_); // (Held: the lobby's load next)
    wiped_ = true;
}

void DelveDirector::tellLevel(const Party& party)
{
    // (Told: the script's hook, level.told; whether the deepest on record was told passed now.)
    told_ = true;
    const std::vector<std::string> elites = std::exchange(eliteLines_, {});
    const int swiftGold = swiftGold_ > 0 ? std::exchange(swiftGold_, 0) : swiftGold_; // (the last level's Swift)
    const int best = options_.records ? options_.records->best() : 0;
    bool toldBest = false;
    rules_->callWith(
        rules_->levelTold, 1,
        [&](lua_State* L) {
            lua_createtable(L, 0, 9);
            lua_createtable(L, static_cast<int>(party.size()), 0);
            int n = 0;
            for (const auto& [p, unit] : party) {
                if (!unit->gone()) { // (a destroyed unit: never one of a hosted, ready party)
                    lua_pushnumber(L, unit->extent);
                    lua_rawseti(L, -2, ++n);
                }
            }
            lua_setfield(L, -2, "party");
            pushStrings(L, elites);
            lua_setfield(L, -2, "elites");
            lua_pushnumber(L, swiftGold);
            lua_setfield(L, -2, "swiftGold");
            lua_pushnumber(L, static_cast<int>(level_.objective));
            lua_setfield(L, -2, "objective");
            lua_pushnumber(L, objective_.state);
            lua_setfield(L, -2, "objectiveState");
            lua_pushlstring(L, objective_.quarryName.data(), objective_.quarryName.size());
            lua_setfield(L, -2, "quarry");
            lua_pushnumber(L, level_.objectiveSeconds);
            lua_setfield(L, -2, "seconds");
            lua_pushnumber(L, best);
            lua_setfield(L, -2, "best");
            lua_pushboolean(L, toldBest_);
            lua_setfield(L, -2, "toldBest");
            return 1;
        },
        [&](lua_State* L) { toldBest = lua_toboolean(L, -1) != 0; });
    if (toldBest) {
        toldBest_ = true;
    }
}
void DelveDirector::findBoss(World& world)
{
    // (Sought each pass till found: a finder.)
    if (!level_.boss.empty() && boss_ == 0 && !spots_.centre.empty()) {
        if (!bossSought_) {
            bossSought_ = true;
            const mapgen::ArenaSpots::Point at = spots_.centre.front();
            watch_->seek(bossFinder_, level_.boss, at.x, at.y, data().near.boss);
        }
        watch_->evaluateFind(bossFinder_, world);
    }
}

void DelveDirector::bossFound(ServerObject& o)
{
    GameHost& host = *levelHost_;
    World& world = host.server().world();
    boss_ = o.extent;
    // (The script's rules: bossHealth, bossTraits.)
    o.maxHealth = o.health =
        static_cast<std::uint16_t>(rules_->one(rules_->bossHealth, {static_cast<double>(o.maxHealth), static_cast<double>(level_.bossHealthTimes)}));
    o.touch();
    keepHealth(o);
    if (level_.bossDrop > 0) { // (a lone champion's rare thing: it drops it as it dies)
        Fight loot = host.server().fight();
        giveRewards(loot, o, static_cast<std::uint32_t>(std::max(1, world.questStage) + level_.bossDrop), 1);
    }
    // Its traits: an elite room's champion one (two from act 3), a guardian one in act 2 (two
    // from act 3), a lone champion one; the harder difficulties one more each.
    const int traits = static_cast<int>(
        rules_->one(rules_->bossTraits, {static_cast<double>(level_.room), static_cast<double>(act_.number), static_cast<double>(difficulty_)}));
    // (Which, the level's own draw: the script's, draws.bossTraits.)
    makeElite(o, std::span<const int>(rules_->numbersInt(rules_->bossDraw, {static_cast<double>(runSeed_), static_cast<double>(levelSerial_), static_cast<double>(traits)})));
    if (told_) { // (found after the level's line: told now)
        for (const std::string& line : std::exchange(eliteLines_, {})) {
            world.textToAll(widen(line));
        }
    }
}

void DelveDirector::runGates(World& world, std::uint32_t frame)
{
    // The gates: a level's in its act open from the start, a guardian's when it dies.
    const auto openGates = [&] {
        gatesOpen_ = true;
        for (const mapgen::ArenaSpots::Gate& spot : spots_.gates) {
            if (const ServerObject* gate = world.createAt(things_, spot.type, spot.at.x, spot.at.y, frame)) { // (CreateObject's)
                gates_.emplace_back(gate->extent);
                playSoundAt(world, "TeleportIn", spot.at.x, spot.at.y);
            }
        }
    };
    if (level_.room != DelveRoom::Guardian && !gatesOpen_ && !spots_.gates.empty()) {
        openGates();
    }
    if (level_.room == DelveRoom::Guardian && boss_ != 0 && !bossDown_) {
        // (Watched: a list of the one, followed as boss_ changes: a bot's, a stand-in's.)
        if (guardianWatched_ != boss_) {
            watch_->unwatchAll(guardianList_);
            watch_->watch(guardianList_, boss_);
            guardianWatched_ = boss_;
        }
        guardianFell_ = false;
        watch_->evaluateList(guardianList_, world);
        if (guardianFell_) {
            guardianWatched_ = 0;
            bossDown_ = true;
            openGates();
            // (Told: the script's hook, level.guardianFell.)
            rules_->call(rules_->guardianFell, {static_cast<double>(spots_.gates.size())}, {});
        }
    }
    // The gates' signs (found once by their spots) say where each leads to a player near, each
    // at most every 5 s (watched: a zone each).
    if (gateSigns_.size() != spots_.gates.size()) {
        gateSigns_.clear();
        const detail::ThingTypes& types = thingTypes();
        for (const mapgen::ArenaSpots::Gate& spot : spots_.gates) {
            std::uint32_t found = 0;
            for (const auto& [extent, o] : world.objects()) {
                if (types.isSign(o.typeName) && within(o, spot.sign.x, spot.sign.y, data().near.gateSign)) {
                    found = extent;
                }
            }
            gateSigns_.emplace_back(found);
        }
        watchLevel();
        watch_->reset(gateSignGroup_);
        for (std::size_t i = 0; i < gateSigns_.size(); ++i) {
            const std::uint32_t z = watch_->zone(gateSignGroup_, 0, 0, 0, data().near.gateSignSpeaks, 0, Watchers::Mode::AnyCooldown, data().every.gateSign,
                                                 [this, i](const Watchers::Event&) {
                                                     // (Said: the script's hook, level.gateSign.)
                                                     if (levelHost_->server().world().byExtent(gateSigns_[i])) {
                                                         rules_->call(rules_->gateSign, {static_cast<double>(i), static_cast<double>(gateSigns_[i])}, {});
                                                     }
                                                 });
            watch_->retarget(z, gateSigns_[i]);
        }
    }
    watch_->evaluate(gateSignGroup_, *levelHost_, frame);
    // Who took which gate (watched: each of the party once a level, as its exit gate is one of the
    // level's).
    if (exitGates_ != gates_.size()) {
        exitGates_ = gates_.size();
        watch_->exits(exitGroup_, gates_, [this](const WatchEvent& e) {
            const auto i = static_cast<std::size_t>(e.zone);
            if (firstGate_ < 0) {
                firstGate_ = static_cast<int>(i);
            }
            // (Told: the script's hook, level.gateTaken.)
            const std::string& name = e.player->name;
            rules_->callWith(rules_->gateTaken, 0, [&](lua_State* L) {
                lua_pushnumber(L, static_cast<double>(i));
                lua_pushlstring(L, name.data(), name.size());
                return 2;
            });
        });
    }
    watch_->evaluateExits(exitGroup_, *levelHost_);
}

void DelveDirector::clearBars()
{
    // A run's start (and anyone back mid-run): the spell bar emptied of what the player hasn't
    // learnt, so it holds the quest's starting spells alone. Each spell, ability and creature it
    // lacks revoked (MSG_GAUNTLET 0x11 / 0x12 / 0x13, as the quest's death penalty does): the
    // clients take those off their bars (Cli_RevokeSpell ...), quietly.
    // (Watched: each of the party once a run, by name, as it's first not observing.)
    watch_->evaluateFirsts(barsGroup_, *levelHost_);
}

void DelveDirector::clearBar(const WatchEvent& e)
{
    // (Each ability, spell and creature it lacks revoked: the script's hook, level.clearBar; each
    // kind it has, a multiclass player's every kind, PlayerControl::multiclass.)
    const PlayerControl& pc = *e.unit->player;
    const std::string& name = e.player->name;
    const bool fights = pc.usesAbilities();
    const bool casts = pc.castsSpells();
    const bool guide = pc.classId == 2 || pc.multiclass;
    rules_->callWith(
        rules_->clearBar, 0,
        [&](lua_State* L) {
            lua_pushnumber(L, e.unit->extent);
            lua_pushlstring(L, name.data(), name.size());
            lua_pushboolean(L, fights);
            lua_pushboolean(L, casts);
            lua_pushboolean(L, guide);
            return 5;
        });
    revokesOwed_ = true;
}
void DelveDirector::runAir(GameHost& host, std::uint32_t frame)
{
    // The air: the cold keeps everyone in play slowed for good (put back after a death), the heat
    // burns them a little every few seconds; off a cold level, the cold's slow comes off.
    // (Kept on every one of the party: the cold's Slowed for good, or anyone's Slowed for good taken
    // off; the heat's beat, its bites handed out after the pass in the party's order.)
    if (!airKept_) {
        airKept_ = true;
        const mapgen::Biome* here = mapgen::findBiome(level_.params.biome);
        const mapgen::Climate climate = here ? here->climate : mapgen::Climate::Mild;
        Watchers::Keep keep;
        if (climate == mapgen::Climate::Freezing) {
            keep.enchants.emplace_back(Enchant::Slowed, 1);
        } else {
            keep.strip.push_back(Enchant::Slowed);
        }
        if (climate == mapgen::Climate::Scorching) {
            const std::uint32_t heatEvery = data().heatEvery; // (the heat's bite: the script's numbers)
            keep.beat = heatEvery;
            keep.beatFrom = levelFrame_;
            keep.beatAt = heatEvery - 1;
        }
        watch_->unkeepAll(airGroup_);
        watch_->keepEveryone(airGroup_, std::move(keep));
    }
    heat_.clear();
    watch_->evaluateKeep(airGroup_, host, frame, levelSerial_);
    // (The bites: the script's hook, level.heat: a lasting invulnerability off, the fire.)
    if (!heat_.empty()) {
        rules_->callWith(
            rules_->heat, 0,
            [&](lua_State* L) {
                pushNumbers(L, heat_, [](const WatchEvent& e) { return e.unit->extent; });
                return 1;
            });
    }
}

void DelveDirector::runShrine()
{
    // The shrine: a player near, hurt and up, is healed whole, once (watched: a zone, as such a
    // player comes in; one whole already keeps the heal for later).
    if (level_.mystery == DelveMystery::Shrine && !spots_.centre.empty()) {
        if (!shrineWatched_) {
            shrineWatched_ = true;
            watchLevel();
            watch_->reset(shrineGroup_);
            watch_->zone(shrineGroup_, 0, spots_.centre.front().x, spots_.centre.front().y, data().near.shrine, Watchers::Alive | Watchers::NotWhole,
                         Watchers::Mode::EachEnter, 0, [this](const Watchers::Event& e) {
                             if (e.kind != Watchers::Event::Kind::Enter || healed_.contains(e.unit->extent)) {
                                 return;
                             }
                             // (Healed whole, told: the script's hook, level.shrine.)
                             const std::string& name = e.player->name;
                             rules_->callWith(
                                 rules_->shrine, 0,
                                 [&](lua_State* L) {
                                     lua_pushnumber(L, e.unit->extent);
                                     lua_pushlstring(L, name.data(), name.size());
                                     return 2;
                                 });
                         });
        }
        watch_->evaluate(shrineGroup_, *levelHost_, levelHost_->server().clock().frame());
    }
}


std::vector<delvemap::Upgradable> DelveDirector::offer(std::uint32_t player) const
{
    const auto it = offers_.find(player);
    return it != offers_.end() ? it->second : std::vector<delvemap::Upgradable>{};
}

void DelveDirector::runCampfire(GameHost& host)
{
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    // The celebrations: sparks of the four colours rain down onto where it happened, two a burst
    // every few frames for two and a half seconds, falling as they go (MSG_FX_*_SPARKS, which
    // every client draws); the glow made with it put away after.
    std::mt19937 rng(frame * 2654435761u + 77u);
    for (auto it = celebrations_.begin(); it != celebrations_.end();) {
        const std::uint32_t age = frame - it->from;
        if (age > data().every.celebration) {  // (75 frames: 2.5 s)
            if (ServerObject* glow = world.byExtent(it->glow)) {
                glow->pendingDelete = true;
            }
            it = celebrations_.erase(it);
            continue;
        }
        if (age % data().every.sparks == 0) {
            for (int k = 0; k < 2; ++k) {
                // Each spark within 60 either side, starting 70 above and falling a unit a frame.
                const float x = it->x + static_cast<float>(static_cast<int>(rng() % 121) - 60);
                const float y = it->y - 70.0f + static_cast<float>(age) + static_cast<float>(static_cast<int>(rng() % 31) - 15);
                const auto op = static_cast<std::uint8_t>(0x81 + rng() % 4); // (blue, yellow, cyan, violet)
                const auto ix = static_cast<std::uint16_t>(std::max(0.0f, x));
                const auto iy = static_cast<std::uint16_t>(std::max(0.0f, y));
                world.broadcasts().push_back({{op, static_cast<std::uint8_t>(ix), static_cast<std::uint8_t>(ix >> 8), static_cast<std::uint8_t>(iy),
                                               static_cast<std::uint8_t>(iy >> 8)},
                                              false,
                                              0});
            }
        }
        ++it;
    }
    if ((level_.room != DelveRoom::Campfire && level_.mystery != DelveMystery::Fire) || spots_.centre.empty()) {
        offers_.clear();
        return;
    }
    // Who's by the fire (alive, not observing: watched, a zone): an offer made as one comes in or
    // its spells change, put away as it walks off (made again on coming back).
    if (!campfireWatched_) {
        campfireWatched_ = true;
        watchLevel();
        watch_->reset(campfireGroup_);
        watch_->zone(campfireGroup_, 0, spots_.centre.front().x, spots_.centre.front().y, data().near.campfire,
                     Watchers::Alive | Watchers::NotObserving | Watchers::SpellsChanged, Watchers::Mode::EachEnter, 0,
                     [this](const Watchers::Event& e) { atCampfire(e); });
    }
    // An offer open for one who isn't by the fire now put away first, as the old frame looked at
    // each one: the zone's Leave does it for one that walked off (then finds none), and this for
    // one carried here still holding the last fire's (two fires in a row, the party's countdown
    // taking it on: the zone's new, it never came within).
    if (!offers_.empty()) {
        const float cx = spots_.centre.front().x;
        const float cy = spots_.centre.front().y;
        for (const auto& [peer, p] : host.players()) {
            const ServerObject* unit = world.byExtent(p.extent);
            if (!unit || !unit->player || !p.ready || !offers_.contains(unit->extent)) {
                continue;
            }
            if (!(within(*unit, cx, cy, data().near.campfire) && unit->health != 0 && (unit->player->statusFlags & 1u) == 0)) {
                offers_.erase(ObjectRef{unit->extent});
                host.sendToIdentified(peer, delvemap::campfireMessage({}));
            }
        }
    }
    watch_->evaluate(campfireGroup_, host, frame);
}

void DelveDirector::atCampfire(const WatchEvent& e)
{
    // (Offered, told, put away: the script's hook, level.atCampfire; the offers kept here.)
    const double kind = static_cast<int>(e.kind);
    const double peer = e.peer;
    const double unit = e.unit->extent;
    const bool identified = levelHost_->identified(e.peer);
    rules_->callWith(
        rules_->atCampfire, 0,
        [&](lua_State* L) {
            lua_pushnumber(L, kind);
            lua_pushnumber(L, peer);
            lua_pushnumber(L, unit);
            lua_pushboolean(L, identified);
            return 4;
        });
}

bool DelveDirector::upgrade(GameHost& host, std::uint8_t peer, ServerObject& unit, std::uint8_t spell)
{
    // (Up a level, its group with it, told: the script's hook, level.campfireUpgrade, when it's
    // offered.)
    if (!offers_.contains(unit.extent) || !unit.player || unit.gone()) {
        return false;
    }
    const auto who = host.players().find(peer);
    const std::string* name = who != host.players().end() ? &who->second.name : nullptr;
    bool done = false;
    rules_->callWith(
        rules_->campfireUpgrade, 1,
        [&](lua_State* L) {
            lua_pushnumber(L, peer);
            lua_pushnumber(L, unit.extent);
            if (name) {
                lua_pushlstring(L, name->data(), name->size());
            } else {
                lua_pushnil(L);
            }
            lua_pushnumber(L, spell);
            return 4;
        },
        [&](lua_State* L) { done = lua_toboolean(L, -1) != 0; });
    return done;
}

void DelveDirector::celebrate(World& world, const ServerObject& unit, std::uint32_t frame)
{
    // The level-up's sound and glow, and a rain of sparks (runCampfire).
    playSound(world, 0x386, unit);
    Celebration c{unit.x, unit.y, frame, ObjectRef{}};
    if (const ServerObject* glow = world.createAt(things_, "LevelUp", unit.x, unit.y, frame)) {
        c.glow = glow->extent;
    }
    celebrations_.push_back(c);
}
std::vector<delvemap::BlessingOffer> DelveDirector::blessingOffer(std::uint32_t player) const
{
    const auto it = blessOffers_.find(player);
    return it != blessOffers_.end() ? it->second : std::vector<delvemap::BlessingOffer>{};
}

std::array<std::uint8_t, delvemap::kBlessings> DelveDirector::blessings(const std::string& name) const
{
    const auto it = blessed_.find(name);
    return it != blessed_.end() ? it->second.held : std::array<std::uint8_t, delvemap::kBlessings>{};
}

void DelveDirector::runAltar(GameHost& host)
{
    if (level_.room != DelveRoom::Blessing || spots_.centre.empty()) {
        blessOffers_.clear();
        return;
    }
    // Who's by the altar (alive, not observing: watched, a zone): an offer made as one comes in,
    // put away as it walks off (made again on coming back).
    if (!altarWatched_) {
        altarWatched_ = true;
        watchLevel();
        watch_->reset(altarGroup_);
        watch_->zone(altarGroup_, 0, spots_.centre.front().x, spots_.centre.front().y, data().near.altar, Watchers::Alive | Watchers::NotObserving,
                     Watchers::Mode::EachEnter, 0, [this](const Watchers::Event& e) { atAltar(e); });
    }
    watch_->evaluate(altarGroup_, host, host.server().clock().frame());
}

void DelveDirector::atAltar(const WatchEvent& e)
{
    // (Offered, told, put away: the script's hook, level.atAltar; the offers kept here.)
    const ServerObject* unit = e.unit;
    const std::string& name = e.player->name;
    const bool identified = levelHost_->identified(e.peer);
    const bool casts = unit->player->castsSpells();
    const bool fights = unit->player->usesAbilities();
    rules_->callWith(
        rules_->atAltar, 0,
        [&](lua_State* L) {
            lua_pushnumber(L, static_cast<int>(e.kind));
            lua_pushnumber(L, e.peer);
            lua_pushnumber(L, unit->extent);
            lua_pushlstring(L, name.data(), name.size());
            lua_pushboolean(L, identified);
            lua_pushboolean(L, casts);
            lua_pushboolean(L, fights);
            lua_pushnumber(L, runSeed_);
            lua_pushnumber(L, levelSerial_);
            return 9;
        });
}
bool DelveDirector::bless(GameHost& host, std::uint8_t peer, ServerObject& unit, delvemap::Blessing b)
{
    const auto it = blessOffers_.find(unit.extent);
    const auto who = host.players().find(peer);
    if (it == blessOffers_.end() || who == host.players().end() || !unit.player ||
        std::ranges::none_of(it->second, [&](const delvemap::BlessingOffer& o) { return o.blessing == b; })) {
        return false;
    }
    World& world = host.server().world();
    Blessed& held = blessed_[who->second.name];
    ++held.held[static_cast<std::size_t>(b)];
    keepBlessings(who->second.name); // (what's kept on it, its raises worked out again)
    watch_->restat(blessGroup_, who->second.name);
    blessOffers_.erase(it);
    healed_.emplace(unit.extent); // (one blessing at an altar)
    host.sendToIdentified(peer, delvemap::blessingMessage({}));
    celebrate(world, unit, host.server().clock().frame());
    const std::string& name = who->second.name;
    rules_->callWith(
        rules_->blessed, 0,
        [&](lua_State* L) {
            lua_pushlstring(L, name.data(), name.size());
            lua_pushnumber(L, static_cast<int>(b));
            return 2;
        }); // (told: the script's hook, level.blessed)
    watch_->gatherRosters(host); // (between ticks: the players now)
    runBlessings(host); // (taken at once)
    return true;
}

namespace {

/// A blessing's enchantments (those kept for good on the player).
std::vector<int> blessingEnchants(delvemap::Blessing b)
{
    switch (b) {
    case delvemap::Blessing::Swiftness: return {Enchant::Hasted};
    case delvemap::Blessing::Thirst: return {Enchant::Vampirism};
    case delvemap::Blessing::Warding: return {Enchant::ProtectFromFire, Enchant::ProtectFromElectricity, Enchant::ProtectFromPoison};
    default: return {};
    }
}

} // namespace

void DelveDirector::runBlessings(GameHost& host)
{
    // Each blessed player's blessings kept on it (watched: each one's kept, by name): its health,
    // mana and strength raised (again, when a respawn or a new map put its class's numbers back:
    // Stats), its enchantments for good (put back after a death: the keeper's own), Renewal's
    // health and mana every two seconds (Beat), Fortune's gold once a level (Once).
    watchLevel();
    watch_->evaluateKeep(blessGroup_, host, host.server().clock().frame(), levelSerial_);
}

void DelveDirector::keepBlessings(const std::string& name)
{
    // What's kept on a blessed player: its raises (Stats), its blessings' enchantments for good,
    // Renewal's beat, Fortune's once a level.
    using delvemap::Blessing;
    const Blessed& b = blessed_.at(name);
    Watchers::Keep keep;
    keep.stats = true;
    for (std::size_t i = 0; i < delvemap::kBlessings; ++i) {
        if (b.held[i] != 0) {
            for (const int e : blessingEnchants(static_cast<Blessing>(i))) {
                keep.enchants.emplace_back(e, 3);
            }
        }
    }
    if (b.held[static_cast<std::size_t>(Blessing::Renewal)] > 0) {
        keep.beat = data().every.renewal; // (every 60 frames, 2 s: a 40th of each)
    }
    keep.once = b.held[static_cast<std::size_t>(Blessing::Fortune)] > 0;
    watch_->keep(blessGroup_, name, std::move(keep));
}

void DelveDirector::blessingKept(const WatchEvent& e)
{
    // (Raised, renewed, Fortune paid: the script's hook, level.blessingKept.)
    if (!blessed_.contains(e.player->name)) {
        return;
    }
    const std::string& name = e.player->name;
    rules_->callWith(
        rules_->blessingKept, 0,
        [&](lua_State* L) {
            const ServerObject& u = *e.unit;
            lua_pushnumber(L, static_cast<int>(e.kind));
            lua_pushnumber(L, u.extent);
            lua_pushlstring(L, name.data(), name.size());
            lua_pushnumber(L, u.health);
            lua_pushnumber(L, u.maxHealth);
            lua_pushnumber(L, u.player->mana);
            lua_pushnumber(L, u.player->maxMana);
            lua_pushnumber(L, u.player->strength);
            return 8;
        });
}
void DelveDirector::unbless(GameHost& host)
{
    // The blessings gone (the run over): each still raised put back, its enchantments off (the
    // script's hook, level.unbless, for each one in with blessings, in the host's order).
    World& world = host.server().world();
    std::vector<std::pair<std::uint32_t, const std::string*>> players;
    for (const auto& [peer, p] : host.players()) {
        const ServerObject* unit = world.byExtent(p.extent);
        if (blessed_.contains(p.name) && unit && unit->player && !unit->gone()) {
            players.emplace_back(unit->extent, &p.name);
        }
    }
    rules_->callWith(
        rules_->unbless, 0,
        [&](lua_State* L) {
            lua_createtable(L, static_cast<int>(players.size()), 0);
            for (std::size_t i = 0; i < players.size(); ++i) {
                lua_createtable(L, 0, 2);
                lua_pushnumber(L, players[i].first);
                lua_setfield(L, -2, "unit");
                lua_pushlstring(L, players[i].second->data(), players[i].second->size());
                lua_setfield(L, -2, "name");
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            return 1;
        });
    blessed_.clear();
    blessOffers_.clear();
    if (watch_ && blessGroup_ >= 0) {
        watch_->unkeepAll(blessGroup_);
    }
}
void DelveDirector::makeElite(ServerObject& unit, std::span<const int> traits)
{
    if (traits.empty() || !unit.monster) {
        return;
    }
    Elite e{ObjectRef{unit.extent}, {}};
    for (const int t : traits) {
        e.traits.push_back(static_cast<Trait>(t));
    }
    // (Made so, and its line: the script's hook, level.makeElite: its traits' enchantments, Tough's
    // health; that health kept here, keptHealth_.)
    std::string line;
    double raised = 0;
    const std::string& type = unit.typeName;
    rules_->callWith(
        rules_->makeElite, 2,
        [&](lua_State* L) {
            lua_pushnumber(L, unit.extent);
            lua_pushlstring(L, type.data(), type.size());
            lua_pushnumber(L, unit.maxHealth);
            pushNumbers(L, traits);
            return 4;
        },
        [&](lua_State* L) {
            line = stringOf(L, -2);
            raised = lua_tonumber(L, -1);
        });
    if (raised > 0) {
        keepHealth(unit);
    }
    eliteLines_.push_back(std::move(line)); // (told with the level's line)
    watch_->watch(eliteList_, e.extent); // (watched till it's gone or dead)
    elites_.push_back(std::move(e));
}
void DelveDirector::keepHealth(const ServerObject& o)
{
    keptHealth_[ObjectRef{o.extent}] = o.maxHealth;
}

void DelveDirector::runElites(GameHost& host)
{
    // The regenerating: a fiftieth of its health back every second while it lives.
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    watch_->evaluateList(eliteList_, world); // (one gone or dead dropped)
    watch_->evaluateTimer(eliteTimer_, frame); // (every second: regenerateElites)
}

void DelveDirector::regenerateElites()
{
    World& world = levelHost_->server().world();
    // Health the director raised, put back where the quest's rescale took it (an unhurt one
    // whole again).
    for (auto it = keptHealth_.begin(); it != keptHealth_.end();) {
        ServerObject* o = world.byExtent(it->first);
        if (!o || !o->monster || (o->flags & ObjFlag::Dead) || o->gone()) {
            it = keptHealth_.erase(it);
            continue;
        }
        if (o->maxHealth != it->second) {
            if (o->health == o->maxHealth) {
                o->health = it->second;
            }
            o->maxHealth = it->second;
            o->health = std::min(o->health, o->maxHealth);
            o->touch();
        }
        ++it;
    }
    for (const Elite& e : elites_) {
        ServerObject* o = world.byExtent(e.extent);
        if (o && std::ranges::find(e.traits, Trait::Regenerating) != e.traits.end() && o->health < o->maxHealth) {
            // (What comes back: the script's rule, regenerate.)
            const int back = static_cast<int>(rules_->one(rules_->regenerate, {static_cast<double>(o->maxHealth)}));
            o->health = static_cast<std::uint16_t>(std::min<int>(o->maxHealth, o->health + back));
            o->touch();
        }
    }
}

std::string DelveDirector::objectiveText() const
{
    // (The script's words, told.objectiveText: the kind's, the Hunt's quarry, Swift's time.)
    const std::string& quarry = objective_.quarryName;
    return rules_->text(rules_->objectiveText, [&](lua_State* L) {
        lua_pushnumber(L, static_cast<int>(level_.objective));
        lua_pushlstring(L, quarry.data(), quarry.size());
        lua_pushnumber(L, level_.objectiveSeconds);
        return 3;
    });
}
delvemap::Objective DelveDirector::objective() const
{
    if (state_ != State::Running || level_.objective == DelveObjective::None || !objective_.started) {
        return {};
    }
    delvemap::Objective o;
    o.kind = static_cast<std::uint8_t>(level_.objective);
    o.state = objective_.state;
    o.done = static_cast<std::uint16_t>(objective_.done);
    o.of = static_cast<std::uint16_t>(objective_.of);
    if (level_.objective == DelveObjective::Swift && objective_.state == 0) {
        o.secondsLeft = static_cast<std::uint16_t>(frameNow_ < objective_.deadline ? (objective_.deadline - frameNow_) / 30 : 0);
    }
    o.text = level_.objective == DelveObjective::Swift ? words().objSwiftShort : objectiveText();
    return o;
}

void DelveDirector::startObjective(World& world, const ServerObject& someone)
{
    objective_.started = true;
    ++objective_.version;
    const auto foe = [&](const ServerObject& o) {
        return o.monster && !o.shopkeeper && !o.gone() && !(o.flags & ObjFlag::Dead) && o.holder == 0 && isEnemy(world, o, someone);
    };
    switch (level_.objective) {
    case DelveObjective::None:
        break;
    case DelveObjective::Purge:
        for (const auto& [extent, o] : world.objects()) {
            if (foe(o)) {
                objective_.targets.emplace_back(extent);
            }
        }
        objective_.of = static_cast<int>(objective_.targets.size());
        break;
    case DelveObjective::Hunt: {
        // One of the level's foes (not an elite already) made an elite of two traits: the quarry.
        std::vector<std::uint32_t> can;
        for (const auto& [extent, o] : world.objects()) {
            if (foe(o) && std::ranges::none_of(elites_, [&](const Elite& e) { return e.extent == extent; })) {
                can.push_back(extent);
            }
        }
        if (can.empty()) {
            break;
        }
        // (Which, and its traits: the level's own draw, the script's draws.hunt.)
        std::size_t which = 0;
        std::vector<int> traits;
        rules_->callWith(
            rules_->hunt, 2,
            [&](lua_State* L) {
                return DelveRules::pushAll(L, {static_cast<double>(runSeed_), static_cast<double>(levelSerial_), static_cast<double>(can.size())});
            },
            [&](lua_State* L) {
                which = static_cast<std::size_t>(lua_tonumber(L, -2));
                traits = numbersAt(L, lua_gettop(L));
            });
        ServerObject* q = world.byExtent(can[std::clamp<std::size_t>(which, 1, can.size()) - 1]);
        objective_.quarry = q->extent;
        objective_.quarryName = spacedName(q->typeName);
        objective_.of = 1;
        makeElite(*q, traits);
        break;
    }
    case DelveObjective::Seals:
        for (const auto& [extent, o] : world.objects()) {
            if (o.typeName == kDelveSeal && !o.gone()) {
                objective_.targets.emplace_back(extent);
            }
        }
        objective_.of = static_cast<int>(objective_.targets.size());
        // (Who's by each, watched: a zone each, the last of the party in play within.)
        watchLevel();
        watch_->reset(sealGroup_);
        for (const std::uint32_t seal : objective_.targets) {
            const std::uint32_t z = watch_->zone(sealGroup_, 0, 0, 0, data().near.seal, Watchers::Alive | Watchers::NotObserving, Watchers::Mode::Any, 0,
                                                 [this, seal](const Watchers::Event& e) { sealBy_[seal] = e.unit; });
            watch_->retarget(z, seal);
        }
        break;
    case DelveObjective::Swift:
        objective_.deadline = frameNow_ + static_cast<std::uint32_t>(level_.objectiveSeconds) * 30u;
        watch_->due(swiftTimer_, objective_.deadline);
        break;
    }
}

void DelveDirector::runObjective(GameHost& host)
{
    // objective_.state: 0 under way, 1 done, 2 lost (Swift's deadline passed).
    if (level_.objective == DelveObjective::None || !objective_.started || objective_.state != 0) {
        return;
    }
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    // Party in play: ready, not observing (statusFlags bit 0), alive.
    inPlay_.clear();
    for (const auto& [peer, p] : host.players()) {
        ServerObject* unit = world.byExtent(p.extent);
        if (unit && unit->player && p.ready && (unit->player->statusFlags & 1u) == 0 && unit->health != 0) {
            inPlay_.push_back(unit);
        }
    }
    const auto progress = [this](int done) { objectiveProgress(done); };
    objectiveDone_ = false;
    bool& complete = objectiveDone_;
    switch (level_.objective) {
    case DelveObjective::None:
        break;
    case DelveObjective::Purge:
        watch_->evaluateTimer(purgeTimer_, frame); // (counted twice a second: countPurge)
        break;
    case DelveObjective::Hunt:
        complete = objective_.quarry == 0 || !objectiveAlive(world, objective_.quarry);
        progress(complete ? 1 : 0);
        break;
    case DelveObjective::Seals:
        // A seal touched (one of the party in play beside it): broken, turned off (MSG_DISABLE_OBJECT:
        // a VISIBLE_ENABLE pool off draws empty, its orb gone; an immobile thing deleted or made
        // after the map's load never reaches a client).
        sealBy_.clear();
        watch_->evaluate(sealGroup_, host, frame);
        for (auto it = objective_.targets.begin(); it != objective_.targets.end();) {
            const ServerObject* seal = world.byExtent(*it);
            const auto touched = sealBy_.find(*it); // (within 45 of a seal touches it)
            const ServerObject* by = touched != sealBy_.end() ? touched->second : nullptr;
            if (seal && !by) {
                ++it;
                continue;
            }
            if (seal) {
                // (Switched off, its sound, told: the script's hook, level.sealBroken.)
                const int done = objective_.done + 1;
                progress(done);
                const auto who = std::ranges::find_if(host.players(), [&](const auto& e) { return e.second.extent == by->extent; });
                const std::string* name = who != host.players().end() ? &who->second.name : nullptr;
                rules_->callWith(
                    rules_->sealBroken, 0,
                    [&](lua_State* L) {
                        lua_pushnumber(L, seal->extent);
                        lua_pushnumber(L, seal->x);
                        lua_pushnumber(L, seal->y);
                        lua_pushnumber(L, done);
                        lua_pushnumber(L, objective_.of);
                        if (name) {
                            lua_pushlstring(L, name->data(), name->size());
                        } else {
                            lua_pushnil(L);
                        }
                        return 6;
                    });
            } else {
                progress(objective_.done + 1); // (gone some other way: counted)
            }
            it = objective_.targets.erase(it);
        }
        complete = objective_.of > 0 && objective_.targets.empty();
        break;
    case DelveObjective::Swift: // (done as the party leaves: prepare, swiftGold_)
        watch_->evaluateTimer(swiftTimer_, frame); // (its deadline passed: lost)
        break;
    }
    if (!complete) {
        return;
    }
    objective_.state = 1;
    ++objective_.version;
    // The reward, a find a stage up at each one's feet, and told: the script's hook,
    // level.objectiveDone.
    const std::string& quarry = objective_.quarryName;
    rules_->callWith(
        rules_->objectiveDone, 0,
        [&](lua_State* L) {
            pushNumbers(L, inPlay_, [](const ServerObject* u) { return u->extent; });
            lua_pushnumber(L, static_cast<int>(level_.objective));
            lua_pushlstring(L, quarry.data(), quarry.size());
            lua_pushnumber(L, level_.objectiveSeconds);
            return 4;
        });
}

// A target still counts while alive and (with anyone in play) an enemy of the party.
bool DelveDirector::objectiveAlive(World& world, std::uint32_t extent) const
{
    const ServerObject* o = world.byExtent(extent);
    return o && !o->gone() && !(o->flags & ObjFlag::Dead) && o->health != 0 && (inPlay_.empty() || isEnemy(world, *o, *inPlay_.front()));
}

// Bumps the version on a change, so sendObjective sends the new count.
void DelveDirector::objectiveProgress(int done)
{
    if (done != objective_.done) {
        objective_.done = done;
        ++objective_.version;
    }
}

// Purge's count (twice a second: purgeTimer_): its targets left, half told, done as none's left.
void DelveDirector::countPurge()
{
    World& world = levelHost_->server().world();
    const auto left = std::ranges::count_if(objective_.targets, [&](std::uint32_t e) { return objectiveAlive(world, e); });
    objectiveProgress(objective_.of - static_cast<int>(left));
    if (!objective_.halfTold && objective_.of >= 4 && objective_.done * 2 >= objective_.of && left > 0) {
        objective_.halfTold = true;
        rules_->call(rules_->purgeHalf, {static_cast<double>(left)}, {}); // (told: the script's hook, level.purgeHalf)
    }
    objectiveDone_ = left == 0;
}

void DelveDirector::sendObjective(GameHost& host)
{
    // (The objective, its words with it, made only when it's to be sent: none at all is
    // objective()'s kind 0.)
    const bool none = state_ != State::Running || level_.objective == DelveObjective::None || !objective_.started;
    std::optional<delvemap::Objective> o;
    for (const auto& [peer, p] : host.players()) {
        const auto sent = objectiveSent_.find(peer);
        // (A fresh one each version; none at all to one that never had one.)
        if (!host.identified(peer) || (sent != objectiveSent_.end() && sent->second == objective_.version) ||
            (sent == objectiveSent_.end() && none)) {
            continue;
        }
        if (!o) {
            o = objective();
        }
        objectiveSent_[peer] = objective_.version;
        host.sendToIdentified(peer, delvemap::objectiveMessage(*o));
    }
}

// A bot guardian: its client with its own lobby and game sockets, its name and class, and how far
// it has got (made, set in the arena, fallen, leaving).
struct DelveDirector::BotBoss {
    std::unique_ptr<bot::NetBot> bot;
    net::UdpSocket lobby = net::UdpSocket::open();
    net::UdpSocket game = net::UdpSocket::open();
    std::string name;
    std::uint8_t playerClass = 0;
    std::uint32_t since = 0;  ///< the frame it was made
    bool set = false;         ///< in the arena, ready
    std::uint32_t fellAt = 0; ///< the frame it fell (0: up)
    std::uint8_t team = 0;
    bool leaving = false;
    std::uint32_t leftMs = 0;
};

DelveDirector::~DelveDirector() = default;

namespace {

// A steady clock in milliseconds, for the bot's client.
std::uint32_t steadyMs()
{
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

std::string DelveDirector::botBossName() const
{
    return botBoss_ && !botBoss_->leaving ? botBoss_->name : std::string();
}

bool DelveDirector::isBotBoss(const std::string& name) const
{
    return botBoss_ && name == botBoss_->name;
}

// A champion level's guardian is a bot player: made and joined to this server over the network on
// the level's first frame with the party in, armed and strengthened once it is in, its party
// members kept off its team, and ended a few seconds after it falls (or replaced by the level's
// monster guardian when it can't join).
void DelveDirector::runBotBoss(GameHost& host)
{
    if (!level_.botBoss || spots_.centre.empty()) {
        return;
    }
    World& world = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    const auto [cx, cy] = std::pair(spots_.centre.front().x, spots_.centre.front().y);
    // The monster guardian in its place (no port to join, or the bot didn't come): found and
    // made strong as any guardian (runLevel).
    const auto standIn = [&](std::string_view why) {
        world.createAt(things_, level_.boss, cx, cy, frame);
        if (!why.empty()) {
            world.textToAll(widen(std::string(why)));
        }
    };
    if (botBossSerial_ != levelSerial_) {
        // The level's first frame with the party in: the bot made.
        const bool someone = std::ranges::any_of(host.players(), [&](const auto& e) { return e.second.ready && !isBotBoss(e.second.name); });
        if (!someone) {
            return;
        }
        botBossSerial_ = levelSerial_;
        endBotBoss(host);
        if (serverPort_ == 0) {
            standIn("");
            return;
        }
        auto b = std::make_unique<BotBoss>();
        b->playerClass = *level_.botBoss;
        std::set<std::string> taken;
        for (const auto& [peer, p] : host.players()) {
            taken.insert(p.name);
        }
        // Drawn by the run and the level: the champion's name (none of those in: the champions'
        // names, the script's, a bot boss's with no "[BOT] ": a foe, not one of the players' bots),
        // its made-up 22-digit serial for its join, its brain's seed (the script's draw,
        // draws.champion).
        std::string serial;
        std::uint32_t brainSeed = 0;
        rules_->callWith(
            rules_->championDraw, 3,
            [&](lua_State* L) {
                lua_pushnumber(L, runSeed_);
                lua_pushnumber(L, levelSerial_);
                lua_createtable(L, 0, static_cast<int>(taken.size()));
                for (const std::string& name : taken) {
                    lua_pushboolean(L, 1);
                    lua_setfield(L, -2, name.c_str());
                }
                return 3;
            },
            [&](lua_State* L) {
                b->name = stringOf(L, -3);
                serial = stringOf(L, -2);
                brainSeed = static_cast<std::uint32_t>(lua_tonumber(L, -1));
            });
        b->since = frame;
        if (!botKinds_) {
            botKinds_ = std::make_unique<bot::ItemKinds>(things_, host.server().modifiers());
        }
        client::PlayerProfile profile;
        profile.name = b->name;
        profile.playerClass = b->playerClass;
        profile.quest = true; // (CLIENT_ACCEPT's quest bit: the game's a quest)
        profile.serial = serial;
        // (Whether it plays hard: the script's rule, champion.)
        double champion[5] = {};
        rules_->call(rules_->champion, {static_cast<double>(act_.number), static_cast<double>(difficulty_), 0.0, 0.0}, champion);
        const bot::Skill skill = bot::skillFor(champion[4] != 0 ? bot::Tier::Hard : bot::Tier::Medium);
        auto brain = bot::makeBrain(b->playerClass, skill, brainSeed);
        b->bot = std::make_unique<bot::NetBot>(profile, things_, *botKinds_, options_.mapsDir, options_.mapsDir, std::move(brain));
        GameHost::AccessSettings a = host.accessSettings();
        a.allowed.push_back(b->name); // (the locked game lets it in)
        host.setAccessSettings(a);
        b->bot->client().start(steadyMs());
        botBoss_ = std::move(b);
        return;
    }
    // Once made: waited for, set in the arena once, then watched till it falls.
    if (!botBoss_ || botBoss_->leaving) {
        return;
    }
    BotBoss& b = *botBoss_;
    ServerObject* unit = nullptr;
    for (const auto& [peer, p] : host.players()) {
        if (p.name == b.name && p.ready) {
            unit = world.byExtent(p.extent);
        }
    }
    if (!unit || !unit->player) {
        if (b.set) {
            endBotBoss(host); // (gone after it was set: its team with it, so none joins it)
            return;
        }
        if (!bossDown_ && data().every.championWait < frame - b.since) { // (20 s and not in)
            endBotBoss(host);
            standIn(fillText(words().championLate, {{"creature", spacedName(level_.boss)}}));
        }
        return;
    }
    PlayerControl& pc = *unit->player;
    if (!b.set) {
        b.set = true;
        boss_ = unit->extent; // (the guardian: the gates open as it falls)
        Fight fight = host.server().fight();
        if (pc.statusFlags & 1u) {
            leaveObserver(fight, *unit); // (it joined watching, as anyone joining a quest)
        }
        // A team of its own: the party's foe (isEnemy: friends share a team), every client told.
        if (ServerTeam* t = createTeam(world, 0)) {
            t->color = 1;
            setTeamName(*t, widen(words().championTeam), false);
            world.broadcasts().push_back({teamMessage(world, *t), true, 0});
            std::minstd_rand trng(frame);
            addTeamMember(world, t->id, *unit, true, false, trng, frame);
            b.team = t->id;
        }
        setPosition(world, *unit, cx, cy, frame);
        // Gear for its class (the script's), as worn.
        const delve_data::ChampionClass& kit = data().championClasses[std::min<std::size_t>(b.playerClass, 2)];
        Items items = host.server().items();
        for (const std::string_view type : kit.gear) {
            if (type.empty() || !things_.findThing(type)) {
                continue;
            }
            if (ServerObject* it = items.give(*unit, type, {0xFF, 0xFF, 0xFF, 0xFF}, true); it && !(it->flags & ObjFlag::Equipped)) {
                if (it->classes & things::ObjectClass::Weapon) {
                    items.equipWeapon(*unit, *it, true);
                } else if (it->classes & things::ObjectClass::Armor) {
                    items.equipArmor(*unit, *it, true);
                }
            }
        }
        // Its spells (the script's; a quest's levels: 2, 3 from act 3) or a warrior's abilities;
        // its health and mana (the script's rule, champion).
        double champion[5] = {};
        rules_->call(rules_->champion, {static_cast<double>(act_.number), static_cast<double>(difficulty_), 0.0, 0.0}, champion);
        const int level = static_cast<int>(champion[2]);
        const bool late = champion[3] != 0;
        if (b.playerClass == 0) {
            for (std::size_t a = 1; a < 6; ++a) {  // abilities 1-5 at level 5
                pc.abilityLevel(a) = 5;
                world.broadcasts().push_back({{0xCD, static_cast<std::uint8_t>(a), 5}, true, unit->extent}); // (MSG_REPORT_ABILITY_AWARD)
            }
        } else {
            for (const std::uint32_t s : kit.spells) {
                if (s != data().championLateSpell || late) { // (the Death Ray: the late acts')
                    grantSpell(world, things_, *unit, static_cast<int>(s), false, false, level);
                }
            }
        }
        // A guardian's health (one and the act's number times), a caster's mana three times.
        rules_->call(rules_->champion,
                     {static_cast<double>(act_.number), static_cast<double>(difficulty_), static_cast<double>(unit->maxHealth), static_cast<double>(pc.maxMana)},
                     champion);
        unit->maxHealth = unit->health = static_cast<std::uint16_t>(champion[0]);
        unit->touch();
        pc.healthSent = -1;
        if (pc.maxMana > 0) {
            pc.maxMana = pc.mana = static_cast<std::uint16_t>(champion[1]);
            pc.manaSent = -1;
        }
        world.textToAll(widen(fillText(words().championIs, {{"name", b.name}, {"word", kit.word}})));
        return;
    }
    // A party member put on its team (one back mid-fight joins the emptiest team, joinTeam: the
    // champion's): moved to the party's (the quest's, the other team), not joinTeam again.
    ServerTeam* partyTeam = nullptr;
    // The party's team: the first that isn't the champion's.
    for (std::uint8_t id = 1; id <= 16 && !partyTeam; ++id) {
        if (id != b.team) {
            partyTeam = teamById(world, id);
        }
    }
    for (const auto& [peer, p] : host.players()) {
        ServerObject* o = world.byExtent(p.extent);
        if (o && o->player && o != unit && b.team != 0 && o->team == b.team && partyTeam) {
            std::minstd_rand trng(frame);
            moveTeamMember(world, *o, *partyTeam, false, trng, frame);
        }
    }
    // Fallen: kept down (a quest's game over stops its respawn), gone three seconds on.
    if (unit->health == 0 || (unit->flags & ObjFlag::Dead)) {
        if (b.fellAt == 0) {
            b.fellAt = frame;
            if (pc.questGameOverAt == 0) {
                pc.questGameOverAt = frame;
            }
        }
    }
    if (b.fellAt != 0 && data().every.championFallen < frame - b.fellAt) {  // (90 frames: 3 s)
        endBotBoss(host);
    }
}

void DelveDirector::pumpBotBoss()
{
    if (!botBoss_) {
        return;
    }
    BotBoss& b = *botBoss_;
    const std::uint32_t now = steadyMs();
    // The server on loopback (127.0.0.1), at its own port.
    const net::Endpoint server{0x7F000001, serverPort_};
    auto& c = b.bot->client();
    std::uint8_t buffer[2048];
    try {
        // What the server sent the bot on either socket, then the bot's turn and what it sends back.
        while (const auto got = b.lobby.receive(buffer)) {
            if (got->from == server) {
                c.receiveLobby(std::span(buffer, got->size));
            }
        }
        while (const auto got = b.game.receive(buffer)) {
            if (got->from == server) {
                c.receive(std::span(buffer, got->size), now);
            }
        }
        b.bot->tick(now);
        for (const auto& d : c.takeOutgoing()) {
            (d.lobby ? b.lobby : b.game).sendTo(server, d.bytes);
        }
    } catch (const std::exception&) {
        // (a send refused: the next tick tries again; a bot that never gets in is replaced)
    }
    c.takeEvents();
    using Stage = client::HeadlessClient::Stage;
    // Let go once its client has closed or failed, or 5 s after it was told to quit.
    if (b.leaving && (c.stage() == Stage::Closed || c.stage() == Stage::Failed || 5000u < now - b.leftMs)) {
        botBoss_.reset();
    }
}

void DelveDirector::endBotBoss(GameHost& host)
{
    if (!botBoss_ || botBoss_->leaving) {
        return;
    }
    BotBoss& b = *botBoss_;
    b.leaving = true;
    b.leftMs = steadyMs();
    b.bot->client().quit();
    GameHost::AccessSettings a = host.accessSettings();
    std::erase(a.allowed, b.name);
    host.setAccessSettings(a);
    if (b.team != 0) {
        if (ServerTeam* t = teamById(host.server().world(), b.team)) {
            removeTeam(host.server().world(), *t, true);
        }
        b.team = 0;
    }
}

void DelveDirector::runBarrels(GameHost& host)
{
    World& world = host.server().world();
    // (Each watched: a list, its place kept, broken as it's gone, deleted, destroyed or another
    // thing's number.)
    broken_.clear();
    watch_->evaluateList(barrelList_, world);
    // What each held, where it broke: the script's hook, level.barrels (its draws the server's
    // own stream, in the order they broke).
    std::vector<Barrel> found;
    for (const WatchEvent& e : broken_) {
        const auto it = barrels_.find(e.id);
        if (it == barrels_.end()) {
            continue;
        }
        Barrel b = std::move(it->second);
        barrels_.erase(it);
        b.x = e.x; // (where it broke)
        b.y = e.y;
        found.push_back(std::move(b));
    }
    if (found.empty()) {
        return;
    }
    rules_->callWith(
        rules_->barrelsBroken, 0,
        [&](lua_State* L) {
            lua_createtable(L, static_cast<int>(found.size()), 0);
            for (std::size_t i = 0; i < found.size(); ++i) {
                lua_createtable(L, 0, 3);
                lua_pushnumber(L, static_cast<int>(found[i].holds));
                lua_setfield(L, -2, "holds");
                lua_pushnumber(L, found[i].x);
                lua_setfield(L, -2, "x");
                lua_pushnumber(L, found[i].y);
                lua_setfield(L, -2, "y");
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            return 1;
        });
}
void DelveDirector::fileRecord()
{
    if (options_.records && runDepth_ > 0 && !runParty_.empty()) {
        // (What it holds: the script's rule, record: its act and the difficulty's word.)
        int act = 0;
        std::string difficulty;
        rules_->callWith(
            rules_->record, 2, [&](lua_State* L) { return DelveRules::pushAll(L, {static_cast<double>(runDepth_), static_cast<double>(difficulty_)}); },
            [&](lua_State* L) {
                act = static_cast<int>(lua_tonumber(L, -2));
                difficulty = stringOf(L, -1);
            });
        options_.records->add({.depth = runDepth_, .act = act, .date = detail::recordDate(), .difficulty = std::move(difficulty), .party = runParty_});
    }
    runDepth_ = 0;
    runParty_.clear();
}

void DelveDirector::wipeParty(GameHost& host)
{
    // (Its things, gold, game over, spells, abilities and guide: the script's hook, level.wipe, for
    // each one in, in the host's order; its class's first spells, the script's data, firstSpells.)
    World& world = host.server().world();
    unbless(host);
    const auto& firstSpells = data().firstSpells;
    const std::vector<std::uint32_t> none;
    std::vector<std::tuple<std::uint32_t, const std::string*, const std::vector<std::uint32_t>*>> players;
    for (const auto& [peer, p] : host.players()) {
        const ServerObject* unit = world.byExtent(p.extent);
        if (!unit || !unit->player || unit->gone()) {
            continue;
        }
        const std::uint8_t cls = unit->player->classId;
        players.emplace_back(unit->extent, &p.name, cls < firstSpells.size() ? &firstSpells[cls] : &none);
    }
    const std::string lobby = options_.lobbyName + ".map";
    rules_->callWith(
        rules_->wipe, 0,
        [&](lua_State* L) {
            lua_createtable(L, static_cast<int>(players.size()), 0);
            for (std::size_t i = 0; i < players.size(); ++i) {
                const auto& [unit, name, first] = players[i];
                lua_createtable(L, 0, 3);
                lua_pushnumber(L, unit);
                lua_setfield(L, -2, "unit");
                lua_pushlstring(L, name->data(), name->size());
                lua_setfield(L, -2, "name");
                pushNumbers(L, *first);
                lua_setfield(L, -2, "first");
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            lua_pushlstring(L, lobby.data(), lobby.size());
            return 2;
        });
    revokesOwed_ = true;
    for (const auto& [unit, name, first] : players) {
        watch_->forget(barsGroup_, *name); // (its bars cleared again as the next run begins)
    }
}
bool DelveDirector::onChatRun(GameHost& host, std::uint8_t peer, std::u16string_view text)
{
    const auto who = host.players().find(peer);
    if (who == host.players().end()) {
        return false;
    }
    World& world = host.server().world();
    ServerObject* unit = world.byExtent(who->second.extent);
    // What the line asks for (the script's rule, commands.chat): the line as UTF-8, each of the
    // client's characters its own (a lone surrogate too: none of them comes out as ASCII).
    std::string line;
    for (const char16_t c : text) {
        if (c < 0x80) {
            line += static_cast<char>(c);
        } else if (c < 0x800) {
            line += static_cast<char>(0xC0 | (c >> 6));
            line += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            line += static_cast<char>(0xE0 | (c >> 12));
            line += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    int asks = 0;
    double arg = 0;
    rules_->callWith(
        rules_->chat, 2,
        [&](lua_State* L) {
            lua_pushlstring(L, line.data(), line.size());
            return 1;
        },
        [&](lua_State* L) {
            asks = static_cast<int>(lua_tonumber(L, -2));
            arg = lua_tonumber(L, -1);
        });
    enum Asks { None, Records, Difficulty, Blessings, Number };
    // "?record" (or ?records, ?deepest): the deepest delves, to whoever asked.
    if (asks == Records) {
        if (unit && unit->player && !unit->gone()) {
            rules_->call(rules_->records, {static_cast<double>(unit->extent)}, {});
        }
        return true;
    }
    if (asks == Difficulty) {
        // (A difficulty asked for, or -1: none asked, or none such: told it. The reply, and the
        // difficulty set, the script's: commands.difficulty; to the one who asked, or everyone.)
        int everyone = 0;
        std::string reply;
        rules_->callWith(
            rules_->difficultyAsked, 2,
            [&](lua_State* L) {
                lua_pushlstring(L, who->second.name.data(), who->second.name.size());
                lua_pushnumber(L, arg < 0 ? -1 : static_cast<int>(arg));
                return 2;
            },
            [&](lua_State* L) {
                everyone = static_cast<int>(lua_tonumber(L, -2));
                std::size_t n = 0;
                const char* s = lua_tolstring(L, -1, &n);
                reply.assign(s ? s : "", s ? n : 0);
            });
        if (everyone != 0) {
            world.textToAll(widen(reply));
        } else if (unit) {
            tellPlayer(world, *unit, reply);
        }
        return true;
    }
    if (asks == Blessings) {
        if (unit) {
            // (The reply the script's: commands.blessings, from the player's blessings.)
            const auto held = blessings(who->second.name);
            tellPlayer(world, *unit, rules_->text(rules_->blessingsAsked, [&](lua_State* L) {
                pushNumbers(L, held);
                return 1;
            }));
        }
        return true;
    }
    if (asks != Number || !unit || (!offers_.contains(unit->extent) && !blessOffers_.contains(unit->extent))) {
        return false;
    }
    // A number of the offer (as the lines numbered it).
    const auto n = static_cast<std::size_t>(arg);    // An altar's offer answered first; otherwise the campfire's spell offer.
    if (const auto b = blessOffers_.find(unit->extent); b != blessOffers_.end()) {
        return 1 <= n && n <= b->second.size() && bless(host, peer, *unit, b->second[n - 1].blessing);
    }
    const auto& list = offers_.at(ObjectRef{unit->extent});
    if (n < 1 || list.size() < n) {
        return false;
    }
    return upgrade(host, peer, *unit, list[n - 1].spell);
}

void DelveDirector::onMessageRun(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m)
{
    const auto v = delvemap::parse(m);
    if (!v || (v->first != delvemap::kUpgrade && v->first != delvemap::kBless) || v->second.empty()) {
        return;
    }
    ServerObject* unit = host.unitOf(peer);
    if (!unit) {
        return;
    }
    if (v->first == delvemap::kUpgrade) {
        upgrade(host, peer, *unit, v->second[0]);
    } else if (v->second[0] < delvemap::kBlessings) {
        bless(host, peer, *unit, static_cast<delvemap::Blessing>(v->second[0]));
    }
}

} // namespace nox::server
