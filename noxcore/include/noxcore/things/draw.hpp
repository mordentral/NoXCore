// Decoding a THNG's DRAW payload into the images it draws with.
//
// The draw type names a row of the exe's draw table (name, draw fn, payload kind,
// parse fn); the payload kind picks the layout:
//
//   Static        StaticDraw WeaponDraw ArmorDraw BaseDraw
//                 one image
//   Random        StaticRandomDraw DoorDraw SlaveDraw BoulderDraw ArrowDraw WeakArrowDraw
//                 HarpoonDraw
//                 u8 count, count images (a random pick, a door's states, a direction each)
//   Animate       AnimateDraw GlyphDraw WeaponAnimateDraw ArmorAnimateDraw FlagDraw
//                 SphericalShieldDraw SummonEffectDraw
//                 one Animation
//   Conditional   ConditionalAnimateDraw MonsterGeneratorDraw
//                 u8 count, count Animations
//   Vector        VectorAnimateDraw ReleasedSoulDraw
//                 one DirectionalAnimation
//   Monster       MonsterDraw MaidenDraw
//                 until "END ": u32 tag, u8 state (< 16), string8 name, string8 sound,
//                 DirectionalAnimation
//   AnimateState  AnimateStateDraw
//                 until "END ": u32 tag, u32 flags (2, 4 or 8 pick slot 0, 1, 2),
//                 string8 name, string8 sound, Animation
//   Player        PlayerDraw
//                 u32 tag ("END " = none), then per state: string8 state name (a
//                 kPlayerStates entry), the Animation header (u8 frames, u8 frameDelay,
//                 string8 kind), then parts until a "STAT" tag (next state) or "END ":
//                 u32 tag, string8 part ("NAKED", a kArmorParts or a kWeaponParts name),
//                 8 directions x frames images as in DirectionalAnimation
//   None          every other draw type: code-drawn effects without images
//
// The kind is the payload's layout only: BoulderDraw (a Random payload) and MonsterGeneratorDraw
// (a Conditional one) pick their image from the drawable's state (client/state_draw.hpp).
//
// Animation: u8 frames, u8 frameDelay, string8 kind, frames x image.
// DirectionalAnimation (loadVectorAnimated): the same header, then frames x image for each of
// 8 directions in the order NW, N, NE, W, E, SW, S, SE (the game's 3 x 3 direction grid
// without its centre).
#pragma once

#include "noxcore/things/thing_db.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {

enum class DrawKind { None, Static, Random, Animate, Conditional, Vector, Player, Monster, AnimateState };

/// Grid directions in file order.
enum class GridDirection { NW, N, NE, W, E, SW, S, SE };

struct DirectionalAnimation {
    std::uint8_t frameDelay = 0;
    std::string kind;
    std::array<std::vector<ImageRef>, 8> directions; // indexed by GridDirection
    AnimPlayback playback = AnimPlayback::Other; ///< `kind`, named
};

struct MonsterState {
    std::string name; // "NULL" in the retail file; kMonsterStates names the state number
    std::string sound;
    DirectionalAnimation animation;
};

/// Monster animation states by number (the u8 before each MonsterDraw state). The exe's table,
/// which GAME.EXE itself never reads; the retail file matches it (EmberDemon has 1 ATTACK,
/// 3 ATTACK_FAR, 8 IDLE, 9 DIE, 12 MOVE, 13 MOVE_2).
inline constexpr std::array<std::string_view, 16> kMonsterStates = {
    "SPECIAL_3", "ATTACK", "ATTACK_FINISH", "ATTACK_FAR", "ATTACK_FAR_FINISH", "DEFEND", "DEFEND_FINISH",
    "CAST_SPELL", "IDLE", "DIE", "DEAD", "HURT", "MOVE", "MOVE_2", "SPECIAL_1", "SPECIAL_2"};

