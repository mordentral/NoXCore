// Items on the server (items.hpp): picking things up (each PICKUP function's checks), wearing and
// taking off weapons and armour, dropping (each DROP), using (potions, food), an item's wear and
// breaking, the death's drop and the default outfit. What each client is told goes out through
// World::broadcasts.
#include "noxcore/server/items.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/books.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/item_effects.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/x87.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

constexpr std::uint32_t kModifiable = 0x13001000u; // (weapons, wands, armour, flags)
constexpr std::uint32_t kClothing = 0xC0Du;        // (armour bits kept on death)

using util::hi;
using util::lo;

using util::chopToFloat;

// Whether any of its four modifier slots is set (0xFF: none).
bool hasModifiers(const ServerObject& item)
{
    return std::ranges::any_of(item.modifiers, [](std::uint8_t m) { return m != 0xFF; });
}

} // namespace

int readiness(const ServerObject& item, const things::ModifierDb* modifiers)
{
    if (!modifiers || !(item.classes & kModifiable)) {
        return 0;
    }
    for (std::size_t slot = 2; slot < 4; ++slot) {
        if (const things::ModifierDef* m = modifiers->modifier(item.modifiers[slot]); m && m->attack.fn == things::EffectFn::Readiness) {
            return m->attack.count; // (its ATTACKEFFECT's int)
        }
    }
    return 0;
}

// A reliable message for this player's client alone (nothing for a monster: no client).
void Items::tell(const ServerObject& player, std::vector<std::uint8_t> bytes)
{
    if (!player.player) { // (a monster's: no client)
        return;
    }
    world.broadcasts().push_back({std::move(bytes), true, player.extent});
}

void Items::reportCharges(const ServerObject& player, const ServerObject& item)
{
    // (SrvNet_SendReportCharges: its net code, charges, most; the exe keeps a staff's and a
    // quiver's in different places in their use data, ours in one pair.)
    tell(player, {0x64, lo(item.extent), hi(item.extent), item.charges, item.maxCharges});
}

void Items::tellEveryone(std::vector<std::uint8_t> bytes)
{
    world.broadcasts().push_back({std::move(bytes), true, 0});
}

bool Items::classMay(const ServerObject& player, const ServerObject& item) const
{
    if (!modifiers || !player.player) {
        return true;
    }
    if (player.player->multiclass) {
        return true; // (noxcpp: a multiclass player, a mode's, wields any class's weapons and wears any armour)
    }
    const things::EquipDef* def = (item.classes & oc::Armor) ? modifiers->findArmor(item.typeName) : modifiers->findWeapon(item.typeName);
    return !def || !def->classUse || (*def->classUse & (1u << player.player->classId)) != 0;
}

void Items::setModifiers(ServerObject& item, const std::array<std::uint8_t, 4>& mods)
{
    const bool none = std::ranges::all_of(mods, [](std::uint8_t m) { return m == 0xFF; });
    if (none && !((item.classes & oc::Wand) && (item.subclasses & 0x47F0000u))) { // (a staff: class 0x1000)
        return;
    }
    if (!(item.classes & 0x13001000u) && item.typeName != "TeamBase") {
        return;
    }
    item.modifiers = mods;
    item.touch(); // (unitNeedSync)
    item.modifierOwed = (item.classes & 0x20400004u) ? 0xFFFFFFFFu : (item.modifierOwed | item.knownBy);
}

void Items::recalculateArmor(ServerObject& player)
{
    // (recalculateArmorVal: each worn piece's value, itemApplyDefendEffect: its ARMOR_VALUE by
    // its effectiveness)
    float total = 0;
    for (const std::uint32_t e : player.inventory) {
        const ServerObject* item = world.byExtent(e);
        if (item && (item->classes & oc::Armor) && (item->flags & ObjFlag::Equipped)) {
            total = (util::X87(armorValueOf(modifiers, *item)) + total).f(); // (FADD the local, FSTP)
        }
    }
    player.player->armorValue = std::min(total, 1.0f);
}

ServerObject* Items::give(ServerObject& player, std::string_view type, std::array<std::uint8_t, 4> mods, bool everyone,
                          const std::function<bool(ServerObject&)>& prepare)
{
    if (!player.player || things.findThing(type) == nullptr) {
        return nullptr;
    }
    ServerObject& item = world.spawn(things, type, player.x, player.y);
    setModifiers(item, mods);
    if (prepare && !prepare(item)) { // (noxcpp: a mode's own set-up, before anyone hears of it)
        item.pendingDelete = true;
        return nullptr;
    }
    // (Obj_TryPickup: weapons and armour only for a class that may; DefaultPickup's weight and
    // team checks don't bite a fresh outfit.)
    if ((item.classes & (oc::Weapon | oc::Wand | oc::Armor)) && !classMay(player, item)) {
        item.pendingDelete = true;
        return nullptr;
    }
    put(player, item, everyone);
    item.flags &= ~0x80000u; // (no longer RESPAWN)
    if (item.classes & 0x3001000u) {  // a weapon, armour or wand
        item.itemFlags |= 1u;
    }
    return &item;
}

bool Items::pickUp(ServerObject& player, ServerObject& item, bool everyone)
{
    // Obj_TryPickup: a creature (a player or a monster) with room, not dead; a live
    // item.
    if (!(player.classes & (oc::Player | oc::Monster)) || player.carryCapacity == 0 || item.gone() || (item.flags & ObjFlag::Destroyed) ||
        (player.flags & ObjFlag::Dead) || item.holder != 0 || world.disabledTypes.contains(item.typeName)) {
        return false;
    }
    if (world.modeHooks && !world.modeHooks->mayPickUp(world, player, item)) {
        return false; // (noxcpp: a mode's to leave)
    }
    const float fromX = item.x;
    const float fromY = item.y;
    if (!pickupFunction(player, item, everyone)) {
        return false;
    }
    // Taken: its pickup handler, when its function is in the script (index not -1), called with
    // the taker and the thing (scriptCallByEventBlock: not if it has run), then
    // none (index -1) whether it ran or not.
    if (world.script && world.script->resolves(item.onPickup)) {
        world.script->callBlock(item.onPickup, player.extent, item.extent);
        item.onPickup.function.clear();
    }
    if (world.modeHooks) {
        world.modeHooks->pickedUp(world, player, item, fromX, fromY); // (noxcpp: told a mode)
    }
    return true;
}

