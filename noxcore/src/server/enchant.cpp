// Enchantments (buffs) on units: putting them on and taking them off, the per-frame
// count-down, and marking which clients are owed a MSG_REPORT_ENCHANTMENT (enchant.hpp).
#include "noxcore/server/enchant.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"

namespace nox::server {

void setEnchantments(ServerObject& o, std::uint32_t bits)
{
    o.touch(); // (unitNeedSync)
    o.buffs = bits;
    // Players and always-sent objects (Player, Immobile or ClientPersist classes): every client.
    if (o.classes & 0x20400004u) {
        o.enchantOwed = 0xFFFFFFFFu;
    } else {
        // (owed to those who know it; the others' stays only while it has any)
        o.enchantOwed |= o.knownBy;
        if (bits == 0) {
            o.enchantOwed &= o.knownBy;
        }
    }
}

void enchant(World& world, ServerObject& o, int e, int frames, int power)
{
    e &= 31;
    if (o.typeName == "Hecubah" && e == Enchant::AntiMagic) {
        return;
    }
    // A quest's Hecubah and Necromancer laugh confusion off (0x246 / 0x253).
    if (world.quest() && e == Enchant::Confused && (o.typeName == "Hecubah" || o.typeName == "Necromancer")) {
        playSound(world, o.typeName == "Hecubah" ? 0x246u : 0x253u, o);
        return;
    }
    // Fear-proof (subclass 0x1000) out of the solo game; the two say so.
    if ((o.classes & things::ObjectClass::Monster) && (o.subclasses & 0x1000u) && e == Enchant::Afraid && !(world.gameFlags & GameFlag::Solo)) {
        if (o.typeName == "Hecubah") {
            playSound(world, 0x246, o);
        } else if (o.typeName == "Necromancer") {
            playSound(world, 0x253, o);
        }
        return;
    }
    // Not on the dead, the destroyed, nor the frozen-out (NoUpdate).
    if (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed | ObjFlag::NoUpdate)) {
        return;
    }
    if (enchanted(o, e) && o.buffTime[static_cast<std::size_t>(e)] == 0) {
        return; // (on for good already)
    }
    if (e != Enchant::Invisible) {
        disenchant(world, o, Enchant::Invisible);
    }
    o.buffTime[static_cast<std::size_t>(e)] = static_cast<std::uint16_t>(frames);
    o.buffPower[static_cast<std::size_t>(e)] = static_cast<std::uint8_t>(power);
    setEnchantments(o, o.buffs | (1u << e));
    if (world.thingDb) {
        playSound(world, spellSound(*world.thingDb, enchantmentSpell(e), 1), o); // its spell's "on"
    }
}

void disenchant(World& world, ServerObject& o, int e)
{
    e &= 31;
    if (!enchanted(o, e)) {
        return;
    }
    setEnchantments(o, o.buffs & ~(1u << e));
    o.buffTime[static_cast<std::size_t>(e)] = 0;
    o.buffPower[static_cast<std::size_t>(e)] = 0;
    if (e != Enchant::Death && e != Enchant::Crown) {
        if (world.thingDb) {
            playSound(world, spellSound(*world.thingDb, enchantmentSpell(e), 2), o); // its spell's "off"
        }
    }
}

void clearEnchantments(ServerObject& o)
{
    setEnchantments(o, 0);
    o.buffTime.fill(0);
    o.buffPower.fill(0);
}

void stepEnchantments(ServerObject& o, Fight& fight)
{
    if (o.buffs == 0) {
        return;
    }
    // Each of the 32 enchantment bits that is on.
    for (int e = 0; e < 32; ++e) {
        if (!enchanted(o, e)) {
            continue;
        }
        auto& left = o.buffTime[static_cast<std::size_t>(e)];
        if (e == Enchant::Death && left % 30u == 29u) { // (Death's tick each second: 0x1A)
            playSound(fight.world, 0x1A, o);
        }
        // A time of 0 means "for good": it never counts down. Otherwise the enchantment ends on
        // the frame it reaches 0.
        if (left == 0 || --left != 0) {
            continue;
        }
        if (e == Enchant::Ethereal) {
            o.flags &= ~ObjFlag::NoCollide;
        } else if (e == Enchant::Death) {
            o.hurtBy = 0;
            o.hurtType = 0xD;
            fight.takeHealth(o, 9999999);
            playSound(fight.world, 0x30B, o);
            // A player's death counted here as well (playerIncrementElimDeath,
            // reported): its scoring at the death counted it too in elimination
            // (an original bug).
            if (o.classes & things::ObjectClass::Player) {
                addDeath(fight, o);
                reportLesson(fight.world, o);
            }
        }
        disenchant(fight.world, o, e);
        o.buffPower[static_cast<std::size_t>(e)] = 0;
    }
    // Haste works on this frame's speed, after the slowdowns physics applied.
    if (enchanted(o, Enchant::Hasted)) {
        o.speedCur *= 1.25f;
    }
}

} // namespace nox::server
