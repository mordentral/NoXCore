// A player's layered sprite: the naked body, then armour and weapon layers, as the game's
// PlayerDraw stacks them.
//
//   state      the drawable's AnimInd; WALK with enchantment 31 shows SNEAK
//   frame      from the state's animation; every layer uses the body's frame
//   order      body; then weapons before armour when facing away (AnimDir 0, 1, 2, 3 or 6, except
//              in GREAT_SWORD_STRIKE), armour before weapons otherwise
//   armour     the cloak first when facing south (AnimDir 6-8) outside RUN, AXE_STRIKE,
//              HAMMER_STRIKE and RAISE_SHIELD, else last; the other pieces in class-bit order,
//              shirts hidden under a wizard robe; a shield in order unless facing away (AnimDir 0,
//              3, 6, 7), then after everything (the round shield if both are set)
//   weapons    weapon class bits 2-26 in order (the flag and quiver are drawn apart)
//   colours    the game keeps one set of material colours while drawing the stack. The body sets
//              slots 1-6 to the player's hair, goatee, white, beard, mustache and skin colours
//              (Plr_InitColors + PlayerDraw). An armour or weapon layer whose item was
//              reported equipped then sets them to its modifier.bin definition's COLOR1-6 and
//              recolours the definition's four modifier slots with the item's effectiveness,
//              material and enchantment colours. Otherwise the colours
//              carry over from the layer below.
//   tints      the body is blue (0, 160, 255) while frozen (enchantment 25), and white on odd
//              game frames and blue on even ones while invulnerable (23); a poisoned player
//              (status 0x400) has 100 added to each body colour's green. Armour flashes the same
//              way while invulnerable, weapons while frozen (the game checks 23 and 25 that way
//              round in the armour's and the weapons' draws).
//
// An NPC (thing NPC / NPCWizard, NPCDraw) is drawn the same way from its client record
// (nox::client::NpcEntry), with the local player's drawable's art (NewPlayer's):
//   state      spriteNPCInfo (nox::client::npcPlayerState) turns its monster state into a
//              player state; none of the player's swaps (SNEAK) and, if that state has
//              no frames, nothing at all is drawn (no IDLE in its place)
//   order      the same armour and weapon functions, which test the drawable's own state (the
//              monster's) for the cloak and the weapons-first order (PlayerLook::drawableState)
//   colours    the body's slots 1-6 are the record's six colours in order; while its poison
//              level is exactly 1, each gets 100 more green; frozen and invulnerable as a player
//   overhead   the skull (enchantment 16) and crown (30); no name, no team dot, no flag
// A female NPC (Maiden, MaidenDraw) is a monster drawn with the record's six colours as
// slots 1-6 (maidenMaterials); without a record neither is drawn at all.
#pragma once

#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/draw.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace noxsdl {

enum class LayerSource { Body, Armor, Weapon };

struct PlayerLayer {
    nox::things::ImageRef image;
    LayerSource source = LayerSource::Body;
    std::uint32_t bit = 0; ///< armour or weapon class bit
};

struct PlayerLook {
    std::uint32_t armorEquip = 0;
    std::uint32_t weaponEquip = 0;
    std::uint32_t buffs = 0;
    /// An NPC's: the drawable's own (monster) state, which the layer order tests read while
    /// pose.animState is the player state picked for it (npcPlayerState). Set, the state isn't
    /// swapped for SNEAK nor, without frames, for IDLE (NPCDraw).
    std::optional<std::uint8_t> drawableState;
};

/// The layers to draw, back to front, for a player in `pose`.
std::vector<PlayerLayer> playerLayers(const nox::things::DrawSpec& draw, const Pose& pose, const PlayerLook& look);

/// Material colours for a player's body from their 5 RGB colours (PlayerEntry::colors).
SpriteCache::Materials playerMaterials(const std::array<std::uint8_t, 15>& colors);

/// Material colours for an NPC's body or a female NPC (MaidenDraw): slots 1-6 its six colours.
SpriteCache::Materials maidenMaterials(const nox::client::NpcEntry& npc);

/// Who wears the layers, for their colours: the body's materials (nullopt: white, an unknown
/// player) and the worn things' modifier ids by class bit (null: none known).
struct Wearer {
    std::optional<SpriteCache::Materials> body;
    const std::map<std::uint32_t, std::array<std::uint8_t, 4>>* armorModifiers = nullptr;
    const std::map<std::uint32_t, std::array<std::uint8_t, 4>>* weaponModifiers = nullptr;
};

/// A player's (may be null): their colours, poisoned (status 0x400) 100 greener (PlayerDraw).
Wearer playerWearer(const nox::client::PlayerEntry* player);
/// An NPC's: its six colours, 100 greener while its poison is 1 (NPCDraw).
Wearer npcWearer(const nox::client::NpcEntry& npc);

