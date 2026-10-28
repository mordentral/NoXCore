// The campaign NPCs' server side: their colours and voices, the weapons and armour they wear
// (the same rules as a player's equipping), their attacks frame by frame, and the
// MSG_REPORT_NPC / equip-class messages the clients draw them by. See npc.hpp.
#include "noxcore/server/npc.hpp"

#include "noxcore/server/item_effects.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <random>
#include <tuple>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

constexpr std::uint32_t kWeapons = 0x1001000u;   // (weapons and wands)
constexpr std::uint32_t kTwoHanded = 0x7FFE40Cu; // (bows, crossbows, the great sword, staves)

using util::hi;
using util::lo;

bool worn(const ServerObject& item) { return (item.flags & ObjFlag::Equipped) != 0; }

/// `item` is in `holder`'s inventory (the walk the equip functions start with).
bool carries(const ServerObject& holder, const ServerObject& item)
{
    return std::ranges::find(holder.inventory, item.extent) != holder.inventory.end();
}

/// The inventory's things from its head, as the exe walks InvFirstItem / InvNextItem (a copy: the
/// walk's calls may change it).
std::vector<ServerObject*> carried(World& world, const ServerObject& holder)
{
    std::vector<ServerObject*> out;
    for (const std::uint32_t e : holder.inventory) {
        if (ServerObject* item = world.byExtent(e)) {
            out.push_back(item);
        }
    }
    return out;
}

/// a staff's lasting spell ends as it comes off (a Force of Nature's 0x2B, an
/// Oblivion Orb's 0x3B).
void endStaffSpells(World& world, const ServerObject& wearer, const ServerObject& item)
{
    if (item.classes & oc::Wand) {
        if (item.subclasses & 0x40000u) {
            cancelLastingSpells(world, 0x2B, wearer);
        }
        if (item.subclasses & 0x4000000u) {
            cancelLastingSpells(world, 0x3B, wearer);
        }
    }
}

/// The quiver off (with 1, 1) for an NPC: the first worn quiver (weapon bits 2) off through
/// playerDequipWeapon, a monster's being equipWeaponNPC.
void quiverOff(World& world, const things::ModifierDb* modifiers, ServerObject& npc)
{
    for (ServerObject* item : carried(world, npc)) {
        if (worn(*item) && things::weaponBitOf(item->typeName) == 2) {
            npcDequipWeapon(world, modifiers, npc, *item);
            return;
        }
    }
}

/// worn armour of the shield bits (0x3000000) off.
void shieldsOff(World& world, const things::ModifierDb* modifiers, ServerObject& npc)
{
    for (ServerObject* item : carried(world, npc)) {
        if ((item->classes & oc::Armor) && worn(*item) && (things::armorBitOf(item->typeName) & 0x3000000u)) {
            npcDequipArmor(world, modifiers, npc, *item);
        }
    }
}

/// worn two-handed weapons (their bits 0x7FFE40C) off (playerDequipWeapon -> equipWeaponNPC).
void twoHandedOff(World& world, const things::ModifierDb* modifiers, ServerObject& npc)
{
    for (ServerObject* item : carried(world, npc)) {
        if (worn(*item) && (item->classes & kWeapons) && (things::weaponBitOf(item->typeName) & kTwoHanded)) {
            npcDequipWeapon(world, modifiers, npc, *item);
        }
    }
}

} // namespace

bool npcReported(const ServerObject& o)
{
    return (o.classes & oc::Monster) && (o.subclasses & 0x30u);
}

void oweNpcReport(ServerObject& o, bool state)
{
    // The owe (o, 0x4000000, 0x400, state): each player's state bit set or cleared (the same
    // for all); owed to those who know it; the others' owed cleared when the state isn't set.
    o.npcState = state;
    o.npcOwed = o.knownBy | (state ? o.npcOwed : 0u);
}

void setNpcColor(ServerObject& o, std::uint8_t index, const std::array<std::uint8_t, 3>& rgb)
{
    o.touch(); // (unitNeedSync)
    if (o.monster && index < o.monster->colors.size()) {
        o.monster->colors[index] = rgb;
    }
    if (o.classes & 0x20400004u) {
        o.npcOwed = 0xFFFFFFFFu; // (each player owed the NPC report, 0x4000000)
        return;
    }
    oweNpcReport(o, npcReported(o));
}