bool Items::pickupFunction(ServerObject& player, ServerObject& item, bool everyone)
{
    // (the block on using things while a player's .plr is applied: in use().)
    if (item.pickup == "FoodPickup") {
        // pickupFood: not a potion's kind (subclass 0x84), used at once (its USE:
        // ConsumeUse eats it when short of health); gone, picked up all the same; else
        // DefaultPickup, with its sound by kind or material (the exe's table: flesh 0x342, subclass 2
        // 0x344, 4 0x340, 0x80 0x346; the first that fits).
        if (!(item.subclasses & 0x84u)) {
            use(player, item);
        }
        if (item.pendingDelete) {
            return true;
        }
        if (!pickUpDefault(player, item, everyone)) {
            return false;
        }
        struct Kind {
            std::uint32_t subclasses;
            std::uint16_t material;
            std::uint16_t sound;
        };
        static constexpr Kind kKinds[] = {{0, 1, 0x342}, {2, 0, 0x344}, {4, 0, 0x340}, {0x80, 0, 0x346}};
        for (const Kind& k : kKinds) {
            if ((item.subclasses & k.subclasses) || (item.material & k.material)) {
                playSound(world, k.sound, player);
                break;
            }
        }
        return true;
    }
    if (item.pickup == "GoldPickup") {
        // GoldPickup: a player's gold up by the pile's (InitData), the pile gone,
        // "GoldPickup" with the amount, 0x133; anything else DefaultPickup (0x133 when it took it).
        if (player.classes & oc::Player) {
            addGold(player, static_cast<std::int32_t>(item.goldAmount));
            item.pendingDelete = true;
            const std::string amount = std::to_string(item.goldAmount);
            world.textTo(player, "pickup.c:GoldPickup", std::u16string(amount.begin(), amount.end()));
            playSound(world, 0x133, player);
            return true;
        }
        if (!pickUpDefault(player, item, everyone)) {
            return false;
        }
        playSound(world, 0x133, player);
        return true;
    }
    if (item.pickup == "AnkhTradablePickup") {
        // AnkhTradablePickup: a player's life, the ankh gone, 0x3EC.
        if (!(player.classes & oc::Player) || !player.player) {
            return false;
        }
        ++player.player->questLives;
        item.pendingDelete = true;
        playSound(world, 0x3EC, player);
        return true;
    }
    if (item.pickup == "PotionPickup") {
        // pickupPotion: in a network game (not a quest) only what a player's class may
        // use ("pickup.c:ObjectEquipClassFail"); drunk at once when it doesn't overfill: health
        // (its number, a player's by its class's multiplier) while short of more than that, mana
        // the same (from the health's amount when both), poison cured (a player's); drunk, gone;
        // else no longer decaying and DefaultPickup. Sounds: 0x39D refused, 0x2F2 health, 0x2F3
        // mana, Cure Poison's on, 0x340 picked up.
        if ((world.gameFlags & GameFlag::Network) && !world.quest() && player.player && !classMay(player, item)) {
            tellString(player, "pickup.c:ObjectEquipClassFail");
            playSound(world, 0x39D, player, 2, player.extent);
            return false;
        }
        const int cls = player.player ? std::min<int>(player.player->classId, 2) : 0;
        int amount = item.useArg;
        bool drunk = false;
        if ((item.subclasses & 0x10u) && player.maxHealth != 0) {
            if (player.player) {
                // (FILD, FMUL the class's multiplier, FSTP, Math_FloatToInt)
                amount = util::floatToInt(util::X87::fromDouble(amount) * world.healthMultiplier[static_cast<std::size_t>(cls)]);
            }
            if (amount + player.health < player.maxHealth) {
                adjustHealth(world, player, amount);
                playSound(world, 0x2F2, player);
                drunk = true;
            }
        }
        if ((item.subclasses & 0x20u) && player.player) {
            PlayerControl& p = *player.player;
            amount = util::floatToInt(util::X87::fromDouble(amount) * world.manaMultiplier[p.multiclass ? 2u : static_cast<std::size_t>(cls)]);
            if (amount + p.mana < p.maxMana) {
                p.mana = static_cast<std::uint16_t>(std::clamp(p.mana + amount, 0, static_cast<int>(p.maxMana)));
                playSound(world, 0x2F3, player);
                drunk = true;
            }
        }
        if (((item.subclasses & 0x40u) && player.player && player.poison != 0) || drunk) {
            if (item.subclasses & 0x40u && player.player && player.poison != 0) {
                Fight{world, rng, frame, &things, modifiers}.removePoison(player);
                playSound(world, spellSound(things, 0x0E, 1), player); // (Cure Poison's on)
            }
            item.flags |= ObjFlag::Destroyed;
            item.pendingDelete = true;
            return true;
        }
        if (item.decayAt != 0) {
            item.timer = item.decayAt; // (its timer left at the decay's end)
        }
        item.decayAt = 0;
        if (!pickUpDefault(player, item, everyone)) {
            return false;
        }
        playSound(world, 0x340, player);
        return true;
    }
    if (item.pickup == "SpellBookPickup" || item.pickup == "AbilityBookPickup") {
        return bookPickup(*this, player, item, everyone); // (pickupSpellbook / pickupAbilitybook: books.hpp)
    }
    // WeaponPickup / ArmorPickup: only what its class may use. (OblivionPickup is
    // WeaponPickup, then its line, the level-up moment's OblivionUp and worn: books.hpp.)
    // Not in solo or quest games; refused with sound 0x39D to the player alone (Obj_WeaponPickup
    // Obj_WeaponPickup, pickupArmor).
    if (item.pickup == "AmmoPickup" && player.player && (things::weaponBitOf(item.typeName) & 0x82u)) {
        // pickupAmmo: a player's quiver (weapon bits 0x82) joins one it carries of
        // the same type and modifiers, not endless, the two holding under 251
        // together: its arrows and its most added to that one's (MSG_REPORT_CHARGES), it gone
        // (delayedDeleteObject), the pickup's sound. Else
        // Obj_WeaponPickup.
        const std::uint32_t bits = things::weaponBitOf(item.typeName);
        for (const std::uint32_t e : player.inventory) {
            ServerObject* o = world.byExtent(e);
            if (!o || o->type != item.type || !(o->classes & oc::Weapon) || !(things::weaponBitOf(o->typeName) & bits) ||
                o->modifiers != item.modifiers || o->infiniteAmmo || 0xFB <= o->maxCharges + item.maxCharges) {
                continue;
            }
            o->charges = static_cast<std::uint8_t>(o->charges + item.charges);
            o->maxCharges = static_cast<std::uint8_t>(o->maxCharges + item.maxCharges);
            tell(player, {0x64, lo(o->extent), hi(o->extent), o->charges, o->maxCharges});
            item.flags |= ObjFlag::Destroyed;
            item.pendingDelete = true;
            weaponPickupSound(world, player, item);
            return true;
        }
    }
    const bool weaponPickup = item.pickup == "WeaponPickup" || item.pickup == "OblivionPickup" || item.pickup == "AmmoPickup";
    const auto carriesSame = [&] { // (the same type and modifiers)
        return std::ranges::any_of(player.inventory, [&](std::uint32_t e) {
            const ServerObject* o = world.byExtent(e);
            return o && o->type == item.type && (!(o->classes & kModifiable) || o->modifiers == item.modifiers);
        });
    };
    if (item.pickup == "ArmorPickup" && !(world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && (world.serverOptions & 2u) && carriesSame()) {
        // pickupArmor's first: under server option 2 no second of an armour it carries.
        tellString(player, "armor.c:CannotPickupDuplicateArmor");
        playSound(world, 0x39D, player, 2, player.extent);
        return false;
    }
    if (weaponPickup) {
        // Obj_WeaponPickup's first: a quest player's Force of Nature staves (subclass 0x200000) at
        // most ForceOfNatureStaffLimit ("pickup.c:MaxSameItem", 0x39D).
        if ((player.classes & oc::Player) && world.quest() && (item.subclasses & 0x200000u)) {
            const auto most = static_cast<int>(world.number("ForceOfNatureStaffLimit")); // (Math_FloatToInt)
            const auto held = std::ranges::count_if(player.inventory, [&](std::uint32_t e) {
                const ServerObject* o = world.byExtent(e);
                return o && o->type == item.type && !(o->flags & ObjFlag::Destroyed); // (inventoryCountObjects)
            });
            if (most <= held) {
                tellString(player, "pickup.c:MaxSameItem");
                playSound(world, 0x39D, player);
                return false;
            }
        }
        // Then, out of solo and quest games under server option 2, no second of a weapon it
        // carries (the same type and modifiers; not ammunition,
        // subclass 0x82), nor a chakram (subclass 0x40) while one it owns is about:
        // "weapon.c:CannotPickupDuplicateWeapon", 0x39D to it alone (a creature: refused quietly).
        if (!(world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && (world.serverOptions & 2u)) {
            bool duplicate = !(item.subclasses & 0x82u) && carriesSame();
            if (!duplicate && (item.subclasses & 0x40u)) {
                duplicate = std::ranges::any_of(world.objects(), [&](const auto& e) {
                    return e.second.owner == player.extent && (e.second.classes & oc::Weapon) && (e.second.subclasses & 0x40u);
                });
            }
            if (duplicate) {
                if (!(player.classes & oc::Player)) {
                    return false;
                }
                tellString(player, "weapon.c:CannotPickupDuplicateWeapon");
                playSound(world, 0x39D, player, 2, player.extent);
                return false;
            }
        }
    }
    if ((weaponPickup || item.pickup == "ArmorPickup") && !(world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) &&
        !classMay(player, item)) {
        tellString(player, weaponPickup ? "weapon.c:WeaponEquipClassFail" : "armor.c:ArmorEquipClassFail");
        playSound(world, 0x39D, player, 2, player.extent);
        return false;
    }
    // TrapPickup (pickupTrap): only its own (up its owners); refused, a player hears
    // 0x39D alone; picked up, 0x338.
    if (item.pickup == "TrapPickup") {
        bool own = false;
        for (const ServerObject* o = &item; o && !own; o = o->owner ? world.byExtent(o->owner) : nullptr) {
            own = o == &player;
        }
        if (!own) {
            if (player.classes & oc::Player) {
                playSound(world, 0x39D, player, 2, player.extent);
            }
            return false;
        }
        if (!pickUpDefault(player, item, everyone)) {
            return false;
        }
        playSound(world, 0x338, player);
        return true;
    }
    if (item.pickup == "CrownPickup") {
        return pickupCrown(*this, player, item); // (Obj_PickupCrown: modes.hpp)
    }
    if (!pickUpDefault(player, item, everyone)) {
        return false;
    }
    // Their sounds at the taker, by kind and material (ArmorPickup's end); an
    // AudEventPickup's its named one (objectPickupAudEvent: "AudEventPickup KeyPickup").
    if (weaponPickup) {
        weaponPickupSound(world, player, item);
        if (item.pickup == "OblivionPickup") {
            oblivionPickedUp(*this, player, item);
        }
    } else if (item.pickup == "ArmorPickup") {
        armorPickupSound(world, player, item);
    } else if (item.pickup.starts_with("AudEventPickup ")) {
        playSound(world, std::string_view(item.pickup).substr(15), player);
    }
    return true;
}

bool Items::pickUpDefault(ServerObject& player, ServerObject& item, bool everyone)
{
    if (item.holder != 0) {
        return false;
    }
    // DefaultPickup: outside a quest not another team's (its client told the team's
    // colour, MSG_INFORM 0x10); twice its carry capacity at most; three of a food or potion
    // (nine in the campaign or a quest).
    if (!world.quest() && item.team != 0 && !(player.team != 0 && player.team == item.team)) {
        if (const ServerTeam* t = teamById(world, item.team)) {
            tell(player, {0xA9, 0x10, t->color, 0, 0, 0});
            return false;
        }
    }
    int carried = 0;
    for (const std::uint32_t e : player.inventory) {
        if (const ServerObject* o = world.byExtent(e)) {
            carried += o->weight;
        }
    }
    if (player.carryCapacity * 2 - carried < item.weight) {
        tellString(player, "pickup.c:CarryingTooMuch");
        return false;
    }
    if (item.classes & oc::Food) {
        const auto same = std::ranges::count_if(player.inventory, [&](std::uint32_t e) {
            const ServerObject* o = world.byExtent(e);
            return o && o->type == item.type;
        });
        if (same >= ((world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) ? 9 : 3)) {
            tellString(player, "pickup.c:MaxSameItem");
            return false;
        }
    }
    // Out of the world (servFinalizeDelObject: the clients that know it hear
    // it's gone), into the inventory, worn if it may be (its class told to everyone).
    world.removed().push_back(item.extent);
    item.flags &= ~ObjFlag::Active;
    // No longer decaying (held), its timer left at the
    // decay's end (what a save's header writes, less the frame).
    if (item.decayAt != 0) {
        item.timer = item.decayAt;
    }
    item.decayAt = 0;
    put(player, item, everyone);
    item.flags &= ~ObjFlag::NoCollide;
    return true;
}

bool Items::tryDrop(ServerObject& player, ServerObject& item, float x, float y)
{
    if (item.holder != player.extent) {
        return false;
    }
    // (On the x87: dx kept on the stack; dy stored, and squared as the stack's value times the
    // stored one; the distance stored, tested on the stack.)
    const util::X87 dx = util::X87(x) - player.x;
    const util::X87 dy53 = util::X87(y) - player.y;
    const float dy = dy53.f();
    const util::X87 d53 = util::sqrt(dy53 * dy + dx * dx);
    const float d = d53.f();
    if (util::X87(75.0f) < d53) {
        x = (dx * 75.0f / d + player.x).f();
        y = (util::X87(dy) * 75.0f / d + player.y).f();
    }
    if (!world.traceRay(player.x, player.y, x, y, 0)) {
        tellString(player, "drop.c:DropNotAllowed");
        playSound(world, 0x39D, player, 2, player.extent);
        return false;
    }
    // (Obj_DropAt: its DROP; the quest items kept by DefaultDrop's own check, Items::dropDefault.)
    return drop(player, item, x, y);
}

bool Items::tryEquip(ServerObject& player, ServerObject& item)
{
    if (item.holder != player.extent) {
        return false;
    }
    // Asked for (Obj_PlayerEquipWeapon / Obj_PlayerEquipArmor with their fail sound: a refusal's
    // 0x39D to the player alone).
    if (!player.player) {
        return false;
    }
    return equipWeapon(player, item, true, true) || equipArmor(player, item, true, true);
}

bool Items::tryDequip(ServerObject& player, ServerObject& item)
{
    if (item.holder != player.extent || !(item.flags & ObjFlag::Equipped)) {
        return false;
    }
    // (Not a bow mid-shot.)
    if (player.player->state == 1 && (item.classes & oc::Weapon) && (item.subclasses & 8)) {
        return false;
    }
    takeOff(player, item, true, true);
    return true;
}

void Items::reportEquip(const ServerObject& player, const ServerObject& item)
{
    const bool weapon = (item.classes & 0x11001000u) != 0;  // a weapon, wand or flag: reported as a weapon
    if (!weapon && !(item.classes & oc::Armor)) {
        return;
    }
    const std::uint32_t bit = weapon ? things::weaponBitOf(item.typeName) : things::armorBitOf(item.typeName);
    const std::uint32_t holder = player.extent | ((player.classes & oc::Player) ? 0x8000u : 0);
    const std::uint8_t op = weapon ? (hasModifiers(item) ? 0x51 : 0x50) : (hasModifiers(item) ? 0x52 : 0x4F);
    // Layout: op, u16 its holder, u32 its weapon or armour bits, then its four modifiers if any.
    std::vector<std::uint8_t> m = {op, lo(holder), hi(holder), lo(bit), hi(bit), static_cast<std::uint8_t>(bit >> 16), static_cast<std::uint8_t>(bit >> 24)};
    if (hasModifiers(item)) {
        m.insert(m.end(), item.modifiers.begin(), item.modifiers.end());
    }
    tellEveryone(std::move(m));
}

void Items::reportWornAgain(const ServerObject& player)
{
    // (CliNet_OnPacket's MSG_PLAYER_DIED outside a quest: the player's weapon bits and records
    // cleared, its armour's too but the street clothes' (kClothing); its MSG_REPORT_*_EQUIP
    // adds a record each, so only what was cleared is told.)
    for (const std::uint32_t e : player.inventory) {
        const ServerObject* item = world.byExtent(e);
        if (!item || !(item->flags & ObjFlag::Equipped)) {
            continue;
        }
        const bool weapon = (item->classes & 0x11001000u) != 0;
        if (weapon || ((item->classes & oc::Armor) && !(things::armorBitOf(item->typeName) & kClothing))) {
            reportEquip(player, *item);
        }
    }
}

void Items::tellString(const ServerObject& player, std::string_view id)
{
    // (SrvNet_SendInformString: MSG_INFORM 0x0F, not a sign, the string's id)
    tell(player, informStringBytes(id));
}

void Items::inventoryPut(ServerObject& player, ServerObject& item, bool report)
{
    // Off the map first (Obj_PickupDefault's servFinalizeDelObject: ACTIVE off, out of
    // the grid and the lists).
    world.gridRelease(item);
    item.holder = player.extent;
    setOwner(world, &player, item); // (inventoryPutImpl -> unitSetOwner)
    item.vx = item.vy = 0;
    player.inventory.insert(player.inventory.begin(), item.extent);
    // Its client hears (SrvNet_SendReportPickup).
    const std::uint32_t code = item.extent;
    if (report) {
        if (item.classes & kModifiable) {
            tell(player, {0x4C, lo(code), hi(code), lo(item.type), hi(item.type), item.modifiers[0], item.modifiers[1],
                          item.modifiers[2], item.modifiers[3]});
        } else {
            tell(player, {0x4B, lo(code), hi(code), lo(item.type), hi(item.type)});
        }
        if (item.maxHealth != 0) {
            tell(player, {0x44, lo(code), hi(code), lo(item.health), hi(item.health), lo(item.maxHealth), hi(item.maxHealth)});
        }
    }
    if (item.classes & oc::Key) {
        playSound(world, 0x334, player); // (a key's jingle, inventoryPutImpl)
    }
}

void Items::put(ServerObject& player, ServerObject& item, bool everyone)
{
    inventoryPut(player, item, true);
    // Worn when it may be (WeaponPickup / ArmorPickup).
    if (!player.player) {
        return; // (a monster's: kept, not worn)
    }
    PlayerControl& p = *player.player;
    if (item.classes & (oc::Weapon | oc::Wand)) {
        // (Not worn while its .plr is applied: Obj_WeaponPickup's.)
        const std::uint32_t bit = things::weaponBitOf(item.typeName);
        bool worn = false;
        if (p.currentWeapon == 0 && !p.transfer && bit != 2) {
            worn = equipWeapon(player, item, everyone);
        }
        if (bit == 2 && !p.transfer && (p.weaponEquip & 0xCu) && !(p.weaponEquip & 2u)) {
            worn = equipWeapon(player, item, everyone); // (a quiver for the bow or crossbow it holds)
        }
        // Not worn: its charges told all the same (worn, Obj_PlayerEquipWeapon told them), a
        // magical staff's (wand class, subclass 0x47F0000) or a quiver's or throwing stars'
        // (weapon class, subclass 0x82).
        if (!worn && (((item.classes & oc::Wand) && (item.subclasses & 0x47F0000u)) ||
                      ((item.classes & oc::Weapon) && (item.subclasses & 0x82u)))) {
            reportCharges(player, item);
        }
    } else if (item.classes & oc::Armor && !p.transfer) {
        // ArmorPickup's end (pickupArmor; not while its .plr is applied): worn when
        // nothing of its kind is (armorHaveSameSubclass: worn, the same subclass),
        // or what is is the stock outfit's: sneakers, a plain wizard's robe, a plain wooden
        // shield; a plain steel shield gives way to a steel one or a wooden one with modifiers. A
        // shield onto a free arm only without a two-hander in hand (subclass 0x7FFE40C).
        const ServerObject* same = nullptr;
        for (const std::uint32_t e : player.inventory) {
            const ServerObject* o = world.byExtent(e);
            if (o && o != &item && (o->classes & oc::Armor) && (o->flags & ObjFlag::Equipped) && o->subclasses == item.subclasses) {
                same = o;
                break;
            }
        }
        const auto plain = [](const ServerObject& o) { return o.modifiers == std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF}; };
        // Which they are by their armour bits, one item each (0x4000 the wizard's robe, 0x1000000
        // the wooden shield, 0x2000000 the steel one, 1 the sneakers): a type a content pack
        // added as the one it's made like.
        constexpr std::uint32_t kRobe = 0x4000u, kWooden = 0x1000000u, kSteel = 0x2000000u, kSneakers = 0x1u;
        const std::uint32_t sameBit = same ? things::armorBitOf(same->typeName) : 0;
        bool wear = false;
        if (same && sameBit == kRobe) {
            wear = plain(*same);
        } else if (!same || sameBit == kWooden || sameBit == kSteel || sameBit == kSneakers) {
            if (!(item.subclasses & 2u)) {
                wear = true;
            } else if (!same) {
                const ServerObject* weapon = p.currentWeapon ? world.byExtent(p.currentWeapon) : nullptr;
                wear = !weapon || !(weapon->subclasses & 0x7FFE40Cu);
            } else if (sameBit == kWooden) {
                wear = plain(*same);
            } else if (sameBit == kSteel) {
                const std::uint32_t itemBit = things::armorBitOf(item.typeName);
                wear = plain(*same) && (itemBit == kSteel || (itemBit == kWooden && !plain(item)));
            }
        }
        if (wear) {
            equipArmor(player, item, everyone);
        }
    }
}

bool Items::strongEnough(const ServerObject& player, const ServerObject& item) const
{
    if (!(player.classes & oc::Player) || !player.player) {
        return false;
    }
    if (!modifiers) {
        return true;
    }
    const things::EquipDef* def = (item.classes & oc::Armor) ? modifiers->findArmor(item.typeName) : modifiers->findWeapon(item.typeName);
    return def && def->requiredStrength <= static_cast<int>(player.player->strength);
}

bool Items::equipWeapon(ServerObject& player, ServerObject& item, bool everyone, bool failSound)
{
    PlayerControl& p = *player.player;
    if (!(item.classes & (oc::Weapon | oc::Wand)) || (item.flags & ObjFlag::Equipped)) {
        return false;
    }
    // Not while Berserker Charge or War Cry is under way (probablyWarcryCheck 2, 1);
    // another class's, or one it isn't strong enough for, refused with its line (and 0x39D when
    // asked for); one it doesn't carry, nothing.
    if (abilityFlag(world, player, 2) != 0 || abilityFlag(world, player, 1) != 0) {
        return false;
    }
    if (const char* fail = !classMay(player, item)        ? "weapon.c:WeaponEquipClassFail"
                           : !strongEnough(player, item) ? "weapon.c:WeaponEquipStrengthFail"
                                                         : nullptr) {
        tellString(player, fail);
        if (failSound) {
            playSound(world, 0x39D, player, 2, player.extent);
        }
        return false;
    }
    if (item.holder != player.extent) {
        return false;
    }
    const std::uint32_t bit = things::weaponBitOf(item.typeName);
    if (p.state == 1) {
        p.state = 0xD; // (Plr_SetState: the swing's over)
    }
    if (bit == 2) {  // weapon bit 2: a quiver
        // A quiver only with a bow or crossbow worn, or the first one carried put on;
        // else "weapon.c:BowNotFound". The quiver worn comes off.
        bool bow = (p.weaponEquip & 0xCu) != 0;  // bits 0xC: a bow or crossbow
        if (!bow) {
            for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
                ServerObject* o = world.byExtent(e);
                if (o && (things::weaponBitOf(o->typeName) & 0xCu)) {
                    bow = equipWeapon(player, *o, everyone);
                    break;
                }
            }
        }
        if (!bow) {
            tellString(player, "weapon.c:BowNotFound");
            return false;
        }
        takeOffQuiver(player, true, true);
    }
    if (bit != 2 && p.currentWeapon != 0) {
        if (ServerObject* old = world.byExtent(p.currentWeapon)) {
            takeOff(player, *old, true, true);
        }
    }
    item.flags |= ObjFlag::Equipped;
    p.weaponEquip |= bit;
    tell(player, {0x60, lo(item.extent), hi(item.extent)});
    if (everyone) {
        reportEquip(player, item);
    }
    if (bit != 2) {
        p.currentWeapon = item.extent;
    }
    // Its charges to its client (SrvNet_SendReportCharges): a magical staff's (wand class, subclass
    // 0x47F0000), else a quiver's or throwing stars' (weapon class, weapon bits 0x82). (A bow's or
    // crossbow's reload counter zeroed here is never anything else: Missiles::shoot.)
    if ((item.classes & oc::Wand) && (item.subclasses & 0x47F0000u)) {
        reportCharges(player, item);
    } else if ((item.classes & oc::Weapon) && (bit & 0x82u)) {
        reportCharges(player, item);
    }
    engageEffects(world, modifiers, player, item); // (itemApplyEngageEffect)
    if (bit & 0x7FFE40Cu) {
        // Two-handed (bows, crossbows, the great sword, staves): the shields worn off
        // (armour bits 0x3000000).
        for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
            ServerObject* o = world.byExtent(e);
            if (o && (o->classes & oc::Armor) && (o->flags & ObjFlag::Equipped) && (things::armorBitOf(o->typeName) & 0x3000000u)) {
                takeOff(player, *o, true, true);
            }
        }
    }
    if (world.modeHooks && (item.modeEvents & ModeEvent::Equip)) {
        world.modeHooks->equipped(world, player, item, true); // (noxcpp: told a mode)
    }
    return true;
}

