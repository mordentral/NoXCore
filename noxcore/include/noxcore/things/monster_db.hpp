// monster.bin: the monsters' numbers (servParseMonsterDef), one entry a monster:
//
//   GruntAxe
//     EXPERIENCE 40000
//     SOLO SPEED 60
//     ARENA SPEED 90
//     STATUS CAN_HEAR
//     ...
//   END
//
// A line led by SOLO is a solo game's (game flags 0x800 / 0x200000), ARENA a network game's
// (0x2000): the other's skipped to its end, its own prefix passed over. Values are ints, floats,
// a '+' list of status flags (unknown names count for nothing: CAN_HEAR), a missile's name
// (NULL: none), a damage type (DAMAGE_<name>), or a strike / die / dead function's name.
#pragma once

#include "noxcore/util/text.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::things {

/// STATUS's names, by bit.
namespace MonsterStatus {
inline constexpr std::uint32_t DestroyWhenDead = 1u << 0;
inline constexpr std::uint32_t Check = 1u << 1;
inline constexpr std::uint32_t CanBlock = 1u << 2;
inline constexpr std::uint32_t CanDodge = 1u << 3;
inline constexpr std::uint32_t CanCastSpells = 1u << 5;
inline constexpr std::uint32_t HoldYourGround = 1u << 6;
inline constexpr std::uint32_t Summoned = 1u << 7;
inline constexpr std::uint32_t Alert = 1u << 8;
inline constexpr std::uint32_t Injured = 1u << 9;
inline constexpr std::uint32_t CanSeeFriends = 1u << 10;
inline constexpr std::uint32_t CanHealSelf = 1u << 11;
inline constexpr std::uint32_t CanHealOthers = 1u << 12;
inline constexpr std::uint32_t CanRun = 1u << 13;
inline constexpr std::uint32_t Running = 1u << 14;
inline constexpr std::uint32_t AlwaysRun = 1u << 15;
inline constexpr std::uint32_t NeverRun = 1u << 16;
inline constexpr std::uint32_t Bot = 1u << 17;
inline constexpr std::uint32_t Morphed = 1u << 18;
inline constexpr std::uint32_t OnFire = 1u << 19;
inline constexpr std::uint32_t StayDead = 1u << 20;
inline constexpr std::uint32_t Frustrated = 1u << 21;
} // namespace MonsterStatus

/// One monster's numbers, each from the monster.bin key noted.
struct MonsterDef {
    std::string name;
    int experience = 0;          ///< EXPERIENCE
    int health = 0;              ///< HEALTH
    int questHealth = 0;         ///< QUESTHEALTH
    int speed = 0;               ///< SPEED
    float retreatRatio = 0;      ///< RETREAT_RATIO
    float resumeRatio = 0;       ///< RESUME_RATIO
    float fleeRange = 0;         ///< FLEE_RANGE
    std::uint16_t status = 0;    ///< STATUS (MonsterStatus bits)
    float runMultiplier = 0;     ///< RUN_MULTIPLIER
    int moveSoundFrameA = 0;     ///< MOVE_SOUND_FRAME_A
    int moveSoundFrameB = 0;     ///< MOVE_SOUND_FRAME_B
    int meleeFrame = 0;          ///< MELEE_ATTACK_FRAME
    float meleeRange = 0;        ///< MELEE_ATTACK_RANGE
    int meleeDamage = 0;         ///< MELEE_ATTACK_DAMAGE
    float meleeImpact = 0;       ///< MELEE_ATTACK_IMPACT
    int meleeDamageType = 0;     ///< MELEE_ATTACK_DAMAGE_TYPE (DAMAGE_BLADE 0 ... AIRBORNE_ELECTRIC 17)
    int meleeMinDelay = 0;       ///< MELEE_ATTACK_MIN_DELAY
    int meleeMaxDelay = 0;       ///< MELEE_ATTACK_MAX_DELAY
    int poisonChance = 0;        ///< MELEE_ATTACK_POISON_CHANCE
    int poisonStrength = 0;      ///< MELEE_ATTACK_POISON_STRENGTH
    int poisonMax = 0;           ///< MELEE_ATTACK_POISON_MAX
    std::string missile;         ///< MISSILE_NAME (empty: none)
    float missileRange = 0;      ///< MISSILE_ATTACK_RANGE
    int missileFrame = 0;        ///< MISSILE_ATTACK_FRAME
    int missileMinDelay = 0;     ///< MISSILE_ATTACK_MIN_DELAY
    int missileMaxDelay = 0;     ///< MISSILE_ATTACK_MAX_DELAY
    std::string dieFn;           ///< DIE_FUNCTION
    std::string deadFn;          ///< DEAD_FUNCTION
    std::string strikeFn;        ///< MELEE_STRIKE_FUNCTION
};

struct MonsterDb {
    std::vector<MonsterDef> monsters;
    /// The first monster of that name (without case), or nullptr. (By reindex's index when it's
    /// of `monsters` as they stand, else by walking the list.)
    const MonsterDef* find(std::string_view name) const;

    /// The one rebuild of this table's index (each name's first entry, without case).
    /// parseMonsterDb calls it; ANYTHING that changes `monsters` after load must call it again:
    /// the size check only notices entries added or removed, an in-place change of the same size
    /// (a renamed monster) leaves it stale until this is called. A cache of what was loaded.
    void reindex();
    std::unordered_map<std::string, std::uint32_t, util::NoCaseHash, util::NoCaseEqual> index; ///< (find's cache)
    std::size_t indexedOf = static_cast<std::size_t>(-1); ///< (the entries it was made from)
};

/// Parses decrypted monster.bin text for a solo game's numbers or a network game's.
MonsterDb parseMonsterDb(std::string_view text, bool solo);
MonsterDb loadMonsterDb(const std::filesystem::path& path, bool solo);

} // namespace nox::things
