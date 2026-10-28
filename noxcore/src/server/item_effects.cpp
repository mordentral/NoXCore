// Items' modifier effects (item_effects.hpp): the named effect functions of modifier.bin run
// by name when an item is worn or taken off, strikes, is struck through, or each frame.
// An item has four modifier slots: 0 its effectiveness, 1 its material, 2 and 3 its enchantments.
#include "noxcore/server/item_effects.hpp"

#include "noxcore/server/enchant.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
using Fn = things::EffectFn;

constexpr std::uint32_t kModifiable = 0x13001000u; // (weapons, wands, armour, flags)
constexpr std::uint32_t kGameFps = 30;             // (gameFPS)

using util::hi;
using util::lo;

/// The modifier in `item`'s slot (InitData's four records).
const things::ModifierDef* slotOf(const things::ModifierDb* modifiers, const ServerObject& item, std::size_t slot)
{
    return modifiers ? modifiers->modifier(item.modifiers[slot]) : nullptr;
}

/// A worn weapon, wand, piece of armour or flag.
bool wornModifiable(const ServerObject& o)
{
    return (o.flags & ObjFlag::Equipped) && (o.classes & kModifiable);
}

/// Whether one of `wearer`'s worn things still has the ENGAGEEFFECT `engage` in any slot
/// (enchantItemTestInventory: by the table of engage functions and
/// their itemBuffs bits).
bool stillGiven(World& world, const things::ModifierDb* modifiers, const ServerObject& wearer, Fn engage)
{
    for (const std::uint32_t e : wearer.inventory) {
        const ServerObject* item = world.byExtent(e);
        if (!item || !wornModifiable(*item)) {
            continue;
        }
        for (std::size_t slot = 0; slot < 4; ++slot) {
            if (const things::ModifierDef* m = slotOf(modifiers, *item, slot); m && m->engage.fn == engage) {
                return true;
            }
        }
    }
    return false;
}

/// To a player's client alone.
void tell(World& world, const ServerObject& player, std::vector<std::uint8_t> bytes)
{
    if (player.player) {
        world.broadcasts().push_back({std::move(bytes), true, player.extent});
    }
}

/// SrvNet_SendReportStatModifier: MSG_REPORT_STAT_MODIFIER, its code, the value's
/// float, the stat (0: its speed).
void reportSpeed(World& world, const ServerObject& player)
{
    const auto bits = std::bit_cast<std::uint32_t>(player.speedBonus);
    tell(world, player, {0x68, lo(player.extent), hi(player.extent), lo(bits), hi(bits), lo(bits >> 16), hi(bits >> 16), 0});
}

/// SrvNet_SendInform 0x0D: what an enchanted weapon did to a player (0 stunned,
/// 1 confused, 2 poisoned).
void informHit(World& world, const ServerObject& player, std::uint32_t what)
{
    tell(world, player, {0xA9, 0x0D, lo(what), hi(what), lo(what >> 16), hi(what >> 16)});
}

/// A spell cast by the effect itself (the spell's function, not spellAccept).
void castDirect(Fight& fight, std::uint32_t spell, ServerObject& caster, ServerObject* target, float x, float y, int level)
{
    if (!fight.things) {
        return;
    }
    Spells spells{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    spells.direct = true;
    spells.cast(spell, caster, target, x, y, level);
}

} // namespace

/// unitGetOldMana: a player's mana, a monster's 1000, else none.
int manaOf(const ServerObject& o)
{
    if (o.player) {
        return o.player->mana;
    }
    return (o.classes & oc::Monster) ? 1000 : 0;
}

/// playerManaAdd / playerManaSub: a player's, at most its most,
/// not below 0.
void addMana(ServerObject& o, int amount)
{
    if (o.player) {
        o.player->mana = static_cast<std::uint16_t>(std::clamp(o.player->mana + amount, 0, static_cast<int>(o.player->maxMana)));
    }
}