bool Items::equipArmor(ServerObject& player, ServerObject& item, bool everyone, bool failSound)
{
    PlayerControl& p = *player.player;
    if (!(item.classes & oc::Armor) || (item.flags & ObjFlag::Equipped)) {
        return false;
    }
    if (const char* fail = !classMay(player, item)        ? "armor.c:ArmorEquipClassFail"
                           : !strongEnough(player, item) ? "armor.c:ArmorEquipStrengthFail"
                                                         : nullptr) {
        tellString(player, fail);
        if (failSound) {
            playSound(world, 0x39D, player, 2, player.extent);
        }
        return false;
    }
    // What's worn of its kind comes off (armorHaveSameSubclass: the same SUBCLASS).
    for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
        ServerObject* o = world.byExtent(e);
        if (o && o != &item && (o->classes & oc::Armor) && (o->flags & ObjFlag::Equipped) && o->subclasses == item.subclasses) {
            takeOff(player, *o, true, true);
            break;
        }
    }
    const std::uint32_t bit = things::armorBitOf(item.typeName);
    item.flags |= ObjFlag::Equipped;
    p.armorEquip |= bit;
    tell(player, {0x60, lo(item.extent), hi(item.extent)});
    if (everyone) {
        reportEquip(player, item);
    }
    if (bit & kClothing) {
        item.flags |= ObjFlag::KeptOnDeath;
    }
    recalculateArmor(player);
    engageEffects(world, modifiers, player, item); // (itemApplyEngageEffect)
    if (item.subclasses & 2u) {
        // A shield (subclass 2): the two-handed weapons in hand off (weapon bits
        // 0x7FFE40C, bows, crossbows, the great sword, staves; playerDequipWeapon).
        for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
            ServerObject* o = world.byExtent(e);
            if (o && (o->flags & ObjFlag::Equipped) && (o->classes & (oc::Weapon | oc::Wand)) && (things::weaponBitOf(o->typeName) & 0x7FFE40Cu)) {
                takeOff(player, *o, true, true);
            }
        }
    }
    if (world.modeHooks && (item.modeEvents & ModeEvent::Equip)) {
        world.modeHooks->equipped(world, player, item, true); // (noxcpp: told a mode)
    }
    return true;
}

