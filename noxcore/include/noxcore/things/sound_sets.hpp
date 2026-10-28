// soundset.bin: the creatures' sound sets (parseSoundSetBin), Blowfish key 5 over
// text. Each set is its name, then KEY SOUNDNAME pairs till END; the keys fill the
// set's slots, the sounds by name (utilFindSound: 0 for none). An unknown key ends
// the reading there (the exe gives up on the rest). A creature's set is the one named as its
// thing (monsterCreateFn -> getDefaultSoundSet); the exe keeps the
// sets newest first, so a name given twice finds the last.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::things {

/// The slots, in the set's order (LISTEN_SOUND 1 ... MOVE_SOUND 18), each named for its key.
enum class SoundSlot : int {
    Listen = 1,
    Taunt,              ///< monsterPlayHurtSound's, after a blow lands
    Threaten,
    Idle,
    Engage,
    MeleeAttackInit,
    MeleeAttack,
    MeleeAttackHitting,
    MeleeAttackMissing,
    MissileAttackInit,
    MissileAttack,
    Flee,
    Retreat,
    SpellInit,
    Die,
    Injured,
    Recognize,
    Move,
};

struct SoundSet {
    std::string name;
    std::array<std::uint32_t, 19> sounds{}; ///< by SoundSlot; 0 none

    std::uint32_t operator[](SoundSlot s) const { return sounds[static_cast<std::size_t>(s)]; }
};

struct SoundSets {
    std::vector<SoundSet> sets; ///< in the file's order

    /// getDefaultSoundSet: by exact name, the last of that name. (By reindex's index when it's of
    /// `sets` as they stand, else by walking the list from the back.)
    const SoundSet* find(std::string_view name) const;

    /// The one rebuild of this table's index (each name's last entry). parseSoundSets calls it;
    /// ANYTHING that changes `sets` after load must call it again: the size check only notices
    /// entries added or removed, an in-place change of the same size (a renamed set) leaves it
    /// stale until this is called. A cache of what was loaded.
    void reindex();
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, std::uint32_t, NameHash, std::equal_to<>> index; ///< (find's cache)
    std::size_t indexedOf = static_cast<std::size_t>(-1); ///< (the entries it was made from)
};

SoundSets parseSoundSets(std::string_view text);
SoundSets loadSoundSets(const std::filesystem::path& path);

} // namespace nox::things
