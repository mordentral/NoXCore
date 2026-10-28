// A map's compiled NoxScript: the ScriptObject section's bytes, which the game writes out as
// nc.obj and loads with. Layout (all little-endian, no padding):
//
//   "SCRIPT03"
//   "STRG" u32 count, count x (u32 length, bytes)          the string constants
//   "CODE" u32 count, count x function:
//     "FUNC" u32 length, name                             (under 1024 bytes)
//     u32 returns                                         values it returns (0 or 1)
//     u32 args                                            arguments, the first locals
//     "SYMB" u32 symbols, u32 (unused), sizes             each symbol's size in words; the
//                                                         first function has one more symbol,
//                                                         of size 0, before the stored ones
//     "DATA" u32 length, code                             u32 words
//   "DONE"
//
// Function 0 holds the map's object variables (see Vm), function 1 is GLOBAL: its locals are the
// script's globals (a savegame's ScriptData keeps them).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::script {

struct Function {
    std::string name; ///< as stored, "%..." part and all (the game finds functions by this)
    /// A function named "Name%Prefix%X%Y" (map generation's prefabs): while it runs, the lookup
    /// builtins (Object, Waypoint, the groups, Wall) take names with "%Prefix" in front, and
    /// the wall and placement builtins move by (x, y).
    struct Prefab {
        std::string prefix; ///< with its leading '%'
        std::int32_t x = 0;
        std::int32_t y = 0;
        bool operator==(const Prefab&) const = default;
    };
    std::optional<Prefab> prefab;
    std::uint32_t returns = 0;
    std::uint32_t args = 0;
    std::uint32_t symbUnused = 0;             ///< SYMB's second word (the loader skips it)
    std::vector<std::uint32_t> symbolSizes;   ///< in words
    std::vector<std::uint32_t> symbolOffsets; ///< in words, into the locals
    std::uint32_t localWords = 0;             ///< the sizes' sum
    std::vector<std::uint32_t> code;
};

/// The most words of locals one function may have (its symbols' sizes in all): 16 MB, far past
/// any map's. ScriptFile::parse refuses more.
inline constexpr std::uint32_t kMaxLocalWords = 1u << 22;

struct ScriptFile {
    std::vector<std::string> strings;
    std::vector<Function> functions;

    /// Throws std::runtime_error on a malformed file (where the game's loader gives up). No
    /// bytes at all is a map without a script: no functions.
    static ScriptFile parse(std::span<const std::uint8_t> data);
    /// Back to bytes (a script without functions or strings: none).
    std::vector<std::uint8_t> serialize() const;
    /// A function by its stored name (the first of a name). (By reindex's index when it's of
    /// `functions` as they stand, else by walking the list.)
    std::optional<std::size_t> find(std::string_view name) const;

    /// The names' index made afresh from `functions` (each name's first): parse makes it; one who
    /// changes `functions` after calls it again.
    void reindex();
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, std::uint32_t, NameHash, std::equal_to<>> byName; ///< (find's cache)
    std::size_t indexedOf = static_cast<std::size_t>(-1); ///< (the functions it was made from)
};

} // namespace nox::script