/// Sets `materials` as the game does for an item: slots 1-6 from its definition, then the
/// definition's four modifier slots from the item's modifier colours (null or 0xFF: none).
/// Returns false, leaving `materials` alone, if the item has no definition.
bool applyItemColours(SpriteCache::Materials& materials, const nox::things::EquipDef* def,
                      const std::array<std::uint8_t, 4>* modifiers, const nox::things::ModifierDb& db);

/// Colours for an item on the ground (WeaponDraw / ArmorDraw), or nullopt if
/// the type isn't drawn with item colours.
std::optional<SpriteCache::Materials> itemMaterials(const TypeArt& art, const std::array<std::uint8_t, 4>* modifiers,
                                                    const nox::things::ModifierDb& db);

/// The material colours each layer is drawn with (see the header comment). `player` may be null
/// (unknown player: white body, no item colours). `buffs` are the drawable's enchantments and
/// `frame` the game frame, for the tints.
std::vector<SpriteCache::Materials> layerMaterials(const std::vector<PlayerLayer>& layers,
                                                   const nox::client::PlayerEntry* player,
                                                   const nox::things::ModifierDb& modifiers, std::uint32_t buffs = 0,
                                                   std::uint32_t frame = 0);
/// The same for any wearer (an NPC's: npcWearer).
std::vector<SpriteCache::Materials> layerMaterials(const std::vector<PlayerLayer>& layers, const Wearer& wearer,
                                                   const nox::things::ModifierDb& modifiers, std::uint32_t buffs = 0,
                                                   std::uint32_t frame = 0);

// --- A map record's look (the map editor's previews: the old community editor's "complex preview") --

/// One thing in an object's inventory, as its map record has it.
struct RecordItem {
    std::string_view type;
    bool worn = false; ///< its header's EQUIPPED flag
    const nox::map::xfer::Modifiers* modifiers = nullptr; ///< a weapon's, armour's, ammo's or flag's
};

/// What the game will draw a map object with, read from its record: an NPC's or maiden's six
/// colours (its xfer) and, for an NPC, the weapons and armour it wears (its inventory's EQUIPPED
/// things, put on in order as the server does at load: npcWearLoaded, NPCEquipWeapon and
/// NPCEquipArmor, which take off what the next one replaces); an item's four modifiers. The client
/// draws the same from the server's reports of them (MSG_REPORT_NPC and the equip reports, an
/// item's modifiers); this makes them straight from the record, so the editor draws what the game
/// will.
struct RecordLook {
    enum class Kind : std::uint8_t {
        Plain,  ///< drawn as it is
        Item,   ///< WeaponDraw / ArmorDraw / FlagDraw: in its definition's colours and its modifiers'
        Maiden, ///< MaidenDraw: its image with materials 1-6 its colours
        Npc,    ///< NPCDraw: NewPlayer's layers in its colours and its worn things'
    };
    Kind kind = Kind::Plain;
    nox::client::NpcEntry npc; ///< Maiden, Npc: as the client's record of it would hold it
    std::optional<std::array<std::uint8_t, 4>> modifiers; ///< Item: its modifier ids (0xFF none)
    /// A hash of all of the above: 0 for Plain (the thumbnails' disk cache keys by it).
    std::uint32_t hash() const;
};

/// The look of an object of type `art` whose record holds `data` and carries `inventory` (in its
/// record order). `things` gives the items' classes, `modifiers` the modifier ids by name.
RecordLook recordLook(const TypeArt& art, const nox::map::xfer::Data& data, std::span<const RecordItem> inventory,
                      const nox::things::ThingDb& things, const nox::things::ModifierDb& modifiers);

/// One image of a look, with the colours it's drawn in (none: as it is).
struct LookLayer {
    nox::things::ImageRef image;
    std::optional<SpriteCache::Materials> colours;
};
/// The images, back to front, the game draws the object with: an NPC standing (its idle monster
/// state turned into the player state NPCDraw draws, npcPlayerState) facing `direction` (an
/// AnimDir), in NewPlayer's layers with layerMaterials' colours; anything else `image` (the
/// caller's pick: its still, a creature's facing), in a maiden's colours (maidenMaterials), an
/// item's (itemMaterials) or as it is. Empty when there is nothing to draw. `arts` is the ThingArt
/// to find NewPlayer in (the caller's thread's).
std::vector<LookLayer> lookLayers(const TypeArt& art, const RecordLook& look, std::optional<nox::things::ImageRef> image,
                                  ThingArt& arts, const nox::things::ModifierDb& modifiers, std::uint8_t direction = 7,
                                  double time = 0);

} // namespace noxsdl