void Items::takeOffQuiver(ServerObject& player, bool tellOwner, bool everyone)
{
    // The first quiver worn comes off.
    for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
        ServerObject* o = world.byExtent(e);
        if (o && (o->flags & ObjFlag::Equipped) && things::weaponBitOf(o->typeName) == 2) {
            takeOff(player, *o, tellOwner, everyone);
            return;
        }
    }
}

void Items::takeOff(ServerObject& player, ServerObject& item, bool tellOwner, bool everyone)
{
    if (!(item.flags & ObjFlag::Equipped) || !player.player) {
        return;
    }
    PlayerControl& p = *player.player;
    std::uint32_t bit = 0;
    std::uint8_t op = 0;
    if (item.classes & oc::Armor) {
        bit = things::armorBitOf(item.typeName);
        item.flags &= 0xEFFFFEFFu;  // EQUIPPED (0x100) and KeptOnDeath off
        p.armorEquip &= ~bit;
        op = 0x53;
    } else if (item.classes & (oc::Weapon | 0x1000u)) { // (playerDequipWeapon)
        if (p.state == 1) {
            p.state = 0xD;
        }
        bit = things::weaponBitOf(item.typeName);
        if (p.currentWeapon == 0 || (p.currentWeapon != item.extent && bit != 2)) {
            return;
        }
        if (bit & 0xCu) {
            takeOffQuiver(player, tellOwner, everyone); // (its quiver first)
        }
        // A staff's lasting spell ends with it.
        if (item.classes & 0x1000u) {
            if (item.subclasses & 0x40000u) {
                cancelLastingSpells(world, 0x2B, player);
            }
            if (item.subclasses & 0x4000000u) {
                cancelLastingSpells(world, 0x3B, player);
            }
        }
        item.flags &= ~ObjFlag::Equipped;
        p.weaponEquip &= ~bit;
        if (bit != 2) {
            p.currentWeapon = 0;
        }
        op = 0x54;
    } else {
        return;
    }
    if (tellOwner) {
        tell(player, {0x61, lo(item.extent), hi(item.extent)});
    }
    if (everyone) {
        tellEveryone({op, lo(player.extent), hi(player.extent), lo(bit), hi(bit), static_cast<std::uint8_t>(bit >> 16),
                      static_cast<std::uint8_t>(bit >> 24)});
    }
    if (item.classes & oc::Armor) {
        recalculateArmor(player);
    }
    disengageEffects(world, modifiers, player, item); // (itemApplyDisengageEffect)
    // A shield (subclass 2) off while raising, raised or lowering it: idle (the armour dequip's
    // Plr_SetState(0xD)).
    if ((item.classes & oc::Armor) && (item.subclasses & 2u) && (p.state == 0xF || p.state == 0x10 || p.state == 0x11)) {
        p.previousState = p.state;
        p.state = 0xD;
        p.stateFrame = frame;
        p.animFrame = 0;
    }
    if (world.modeHooks && (item.modeEvents & ModeEvent::Equip)) {
        world.modeHooks->equipped(world, player, item, false); // (noxcpp: told a mode)
    }
}

