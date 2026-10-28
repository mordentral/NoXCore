// Object CLASS and SUBCLASS bit sets, as thing.bin names them.
//
// Name tables verified against GAME.EXE's CLASS table and its SUBCLASS tables
// (armor, weapon, monster, food, other, book, missile, generator, exit; 32 entries each). Names
// from OpenNox libs `object` (MIT).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace nox::things {

/// The CLASS names (GAME.EXE's table): name i is bit 1 << i.
extern const std::string_view kClassNames[32];
/// The SUBCLASS tables in the order the game searches them (armor, weapon, monster, food, other,
/// book, missile, generator, exit): name i of a table is its bit 1 << i.
extern const std::span<const std::string_view> kSubclassTables[9];
/// The FLAGS names (the object flags, ObjFlag): name i is bit 1 << i.
extern const std::string_view kFlagNames[32];

namespace ObjectClass {
inline constexpr std::uint32_t Missile = 0x1;
inline constexpr std::uint32_t Monster = 0x2;
inline constexpr std::uint32_t Player = 0x4;
inline constexpr std::uint32_t Obstacle = 0x8;
inline constexpr std::uint32_t Food = 0x10;
inline constexpr std::uint32_t Exit = 0x20;
inline constexpr std::uint32_t Key = 0x40;
inline constexpr std::uint32_t Door = 0x80;
inline constexpr std::uint32_t InfoBook = 0x100;
inline constexpr std::uint32_t Trigger = 0x200;
inline constexpr std::uint32_t Transporter = 0x400;
inline constexpr std::uint32_t Hole = 0x800;
inline constexpr std::uint32_t Wand = 0x1000;
inline constexpr std::uint32_t Fire = 0x2000;
inline constexpr std::uint32_t Elevator = 0x4000;
inline constexpr std::uint32_t ElevatorShaft = 0x8000;
inline constexpr std::uint32_t Dangerous = 0x10000;
inline constexpr std::uint32_t MonsterGenerator = 0x20000;
inline constexpr std::uint32_t Readable = 0x40000;
inline constexpr std::uint32_t Light = 0x80000;
inline constexpr std::uint32_t Simple = 0x100000;
inline constexpr std::uint32_t Complex = 0x200000;
inline constexpr std::uint32_t Immobile = 0x400000;
inline constexpr std::uint32_t VisibleEnable = 0x800000;
inline constexpr std::uint32_t Weapon = 0x1000000;
inline constexpr std::uint32_t Armor = 0x2000000;
inline constexpr std::uint32_t NotStackable = 0x4000000;
inline constexpr std::uint32_t Treasure = 0x8000000;
inline constexpr std::uint32_t Flag = 0x10000000;
inline constexpr std::uint32_t ClientPersist = 0x20000000;
inline constexpr std::uint32_t ClientPredict = 0x40000000;
inline constexpr std::uint32_t Pickup = 0x80000000;
} // namespace ObjectClass

/// Subclass bits used by the map xfers. The same bit means different things per table
/// (e.g. 0x8 is CROSSBOW for weapons and SHOPKEEPER for monsters).
namespace ObjectSubclass {
inline constexpr std::uint32_t StaffWands = 0x47F0000; // STAFF_SULPHOROUS_FLARE..STAFF_OBLIVION_ORB
inline constexpr std::uint32_t StaffOblivionOrb = 0x4000000;
inline constexpr std::uint32_t Shopkeeper = 0x8;
inline constexpr std::uint32_t FemaleNpc = 0x20;
inline constexpr std::uint32_t WoundedNpc = 0x80000;
} // namespace ObjectSubclass

/// Parses a CLASS value ("MONSTER+COMPLEX"). Like the game, "NULL" means none and unknown
/// names are ignored; names match case-insensitively.
std::uint32_t parseClassSet(std::string_view value);

/// Parses a SUBCLASS value the same way; each name sets the bit of its first matching table.
std::uint32_t parseSubclassSet(std::string_view value);

struct ThingDef;

/// A thing's CLASS and SUBCLASS attributes as bits (0 when absent).
std::uint32_t thingClasses(const ThingDef& thing);
std::uint32_t thingSubclasses(const ThingDef& thing);

/// A thing's FLAGS line (its last) as object flag bits, each name's bit its place in the ObjFlag
/// names table (BELOW 1 ... MARKED 0x80000000): names split at '+', spaces and tabs, matched
/// exactly; 0 when absent. The line alone: the type reader's own additions (every type ENABLED,
/// one without a COLLIDE function NO_COLLIDE) are the caller's (server::typeFlags).
std::uint32_t thingFlags(const ThingDef& thing);

} // namespace nox::things
