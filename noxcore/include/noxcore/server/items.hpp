// Items on the server: a player's inventory and equipment, the default outfit, dropping.
//
//   - Given (playerRespawnItem -> Obj_TryPickup -> WeaponPickup /
//     ArmorPickup): the item goes to the front of its inventory and its client hears
//     (MSG_REPORT_MODIFIABLE_PICKUP 0x4C for weapons, armour and flags, else MSG_REPORT_PICKUP
//     0x4B; then its durability, 0x44); a weapon is worn when it has none, armour when nothing
//     of its kind is worn.
//   - Worn (Obj_PlayerEquipWeapon / Obj_PlayerEquipArmor): if its class
//     may (modifier.bin CLASSUSE) and it's strong enough (REQUIRED_STRENGTH); the old one of
//     its kind comes off; EQUIPPED, its class bit in the player's WeaponEquip / ArmorEquip;
//     MSG_REPORT_EQUIP (0x60) to its client and, when asked, its class to everyone (0x50 / 0x51
//     a weapon, 0x4F / 0x52 armour). Clothing (bits 0xC0D) is kept on death. The armour value
//     is the worn armour's ARMOR_VALUE summed, at most 1.
//   - Taken off: MSG_REPORT_DEQUIP (0x61), and when asked the class to everyone (0x54 / 0x53).
//   - Dropped (Obj_DropAt -> DefaultDrop): out of its inventory (taken off,
//     MSG_REPORT_DROP 0x4D), into the world; a weapon or armour can't be picked up for now
//     (NO_COLLIDE) and, unless it respawns, decays in 10 seconds. On death everything but its
//     clothing drops round it (dropAllItems: a spiral of 2 x the biggest item's
//     radius + 6, a little shaken, where a wall doesn't stand between).
//   - The default outfit (playerMakeDefItems, multiplayer): what isn't worn
//     clothing goes; MSG_PLAYER_RESPAWN to everyone (they dress it themselves from that); then
//     a street shirt (not for a warrior), pants and sneakers in its colours (modifier
//     UserColor1 + its colour), and a warrior's Longsword and Wooden Shield, a wizard's robe.
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <string_view>

namespace nox::server {

/// The frames a weapon's READINESS enchantment takes off its wait (itemCheckReadinessEffect:
/// its enchantment slots' ReadinessEffect; 0 for none, or with no modifier.bin).
int readiness(const ServerObject& item, const things::ModifierDb* modifiers);

/// delayedDeleteObject: once; a held thing out of its holder's inventory at once
/// (Obj_RemoveFromInventory: taken off, MSG_REPORT_DROP to a player holder); the lasting spells it
/// cast cancelled (playerCancelSpells); then gone at the frame's end
/// (pendingDelete: World::sweepDeleted does the rest as the exe frees it).
void deleteObject(World& world, ServerObject& o);

struct Items {
    World& world;
    const things::ThingDb& things;
    const things::ModifierDb* modifiers; ///< modifier.bin (null: anyone may use anything)
    std::minstd_rand& rng;
    std::uint32_t frame;

