// ArenaParams to and from a script's table, field by field under arena.luau's names: the generator
// hands its script a level's parameters (arena.cpp), the Delve's script makes and changes its
// levels' (levels.luau), the Tide's makes its maps' (params.luau). Each caller names the fields its
// tables carry, and how they're read back.
#pragma once

#include "noxcore/mapgen/arena.hpp"

#include <cstdint>

struct lua_State;

namespace nox::mapgen {

/// ArenaParams' fields, as bits: those a table carries.
struct ArenaField {
    enum : std::uint32_t {
        Title = 1u << 0,
        Author = 1u << 1,
        GameFlags = 1u << 2,
        MinPlayers = 1u << 3,
        MaxPlayers = 1u << 4,
        CenterX = 1u << 5,
        CenterY = 1u << 6,
        Radius = 1u << 7,
        Segments = 1u << 8,
        Corridor = 1u << 9,
        EndRadius = 1u << 10,
        WallSet = 1u << 11,
        FloorName = 1u << 12,
        Ambient = 1u << 13,
        Biome = 1u << 14,
        PlayerStarts = 1u << 15,
        Items = 1u << 16,
        Chests = 1u << 17,
        Monsters = 1u << 18,
        Centre = 1u << 19,
        QuestExit = 1u << 20,
        Gates = 1u << 21,
        WriteGates = 1u << 22,
        Signs = 1u << 23,
        Traps = 1u << 24,
        Puzzles = 1u << 25,
        Ambushes = 1u << 26,
        Doors = 1u << 27,
        Obelisks = 1u << 28,
        Depth = 1u << 29,
        Seed = 1u << 30,
        All = (1u << 31) - 1,
    };
};

/// `p`'s `fields` as a new table on the stack's top (a list of strings for a list; gates each
/// {type, toBiome, sign}).
void pushArenaParams(lua_State* L, const ArenaParams& p, std::uint32_t fields);

/// How a table's fields are read back.
enum class ArenaRead : std::uint8_t {
    /// By lua_getfield, any value but nil taken: a number's boolean as 0 or 1, a string's
    /// anything as its text ("" for what has none), a list's entries likewise (the Delve's).
    Present,
    /// By lua_rawgetfield, only a value of the field's own kind (a number; a string or a number;
    /// a table; a boolean), any other left as it was (the Tide's).
    Typed,
};

/// The table at `t`'s `fields` read into `p`: those it has, the rest left as they are.
void readArenaParams(lua_State* L, int t, ArenaParams& p, std::uint32_t fields, ArenaRead how);

} // namespace nox::mapgen