void engageEffects(World& world, const things::ModifierDb* modifiers, ServerObject& wearer, const ServerObject& item)
{
    // Only the enchantment slots (2 and 3) engage.
    for (std::size_t slot = 2; slot < 4; ++slot) {
        const things::ModifierDef* m = slotOf(modifiers, item, slot);
        if (!m || m->engage.fn == Fn::None) {
            continue;
        }
        const Fn fn = m->engage.fn;
        // itemBuffs bits: 1 fire, 2 poison and 4 lightning protection, 8 brilliance, 0x10 speed,
        // 0x20 regeneration.
        if (fn == Fn::BrillianceEngage) {
            wearer.itemBuffs |= 8u;
            playSound(world, 0x4B, wearer);
        } else if (fn == Fn::SpeedEngage) { // (effectSpeedEngage: a player's)
            if (wearer.classes & oc::Player) {
                wearer.itemBuffs |= 0x10u;
                wearer.speedBonus = (util::X87(m->engage.value) + wearer.speedBonus).f(); // (FLD, FADD, FSTP)
                reportSpeed(world, wearer);
                playSound(world, 0x3B, wearer);
            }
        } else if (fn == Fn::FireProtectEngage) {
            wearer.itemBuffs |= 1u;
            playSound(world, 0x66, wearer);
        } else if (fn == Fn::LightningProtectEngage) {
            wearer.itemBuffs |= 4u;
            playSound(world, 0x6A, wearer);
        } else if (fn == Fn::PoisonProtectEngage) { // (checkPoisonProtectEnch)
            wearer.itemBuffs |= 2u;
            playSound(world, 0x6E, wearer);
        } else if (fn == Fn::RegenerationEngage) {
            wearer.itemBuffs |= 0x20u;
            playSound(world, 0x7B, wearer);
        }
    }
}

void disengageEffects(World& world, const things::ModifierDb* modifiers, ServerObject& wearer, const ServerObject& item)
{
    // Its bit cleared unless another worn thing still gives it.
    const auto clear = [&](Fn engage, std::uint8_t bit) {
        if (!stillGiven(world, modifiers, wearer, engage)) {
            wearer.itemBuffs &= static_cast<std::uint8_t>(~bit);
        }
    };
    for (std::size_t slot = 2; slot < 4; ++slot) {
        const things::ModifierDef* m = slotOf(modifiers, item, slot);
        if (!m || m->disengage.fn == Fn::None) {
            continue;
        }
        const Fn fn = m->disengage.fn;
        if (fn == Fn::BrillianceDisengage) {
            clear(Fn::BrillianceEngage, 8);
            playSound(world, 0x4C, wearer);
        } else if (fn == Fn::SpeedDisengage) { // (effectSpeedDisengage: the engage's number off)
            if (wearer.classes & oc::Player) {
                clear(Fn::SpeedEngage, 0x10);
                wearer.speedBonus = (util::X87(wearer.speedBonus) - m->engage.value).f(); // (FLD, FSUB, FSTP)
                reportSpeed(world, wearer);
                playSound(world, 0x3C, wearer);
            }
        } else if (fn == Fn::FireProtectDisengage) { // (modifFireProtection)
            clear(Fn::FireProtectEngage, 1);
            playSound(world, 0x67, wearer);
        } else if (fn == Fn::LightningProtectDisengage) {
            clear(Fn::LightningProtectEngage, 4);
            playSound(world, 0x6B, wearer);
        } else if (fn == Fn::PoisonProtectDisengage) {
            clear(Fn::PoisonProtectEngage, 2);
            playSound(world, 0x6F, wearer);
        } else if (fn == Fn::RegenerationDisengage) { // (a player's)
            if (wearer.classes & oc::Player) {
                clear(Fn::RegenerationEngage, 0x20);
                playSound(world, 0x7C, wearer);
            }
        }
    }
}

float wornProtection(World& world, const things::ModifierDb* modifiers, const ServerObject& o, Fn engage)
{
    // (checkFireProtect and its kin: each FADD on the x87, the sum kept on the stack throughout,
    // FST to the float local at the end.)
    util::X87 sum = 0.0f;
    const auto add = [&](const ServerObject& item) {
        for (std::size_t slot = 0; slot < 4; ++slot) {
            if (const things::ModifierDef* m = slotOf(modifiers, item, slot); m && m->engage.fn == engage) {
                sum = sum + m->engage.value;
            }
        }
    };
    for (const std::uint32_t e : o.inventory) {
        if (const ServerObject* item = world.byExtent(e); item && wornModifiable(*item)) {
            add(*item);
        }
    }
    if (o.classes & kModifiable) {
        add(o);
    }
    return sum.f();
}