    /// A new item of `type` into the player's inventory, worn if it may be (respawn items:
    /// playerRespawnItem). `everyone` is its last argument, SrvNet_SendReportEquip's
    /// broadcastClass: what it wears told to every client, not its owner alone. The exe's
    /// MakeDefItems passes 0 (defaultItems), startQuest's class item and OblivionGive 1.
    /// `prepare` (noxcpp, a mode's): the new item set up after its modifiers, before it's placed
    /// or told to anyone; false: it isn't given (deleted, null returned).
    ServerObject* give(ServerObject& player, std::string_view type, std::array<std::uint8_t, 4> mods = {0xFF, 0xFF, 0xFF, 0xFF},
                       bool everyone = false, const std::function<bool(ServerObject&)>& prepare = {});
    /// Picks `item` up off the ground (Obj_TryPickup with its PICKUP function): its
    /// class must be allowed it, and it must have room (twice its carry capacity, three of a
    /// food); then it leaves the world, goes in the inventory and is worn if it may be, its
    /// class told to everyone when `everyone`.
    bool pickUp(ServerObject& player, ServerObject& item, bool everyone);
    /// The item's own PICKUP function alone (what Obj_TryPickup calls once its checks pass; a
    /// shop's sale calls it directly: Shop_OnBuyItem).
    bool pickupFunction(ServerObject& player, ServerObject& item, bool everyone);
    /// DefaultPickup alone: not another team's (outside a quest), not held, its
    /// weight and the three-of-a-kind limit; then out of the world, into the inventory.
    bool pickUpDefault(ServerObject& player, ServerObject& item, bool everyone);
    /// MSG_TRY_DROP (Obj_TryDropAt): at (x, y), at most 75 from it and not through a
    /// wall; quest items stay ("drop.c:CantDropThat").
    bool tryDrop(ServerObject& player, ServerObject& item, float x, float y);
    /// MSG_TRY_EQUIP / MSG_TRY_DEQUIP (Plr_TryEquip / Plr_TryDequip), told to
    /// everyone.
    bool tryEquip(ServerObject& player, ServerObject& item);
    bool tryDequip(ServerObject& player, ServerObject& item);
    /// Obj_PlayerEquipWeapon / Obj_PlayerEquipArmor: another class's, or one
    /// it isn't strong enough for, is refused with its line ("weapon.c:WeaponEquipClassFail",
    /// "...StrengthFail", the armour's alike), and 0x39D to the player alone when `failSound`.
    bool equipWeapon(ServerObject& player, ServerObject& item, bool tellEveryone, bool failSound = false);
    bool equipArmor(ServerObject& player, ServerObject& item, bool tellEveryone, bool failSound = false);
    /// playerCheckStrength: a player's strength at least its weapon's
    /// or armour's record's required strength; no record, not strong enough. (No records loaded, ours only:
    /// strong enough.)
    bool strongEnough(const ServerObject& player, const ServerObject& item) const;
    /// Takes it off (if worn); `tellOwner` / `tellEveryone` as the exe's flags.
    void takeOff(ServerObject& player, ServerObject& item, bool tellOwner, bool tellEveryone);
    /// The quiver worn comes off.
    void takeOffQuiver(ServerObject& player, bool tellOwner, bool tellEveryone);
    /// Out of its inventory onto the ground at (x, y) (Obj_DropAt with DefaultDrop; a CTF
    /// flag's drop told: flagDropped); whether it dropped (the DROP function's result).
    bool drop(ServerObject& player, ServerObject& item, float x, float y);
    /// MSG_TRY_USE (Obj_UseItem): its USE. PotionUse (by its subclass, health its
    /// number x the class's multiplier when short of it, mana the same, poison cured, Haste,
    /// invisibility, Shield cast, the protections, invulnerability, infravision, vampirism for
    /// 120 s at power 3; used up only when it did something, vampirism always); ConsumeUse
    /// (useConsume: food, not a potion, when short of health; its munch by its kind and the
    /// eater's sex); ConsumeConfuseUse (confused 5 s, "Use.c:CiderConfuse", then as food);
    /// MushroomUse (poison cured "Use.c:MushroomClean" or "Use.c:MushroomConfuse",
    /// confused 300 frames); the books', guides' and signs' (books.hpp). Wands': with those.
    void use(ServerObject& player, ServerObject& item);
    /// DefaultDrop alone: out of the inventory, into the world at (x, y).
    bool dropDefault(ServerObject& player, ServerObject& item, float x, float y);
    /// Out of its inventory and nowhere (Obj_RemoveFromInventory): taken off (in CTF a
    /// flag's weapon bit 1 off, MSG_REPORT_DEQUIP to everyone), MSG_REPORT_DROP to its client, no
    /// longer its holder's or owned by it. Its holder's things are told to everyone but a dead
    /// one's weapons, armour and flags outside a quest.
    void removeFromInventory(ServerObject& player, ServerObject& item);
    /// A thing out of an inventory into the world at (x, y) (createAt: flags but
    /// the type's and ACTIVE cleared, made this frame).
    void place(ServerObject& item, float x, float y);
    /// SrvNet_SendReportEquipClass to everyone: its holder's net code (bit 15 for a
    /// player), its weapon or armour bits, the modifiers if it has any (0x51 / 0x52; 0x50 / 0x4F
    /// without).
    void reportEquip(const ServerObject& player, const ServerObject& item);
    /// noxcpp (World::keepThingsOnDeath): a dead player's worn weapons and armour but the street
    /// clothes, what every client forgets at MSG_PLAYER_DIED outside a quest, reported to everyone
    /// again (reportEquip, the inventory's order), as to a player joining.
    void reportWornAgain(const ServerObject& player);
    /// A dead player's things round it. `piecesOnly` (noxcpp, World::keepThingsOnDeath): only the
    /// game's pieces it carries, a flag or a crown; the rest kept.
    void dropAll(ServerObject& player, bool piecesOnly = false);
    /// The default outfit, after a respawn or on joining.
    void defaultItems(ServerObject& player);
    /// Into its inventory, as a pickup is (MSG_REPORT_PICKUP; worn if it may be): a caught
    /// chakram coming back (inventoryPutImpl then Obj_PlayerEquipWeapon).
    void receive(ServerObject& player, ServerObject& item) { put(player, item, true); }
    /// Worn `amount` points (equipDamage, or playerDamageWeapon
    /// for `weapon`: not a weapon of subclass 0x7800000), `holder`'s, from `src` / `obj3` with
    /// `type`: only with HealthData; its material's DEFENDEFFECT on the amount
    /// (DurabilityMultiplierEffect); the fraction carried to the next time, the whole points by
    /// its own DAMAGE (WeaponDamage / ArmorDamage, DefaultDamage's: at 0 its DIE, WeaponDie /
    /// ArmorDie, gone through deleteObject: MSG_REPORT_DROP); a player holder told its health
    /// (MSG_REPORT_ITEM_HEALTH 0x44) in solo games, else when it passes a quarter (itemDestroyed with
    /// the health band's full / half / quarter / less). True if it broke (deleted).
    bool wear(ServerObject& holder, ServerObject& item, double amount, ServerObject* src, ServerObject* obj3, int type, bool weapon);
    /// An item's whole name as the server words it: its effectiveness's and
    /// material's words, its definition's name, its enchantments' words, a space between each
    /// (not an item: "objutil.c:NoDescription"; no definition: "objutil.c:NoInfo" with its type).
    std::u16string fullName(const ServerObject& item) const;
    /// A weapon's or armour's DIE (WeaponDie, ArmorDie): its holder told it
    /// broke ("die.c:WeaponDieMetal", "die.c:ArmorDieClothPlural"... by its material, the
    /// plural when its name ends in s, with its whole name), its breaking sound by its material.
    void breakNotice(const ServerObject& item);
    /// inventoryPutImpl (and a player's pickup's wearing): item into holder's
    /// inventory, at its head, holder its owner; its client told; a player's worn when it may be.
    void put(ServerObject& holder, ServerObject& item, bool everyone);
    /// inventoryPutImpl alone: into holder's inventory at its head, holder its
    /// owner, its client told when `report` (MSG_REPORT_PICKUP), a key's jingle; nothing worn.
    void inventoryPut(ServerObject& holder, ServerObject& item, bool report);
    /// playerClassCanUseItem: whether player's class may use item.
    bool classMay(const ServerObject& player, const ServerObject& item) const;
    /// modifSetItemAttrs: an item's four modifiers (0xFF none) set: none at all
    /// leaves it be (but for a weapon of subclass 0x47F0000); a weapon, armour or flag (class
    /// 0x13001000) or a TeamBase takes them, and the players that know it are owed its
    /// MSG_REPORT_MODIFIER, every player for the always-sent (0x20400004).
    static void setModifiers(ServerObject& item, const std::array<std::uint8_t, 4>& mods);

    /// Obj_DropAt as the game has it (drop: then a mode told, ModeHooks::dropped); a lost chakram's
    /// from its missile too (Missiles::chakramDrops).
    bool dropAt(ServerObject& player, ServerObject& item, float x, float y);

private:
    void tellString(const ServerObject& player, std::string_view id);
    /// SrvNet_SendReportCharges (MSG_REPORT_CHARGES 0x64, important): a held item's charges and
    /// most, to its holder.
    void reportCharges(const ServerObject& player, const ServerObject& item);
    void tell(const ServerObject& player, std::vector<std::uint8_t> bytes);
    void tellEveryone(std::vector<std::uint8_t> bytes);
    void recalculateArmor(ServerObject& player);
};

} // namespace nox::server