/// Player animation states by index (the drawable's AnimInd; the exe's table).
inline constexpr std::array<std::string_view, 55> kPlayerStates = {
    "IDLE", "DIE", "DEAD", "JUMP", "WALK", "WALK_AND_DRAG", "RUN", "RUNNING_JUMP", "PICKUP",
    "DODGE_LEFT", "DODGE_RIGHT", "ELECTROCUTED", "FALL", "TRIP", "GET_UP", "LAUGH", "POINT", "SIT",
    "SLEEP", "TALK", "TAUNT", "CAST_SPELL", "CONCENTRATE", "PUNCH_LEFT", "PUNCH_RIGHT",
    "PUNCH_RIGHT_HOOK", "MACE_STRIKE", "SWORD_STRIKE", "LONG_SWORD_STRIKE", "STAFF_STRIKE",
    "STAFF_BLOCK", "STAFF_SPELL_BLAST", "STAFF_THRUST", "SHOOT_BOW", "SHOOT_CROSSBOW", "AXE_STRIKE",
    "GREAT_SWORD_PARRY", "GREAT_SWORD_STRIKE", "GREAT_SWORD_IDLE", "HAMMER_STRIKE", "RAISE_SHIELD",
    "RECOIL_FORWARD", "RECOIL_BACKWARD", "RECOIL_SHIELD", "CHAKRAM_STRIKE", "BERSERKER_CHARGE",
    "WARCRY", "GREAT_SWORD_BLOCK_LEFT", "GREAT_SWORD_BLOCK_RIGHT", "GREAT_SWORD_BLOCK_DOWN",
    "ELECTRIC_ZAP", "DUST", "RECOIL", "SNEAK", "HARPOONTHROW"};

/// Armour layers, by armour class bit (1 << index; the exe's table).
inline constexpr std::array<std::string_view, 26> kArmorParts = {
    "STREET_SNEAKERS", "MEDIEVAL_CLOAK", "STREET_PANTS", "MEDIEVAL_PANTS", "LEATHER_LEGGINGS",
    "CHAIN_LEGGINGS", "LEATHER_BOOTS", "LEATHER_ARMORED_BOOTS", "PLATE_BOOTS", "PLATE_LEGGINGS",
    "STREET_SHIRT", "MEDIEVAL_SHIRT", "LEATHER_ARMBANDS", "PLATE_ARMS", "WIZARD_ROBE", "LEATHER_TUNIC",
    "CHAIN_TUNIC", "PLATE_BREAST", "CHAIN_COIF", "WIZARD_HELM", "CONJURER_HELM", "LEATHER_HELM",
    "PLATE_HELM", "ORNATE_HELM", "ROUND_SHIELD", "KITE_SHIELD"};

/// Weapon layers, by weapon class bit (1 << index; the exe's table).
inline constexpr std::array<std::string_view, 27> kWeaponParts = {
    "FLAG", "QUIVER", "BOW", "CROSSBOW", "ARROW", "BOLT", "CHAKRAM", "SHURIKEN", "SWORD",
    "LONG_SWORD", "GREAT_SWORD", "MACE", "AXE", "OGRE_AXE", "HAMMER", "STAFF",
    "STAFF_SULPHOROUS_FLARE", "STAFF_SULPHOROUS_SHOWER", "STAFF_LIGHTNING", "STAFF_FIREBALL",
    "STAFF_TRIPLE_FIREBALL", "STAFF_FORCE_OF_NATURE", "STAFF_DEATH_RAY", "STAFF_OBLIVION_HALBERD",
    "STAFF_OBLIVION_HEART", "STAFF_OBLIVION_WIERDLING", "STAFF_OBLIVION_ORB"};

/// One layer of a player state: frames per direction (GridDirection order).
using PlayerPart = std::array<std::vector<ImageRef>, 8>;

struct PlayerState {
    std::uint8_t frames = 0;
    std::uint8_t frameDelay = 0;
    std::string kind;
    AnimPlayback playback = AnimPlayback::Other; ///< `kind`, named
    std::optional<PlayerPart> naked;
    std::array<std::optional<PlayerPart>, kArmorParts.size()> armor;
    std::array<std::optional<PlayerPart>, kWeaponParts.size()> weapons;
};

struct DrawSpec {
    DrawKind kind = DrawKind::None;
    std::vector<ImageRef> images;       // Static (one), Random
    std::vector<Animation> animations;  // Animate (one), Conditional
    std::optional<DirectionalAnimation> directional; // Vector
    std::array<std::optional<MonsterState>, 16> monsterStates; // Monster, by state number
    std::array<std::optional<Animation>, 3> stateAnimations;   // AnimateState, by slot
    std::array<std::string, 3> stateNames;                      // AnimateState, as stored
    std::vector<std::optional<PlayerState>> playerStates;       // Player, by kPlayerStates index

    /// A representative still image: the first image, first frame, or south-facing frame.
    std::optional<ImageRef> firstImage() const;
};

/// The payload kind for a draw type name (DrawKind::None for unknown names).
DrawKind drawKind(std::string_view type);

/// Decodes a DRAW attribute. Throws FormatError if the payload doesn't match its kind exactly.
DrawSpec decodeDraw(const ThingDraw& draw);

} // namespace nox::things