void setNpcVoice(const World& world, ServerObject& o, std::string_view name)
{
    // getDefaultSoundSet by the exact name (none: null), set by
    // setNPCVoiceSet on a monster only.
    if (!o.monster) {
        return;
    }
    if (!world.soundSets) {
        o.monster->soundSet = std::string(name); // (no sound sets loaded: the name as read)
        return;
    }
    const things::SoundSet* set = world.soundSets->find(name);
    o.monster->soundSet = set ? set->name : std::string();
}

std::string npcVoiceName(const World& world, const ServerObject& o)
{
    if (!(o.classes & oc::Monster) || !o.monster) {
        return {};
    }
    if (o.monster->soundSet) {
        return *o.monster->soundSet;
    }
    // MonsterCreate's: by its type's name.
    const things::SoundSet* set = world.soundSets ? world.soundSets->find(o.typeName) : nullptr;
    return set ? set->name : std::string();
}

void npcSetItemEquipFlags(World& /*world*/, ServerObject& npc, const ServerObject& item, bool on)
{
    // npcSetItemEquipFlags.
    npc.touch();
    if (npc.monster) {
        auto& m = *npc.monster;
        if (item.classes & kWeapons) {
            const std::uint32_t bit = things::weaponBitOf(item.typeName);
            m.weaponFlags = on ? (m.weaponFlags | bit) : (m.weaponFlags & ~bit);
        } else {
            const std::uint32_t bit = things::armorBitOf(item.typeName);
            m.armorFlags = on ? (m.armorFlags | bit) : (m.armorFlags & ~bit);
        }
    }
    if (npc.classes & 0x20400004u) {
        npc.npcOwed = 0xFFFFFFFFu;
        return;
    }
    oweNpcReport(npc, true); // (the owe with 0x4000000, 0x400, 1)
}

void npcRecalculateArmor(World& world, const things::ModifierDb* modifiers, ServerObject& npc)
{
    // recalculateArmorVal: the worn armour's values (itemApplyDefendEffect) summed in a float,
    // over 1 (a double constant) 1; a monster's armour value set to it.
    float total = 0;
    for (const std::uint32_t e : npc.inventory) {
        const ServerObject* item = world.byExtent(e);
        if (item && (item->classes & oc::Armor) && worn(*item)) {
            total = (util::X87(armorValueOf(modifiers, *item)) + total).f(); // (FADD the local, FSTP)
        }
    }
    if (1.0 < static_cast<double>(total)) {
        total = 1.0f;
    }
    if (npc.monster) {
        npc.monster->armorValue = total;
    }
}

bool npcEquipWeapon(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item)
{
    // NPCEquipWeapon.
    if (!(item.classes & kWeapons) || worn(item) || !npc.monster || !carries(npc, item)) {
        return false;
    }
    auto& m = *npc.monster;
    m.punch = 0;
    if (!(item.subclasses & 0xCu)) { // (not a bow or crossbow: the quiver off)
        quiverOff(world, modifiers, npc);
    }
    if (!(item.subclasses & 2u)) { // (not a quiver: the weapon worn off, the first that is)
        for (ServerObject* other : carried(world, npc)) {
            if (worn(*other) && (other->classes & kWeapons) && (!(item.subclasses & 0xCu) || !(other->subclasses & 2u))) {  // a bow or crossbow keeps its quiver
                npcDequipWeapon(world, modifiers, npc, *other);
                break;
            }
        }
    }
    item.flags |= ObjFlag::Equipped;
    if (npc.subclasses & 0x10u) {
        npcSetItemEquipFlags(world, npc, item, true);
    }
    if (!(item.subclasses & 2u)) {
        m.weapon = item.extent;
    }
    engageEffects(world, modifiers, npc, item); // (itemApplyEngageEffect)
    if (things::weaponBitOf(item.typeName) & kTwoHanded) {
        shieldsOff(world, modifiers, npc);
    }
    return true;
}

bool npcEquipArmor(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item)
{
    // NPCEquipArmor.
    if (!(item.classes & oc::Armor) || worn(item) || !carries(npc, item)) {
        return false;
    }
    // Armour worn of the very same subclass off first, the first such.
    for (ServerObject* other : carried(world, npc)) {
        if ((other->classes & oc::Armor) && worn(*other) && other->subclasses == item.subclasses) {
            npcDequipArmor(world, modifiers, npc, *other);
            break;
        }
    }
    item.flags |= ObjFlag::Equipped;
    if (npc.subclasses & 0x10u) {
        npcSetItemEquipFlags(world, npc, item, true);
    }
    // clothing (armour bits 0xC0D) is kept on death (0x10000000).
    if (things::armorBitOf(item.typeName) & 0xC0Du) {
        item.flags |= ObjFlag::KeptOnDeath;
    }
    npcRecalculateArmor(world, modifiers, npc);
    engageEffects(world, modifiers, npc, item);
    if (item.subclasses & 2u) { // (a shield: the two-handed weapons off)
        twoHandedOff(world, modifiers, npc);
    }
    return true;
}

