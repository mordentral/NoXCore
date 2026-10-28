// Delving Mode's data read from its script (delve_data.hpp).
#include "delve_data.hpp"

#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/things/spell_ids.hpp"

#include <stdexcept>

namespace nox::server::delve_data {

namespace {

using modes::script::DataValue;

Names names(const DataValue& v, bool needOne = true)
{
    Names out;
    for (const DataValue& s : v.list()) {
        out.names.push_back(s.asString());
    }
    if (needOne && out.names.empty()) {
        throw std::runtime_error(v.where() + ": none");
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
    {"listedLevel", &Text::listedLevel, "depth"},
    {"lobbyTitle", nullptr, ""},
    {"levelTitle", &Text::levelTitle, "depth"},
    {"shopkeeper", &Text::shopkeeper, "name"},
    {"clientGreeting", nullptr, ""},
    {"begins", nullptr, ""},
    {"difficultyIs", nullptr, "difficulty,about"},
    {"difficultyLocked", nullptr, "difficulty"},
    {"setsDifficulty", nullptr, "player,difficulty,about"},
    {"levelLine", nullptr, "depth,act,difficulty,place,what,ways,air"},
    {"levelDifficulty", nullptr, "difficulty"},
    {"levelGates", nullptr, "left,right"},
    {"levelAhead", nullptr, "next"},
    {"levelAir", nullptr, "air"},
    {"placeUnknown", nullptr, "biome"},
    {"actPlan", nullptr, "first,ways"},
    {"actWays", nullptr, "rooms"},
    {"actCount", nullptr, "count,rooms"},
    {"listComma", nullptr, ""},
    {"listAnd", nullptr, ""},
    {"gateLeft", nullptr, ""},
    {"gateRight", nullptr, ""},
    {"gateToAct", nullptr, "side,place,climate,act"},
    {"gateToRoom", nullptr, "side,room,then"},
    {"thenGuardian", nullptr, ""},
    {"thenRooms", nullptr, "left,right"},
    {"takesLeft", nullptr, "player,where"},
    {"takesRight", nullptr, "player,where"},
    {"guardianFalls", nullptr, ""},
    {"partyFallen", nullptr, "depth"},
    {"wiped", nullptr, ""},
    {"newDeepest", nullptr, "depth,best"},
    {"shrine", nullptr, "player"},
    {"elite", nullptr, "creature,traits"},
    {"someone", nullptr, ""},
    {"fireNothing", nullptr, ""},
    {"fireOffer", nullptr, ""},
    {"fireItem", nullptr, "number,spell,level,next"},
    {"fireGap", nullptr, ""},
    {"fireRises", nullptr, "player,spell,level"},
    {"spellUnknown", nullptr, "id"},
    {"altarOffer", nullptr, ""},
    {"altarItem", nullptr, "number,name,text"},
    {"altarHeldOne", nullptr, "count"},
    {"altarHeldMany", nullptr, "count"},
    {"blessed", nullptr, "player,name,text"},
    {"fortune", nullptr, "gold"},
    {"blessingsNone", nullptr, ""},
    {"blessingsYours", nullptr, "list"},
    {"blessingHeld", nullptr, "name"},
    {"blessingHeldMany", nullptr, "name,count"},
    {"bonus", nullptr, "objective"},
    {"bonusDone", nullptr, "objective"},
    {"objPurge", nullptr, ""},
    {"objHunt", nullptr, "quarry"},
    {"objSeals", nullptr, ""},
    {"objSwift", nullptr, "minutes,seconds"},
    {"objSwiftShort", &Text::objSwiftShort, ""},
    {"purgeHalf", nullptr, "left"},
    {"sealBroken", nullptr, "player,done,of"},
    {"tooSlow", nullptr, ""},
    {"swiftDone", nullptr, "gold"},
    {"championIs", &Text::championIs, "name,word"},
    {"championLate", &Text::championLate, "creature"},
    {"championTeam", &Text::championTeam, ""},
    {"recordsNone", nullptr, ""},
    {"recordsTitle", nullptr, ""},
    {"recordLine", nullptr, "place,depth,act,date,party"},
    {"recordMember", nullptr, "name,class"},
    {"recordMore", nullptr, "count"},
};

Data load()
{
    const DataValue root = modes::script::readBuiltinData("delve", "server/data.luau");
    Data d;

    // (What the script alone reads: the creatures by tier, the guardians, the rooms' and
    // mysteries' words, the traits, the climates' words, the shopkeepers, barrels' gems, the
    // champions, the board's sizes, the heat's bite: checked here, as each is read.)
    needCount(root["tiers"], 5);
    for (const DataValue& t : root["tiers"].list()) {
        (void)names(t);
    }
    // (Each act's guardians are the script's levels.luau's alone; checked here, as it's read.)
    if (root["guardians"].list().empty()) {
        throw std::runtime_error("guardians: none");
    }
    for (const DataValue& g : root["guardians"].list()) {
        (void)names(g);
    }

    const DataValue& difficulties = root["difficulties"];
    needCount(difficulties, kDelveDifficulties);
    for (const DataValue& h : difficulties.list()) {
        d.difficulties.push_back({h["name"].asString()});
        (void)h["about"].asString(); // (the script's commands.luau tells it; checked here)
    }

    d.rules = names(root["rules"]);

    const DataValue& rooms = root["rooms"];
    needCount(rooms, static_cast<std::size_t>(DelveRoom::Blessing) + 1);
    for (const DataValue& r : rooms.list()) {
        Room room{r["name"].asString()};
        (void)r["phrase"].asString();
        (void)r["told"].asString();
        if (r.find("many")) {
            (void)r["many"].asString();
        }
        d.rooms.push_back(std::move(room));
    }
    needCount(root["mysteries"], static_cast<std::size_t>(DelveMystery::Champion) + 1);
    (void)names(root["mysteries"]);
    needCount(root["traits"], 5);
    (void)names(root["traits"]);
    needCount(root["blessings"], delvemap::kBlessings);
    for (const DataValue& b : root["blessings"].list()) {
        d.blessings.push_back({b["name"].asString(), b["text"].asString()});
    }
    modes::script::readText(root["text"], d.text, kTextLines, "the Delve");

    const DataValue& climates = root["climates"];
    (void)climates["Freezing"]["line"].asString();
    (void)climates["Freezing"]["tag"].asString();
    (void)climates["Scorching"]["line"].asString();
    (void)climates["Scorching"]["tag"].asString();

    (void)names(root["shopkeepers"]); // (the script's draws.luau names them; checked here)
    d.breakables = names(root["breakables"]);
    (void)names(root["barrelGems"]);
    (void)names(root["champions"]); // (the script's draws.luau names them; checked here)

    const DataValue& classes = root["championClasses"];
    needCount(classes, 3);
    for (const DataValue& c : classes.list()) {
        ChampionClass x;
        x.word = c["word"].asString();
        x.gear = names(c["gear"]).names;
        for (const DataValue& s : c["spells"].list()) {
            const auto id = things::spellId(s.asString());
            if (!id) {
                throw std::runtime_error(s.where() + ": no spell " + s.asString());
            }
            x.spells.push_back(*id);
        }
        d.championClasses.push_back(std::move(x));
    }
    const DataValue& late = root["championLateSpell"];
    const auto lateId = things::spellId(late.asString());
    if (!lateId) {
        throw std::runtime_error(late.where() + ": no spell " + late.asString());
    }
    d.championLateSpell = *lateId;
    {
        d.things = modes::script::readMapThings(root["things"]);
        const DataValue& n = root["near"];
        needCount(n, 0); // (a table of names, not a list)
        if (n.fields().size() != 10) {
            throw std::runtime_error(n.where() + " has " + std::to_string(n.fields().size()) + " reaches, not 10");
        }
        auto& r = d.near;
        for (auto [name, at] : {std::pair{"sign", &r.sign}, std::pair{"board", &r.board}, std::pair{"portal", &r.portal}, std::pair{"boss", &r.boss},
                                std::pair{"gateSign", &r.gateSign}, std::pair{"gateSignSpeaks", &r.gateSignSpeaks}, std::pair{"shrine", &r.shrine},
                                std::pair{"campfire", &r.campfire}, std::pair{"altar", &r.altar}, std::pair{"seal", &r.seal}}) {
            *at = n[name].asFloat();
        }
        const DataValue& e = root["every"];
        if (e.fields().size() != 12) {
            throw std::runtime_error(e.where() + " has " + std::to_string(e.fields().size()) + " cadences, not 12");
        }
        auto& w = d.every;
        for (auto [name, at] : {std::pair{"sign", &w.sign}, std::pair{"board", &w.board}, std::pair{"levelLine", &w.levelLine}, std::pair{"wipe", &w.wipe},
                                std::pair{"gateSign", &w.gateSign}, std::pair{"renewal", &w.renewal}, std::pair{"regenerate", &w.regenerate},
                                std::pair{"purge", &w.purge}, std::pair{"celebration", &w.celebration}, std::pair{"sparks", &w.sparks},
                                std::pair{"championWait", &w.championWait}, std::pair{"championFallen", &w.championFallen}}) {
            *at = e[name].asUint();
        }
        for (const std::uint32_t beat : {w.renewal, w.regenerate, w.purge, w.sparks}) {
            if (beat == 0) {
                throw std::runtime_error(e.where() + ": a beat of 0 frames");
            }
        }
    }
    needCount(root["firstSpells"], 3);
    for (const DataValue& cls : root["firstSpells"].list()) {
        std::vector<std::uint32_t> spells;
        for (const DataValue& s : cls.list()) {
            const auto id = things::spellId(s.asString());
            if (!id) {
                throw std::runtime_error(s.where() + ": no spell " + s.asString());
            }
            spells.push_back(*id);
        }
        d.firstSpells.push_back(std::move(spells));
    }
    (void)root["board"]["shown"].asUint();
    (void)root["board"]["named"].asUint();
    d.recordsSent = root["board"]["sent"].asUint();
    (void)root["heat"]["damage"].asInt();
    d.heatEvery = root["heat"]["every"].asUint();
    if (d.heatEvery == 0) {
        throw std::runtime_error(root["heat"]["every"].where() + ": not 0");
    }
    return d;
}

void makeViews(Names& n)
{
    n.views.assign(n.names.begin(), n.names.end());
}

// The views into the data's own strings, made once it's where it stays.
Data loaded()
{
    Data d = load();
    for (Names* n : {&d.rules, &d.breakables}) {
        makeViews(*n);
    }
    return d;
}

} // namespace

const Data& data()
{
    // (Never destroyed: a level built ahead and let go at the program's end may still read it.)
    static const Data* const d = new Data(loaded());
    return *d;
}

} // namespace nox::server::delve_data