float armorValueOf(const things::ModifierDb* modifiers, const ServerObject& item)
{
    if (!(item.classes & oc::Armor) || !modifiers) {
        return 0;
    }
    const things::EquipDef* def = modifiers->findArmor(item.typeName);
    if (!def) {
        return 0;
    }
    float value = def->armorValue;
    // Its first slot's DEFENDEFFECT (ArmorMultiplierEffect
    // DurabilityMultiplierEffect, (1 - its number) + 1, isn't any armour's first). Each on the
    // x87, FSTP to the float.
    if (const things::ModifierDef* m = slotOf(modifiers, item, 0)) {
        if (m->defend.fn == Fn::ArmorMultiplier) {
            value = (util::X87(m->defend.value) * value).f();
        } else if (m->defend.fn == Fn::DurabilityMultiplier) {
            value = (((util::X87(1.0f) - m->defend.value) + 1.0f) * value).f();
        }
    }
    return value;
}

void attackEffects(Fight& fight, ServerObject& weapon, ServerObject* attacker, float& damage)
{
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const things::ModifierDef* m = slotOf(fight.modifiers, weapon, slot);
        if (!m || m->attack.fn == Fn::None) {
            continue;
        }
        const Fn fn = m->attack.fn;
        if (fn == Fn::DamageMultiplier) { // (effectDamageMultiplier: FLD, FMUL, FSTP)
            damage = (util::X87(m->attack.value) * damage).f();
        } else if ((fn == Fn::FireRing || fn == Fn::BlueFireRing) && attacker) {
            // (fireRingEffect / blueFREffect) Cleansing Flame (10), or its blue ring (11), round the
            // attacker at the number's power.
            castDirect(fight, fn == Fn::FireRing ? 0x0A : 0x0B, *attacker, nullptr, attacker->x, attacker->y, m->attack.count);
        }
        // (ProjectileSpeedEffect's, effectProjectileSpeed, multiplies a float 0x220 past the blow's record,
        // on its caller's stack: an original bug; nothing here. ReadinessEffect's and
        // ReplenishmentEffect's do nothing.)
    }
}

void preHitEffects(Fight& fight, ServerObject& victim, ServerObject* attacker, ServerObject* weapon)
{
    if (!weapon) {
        return;
    }
    World& world = fight.world;
    // Not on a friend (but with gameplay flag 1), nor on one INVULNERABLE or behind a
    // REFLECTIVE_SHIELD.
    if (!(world.gameplayFlags & 1u) && attacker && (attacker->classes & (oc::Monster | oc::Player)) && !isEnemy(world, *attacker, victim)) {
        return;
    }
    if (enchanted(victim, Enchant::Invulnerable) || enchanted(victim, Enchant::ReflectiveShield)) {
        return;
    }
    const bool creature = (victim.classes & (oc::Monster | oc::Player)) != 0;
    for (std::size_t slot = 2; slot < 4; ++slot) {
        const things::ModifierDef* m = slotOf(fight.modifiers, *weapon, slot);
        if (!m || m->preHit.fn == Fn::None) {
            continue;
        }
        const Fn fn = m->preHit.fn;
        if ((fn == Fn::Stun || fn == Fn::Confuse) && creature && attacker) {
            // (stunEffect / Obj_ModConfuseEffect) Stun (0x4A) or Confuse
            // (0x0C) at it, the number its power; a player told (INFORM 0x0D: 0 / 1).
            const bool stun = fn == Fn::Stun;
            castDirect(fight, stun ? 0x4A : 0x0C, *attacker, &victim, victim.x, victim.y, m->preHit.count);
            if (victim.classes & oc::Player) {
                informHit(world, victim, stun ? 0 : 1);
            }
        } else if (fn == Fn::Fire || fn == Fn::Lightning) {
            // (fireEffect / lightngEffect) Its number as damage of
            // type 7 (fire: a spark burst of 10 x it, 0xE0) or 9 (lightning: FX 0x81, 0xE1).
            const bool fire = fn == Fn::Fire;
            fight.damage(victim, attacker, weapon->extent, static_cast<int>(m->preHit.value), fire ? 7 : 9);
            // The FX's point is the victim's whole-number spot, 16 bits each.
            const auto ix = static_cast<std::uint16_t>(static_cast<int>(victim.x));
            const auto iy = static_cast<std::uint16_t>(static_cast<int>(victim.y));
            if (fire) {
                const auto size = static_cast<std::uint8_t>(util::ftol(util::X87(m->preHit.value) * 10.0f)); // (FMUL 10, __ftol)
                world.broadcasts().push_back({{0x93, lo(ix), hi(ix), lo(iy), hi(iy), size}, false, 0}); // (netSparkExplosionFx)
            } else {
                world.broadcasts().push_back({{0x81, lo(ix), hi(ix), lo(iy), hi(iy)}, false, 0}); // (netSendPointFx)
            }
            playSound(world, fire ? 0xE0 : 0xE1, victim);
        } else if (fn == Fn::Recoil) {
            // (recoilEffect) Pushed by its number from the weapon (a carried one's
            // spot is where it last was in the world).
            applyForce(world, victim, weapon->x, weapon->y, m->preHit.value);
        }
        // (FrostEffect's and PanicEffect's do nothing.)
    }
}