bool Items::wear(ServerObject& holder, ServerObject& item, double amount, ServerObject* src, ServerObject* obj3, int type, bool weapon)
{
    // equipDamage / playerDamageWeapon (`weapon`: not a weapon of
    // subclass 0x7800000). Only what has HealthData (HEALTH 0 too: the street clothes).
    if (!item.hasHealthData() || (weapon && (item.classes & oc::Weapon) && (item.subclasses & 0x7800000u))) {
        return false;
    }
    float a5 = chopToFloat(amount); // (the float argument, FSTP in chop mode)
    // Its material's DEFENDEFFECT (DurabilityMultiplierEffect,
    // ((1 - v) + 1) x it; ArmorMultiplierEffect, v x it; the shipped materials only have
    // the first).
    if (const things::ModifierDef* m = modifiers ? modifiers->modifier(item.modifiers[1]) : nullptr) {
        // (FLD 1, FSUB its number, FADD 1, FMUL, FSTP; FLD its number, FMUL, FSTP)
        const util::X87 v = m->defend.value;
        if (m->defend.fn == things::EffectFn::DurabilityMultiplier) {
            a5 = (((util::X87(1.0f) - v) + 1.0f) * a5).f();
        } else if (m->defend.fn == things::EffectFn::ArmorMultiplier) {
            a5 = (v * a5).f();
        }
    }
    a5 = (util::X87(a5) + item.wear).f();                   // (FLD, FADD the carry, FSTP)
    const int whole = util::chop(a5);                       // (Math_FloatToInt: chopped)
    item.wear = (util::X87(a5) - util::X87::fromDouble(whole)).f(); // (FILD, FSUBR, FSTP)
    if (whole <= 0) {
        return false;
    }
    // Its own DAMAGE (WeaponDamage / ArmorDamage -> DefaultDamage: the sounds, the hurt-by, at 0 its
    // DIE: WeaponDie / ArmorDie, the notice and delayedDeleteObject).
    const std::uint16_t before = item.health;
    Fight{world, rng, frame, &things, modifiers}.damage(item, src, obj3 ? obj3->extent : 0, whole, type);
    // itemDestroyed to a player holder: its health (MSG_REPORT_ITEM_HEALTH) in a
    // solo game, else when it passes a quarter.
    // Its health band: 0 full, 1 half or more, 2 a quarter or more, 3 less.
    const auto band = [&](std::uint16_t h) {
        return h == item.maxHealth ? 0 : h >= item.maxHealth * 0.5 ? 1 : h >= item.maxHealth * 0.25 ? 2 : 3;
    };
    if ((holder.classes & oc::Player) && item.health != before && ((world.gameFlags & 0x800u) || band(before) != band(item.health))) {
        tell(holder, {0x44, lo(item.extent), hi(item.extent), lo(item.health), hi(item.health), lo(item.maxHealth), hi(item.maxHealth)});
    }
    return item.pendingDelete; // (ObjFlags 0x20: it broke)
}

void deleteObject(World& world, ServerObject& o)
{
    // delayedDeleteObject. Once (its 0x20: ours pendingDelete).
    if (o.pendingDelete) {
        return;
    }
    // (A player's watched creature it doesn't control: sweepDeleted, as the exe frees it.)
    if (o.holder != 0) {
        if (ServerObject* h = world.byExtent(o.holder)) { // (Obj_RemoveFromInventory: MSG_REPORT_DROP)
            static const things::ThingDb kNoThings{};
            std::minstd_rand unused{1};
            Items{world, world.thingDb ? *world.thingDb : kNoThings, world.modifierDb, unused, world.frameNow}.removeFromInventory(*h, o);
        }
    }
    // playerCancelSpells: the lasting spells it cast.
    for (DurationSpell& s : world.durationSpells()) {
        if (!s.cancelled && s.caster != 0 && s.caster == o.extent) {
            cancelDurationSpell(world, s);
        }
    }
    // (A quest monster's the team: sweepDeleted; a player's none held.)
    o.pendingDelete = true;
}

std::u16string Items::fullName(const ServerObject& item) const
{
    if (!(item.classes & 0x13001000u)) {
        return world.text("objutil.c:NoDescription");
    }
    const things::EquipDef* def = nullptr;
    if (modifiers) {
        def = (item.classes & 0x11001000u) ? modifiers->findWeapon(item.typeName) : modifiers->findArmor(item.typeName);
    }
    if (!def) {
        std::u16string s = world.text("objutil.c:NoInfo");
        const std::u16string type = strings::fromUtf8(item.typeName);
        for (std::size_t i = 0; i + 1 < s.size(); ++i) { // (its %s: the type's name)
            if (s[i] == u'%' && (s[i + 1] == u's' || s[i + 1] == u'S')) {
                s.replace(i, 2, type);
                break;
            }
        }
        return s;
    }
    // (A modifier whose word is missing adds nothing, its space neither.)
    const auto word = [&](std::size_t at, bool secondary) -> std::u16string {
        const things::ModifierDef* m = modifiers->modifier(item.modifiers[at]);
        const std::string& id = m ? (secondary ? m->secondaryDesc : m->desc) : std::string();
        return id.empty() ? std::u16string() : world.text(id);
    };
    std::u16string out;
    if (const auto w = word(0, false); !w.empty()) {
        out += w + u" ";
    }
    if (const auto w = word(1, false); !w.empty()) {
        out += w + u" ";
    }
    if (!def->desc.empty()) {
        out += world.text(def->desc);
    }
    if (const auto w = word(2, false); !w.empty()) {
        out += u" " + w;
    }
    if (const auto w = word(3, true); !w.empty()) {
        out += u" " + w;
    }
    return out;
}

