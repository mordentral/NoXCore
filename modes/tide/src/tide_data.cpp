// Undead Tide's data read from its script (tide_data.hpp).
#include "tide_data.hpp"

#include "noxcore/things/object_class.hpp"

#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/things/spell_ids.hpp"

#include <algorithm>
#include <stdexcept>

namespace nox::server::tide_data {

namespace {

using modes::script::DataValue;

std::vector<std::string> strings(const DataValue& v)
{
    std::vector<std::string> out;
    for (const DataValue& s : v.list()) {
        out.push_back(s.asString());
    }
    return out;
}

using modes::script::needCount;

// Each line of the text table (script_data.hpp's readText): its key in the script, where it goes
// (nullptr: the script's own words, checked and not kept), the fields it takes. The table holds
// these keys and no others.
using TextLine = modes::script::TextLine<Text>;
const TextLine kTextLines[] = {
    {"listedLobby", &Text::listedLobby, ""},
    {"listedRun", &Text::listedRun, "minutes"},
    {"arenaTitle", nullptr, ""},
    {"lobbyTitle", nullptr, ""},
    {"mapAuthor", nullptr, ""},
    {"difficultyIs", nullptr, "difficulty,about"},
    {"difficultyLocked", nullptr, "difficulty"},
    {"setsDifficulty", nullptr, "player,difficulty,about"},
    {"coming", nullptr, "difficulty"},
    {"risesStage", nullptr, "stage,land,difficulty"},
    {"rises", nullptr, "land,difficulty"},
    {"darkLand", nullptr, ""},
    {"goalDawn", nullptr, "where,dawn,guardian"},
    {"goalGuardian", nullptr, "where,guardian"},
    {"firstWeapon", nullptr, "weapon"},
    {"backInFight", nullptr, ""},
    {"risesAgain", nullptr, "player"},
    {"tideTakes", nullptr, "player"},
    {"shrineRises", nullptr, ""},
    {"shrineBrings", nullptr, "player"},
    {"minuteDawn", nullptr, "clock,left"},
    {"minuteGuardian", nullptr, "clock"},
    {"specialOne", nullptr, "creature"},
    {"specialMany", nullptr, "creature"},
    {"wave", nullptr, "creature"},
    {"elite", nullptr, "creature"},
    {"guardianComesDawn", nullptr, "guardian"},
    {"guardianComes", nullptr, "guardian"},
    {"guardianFallsStage", nullptr, "guardian,seconds,next"},
    {"wardingDrop", nullptr, "item"},
    {"repulsionDrop", nullptr, "item"},
    {"chestOpened", nullptr, "player"},
    {"offerTitle", nullptr, "level,count"},
    {"offerItem", nullptr, "number,pick"},
    {"pickHeal", nullptr, ""},
    {"pickNew", nullptr, "name"},
    {"pickLevel", nullptr, "name,level"},
    {"pickedForYou", nullptr, ""},
    {"youTake", nullptr, "what"},
    {"healTaken", nullptr, ""},
    {"me", nullptr, "level,xp,need,kills,kit"},
    {"kitItem", nullptr, "name,level"},
    {"listSeparator", nullptr, ""},
    {"endGuardianDawn", nullptr, "guardian,clock"},
    {"endDawn", nullptr, "clock"},
    {"endLostStage", nullptr, "stage,clock,total"},
    {"endLost", nullptr, "clock"},
    {"results", nullptr, "elites,before,fate"},
    {"eliteOne", nullptr, "count"},
    {"eliteMany", nullptr, "count"},
    {"beforeOne", nullptr, "stage,count"},
    {"beforeMany", nullptr, "stage,count"},
    {"fateNeverCame", nullptr, ""},
    {"fateSlain", nullptr, "guardian"},
    {"fateStanding", nullptr, "guardian"},
    {"runnerLine", nullptr, "player,level,kills,kit"},
    {"boardPlace", nullptr, "place"},
    {"boardEmpty", nullptr, ""},
    {"boardTitle", nullptr, ""},
    {"boardLine", nullptr, "place,run"},
    {"recordLine", nullptr, "outcome,on,members,more,kills,biome,date"},
    {"recordGuardian", nullptr, "clock"},
    {"recordDawn", nullptr, "clock"},
    {"recordStage", nullptr, "stage,clock"},
    {"recordFell", nullptr, "clock"},
    {"recordOn", nullptr, "difficulty"},
    {"recordMember", nullptr, "name,class,level"},
    {"recordMore", nullptr, "count"},
    {"recordBiome", nullptr, "biome"},
    {"recordDate", nullptr, "date"},
};

// A spell by the game's name for it (SPELL_...).
std::uint32_t spellOf(const DataValue& v)
{
    const auto id = things::spellId(v.asString());
    if (!id) {
        throw std::runtime_error(v.where() + ": no spell " + v.asString());
    }
    return *id;
}

// An enchantment by the game's name for it (ENCHANT_...).
int enchantOf(const DataValue& v)
{
    for (int e = 0; e < 32; ++e) {
        if (kEnchantNames[e] == v.asString()) {
            return e;
        }
    }
    throw std::runtime_error(v.where() + ": no enchantment " + v.asString());
}

std::uint8_t classesOf(const DataValue& v)
{
    std::uint8_t bits = 0;
    for (const DataValue& c : v.list()) {
        const std::string& name = c.asString();
        if (name == "warrior") {
            bits |= 1;
        } else if (name == "wizard") {
            bits |= 2;
        } else if (name == "conjurer") {
            bits |= 4;
        } else {
            throw std::runtime_error(c.where() + ": no class " + name);
        }
    }
    return bits;
}

// How a weapon finds what it casts at (the script's weapons.luau's): the director's own (custom),
// the nearest foe, the nearest foes without its enchantment yet, about the player, on the player,
// the strongest foe near.
void checkAim(const DataValue& v)
{
    const std::string& a = v.asString();
    for (const char* aim : {"custom", "atFoe", "onFoe", "atSelf", "onSelf", "onStrongest"}) {
        if (a == aim) {
            return;
        }
    }
    throw std::runtime_error(v.where() + ": no aim " + a);
}

Data load()
{
    const DataValue root = modes::script::readBuiltinData("tide", "server/data.luau");
    Data d;

    const DataValue& weapons = root["weapons"];
    needCount(weapons, kTideWeapons);
    for (const DataValue& w : weapons.list()) {
        Weapon x;
        x.name = w["name"].asString();
        x.text = w["text"].asString();
        x.classes = classesOf(w["classes"]);
        // (How it's cast: the script's own, weapons.luau's, checked here as it's read.)
        if (w.find("spell")) {
            (void)spellOf(w["spell"]);
        }
        checkAim(w["aim"]);
        (void)w["reach"].asFloat();
        if (w.find("enchant")) {
            (void)enchantOf(w["enchant"]);
        }
        if (w.find("out")) {
            (void)w["out"].asString();
        }
        const DataValue& icon = w["icon"];
        x.icon.spell = icon.find("spell") ? spellOf(icon["spell"]) : 0;
        x.iconItem = icon.find("item") ? icon["item"].asString() : std::string();
        x.icon.ability = icon.find("ability") ? icon["ability"].asInt() : 0;
        d.weapons.push_back(std::move(x));
    }
    d.weaponMost = root["weaponMost"].asInt();

    const DataValue& passives = root["passives"];
    needCount(passives, kTidePassives);
    for (const DataValue& p : passives.list()) {
        d.passives.push_back({p["name"].asString(), p["text"].asString(), p["most"].asInt()});
    }

    const DataValue& difficulties = root["difficulties"];
    needCount(difficulties, kTideDifficulties);
    for (const DataValue& h : difficulties.list()) {
        d.difficulties.push_back({h["name"].asString(), h["about"].asString()});
    }

    // The creature sets by their names in the script, then each biome's: the script's own
    // (spawns.luau draws them), checked here as they're read.
    std::vector<std::string> setNames;
    for (const auto& [name, set] : root["creatures"].fields()) {
        const bool low = !strings(set["low"]).empty();
        const bool mid = !strings(set["mid"]).empty();
        const bool hard = !strings(set["hard"]).empty();
        (void)strings(set["special"]);
        if (!low || !mid || !hard) {
            throw std::runtime_error(set.where() + " needs low, mid and hard creatures");
        }
        setNames.push_back(name);
    }
    const auto checkSet = [&](const DataValue& v) {
        if (std::ranges::find(setNames, v.asString()) == setNames.end()) {
            throw std::runtime_error(v.where() + ": no creature set " + v.asString());
        }
    };
    for (const auto& [biome, set] : root["biomeCreatures"].fields()) {
        checkSet(set);
    }
    checkSet(root["otherBiomeCreatures"]);

    for (const DataValue& p : root["pickups"].list()) {
        // (What it says, how often, its sound: the script's own, checked here.)
        Pickup x{p["type"].asString()};
        (void)p["says"].asString();
        (void)p["weight"].asInt();
        (void)spellOf(p["sound"]);
        d.pickups.push_back(std::move(x));
    }
    const bool noChests = strings(root["chests"]).empty(); // (the script's own, fall.luau's: checked)
    d.shrineReach = root["shrine"]["reach"].asFloat();
    const DataValue& lobby = root["lobby"];
    d.lobby = {lobby["signNear"].asFloat(), static_cast<std::uint32_t>(lobby["signEvery"].asInt()), lobby["boardNear"].asFloat(),
               lobby["gateNear"].asFloat(), static_cast<std::uint32_t>(lobby["askWait"].asInt())};
    d.things = modes::script::readMapThings(root["things"]);
    const DataValue& gems = root["gems"];
    d.gems = {gems["take"].asFloat(), gems["base"].asFloat(), static_cast<std::uint32_t>(gems["jitter"].asInt()), gems["pull"].asFloat(),
              gems["near"].asFloat(), static_cast<std::uint32_t>(gems["life"].asInt()), static_cast<std::size_t>(gems["most"].asInt())};
    const DataValue& field = root["pickupField"];
    d.pickupField = {field["touch"].asFloat(), static_cast<std::uint32_t>(field["life"].asInt()), static_cast<std::size_t>(field["most"].asInt())};
    const DataValue& timers = root["timers"];
    d.timers = {static_cast<std::uint32_t>(timers["glow"].asInt()), static_cast<std::uint32_t>(timers["body"].asInt()),
                static_cast<std::uint32_t>(timers["openChest"].asInt()), static_cast<std::uint32_t>(timers["ended"].asInt()),
                static_cast<std::uint32_t>(timers["passives"].asInt()), static_cast<std::uint32_t>(timers["charms"].asInt()),
                static_cast<std::uint32_t>(timers["offer"].asInt())};
    const DataValue& scenery = root["scenery"];
    d.scenery = {things::parseClassSet(scenery["classes"].asString()), things::parseClassSet(scenery["working"].asString()), strings(scenery["collides"]),
                 scenery["collidePrefix"].asString()};
    d.disarm = things::parseClassSet(root["disarm"].asString());
    if (noChests) {
        throw std::runtime_error("chests: none");
    }
    d.rules = strings(root["rules"]);

    // (Each biome's guardian: the script's own, spawns.luau's, checked here.)
    for (const auto& [biome, type] : root["guardians"].fields()) {
        (void)type.asString();
    }
    (void)root["otherGuardian"].asString();

    modes::script::readText(root["text"], d.text, kTextLines, "the Tide");
    return d;
}

// The views into the data's own strings, made once it's where it stays.
Data loaded()
{
    Data d = load();
    for (Weapon& w : d.weapons) {
        w.icon.item = w.iconItem;
    }
    return d;
}

} // namespace

const Data& data()
{
    static const Data d = loaded();
    return d;
}

} // namespace nox::server::tide_data