void preDamageEffects(Fight& fight, ServerObject& victim, ServerObject* source, ServerObject& weapon, int damage)
{
    World& world = fight.world;
    const bool creature = (victim.classes & (oc::Monster | oc::Player)) != 0;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const things::ModifierDef* m = slotOf(fight.modifiers, weapon, slot);
        if (!m || m->preDamage.fn == Fn::None) {
            continue;
        }
        const Fn fn = m->preDamage.fn;
        if (fn == Fn::DrainMana) {
            // (drainMEffect) Its number x the damage (at least 1) of the victim's
            // mana to the attacker, as much as it has.
            if (!source || !creature) {
                continue;
            }
            int amount = static_cast<int>(static_cast<double>(m->preDamage.value) * damage);
            if (amount == 0) {
                amount = 1;
            }
            const int has = manaOf(victim) & 0xFFFF;
            const int taken = has < amount ? has : amount;
            addMana(victim, -taken);
            addMana(*source, taken);
        } else if (fn == Fn::Vampirism) {
            // (vampirismEffect) Its number x the damage (at least 1) to the
            // attacker's health, as much as the victim has; not from an undead monster
            // (subclass 0x40).
            if (!source || !creature || ((victim.classes & oc::Monster) && (victim.subclasses & 0x40u))) {
                continue;
            }
            int heal = static_cast<int>(static_cast<double>(damage) * m->preDamage.value);
            if (heal == 0) {
                heal = 1;
            }
            adjustHealth(world, *source, victim.health < heal ? victim.health : heal);
        } else if (fn == Fn::Poison) {
            // (Obj_ModPoisonEffect) Not through a player's raised shield facing the
            // attacker; else poisoned by 1 to at most its number, a player told (INFORM 0x0D: 2).
            if (victim.player && victim.player->state == 0x10 && source &&
                (facingSides(victim.x, victim.y, victim.direction, source->x, source->y) & 1)) {
                continue;
            }
            if (creature && fight.applyPoison(victim, 1, m->preDamage.count) && (victim.classes & oc::Player)) {
                informHit(world, victim, 2);
            }
        } else if (fn == Fn::Sympathy) {
            // (sympathyEffect) The attacker takes its number x the damage (as
            // much as the victim has).
            if (!source || !creature) {
                continue;
            }
            const int dealt = victim.health < damage ? victim.health : damage;
            fight.takeHealth(*source, static_cast<int>(static_cast<double>(dealt) * m->preDamage.value));
        }
    }
}

