// (noxcpp) The game controller's aim: who a creature is to us, the target the pad's cursor point
// goes to (soft target, lock, the right stick's flick and held way), where a spell of each kind
// goes and what A takes. Pure (no SDL, no App), so the tests check it as it is; pad_play.cpp feeds
// it the frame's world.
//
// Aiming is the client putting its cursor point on what it chose: the server takes a spell's or
// a wand's creature from the cursor (the thing at it), so nothing new goes on the wire. The pad
// never steers a free point: the sticks give ways, the point follows from them.
//
//   aim       the right stick's way while it's held, else the left stick's while it moves, else the
//             way the player last went (aimWay).
//   soft      enemies in sight, scored by their angle to the aim (a cone, 35 degrees at Normal,
//             twice that within kCloseRange; with both sticks at rest any monster within
//             kIdleRange) and their distance (to kTargetReach). A new one must
//             beat the one held by kSwitchMargin; the one held is kept kKeepSeconds after it
//             leaves the cone.
//   lock      while the attack or a slot button is held (LockMode held) the target stays till it
//             dies or leaves sight; LockMode retarget picks again as the aim turns.
//   flick     the right stick pushed past kFlickTilt and let go within kFlickSeconds: the lock
//             steps to the nearest enemy that way (from the one locked, or from us); kept till it
//             dies, leaves sight or goes out of reach, or another flick. TargetNext / Previous
//             step round us the same way.
//   held      the right stick held past a flick's time: only a way (the aim, the soft target's
//             cone turned with it), never a point the stick moves about.
//   players   a narrower pull (12 degrees at Normal), never locked past kPlayerKeepSeconds,
//             never flicked onto. Allies, our summons and the neutral are never candidates.
//   Off       no soft target: the aim point is straight ahead (a flick still locks).
#pragma once

#include "pad/pad_bindings.hpp"
#include "pad/pad_math.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace noxsdl::pad {

/// A point in the world.
struct WorldPoint {
    float x = 0;
    float y = 0;
    bool operator==(const WorldPoint&) const = default;
};

/// Who a creature is to us.
enum class Side : std::uint8_t { Enemy, Ally, Neutral };

/// What the client knows of a creature (a player, or a monster), for creatureSide.
struct CreatureFacts {
    bool player = false;
    std::uint32_t subclasses = 0; ///< a monster's SUBCLASS (0x8 SHOPKEEPER)
    bool talks = false;           ///< X_STATUS 0x10: something to say (an NPC to talk to)
    bool friendListed = false;    ///< on our friends list (our summons, the charmed)
    bool partyCreature = false;   ///< the co-op campaign's party's
    std::uint32_t team = 0;       ///< a player's team (0: none)
};
/// A player is an ally in a quest or the campaign (game flags 0x1800) or on our team (`ownTeam`,
/// not 0), else an enemy; a monster on our friends list or the party's an ally, a shopkeeper or
/// one with something to say neutral, any other an enemy (the HUD's red arrow).
Side creatureSide(const CreatureFacts& c, std::uint32_t ownTeam, std::uint32_t gameFlags);

/// An enemy the aim may take.
struct AimCandidate {
    std::uint16_t id = 0; ///< its net code
    WorldPoint at;
    bool player = false;
};

/// The assist's strength, from AimAssist.
struct AimTuning {
    float coneDeg = 0;   ///< a monster's cone, each side of the aim
    float playerDeg = 0; ///< a player's pull
};
AimTuning aimTuning(AimAssist a);

inline constexpr float kTargetReach = 300;      ///< the soft target's distance (about the view)
inline constexpr float kCloseRange = 50;        ///< within it a monster's cone is twice as wide
inline constexpr float kSwitchMargin = 0.15f;   ///< a new target must score this much better
inline constexpr float kKeepSeconds = 0.3f;     ///< a target kept this long after leaving the cone
inline constexpr float kPlayerKeepSeconds = 0.15f;
inline constexpr float kFlickConeDeg = 60;      ///< a flick takes an enemy within this of its way

/// A candidate's score against the aim (lower is better), nullopt outside its cone or reach.
/// `idle`: the sticks at rest (the aim only the way we last went): a monster within kIdleRange
/// is in whichever way it is.
inline constexpr float kIdleRange = 100;
std::optional<float> coneScore(const AimCandidate& c, WorldPoint from, StickVec dir, const AimTuning& t, float reach,
                               bool idle = false);

