// Delving Mode's data as the director reads it: its script's tables (script/server/data.luau,
// built into the program) read once, at set-up, into these structures. Nothing here asks the
// script per frame. (Private to the Delve's sources.)
#pragma once

#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/server/delve.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server::delve_data {

/// A list of names and views of them (what a std::span<const std::string_view> asks for).
struct Names {
    std::vector<std::string> names;
    std::vector<std::string_view> views;
};

/// A difficulty's name (its numbers are the script's rules': rules.luau; what it does, told as
/// it's asked about, the script's commands.luau).
struct Difficulty {
    std::string name;
};

/// A room as players are told it: its name ("fight", "?"). (Its phrase, many of it and what the
/// level's line says of it are the script's.)
struct Room {
    std::string name;
};

/// A blessing's name and what it does.
struct Blessing {
    std::string name;
    std::string text;
};

/// What the Delve tells players and its names: the script's text table, each a template its
/// {fields} are filled into (modes::script::fillText). The fields each takes are fixed here
/// (delve_data.cpp's list): a script line that drops or adds one is refused at set-up. (Kept here:
/// the lines the C++ says, the server's listed names, the levels' titles, a shopkeeper's name,
/// Swift's short words, the champion's; the rest, the script's own words, are read for that check
/// alone.)
struct Text {
    std::string listedLobby, listedLevel, levelTitle, shopkeeper;
    std::string objSwiftShort;
    std::string championIs, championLate, championTeam;
};

/// A champion's class: the word for it, the gear it wears, its spells (ids).
struct ChampionClass {
    std::string word;
    std::vector<std::string> gear;
    std::vector<std::uint32_t> spells;
};

/// The tables the director reads. (The rest of data.luau, the creatures by tier, the mysteries',
/// traits' and climates' words, the barrels' gems, the board's sizes, the heat's bite, is the
/// script's alone: checked as it's read, never kept here.)
struct Data {
    std::vector<Difficulty> difficulties; ///< by DelveDifficulty (kDelveDifficulties)
    Text text;
    Names rules;
    std::vector<Room> rooms;       ///< by DelveRoom
    std::vector<Blessing> blessings;    ///< by delvemap::Blessing
    Names breakables;
    std::vector<ChampionClass> championClasses; ///< warrior, wizard, conjurer
    std::uint32_t championLateSpell = 0;        ///< (an early act's champion goes without it)
    /// The thing types found on its maps as they load (the lobby's, a level's gate signs):
    /// data.luau's `things`.
    modes::script::MapThings things;
    /// How near a player counts (world units), how often things happen (frames): data.luau's.
    struct Near {
        float sign = 0, board = 0, portal = 0, boss = 0, gateSign = 0, gateSignSpeaks = 0, shrine = 0, campfire = 0, altar = 0, seal = 0;
    } near;
    struct Every {
        std::uint32_t sign = 0, board = 0, levelLine = 0, wipe = 0, gateSign = 0, renewal = 0, regenerate = 0, purge = 0, celebration = 0, sparks = 0,
                      championWait = 0, championFallen = 0;
    } every;
    /// A class's first spells (ids) after a wipe, by class.
    std::vector<std::vector<std::uint32_t>> firstSpells;
    std::size_t recordsSent = 0;                ///< the records in a NoX Core client's table
    std::uint32_t heatEvery = 0;                ///< the heat's bite every so many frames
};

/// The data, read from the script the first time it's asked for (a set-up: the director's
/// constructor asks), then kept. Throws std::runtime_error when the built-in script isn't the
/// data it should be (a fault in the build: the tests read it).
const Data& data();

} // namespace nox::server::delve_data