void Items::use(ServerObject& player, ServerObject& item)
{
    // (Obj_UseItem: the item's USE, none while the user's .plr is applied.)
    if (player.player && player.player->transfer) {
        return;
    }
    if (world.modeHooks && (item.modeEvents & ModeEvent::Use) && !world.modeHooks->mayUse(world, player, item)) {
        return; // (noxcpp: a mode's item, the mode's to use)
    }
    const auto usedUp = [&] { // (delayedDeleteObject: out of its inventory first)
        if (item.holder != 0) {
            if (ServerObject* h = world.byExtent(item.holder)) {
                removeFromInventory(*h, item);
            }
        }
        item.pendingDelete = true;
    };
    const auto enchantFor = [&](int e, int frames, int power) { enchant(world, player, e, frames, power); };
    const int cls = player.player ? std::min<int>(player.player->classId, 2) : 0;
    if (item.useFn == "PotionUse") {
        if ((player.classes & oc::Player) && (player.flags & ObjFlag::Dead)) {
            return;
        }
        int amount = item.useArg;
        bool did = false;
        if ((item.subclasses & 0x10u) && player.maxHealth != 0 && player.health < player.maxHealth) {  // subclass 0x10: restores health
            if (player.player) {
                // (FILD, FMUL the class's multiplier, FSTP, Math_FloatToInt)
                amount = util::floatToInt(util::X87::fromDouble(amount) * world.healthMultiplier[static_cast<std::size_t>(cls)]);
            }
            adjustHealth(world, player, amount);
            playSound(world, 0x2F2, player);
            did = true;
        }
        if ((item.subclasses & 0x20u) && player.player && player.player->mana < player.player->maxMana) {  // subclass 0x20: restores mana
            amount = util::floatToInt(util::X87::fromDouble(amount) * world.manaMultiplier[player.player->multiclass ? 2u : static_cast<std::size_t>(cls)]);
            player.player->mana = static_cast<std::uint16_t>(std::clamp(player.player->mana + static_cast<std::int16_t>(amount), 0, static_cast<int>(player.player->maxMana)));
            playSound(world, 0x2F3, player);
            did = true;
        }
        if ((item.subclasses & 0x40u) && player.player && player.poison != 0) {  // subclass 0x40: cures poison
            Fight{world, rng, frame, &things, modifiers}.removePoison(player);
            playSound(world, spellSound(things, 0x0E, 1), player); // (Cure Poison's on)
            did = true;
        }
        constexpr int k120s = 30 * 120;
        const auto buff = [&](std::uint32_t bit, int e) {
            if (item.subclasses & bit) {
                enchantFor(e, k120s, 3);
                did = true;
            }
        };
        buff(0x100u, Enchant::Hasted);
        buff(0x200u, Enchant::Invisible);
        if (item.subclasses & 0x400u) { // (Shield cast on itself, its number the power)
            Spells{world, things, modifiers, rng, frame}.cast(0x33, player, &player, player.x, player.y, amount);
            did = true;
        }
        buff(0x800u, Enchant::ProtectFromFire);
        buff(0x1000u, Enchant::ProtectFromElectricity);
        buff(0x2000u, Enchant::ProtectFromPoison);
        buff(0x4000u, Enchant::Invulnerable);
        buff(0x8000u, Enchant::Infravision);
        // Vampirism always takes; a potion that did nothing at all is kept, not used up.
        if (item.subclasses & 0x10000u) {
            enchantFor(Enchant::Vampirism, k120s, 3);
        } else if (!did) {
            return;
        }
        usedUp();
    } else if (item.useFn == "ConsumeUse" || item.useFn == "ConsumeConfuseUse") {
        if (item.useFn == "ConsumeConfuseUse") {
            if (player.maxHealth == 0) {
                return;
            }
            enchantFor(Enchant::Confused, 30 * 5, 4);
            tellString(player, "Use.c:CiderConfuse");
        }
        if (!(item.classes & oc::Food) || (item.subclasses & 8u) || player.maxHealth == 0 || player.maxHealth <= player.health) {
            return;
        }
        adjustHealth(world, player, item.useArg);
        // Its eating (useConsume): a player's by the food's kind (subclass 1, 2, 4) in
        // a man's (0x13A-0x13C) or a woman's voice (0x144-0x146); anything else's 0x14E.
        std::uint32_t munch = 0x14E;
        if (player.classes & oc::Player) {
            const bool female = player.typeName == "PlayerFemale";
            munch = (item.subclasses & 1u) ? (female ? 0x144u : 0x13Au)
                    : (item.subclasses & 2u) ? (female ? 0x145u : 0x13Bu)
                    : (item.subclasses & 4u) ? (female ? 0x146u : 0x13Cu)
                    : 0u;
        }
        playSound(world, munch, player);
        usedUp();
    } else if (item.useFn == "MushroomUse") {
        if (player.poison == 0) {
            tellString(player, "Use.c:MushroomConfuse");
        } else {
            Fight{world, rng, frame, &things, modifiers}.removePoison(player);
            tellString(player, "Use.c:MushroomClean");
            playSound(world, spellSound(things, 0x0E, 1), player);
        }
        enchantFor(Enchant::Confused, 300, 5);
        usedUp();
    } else {
        useBook(*this, player, item); // (books, guides, signs: books.hpp)
    }
}

void Items::breakNotice(const ServerObject& item)
{
    ServerObject* holder = item.holder ? world.byExtent(item.holder) : nullptr;
    const bool metal = (item.material & 0x10u) != 0;
    const bool wood = (item.material & 0x8u) != 0;
    // Its breaking sound at its holder, or where it lies: a weapon's metal 0x332 / wood 0x333; a
    // piece of armour's metal 0x326, wood 0x32C, leather 0x329, cloth 0x32F (a generic piece's
    // passes a position as the sound: none, an original bug).
    const ServerObject& at = holder ? *holder : item;
    if (item.dieFn == "WeaponDie") {
        if (metal || wood) {
            playSoundAt(world, metal ? 0x332u : 0x333u, at.x, at.y);
        }
    } else if (metal || wood || (item.material & 0x6u)) {
        playSoundAt(world, metal ? 0x326u : wood ? 0x32Cu : (item.material & 0x4u) ? 0x329u : 0x32Fu, at.x, at.y);
    }
    if (!holder) {
        return;
    }
    if (item.dieFn == "WeaponDie") {
        // (Neither metal nor wood, the exe names it from the armour table, which no
        // weapon is in: "result:ERROR"; every stock weapon is metal or wood.)
        world.textTo(*holder, metal ? "die.c:WeaponDieMetal" : wood ? "die.c:WeaponDieWood" : "die.c:WeaponDieGeneric",
                     metal || wood ? fullName(item) : world.text("result:ERROR"));
        return;
    }
    // Armour: plural when its definition's name ends in an s (equipClothFindDefByTT: the English game's).
    bool plural = false;
    if (const things::EquipDef* def = modifiers ? modifiers->findArmor(item.typeName) : nullptr; def && !def->desc.empty()) {
        const std::u16string name = world.text(def->desc);
        plural = !name.empty() && (name.back() == u'S' || name.back() == u's');
    }
    const char* label = metal                       ? (plural ? "die.c:ArmorDieMetalPlural" : "die.c:ArmorDieMetal")
                        : wood                      ? (plural ? "die.c:ArmorDieWoodPlural" : "die.c:ArmorDieWood")
                        : (item.material & 0x4u)    ? (plural ? "die.c:ArmorDieHidePlural" : "die.c:ArmorDieHide")
                        : (item.material & 0x2u)    ? (plural ? "die.c:ArmorDieClothPlural" : "die.c:ArmorDieCloth")
                                                    : "die.c:ArmorDieGeneric";
    world.textTo(*holder, label, fullName(item));
}

bool Items::drop(ServerObject& player, ServerObject& item, float x, float y)
{
    const bool dropped = dropAt(player, item, x, y);
    if (dropped && world.modeHooks && (item.modeEvents & ModeEvent::Drop)) {
        world.modeHooks->dropped(world, player, item); // (noxcpp: told a mode)
    }
    return dropped;
}