void updateEffects(Fight& fight, ServerObject& player)
{
    World& world = fight.world;
    const std::uint32_t frame = fight.frame;
    // (Nothing worn with an update that does something: nothing to do, and no copy of the
    // inventory made.)
    const auto acts = [&](const ServerObject& item) {
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const things::ModifierDef* m = slotOf(fight.modifiers, item, slot);
            if (m && (m->update.fn == Fn::RegenerationUpdate || m->update.fn == Fn::ContinualReplenishmentUpdate)) {
                return true;
            }
        }
        return false;
    };
    if (std::ranges::none_of(player.inventory, [&](std::uint32_t e) {
            const ServerObject* item = world.byExtent(e);
            return item && wornModifiable(*item) && acts(*item);
        })) {
        return;
    }
    for (const std::uint32_t e : std::vector<std::uint32_t>(player.inventory)) {
        ServerObject* item = world.byExtent(e);
        if (!item || !wornModifiable(*item)) {
            continue;
        }
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const things::ModifierDef* m = slotOf(fight.modifiers, *item, slot);
            if (!m || m->update.fn == Fn::None) {
                continue;
            }
            const Fn fn = m->update.fn;
            ServerObject* holder = item->holder ? world.byExtent(item->holder) : nullptr;
            if (fn == Fn::RegenerationUpdate) {
                // (effectRegeneration) Its holder, a second after its last hurt and
                // short of its most: a point back each (FPS x its number / its most)th frame (a
                // wizard's robe's number a third).
                if (!holder || holder->maxHealth == 0 || frame - holder->hurtFrame < kGameFps ||
                    (holder->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || holder->maxHealth <= holder->health) {
                    continue;
                }
                auto every = static_cast<std::uint32_t>(m->update.count);
                if ((item->classes & oc::Armor) && (things::armorBitOf(item->typeName) & 0x4000u)) {
                    every /= 3;
                }
                // Frames between points: 30 x its number / the holder's most health.
                const std::uint64_t period = static_cast<std::uint64_t>(kGameFps) * every / holder->maxHealth;
                if (period != 0 && frame % period == 0) { // (0: the exe divides by it)
                    adjustHealth(world, *holder, 1);
                }
            } else if (fn == Fn::ContinualReplenishmentUpdate) {
                // (attribContinualReplen) A wand's charge back every so many
                // frames, to its most; a player holding it told (MSG_REPORT_CHARGES).
                const auto every = static_cast<std::uint32_t>(m->update.count);
                if (every == 0 || frame % every != 0 || !(item->classes & oc::Wand)) {
                    continue;
                }
                const std::uint8_t before = item->charges;
                auto now = static_cast<std::uint8_t>(before + 1);
                if (item->maxCharges < now) {
                    now = item->maxCharges;
                }
                item->charges = now;
                if (before != now && holder && (holder->classes & oc::Player)) {
                    // MSG_REPORT_CHARGES (0x64): the wand's extent (16 bits), its charges and its most.
                    tell(world, *holder, {0x64, lo(item->extent), hi(item->extent), item->charges, item->maxCharges});
                }
            }
            // (ParasiteUpdate's and AttractionUpdate's do nothing.)
        }
    }
}

bool inverts(World& world, const things::ModifierDb* modifiers, const ServerObject& wearer)
{
    for (const std::uint32_t e : wearer.inventory) {
        const ServerObject* item = world.byExtent(e);
        if (!item || !wornModifiable(*item)) {
            continue;
        }
        for (std::size_t slot = 2; slot < 4; ++slot) {
            // (inversionEffect: its number over 0)
            if (const things::ModifierDef* m = slotOf(modifiers, *item, slot);
                m && m->defendCollide.fn == Fn::Inversion && 0 < m->defendCollide.count) {
                return true;
            }
        }
    }
    return false;
}

void shootEffects(const things::ModifierDb* modifiers, const ServerObject& bow, ServerObject& arrow)
{
    for (std::size_t slot = 2; slot < 4; ++slot) {
        const things::ModifierDef* m = slotOf(modifiers, bow, slot);
        if (!m) {
            continue;
        }
        // The bow's Recoil rides in the arrow's last slot, so the arrow's hit pushes as the bow's would.
        if (m->preHit.fn == Fn::Recoil) {
            arrow.modifiers[3] = m->id;
        } else if (m->attack.fn == Fn::ProjectileSpeed) { // (effectProjectileSpeed)
            arrow.speedCur = (util::X87(m->attack.value) * arrow.speedCur).f(); // (FLD, FMUL, FSTP)
        }
    }
}

} // namespace nox::server
