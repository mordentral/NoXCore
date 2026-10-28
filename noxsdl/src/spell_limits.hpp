// Spells the server won't cast again while the caster still has the last one's objects out
// (checkPlrCantCastSpell answers 3, and the cast just fails, without a word):
//   Fist of Vengeance   while it owns a SmallFist, MediumFist or LargeFist
//   Force of Nature     while it owns a DeathBall
//   Meteor              while it owns a Meteor
//   Magic Missile       while it owns MagicMissileCount[power - 1] MagicMissiles
//   Pixie Swarm         while it owns PixieCount[power - 1] Pixies
// (gamedata.bin's ARENA tables; power is 3 in multiplayer games, flags 0x570, else the spell's
// level: spellGetPower.)
//
// The client isn't told who owns a missile, so this guesses: objects of those types that appear
// within a couple of seconds of our casting the spell, near us (missiles, pixies, the death ball
// spawn at the caster) or near where the cursor was (a fist or meteor drops there), are ours
// until they're gone. The HUD marks a spell at its limit so a cast that would fail shows why.
#pragma once

#include "noxcore/client/world.hpp"
#include "noxcore/things/game_data.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace noxsdl {

class SpellLimits {
public:
    /// We asked to cast `spell` at game frame `frame`, standing at (x, y) with the cursor at
    /// (cursorX, cursorY).
    void cast(std::uint32_t spell, std::uint32_t frame, float x, float y, float cursorX, float cursorY);
    /// Claims new objects for recent casts and forgets those gone. `us` is our net code.
    void update(const nox::client::World& world, std::uint16_t us, std::uint32_t frame);
    /// The spells the server would refuse now: the counts from `gameData` as a game with
    /// `gameFlags` reads them (a solo game its SOLO lines).
    std::set<std::uint32_t> limited(const nox::client::World& world, const nox::things::GameData& gameData,
                                    std::uint32_t gameFlags) const;
    void clear();

private:
    struct Pending {
        std::uint32_t spell = 0;
        std::uint32_t frame = 0;
        float x = 0, y = 0;             ///< where we stood
        float cursorX = 0, cursorY = 0; ///< where the cursor was
    };
    struct Owned {
        std::uint32_t spell = 0;
        std::uint16_t typeId = 0;
    };
    std::vector<Pending> pending_;
    std::map<std::uint16_t, Owned> owned_; ///< by net code
    /// Drawables seen before (not new), a bit by net code; and the one update() fills, swapped in.
    std::vector<std::uint64_t> known_ = std::vector<std::uint64_t>(65536 / 64);
    std::vector<std::uint64_t> now_ = std::vector<std::uint64_t>(65536 / 64);
    /// By type id: its place in the tracked types (spell_limits.cpp), or -1; worked out from the
    /// world's thing.bin names once.
    std::vector<std::int8_t> trackedByType_;
    const nox::things::ThingDb* trackedOf_ = nullptr;
};

} // namespace noxsdl