/// One frame's look for a target.
struct TargetFrame {
    WorldPoint from;                    ///< us
    StickVec dir{1, 0};                 ///< the aim, one long
    bool idle = false;                  ///< neither stick leans (the aim is the way we last went)
    std::span<const AimCandidate> enemies; ///< the enemies in sight
    float reach = kTargetReach;
    AimAssist assist = AimAssist::Normal;
    LockMode lockMode = LockMode::WhileHeld;
    bool lockHeld = false;              ///< the attack or a slot button is held
    float seconds = 0;                  ///< since the last frame
    std::optional<StickVec> flick;      ///< the right stick flicked this way this frame
    int step = 0;                       ///< TargetNext (+1) / TargetPrevious (-1) this frame
};

/// The target, kept from frame to frame.
class Targeter {
public:
    /// This frame's target (as target()).
    std::optional<std::uint16_t> update(const TargetFrame& f);
    std::optional<std::uint16_t> target() const { return current_; }
    /// Whether it's held by a flick or a step (manual), or by the attack or a slot button held.
    bool locked() const { return manual_ || held_; }
    void clear();

private:
    std::optional<std::uint16_t> current_;
    bool manual_ = false;
    bool held_ = false;
    float outFor_ = 0; ///< seconds the target has been out of its cone
};

/// The right stick: a flick, or a way while it's held.
inline constexpr float kFlickTilt = 0.6f;
inline constexpr float kFlickBack = 0.25f;    ///< back under this: let go
inline constexpr float kFlickSeconds = 0.25f; ///< held longer than this: a way, not a flick
inline constexpr float kAimHoldTilt = 0.3f;   ///< under this the way stays as it was (the stick going home)

struct RightStickFrame {
    std::optional<StickVec> flick; ///< a flick's way this frame
    std::optional<StickVec> aim;   ///< the way it's held, one long, while it's held past a flick's time
};

class RightStick {
public:
    RightStickFrame update(StickVec v, float seconds);
    void clear() { *this = RightStick{}; }
    bool aiming() const { return phase_ == Phase::Aiming; }

private:
    enum class Phase : std::uint8_t { Idle, Pending, Aiming };
    Phase phase_ = Phase::Idle;
    float t_ = 0;
    float peak_ = 0;
    StickVec peakDir_{};
    StickVec way_{};
};

/// The aim's way this frame: the right stick's while it's held, else the left stick's while it
/// leans, else the way last aimed (`last`, which the caller keeps: the way the player last went or
/// aimed, so letting the sticks go never turns the player), else `fallback` (the way the player
/// faces before the pad has leaned). All one long.
StickVec aimWay(std::optional<StickVec> right, StickVec left, std::optional<StickVec> last, StickVec fallback);

/// A spell's (or ability's) aim: on us, at a creature, or at a point.
enum class SpellAim : std::uint8_t { Self, Creature, Point };
/// From its SPEL flags and the slot's on-self flag: on us when the slot says so; at a creature
/// for flag 4 (it flies to one) or 0x80 (only a creature); else at a point.
SpellAim spellAim(std::uint32_t spellFlags, bool onSelf);
/// A warrior's ability: Berserker Charge (1) and the harpoon (3) are aimed, the rest go at once.
SpellAim abilityAim(std::uint32_t ability);
/// Where a cast of that kind goes: the target's spot (the target in the aim's cone), else
/// kCastAhead along the aim; nothing for one on us.
inline constexpr float kCastAhead = 120;
std::optional<WorldPoint> castPoint(SpellAim kind, std::optional<WorldPoint> target, WorldPoint from, StickVec dir);

/// A thing A may take (an item, an NPC, a lever...), by its key.
struct InteractOption {
    std::uint32_t key = 0;
    WorldPoint at;
    float reach = 75;  ///< its hand's reach
    bool talks = false; ///< someone to talk or trade with (an NPC, a shopkeeper)
};
/// The best in front of us (within its own reach): by distance and the angle to the aim, within 120
/// degrees of it (anything within kUnderfoot whatever the angle); someone to talk or trade with
/// within kFacingCone of the aim is preferred (kTalkPreference) over things off to the side; those
/// in `skip` left out unless every one is (a pile taken in turn starts again).
inline constexpr float kUnderfoot = 30;
inline constexpr float kFacingCone = 40;
inline constexpr float kTalkPreference = 0.15f;
std::optional<std::size_t> bestInteract(std::span<const InteractOption> options, WorldPoint from, StickVec dir,
                                        std::span<const std::uint32_t> skip = {});

/// The angle between two ways, in degrees (0 to 180).
float angleBetween(StickVec a, StickVec b);
/// The way from a to b, one long ({1, 0} when they're the same point).
StickVec wayTo(WorldPoint a, WorldPoint b);
float distance(WorldPoint a, WorldPoint b);

} // namespace noxsdl::pad