bool Items::dropAt(ServerObject& player, ServerObject& item, float x, float y)
{
    if (item.holder != player.extent) {
        return false;
    }
    // (Obj_DropAt) In a network game but a quest a weapon, wand, armour or food (0x3001010) can't
    // be picked up at once.
    if ((world.gameFlags & GameFlag::Network) && !(world.gameFlags & GameFlag::Quest) && (item.classes & 0x3001010u)) {
        item.flags |= ObjFlag::NoCollide;
    }
    // Its DROP (CrownDrop: modes.hpp), else DefaultDrop.
    if (item.dropFn == "CrownDrop") {
        dropCrown(*this, player, item, x, y);
        return true;
    }
    if (item.dropFn == "GlyphDrop" || item.dropFn == "TrapDrop") {
        // TrapDrop: not onto water a teleport can't reach (0x39D to the dropper);
        // DefaultDrop, then the dropper's again (unitSetOwner), however it's
        // dropped: by hand or at its death (Obj_DropAt calls it). GlyphDrop: then its
        // spells aimed where it lies and it facing back at the dropper, with 0x339 at it.
        if (world.noTeleportAt(x, y)) {
            playSound(world, 0x39D, player, 2, player.extent);
            return false;
        }
        if (!dropDefault(player, item, x, y)) {
            return false;
        }
        setOwner(world, &player, item);
        if (item.dropFn == "GlyphDrop" && item.trap) {
            item.trap->x = x;
            item.trap->y = y;
            // ((atan2 + 2 pi) x 128 / pi + 0.5, chopped: the way from the stored differences, FPATAN
            // on the x87 (ours the library's in double), FADD, FMUL, FADD, FSTP, Math_FloatToInt)
            const float wx = (util::X87(player.x) - x).f();
            const float wy = (util::X87(player.y) - y).f();
            const util::X87 turn = util::X87::fromDouble(std::atan2(static_cast<double>(wy), static_cast<double>(wx)));
            const int d = util::floatToInt((turn + std::bit_cast<float>(0x40C90FDBu)) * std::bit_cast<float>(0x4222F983u) + 0.5f);
            item.direction = static_cast<std::uint16_t>(d & 0xFF);
            playSound(world, 0x339, item);
        }
        return true;
    }
    if (!dropDefault(player, item, x, y)) {
        return false;
    }
    // WeaponDrop / ArmorDrop (dropArmor): DefaultDrop, then its sound at it.
    if (item.dropFn == "WeaponDrop") {
        weaponDropSound(world, item);
        // (dropWeapon) Out of solo and quest games under server option 2 it
        // decays in 25 s.
        if (!(world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && (world.serverOptions & 2u)) {
            item.decayAt = frame + 30u * 25u;
        }
    } else if (item.dropFn == "ArmorDrop") {
        armorDropSound(world, item);
        // (dropArmor) the same.
        if (!(world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && (world.serverOptions & 2u)) {
            item.decayAt = frame + 30u * 25u;
        }
    } else if (item.dropFn == "PotionDrop") {
        // PotionDrop: 0x341 at it; out of solo and quest games it decays in 25 s.
        playSound(world, 0x341, item);
        if (!(world.gameFlags & (GameFlag::Solo | GameFlag::Quest))) {
            item.decayAt = frame + 30u * 25u;
        }
    } else if (item.dropFn == "FoodDrop") {
        // dropFood: out of a solo game it decays in 25 s; the first in the exe's table
        // (subclass, material, sound) that fits it sounds at the dropper.
        if (!(world.gameFlags & GameFlag::Solo)) {
            item.decayAt = frame + 30u * 25u;
        }
        struct Kind {
            std::uint32_t subclasses;
            std::uint16_t material;
            std::uint16_t sound;
        };
        static constexpr Kind kKinds[] = {{0, 1, 0x343}, {2, 0, 0x345}, {4, 0, 0x341}, {0x80, 0, 0x347}};
        for (const Kind& k : kKinds) {
            if ((k.subclasses & item.subclasses) || (k.material & item.material)) {
                playSound(world, k.sound, player);
                break;
            }
        }
    } else if (item.dropFn.starts_with("AudEventDrop ")) {
        playSound(world, std::string_view(item.dropFn).substr(13), player); // (objectDropAudEvent, at the dropper)
    }
    return true;
}

bool Items::dropDefault(ServerObject& player, ServerObject& item, float x, float y)
{
    if (item.holder != player.extent) {
        return false;
    }
    // A player's quest items stay with it (ItemIsDroppable(1): the exe's table),
    // however they'd go, its death's drop too (dropAllItems ->
    // Obj_DropAt); told only while it's neither dead nor destroyed (0x8020).
    constexpr std::string_view kKept[] = {
        "BookOfOblivion", "OblivionHalberd", "OblivionHeart", "OblivionWierdling", "OblivionOrb", "AmuletOfClarity",
        "AmuletOfTeleportation", "BridgeGuardsBoots", "Spectacles", "MatildasCloak", "ThaviusStaff",
        "BlueOrbKeyOfTheLich", "RedOrbKeyOfTheLich", "SilverKey", "GoldKey", "SapphireKey", "RubyKey",
        "SponsorshipLetter", "MayorsScepter",
    };
    if ((player.classes & oc::Player) && std::ranges::find(kKept, std::string_view(item.typeName)) != std::end(kKept)) {
        if (!(player.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            tellString(player, "drop.c:CantDropThat");
            playSound(world, 0x39D, player, 2, player.extent);
        }
        return false;
    }
    removeFromInventory(player, item);
    // Into the world (createAt); a flag's drop told (DefaultDrop's flag part);
    // decaying in 10 s unless it respawns (not a solo game's or a quest's, a flag or a glyph).
    place(item, x, y);
    if ((item.classes & oc::Weapon) && things::weaponBitOf(item.typeName) == 4u) {
        // A bow dropped takes with it the first quiver carried, not worn, that's endless
        // (infinite ammo): out of the inventory and deleted (delayedDeleteObject).
        for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
            ServerObject* q = world.byExtent(e);
            if (q && (q->classes & oc::Weapon) && things::weaponBitOf(q->typeName) == 2u && !(q->flags & ObjFlag::Equipped) &&
                q->infiniteAmmo) {
                removeFromInventory(player, *q);
                q->flags |= ObjFlag::Destroyed;
                q->pendingDelete = true;
                break;
            }
        }
    }
    if (item.classes & oc::Key) {
        playSound(world, 0x335, item);
    }
    if (item.classes & oc::Flag) {
        flagDropped(world, player, item, modifiers, frame);
    }
    if (!(world.gameFlags & 0x1800u) && !(item.flags & 0x80000u) && !(item.classes & 0x10000000u) && item.typeName != "Glyph") {
        item.decayAt = frame + 30 * 10;
    }
    item.touch();
    return true;
}

void Items::place(ServerObject& item, float x, float y)
{
    // (createAt for a thing out of an inventory)
    item.flags = (item.flags & 0x35E9FEDBu) | ObjFlag::Active;
    item.born = item.timer = frame;
    // (In a network game not a quest, gold and items (0x3001110, no missile) NO_COLLIDE.)
    if (world.network() && !world.quest() && (item.typeName == "Gold" || (item.classes & 0x3001110u)) && !(item.classes & oc::Missile)) {
        item.flags |= ObjFlag::NoCollide;
    }
    item.x = item.frameStartX = item.newX = x;
    item.y = item.frameStartY = item.newY = y;
    item.vx = item.vy = 0;
    item.touch();
    world.noteNew(item); // (the new list: the grid and the lists at the next pass)
}

void Items::removeFromInventory(ServerObject& player, ServerObject& item)
{
    // (noxcpp: a mode's World::keepThingsOnDeath tells everyone, as a quest does: its clients
    // still know what a dead player wears.)
    const bool everyone = world.quest() || world.keepThingsOnDeath || !((player.flags & ObjFlag::Dead) && (item.classes & kModifiable));
    if (player.player && (item.classes & oc::Flag) && (world.gameFlags & GameFlag::CaptureTheFlag)) {
        player.player->weaponEquip &= ~1u;
        if (everyone) { // (SrvNet_SendReportDequipClass: its holder's net code, the weapon bits)
            const std::uint32_t bit = things::weaponBitOf(item.typeName);
            tellEveryone({0x54, lo(player.extent), hi(player.extent), lo(bit), hi(bit), static_cast<std::uint8_t>(bit >> 16),
                          static_cast<std::uint8_t>(bit >> 24)});
        }
    }
    if (!player.player && (player.classes & oc::Monster)) {
        // A monster's (Obj_RemoveFromInventory's): an NPC's flag's bits off in CTF; its worn armour
        // or weapon (playerDequipWeapon -> equipWeaponNPC) off.
        if ((player.subclasses & 0x10u) && (item.classes & oc::Flag) && (world.gameFlags & GameFlag::CaptureTheFlag)) {
            npcSetItemEquipFlags(world, player, item, false);
        }
        if ((item.classes & oc::Armor) && (item.flags & ObjFlag::Equipped)) {
            npcDequipArmor(world, modifiers, player, item);
        }
        npcDequipWeapon(world, modifiers, player, item);
    }
    takeOff(player, item, false, everyone);
    if (player.player) { // (a chest's or a creature's: no client)
        tell(player, {0x4D, lo(item.extent), hi(item.extent), lo(item.type), hi(item.type)});
    }
    std::erase(player.inventory, item.extent);
    item.holder = 0;
    clearOwner(world, item); // (unitClearOwner)
}

void Items::dropAll(ServerObject& player, bool piecesOnly)
{
    // (unitDropCheckSome) A creature keeps its clothing.
    // Everything but what a creature keeps on death (worn clothing), unless that's destroyed.
    // (noxcpp, piecesOnly: a mode's player that keeps its things lets go only of the game's
    // pieces it carries, a flag or a crown, dropped as the full drop drops them.)
    const auto droppable = [&](const ServerObject& item) {
        if (piecesOnly) {
            return item.typeName == NameId::Crown || (item.classes & oc::Flag) != 0;
        }
        const bool kept = !(item.flags & ObjFlag::Destroyed) && (player.classes & (oc::Monster | oc::Player)) && (item.flags & ObjFlag::KeptOnDeath);
        return !kept;
    };
    float biggest = 0;
    for (const std::uint32_t e : player.inventory) {
        const ServerObject* item = world.byExtent(e);
        if (item && droppable(*item) && biggest < item->radius) {
            biggest = item->radius;
        }
    }
    const float spacing = (util::X87(biggest) + biggest + 6.0f).f();  // two of the biggest radius apart, plus 6 (FSTP'd)
    const float px = player.x;
    const float py = player.y;
    // The spiral: square rings about the player (3 cells a side, then 5, 7...), `spacing` apart,
    // walked side by side; each item goes to the next cell with no wall between it and the player
    // (shaken by up to 3 each way). A whole ring walked with nothing placed: the rest at its feet.
    int ring = 3;
    int xi = 0;
    int yi = -1;
    int side = 0;
    int steps = 0;
    int cellX = 0;
    int cellY = -1;
    bool placedInRing = false;
    const auto items = player.inventory;
    for (std::size_t n = 0; n < items.size(); ++n) {
        ServerObject* item = world.byExtent(items[n]);
        if (!item || !droppable(*item)) {
            continue;
        }
        int sideLength = ring - 1;
        bool placed = false;
        // Along the spiral, cell by cell, till this item finds a place.
        do {
            // (FILD, FMUL, FADD / FSUBR, FSTP; the shake FADD'd, FSTP)
            float tx = (util::X87::fromDouble(cellX) * spacing + px).f();
            float ty = (util::X87(py) - util::X87::fromDouble(cellY) * spacing).f();
            tx = (util::randomFloat(rng, -3.0f, 3.0f) + tx).f(); // (randomFloat's stack value)
            ty = (util::randomFloat(rng, -3.0f, 3.0f) + ty).f();
            if (world.traceRay(px, py, tx, ty, Trace::Doors)) {
                drop(player, *item, tx, ty);
                placedInRing = true;
                placed = true;
            }
            // Then on to the next cell: along the side, round a corner, or (the ring done) out a ring.
            bool turn = false;
            if (steps == sideLength) {
                if (side != 3 && sideLength != 0) {
                    ++side;
                    steps = 1;
                    turn = true;
                } else if (!placedInRing) {
                    if (!placed) { // nowhere at all: the rest at its feet
                        for (std::size_t m = n; m < items.size(); ++m) {
                            if (ServerObject* rest = world.byExtent(items[m]); rest && rest->holder == player.extent && droppable(*rest)) {
                                drop(player, *rest, px, py);
                            }
                        }
                        return;
                    }
                    break;
                } else {
                    // The ring walked with something placed in it: out to the next ring, 2 cells wider.
                    sideLength += 2;
                    ring += 2;
                    xi = 1 - ring / 2;
                    steps = 1;
                    yi = -(ring / 2);
                    side = 0;
                    placedInRing = false;
                    cellX = xi;
                    cellY = yi;
                }
            } else {
                ++steps;
                turn = true;
            }
            if (turn) {
                switch (side) {
                case 0: cellX = ++xi; break;
                case 1: cellY = ++yi; break;
                case 2: cellX = --xi; break;
                case 3: cellY = --yi; break;
                }
            }
        } while (!placed);
    }
}

void Items::defaultItems(ServerObject& player)
{
    PlayerControl& p = *player.player;
    // What isn't worn clothing goes (but medieval pants and shirts).
    for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
        ServerObject* item = world.byExtent(e);
        if (!item) {
            continue;
        }
        const std::uint32_t armorBit = (item->classes & oc::Armor) ? things::armorBitOf(item->typeName) : 0;
        const bool clothing = (item->classes & oc::Armor) && (armorBit & kClothing);
        if (!clothing || !(item->flags & ObjFlag::Equipped) || (armorBit & 0x808u)) {
            takeOff(player, *item, false, false);
            tell(player, {0x4D, lo(item->extent), hi(item->extent), lo(item->type), hi(item->type)});
            std::erase(player.inventory, item->extent);
            item->holder = 0;
            item->pendingDelete = true;
        }
    }
    // Everyone dresses it themselves (MSG_PLAYER_RESPAWN; every default item allowed: 0xFF).
    const std::uint32_t code = player.extent;
    // MSG_PLAYER_RESPAWN (0xE9): u16 net code, u32 frame, 0xFF, 1.
    tellEveryone({0xE9, lo(code), hi(code), lo(frame), hi(frame), static_cast<std::uint8_t>(frame >> 16),
                  static_cast<std::uint8_t>(frame >> 24), 0xFF, 1});
    const things::ModifierDef* base = modifiers ? modifiers->findModifier("UserColor1") : nullptr;
    const auto colour = [&](std::uint8_t index) -> std::uint8_t {
        return base ? static_cast<std::uint8_t>(base->id + index) : 0xFF;
    };
    // (clothing: pants, shirt 1, shirt 2, shoes 1, shoes 2; a warrior's shirt only in a co-op or
    // solo game, game flags 0xA00)
    if (((world.gameFlags & (GameFlag::Coop | GameFlag::Solo)) || p.classId != 0) && !(p.armorEquip & 0x400u)) {
        give(player, "StreetShirt", {0xFF, colour(p.clothing[1]), colour(p.clothing[2]), 0xFF});
    }
    if (!(p.armorEquip & 0x4u)) {
        give(player, "StreetPants", {0xFF, colour(p.clothing[0]), 0xFF, 0xFF});
    }
    if (!(p.armorEquip & 0x1u)) {
        give(player, "StreetSneakers", {colour(p.clothing[4]), colour(p.clothing[3]), 0xFF, 0xFF});
    }
    const auto modifier = [&](const char* name) -> std::uint8_t {
        const things::ModifierDef* m = modifiers ? modifiers->findModifier(name) : nullptr;
        return m ? static_cast<std::uint8_t>(m->id) : 0xFF;
    };
    if (world.gameFlags & GameFlag::Solo) {
        // The campaign's: Sword, StaffWooden, StaffWooden with ArmorQuality1 (and a
        // warrior's Material1).
        static constexpr const char* kSolo[3] = {"Sword", "StaffWooden", "StaffWooden"};
        give(player, kSolo[p.classId % 3], {modifier("ArmorQuality1"), p.classId == 0 ? modifier("Material1") : std::uint8_t{0xFF}, 0xFF, 0xFF});
    } else if ((world.gameFlags & GameFlag::Quest) && !(world.mapTypes & 0x80000000u)) {
        // A quest's (but on a chat map's bit 31): Sword, a SulphorousFlareWand
        // with Replenishment1, Bow.
        static constexpr const char* kQuest[3] = {"Sword", "SulphorousFlareWand", "Bow"};
        give(player, kQuest[p.classId % 3], {0xFF, 0xFF, p.classId == 1 ? modifier("Replenishment1") : std::uint8_t{0xFF}, 0xFF});
    } else if (p.classId == 0) {
        give(player, "Longsword");
        give(player, "WoodenShield");
    } else if (p.classId == 1) {
        give(player, "WizardRobe");
    }
}

} // namespace nox::server
