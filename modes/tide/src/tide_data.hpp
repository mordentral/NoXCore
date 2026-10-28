// Undead Tide's data as the director and the client read it: its script's tables
// (script/server/data.luau, built into the program) read once, at set-up, into these structures.
// Nothing here asks the script per frame. (Private to the Tide's sources.)
#pragma once

#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/server/tide.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server::tide_data {

/// A weapon: its words, classes (1 warrior, 2 wizard, 4 conjurer), its icon. (How it's cast, its
/// aim, reach and wait are the script's: weapons.luau's, rules.luau's cooldown.)
struct Weapon {
    std::string name;
    std::string text;
    std::uint8_t classes = 0;
    std::string iconItem;
    TideIcon icon; ///< (its item a view of iconItem)
};

struct Passive {
    std::string name;
    std::string text;
    int most = 5; ///< its top level
};

/// A difficulty's words (its numbers are the script's rule's: rules.luau's hardness).
struct Hardness {
    std::string name;
    std::string about;
};

/// A pickup: the stock item shown. (The line told, how often among the drops and its sound are
/// the script's.)
struct Pickup {
    std::string type;
};

/// What the Tide tells players and its names: the script's text table, each a template its
/// {fields} are filled into (modes::script::fillText). The fields each takes are fixed here
/// (tide_data.cpp's list): a script line that drops or adds one is refused at set-up. (Kept here:
/// the lines the C++ says, the server's listed names; the rest, the script's own words, are read
/// for that check alone.)
struct Text {
    std::string listedLobby, listedRun;
};

/// The tables the director and the client read. (The rest of data.luau, each biome's creatures and
/// guardian, the chests, is the script's alone: checked as it's read, never kept here.)
struct Data {
    Text text;
    std::vector<Weapon> weapons;   ///< by TideWeapon (kTideWeapons)
    int weaponMost = 5;            ///< a weapon's top level
    std::vector<Passive> passives; ///< by TidePassive (kTidePassives)
    std::vector<Hardness> difficulties; ///< by TideDifficulty (kTideDifficulties)
    std::vector<Pickup> pickups;    ///< by the director's pickup kinds
    float shrineReach = 45; ///< how near a player in play touches the revive shrine
    /// The lobby's reaches and waits (frames): a sign speaks within signNear, at most every
    /// signEvery; the board tells within boardNear; the gate begins within gateNear, a NoX Core
    /// player asked there begun askWait on.
    struct Lobby {
        float signNear = 60;
        std::uint32_t signEvery = 150;
        float boardNear = 100;
        float gateNear = 40;
        std::uint32_t askWait = 450;
    } lobby;
    /// The lobby's thing types, found as it loads: data.luau's `things`.
    modes::script::MapThings things;
    /// The soul gems' drift and take (their reach the script's, by Magnet), the pickups' touch, and
    /// how long things stay (frames): data.luau's gems, pickupField, timers.
    struct Gems {
        float take = 18, base = 6;
        std::uint32_t jitter = 4;
        float pull = 200, near = 20;
        std::uint32_t life = 3600;
        std::size_t most = 150;
    } gems;
    struct PickupField {
        float touch = 30;
        std::uint32_t life = 1800;
        std::size_t most = 25;
    } pickupField;
    struct Timers {
        std::uint32_t glow = 60, body = 90, openChest = 600, ended = 300, passives = 15, charms = 15, offer = 300;
    } timers;
    /// The scenery walked through as a stage begins (its classes, the working ones kept, the plain
    /// touches) and the classes taken from a player's hands (data.luau's scenery and disarm).
    struct Scenery {
        std::uint32_t classes = 0, working = 0;
        std::vector<std::string> collides;
        std::string collidePrefix;
    } scenery;
    std::uint32_t disarm = 0;
    std::vector<std::string> rules;
};

/// The data, read from the script the first time it's asked for (a set-up: the director's and
/// the client's constructors ask), then kept. Throws std::runtime_error when the built-in script
/// isn't the data it should be (a fault in the build: the tests read it).
const Data& data();

} // namespace nox::server::tide_data
