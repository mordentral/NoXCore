// The modes' map generator: a level built from its parameters (a room, or a winding level of
// chambers) and written as a complete, stock-compatible map. The wall shapes and their combining
// follow the exe's own wall-line logic (the Wall spell's), floors
// follow the editor's block tiling as shipped maps show it, and the result goes through the
// ordinary map writer (map_save.hpp), so a stock client sees an ordinary custom map.
#pragma once

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/map_save.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapgen {

struct ArenaParams {
    std::string title = "Delve";
    std::string author = "Delving Mode";
    /// MapInfo game types (1 solo/coop, 2 quest, 4 arena; 0x80000000 a chat map), arena unless
    /// set: a run's levels are quest maps, its lobby a chat map.
    std::uint32_t gameFlags = 4;
    std::uint8_t minPlayers = 1;
    std::uint8_t maxPlayers = 6;

    /// Room centre and corner distance in wall cells (23 units each). The room is a diamond,
    /// Nox's native shape. The centre's x + y must be even (walls and floor tiles live on
    /// that checkerboard); the radius even and at least 10.
    int centerX = 118;
    int centerY = 118;
    int radius = 12;
    /// Not 0: a long winding level in place of the room (layout.hpp): this many diagonal
    /// corridor segments across the map, chambers along them, the players starting in the
    /// first and the exit in the last; `corridor` cells either side of its path.
    int segments = 0;
    int corridor = 3;
    /// A winding level's end chamber at most this big (a guardian's arena: 9).
    int endRadius = 7;

    std::string wallSet = "DungeonStone";          ///< thing.bin WALL entry
    std::string floorName = "CobblestoneGrayLight"; ///< thing.bin FLOR entry
    std::uint32_t ambient = 150;                    ///< ambient light, each channel 0..255
    /// A biome (biome.hpp: "Forest", "Shop"...): its walls, floors (patches, a path, blending),
    /// light and scenery, in place of wallSet, floorName and ambient. Empty: none, bare.
    std::string biome;

    int playerStarts = 4;
    /// Object types dropped on the floor, one in each side room first (potions and the like,
    /// DefaultXfer types; a reward marker draws every kind but the Ankh's). "RewardMarker@20":
    /// a marker of those kinds alone (hex: 20 gold or gems), always placed.
    std::vector<std::string> items = {"RedPotion", "RedPotion", "CurePoisonPotion"};
    /// Chests (ChestInit types), empty: one in each side room first, then anywhere away from
    /// the start (whoever runs the map fills them).
    std::vector<std::string> chests;
    /// Monster types placed (MonsterXfer types), scattered away from the players' starts.
    std::vector<std::string> monsters;
    /// Things at the end chamber's centre (a room's own), the first on it, the rest round it: a
    /// monster (a guardian; a shopkeeper with an empty shop, which a quest stocks itself), a
    /// reward marker, or a plain thing (a CampFire, a Fountain).
    std::vector<std::string> centre;
    /// A quest exit (GauntletExitA) set into the north-west wall: the way to the next level.
    bool questExit = false;
    /// A fork's gates in the end chamber's back walls, not written: whoever runs the map spawns
    /// them (ArenaSpots::gates). GauntletExitA goes along a "/" wall (north-west, on screen the
    /// left), GauntletExitB along a "\" (north-east, the right). Before each, the floor of the
    /// biome it leads to and two of its larger pieces; beside it, a sign.
    struct Gate {
        std::string type; ///< (the script's: levels.luau names each gate's and its sign's types)
        std::string toBiome;
        std::string sign;
    };
    std::vector<Gate> gates;
    /// The gates written into the map after all (a map viewer's look at the fork; a quest's load
    /// would keep one).
    bool writeGates = false;
    /// Signs (readable types: Sign1...) spread along the room's south side, in order from west
    /// to east, their own text empty (whoever runs the map gives them words).
    std::vector<std::string> signs;

    /// A winding level's traps and puzzles (features.hpp): up to this many traps along the
    /// route (spike bands, glyphs, arrow traps, fist plates) and puzzle vaults (side rooms shut
    /// by secret walls a lever or three pressure plates open), wired by the map's script.
    int traps = 0;
    int puzzles = 0;
    /// Route chambers that shut round the party till their monsters (taken from `monsters`)
    /// are dead.
    int ambushes = 0;
    /// Locked doors across the route (a silver key's, the key before it).
    int doors = 0;
    /// Mana obelisks in the route's chambers (Obelisk in built biomes, ObeliskPrimitive else).
    int obelisks = 0;
    /// How hard they are: the delve's depth (glyphs' spells, fist plates from 2, fire skulls
    /// from 4).
    int depth = 1;

    unsigned seed = 1;
};

/// Where a built map's placed things went, in world units: what a director watches.
struct ArenaSpots {
    struct Point {
        float x = 0;
        float y = 0;
    };
    Point end;                 ///< the end chamber's centre
    std::vector<Point> centre; ///< each of ArenaParams::centre, in order
    struct Gate {
        std::string type;
        Point at;   ///< where the gate goes (spawned there)
        Point sign; ///< its sign
    };
    std::vector<Gate> gates;
    /// The traps and puzzles that went in, by kind ("spikes", "arrows", "lever", "runes"...).
    std::vector<std::string> features;
};

/// Builds the map in memory. Throws FormatError when a named wall set, floor or object type
/// isn't in `things`, or the room doesn't fit the 128x128 floor grid. `spots`, given, says
/// where its things went.
map::MapFile buildArena(const things::ThingDb& things, const ArenaParams& params, ArenaSpots* spots = nullptr);

/// Writes maps\<name>\<name>.map (encrypted, fresh CRC) and its .nxz (u32 stored size +
/// the NXZ stream of the on-disk bytes), creating the folder; the pair is what hosting and
/// the stock client's map download need. Returns the .map path.
std::filesystem::path writeMapFiles(const std::filesystem::path& mapsDir, const std::string& name,
                                    const map::MapFile& map, const things::ThingDb& things);

/// writeMapFiles' two files as bytes, made in memory (no files touched: another thread's to
/// make ahead): the .map as stored (encrypted, fresh CRC) and the .nxz (map_save.hpp).
using EncodedMap = map::EncodedMapFiles;
EncodedMap encodeMapFiles(const map::MapFile& map, const things::ThingDb& things);
/// Writes an encoded pair as maps\<name>\<name>.map and .nxz (each written beside, then moved
/// over: a failed write never leaves half a level); returns the .map path.
std::filesystem::path writeEncodedMapFiles(const std::filesystem::path& mapsDir, const std::string& name, const EncodedMap& files);

} // namespace nox::mapgen