bool npcDequipWeapon(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item)
{
    // equipWeaponNPC.
    if (!(item.classes & kWeapons) || !worn(item) || !npc.monster || !carries(npc, item)) {
        return false;
    }
    auto& m = *npc.monster;
    m.punch = 0;
    item.flags &= ~ObjFlag::Equipped;
    if (npc.subclasses & 0x10u) {
        npcSetItemEquipFlags(world, npc, item, false);
    }
    if (item.subclasses & 0xCu) { // (a bow or crossbow: its quiver off too)
        quiverOff(world, modifiers, npc);
    }
    if (!(item.subclasses & 2u)) {
        m.weapon = 0;
    }
    disengageEffects(world, modifiers, npc, item); // (itemApplyDisengageEffect)
    endStaffSpells(world, npc, item);
    return true;
}

bool npcDequipArmor(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item)
{
    if (!(item.classes & oc::Armor) || !worn(item) || !carries(npc, item)) {
        return false;
    }
    item.flags &= ~ObjFlag::Equipped;
    if (npc.subclasses & 0x10u) {
        npcSetItemEquipFlags(world, npc, item, false);
    }
    item.flags &= ~ObjFlag::KeptOnDeath;
    npcRecalculateArmor(world, modifiers, npc);
    disengageEffects(world, modifiers, npc, item);
    return true;
}

void npcWearLoaded(World& world, const things::ModifierDb* modifiers, ServerObject& npc)
{
    // of the worn things in the inventory's order, the first two-handed weapon (its
    // subclass 0x7FFE40C) or shield (armour subclass 2) found keeps the other kind off.
    bool twoHanded = false;
    bool shield = false;
    for (ServerObject* item : carried(world, npc)) {
        if (!worn(*item)) {
            continue;
        }
        if ((item->classes & kWeapons) && (item->subclasses & kTwoHanded)) {
            if (shield) {
                item->flags &= ~ObjFlag::Equipped;
            } else {
                twoHanded = true;
            }
        } else if ((item->classes & oc::Armor) && (item->subclasses & 2u)) {
            if (twoHanded) {
                item->flags &= ~ObjFlag::Equipped;
            } else {
                shield = true;
            }
        }
    }
    // On the host (game flag 1: always ours) each worn one unmarked and worn again.
    for (ServerObject* item : carried(world, npc)) {
        if (!worn(*item)) {
            continue;
        }
        item->flags &= ~ObjFlag::Equipped;
        if (item->classes & kWeapons) {
            npcEquipWeapon(world, modifiers, npc, *item);
        } else {
            npcEquipArmor(world, modifiers, npc, *item);
        }
    }
}

bool npcAttack(Fight& fight, ServerObject& npc)
{
    // playerAttack's NPC part (class 2, subclass 0x10).
    World& world = fight.world;
    if (!npc.monster || !(npc.classes & oc::Monster) || !(npc.subclasses & 0x10u)) {
        return false;
    }
    auto& m = *npc.monster;
    const std::uint8_t last = m.animFrame;
    const std::uint32_t bits = m.weaponFlags;
    ServerObject* weapon = m.weapon ? world.byExtent(m.weapon) : nullptr;
    const things::EquipDef* def = nullptr;
    if (!weapon) {
        if (m.punch == 0) { // (a jab either hand; only a player's may be a hook)
            m.punch = static_cast<std::uint8_t>(nox::util::uniform_int_distribution<int>(0x17, 0x18)(fight.rng));
        }
    } else {
        // (getProjectileClassById: a weapon without its definition, no attack)
        def = fight.modifiers ? fight.modifiers->findWeapon(weapon->typeName) : nullptr;
        if (fight.modifiers && !def) {
            return false;
        }
        weapon->x = weapon->lastX = npc.x;
        weapon->y = weapon->lastY = npc.y;
    }
    const int strength = m.field331; // (unitGetStrength: an NPC's)
    // (A player's warrior abilities first: playerIsAbilityActive is a player's.)
    const std::uint32_t elapsed = fight.frame - npc.timer; // (its timer: the attack's start)
    int frames = 0;
    int delay = 0;
    // The animation's frame count and delay (ticks per frame - 1); gives the frame reached
    // since the attack began.
    const auto stepOf = [&](int animation) {
        if (world.playerAnimations) {
            std::tie(frames, delay) = (*world.playerAnimations)[static_cast<std::size_t>(animation)];
        }
        return static_cast<int>(elapsed / static_cast<std::uint32_t>(delay + 1));
    };
    // (calcBoltDamage) A weapon's damage: past its required strength so much a
    // point, and its least.
    const auto weaponDamage = [&] { return boltDamage(world, def, strength); };
    const auto weaponReach = [&] { return def ? def->range : 30.0f; };
    // A staff's strike with the weapon (type 0, its reach past the body); a miss sounds 0x36F.
    const auto strike = [&] {
        Attack a;
        a.damage = weaponDamage();
        a.type = 0;
        a.weapon = weapon->extent;
        a.range = (util::X87(npc.radius) + weaponReach()).f();
        a.sides = 1;
        if (!fight.attack(npc, a)) {
            playSound(world, 0x36F, npc);
        }
    };
    int shown = 0; // (a byte)
    if (!weapon) {
        const int step = stepOf(m.punch);
        shown = step & 0xFF;
        if (frames <= shown) {
            // The fist: its strength x 0.04 and 5 (a jab) or 10 (a hook); a punch's type 10.
            Attack a;
            a.damage = m.punch < 0x17 ? 0.0f : (util::X87::fromDouble(strength) * 0.04f + (m.punch == 0x19 ? 10.0f : 5.0f)).f(); // (on the x87, FSTP)
            a.type = 10;
            a.range = (util::X87(npc.radius) + 20.0f).f();
            a.hitsOverlapping = false;
            a.sides = 1;
            if (!fight.attack(npc, a)) {
                playSound(world, 0x36F, npc);
            }
        }
    } else if (bits & 0x47F8000u) {  // the staffs and wands
        // A staff or wand: a wooden staff (or a spent wand swung) strikes (STAFF_STRIKE 0x1D) at its
        // middle; a wand's blast (0x1F) is used at its second frame (its USE: Obj_UseItem), and
        // spent (charges 0 of some), taken off.
        const int animation = ((bits & 0x8000u) || weapon->staffMelee) ? 0x1D : 0x1F;
        const int step = stepOf(animation);
        shown = step & 0xFF;
        if (frames <= shown) {
            if (bits & 0x47F0000u) {  // a staff (StaffWands bits): its melee swing over
                weapon->staffMelee = false;
            }
        } else if (animation == 0x1D && shown == frames / 2 && last < shown) {
            strike(); // (the staff's middle frame, just reached: it strikes)
        } else if (animation != 0x1D && shown == 1 && last == 0 && fight.things) {
            if (weapon->wandSpell != 0) { // (WandCastUse)
                Spells{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.wandCast(npc, *weapon);
            } else { // (WandUse)
                Missiles{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.wandShot(npc, *weapon);
            }
            if (weapon->charges == 0 && weapon->maxCharges != 0) {
                npcDequipWeapon(world, fight.modifiers, npc, *weapon);
            }
        }
    } else if (bits & 0x7800000u) {
        // The Oblivion staves: the halberd, heart and wierdling strike (0x20) at the middle; the
        // orb (0x1F) only its animation.
        const int animation = (bits & 0x3800000u) ? 0x20 : 0x1F;
        const int step = stepOf(animation);
        shown = step & 0xFF;
        if (animation == 0x20 && shown == frames / 2 && last < shown) {
            strike();
        }
    } else if (bits & 0xC0u) {
        // (A chakram's or shuriken's throw by an NPC: not here. None of the campaign's carries one.)
        return false;
    } else if (bits & 0x7F00u) {  // the swung weapons
        struct Swing {
            std::uint32_t bits;
            int animation;
            std::uint8_t type;
            bool overlapping;
            std::uint32_t missSound;
        };
        static constexpr Swing kSwings[] = {
            {0x200, 0x1C, 0, false, 0x370}, // the longsword
            {0x100, 0x1B, 0, false, 0x371}, // the sword
            {0x400, 0x25, 0, false, 0x371}, // the great sword
            {0x4000, 0x27, 2, true, 0x372}, // the war hammer: round its head
            {0x800, 0x1A, 2, true, 0x374},  // the morning star
            {0x3000, 0x23, 0, true, 0x373}, // the axes
        };
        const Swing* s = nullptr;
        for (const Swing& w : kSwings) {
            if (bits & w.bits) {
                s = &w;
                break;
            }
        }
        if (!s) {
            return false;
        }
        const int step = stepOf(s->animation);
        shown = step & 0xFF;
        // Each swing lands once, as its middle frame is first reached.
        if (shown == frames / 2 && last < shown) {
            Attack a;
            a.damage = weaponDamage();
            a.type = s->type;
            a.weapon = weapon->extent;
            a.hitsOverlapping = s->overlapping;
            a.sides = 1;
            if (s->bits == 0x4000) { // (its head, 35 ahead: everything in its reach of it)
                const auto [fx, fy] = directionVector(npc.direction);
                a.area = true;
                a.x = (util::X87(fx) * 35.0f + npc.x).f(); // (FMUL, FADD, FSTP)
                a.y = (util::X87(fy) * 35.0f + npc.y).f();
                a.range = weaponReach();
            } else {
                a.range = (util::X87(npc.radius) + weaponReach()).f();
            }
            const bool hit = fight.attack(npc, a);
            if (s->bits == 0x4000) {
                // The ground shakes (earthquakeSend, its strength x 0.1 chopped):
                // MSG_FX_JIGGLE to each player within 300 of its camera.
                jiggle(world, npc.x, npc.y, util::floatToInt(util::X87::fromDouble(strength) * 0.1f)); // (FILD, FMUL, FSTP)
            }
            if (!hit || s->bits == 0x4000) {
                playSound(world, s->missSound, npc);
            }
        }
    } else if (bits & 0xCu) {  // a bow (4) or crossbow (8)
        // A bow looses at SHOOT_BOW's end (its READINESS on its frame), a crossbow on its second.
        const bool bow = (bits & 4u) != 0;
        const int step = stepOf(bow ? 0x21 : 0x22);
        const int ready = readiness(*weapon, fight.modifiers);
        if (bow) {
            shown = (step + ready) & 0xFF;
            if (frames - 1 <= shown && last < shown && fight.things) {
                Missiles{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.shoot(npc, *weapon);
                shown = frames & 0xFF;
            }
        } else {
            shown = step & 0xFF;
            if (shown == 1 && last == 0 && fight.things) {
                Missiles{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.shoot(npc, *weapon);
            }
            if (shown != 0) {
                shown = (step + ready) & 0xFF;
            }
        }
    } else {
        return false;
    }
    // Its animation's frame, at most its last.
    m.animFrame = static_cast<std::uint8_t>(shown);
    if (frames <= shown) {
        m.animFrame = static_cast<std::uint8_t>(frames - 1);
    }
    return shown < frames;  // true while its attack's animation still runs
}

std::vector<std::uint8_t> npcReport(const ServerObject& npc)
{
    std::uint32_t code = netCodeOf(npc);
    if (npc.poison != 0) {
        code |= 0x8000u;
    }
    // 0x69, u16 code (bit 15: poisoned), six RGB colours.
    std::vector<std::uint8_t> m = {0x69, lo(code), hi(code)};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto rgb = npc.monster ? npc.monster->colors[i] : std::array<std::uint8_t, 3>{};
        m.insert(m.end(), rgb.begin(), rgb.end());
    }
    return m;
}

std::vector<std::uint8_t> equipClassReport(const ServerObject& holder, const ServerObject& item)
{
    const bool weapon = (item.classes & 0x11001000u) != 0;  // weapons, wands and flags
    if (!weapon && !(item.classes & oc::Armor)) {
        return {};
    }
    const bool mods = std::ranges::any_of(item.modifiers, [](std::uint8_t x) { return x != 0xFF; });
    const std::uint32_t bit = weapon ? things::weaponBitOf(item.typeName) : things::armorBitOf(item.typeName);
    std::uint32_t code = netCodeOf(holder);
    if (holder.classes & oc::Player) {
        code |= 0x8000u;
    }
    // 0x50 / 0x51 a weapon, 0x4F / 0x52 armour (the second of each carries its modifiers):
    // the op, u16 holder code, u32 equip bit, then the modifier bytes when it has any.
    const std::uint8_t op = weapon ? (mods ? 0x51 : 0x50) : (mods ? 0x52 : 0x4F);
    std::vector<std::uint8_t> m = {op, lo(code), hi(code), lo(bit), hi(bit), static_cast<std::uint8_t>(bit >> 16), static_cast<std::uint8_t>(bit >> 24)};
    if (mods) {
        m.insert(m.end(), item.modifiers.begin(), item.modifiers.end());
    }
    return m;
}

} // namespace nox::server
