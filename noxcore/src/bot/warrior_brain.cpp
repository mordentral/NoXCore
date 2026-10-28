// WarriorBrain (warrior_brain.hpp): melee, dodges, blocks, the warrior's abilities and its
// two-handed gear, on top of what every player does (player_brain.cpp).
#include "noxcore/bot/warrior_brain.hpp"

#include "noxcore/client/navigation.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nox::bot {

WarriorBrain::WarriorBrain(Skill skill, std::uint32_t seed)
    : PlayerBrain(skill, seed)
{
    reach_ = kBareReach - kReachMargin;
}

void WarriorBrain::prepare(const BotView& view)
{
    // The reach of what's in hand (a Longsword's 65, a GreatSword's 70, bare hands 40).
    float weaponReach = kBareReach;
    for (const auto& i : view.inventory) {
        if (i.kind == ItemKind::Weapon && i.equipped && i.reach > 0) {
            weaponReach = i.reach;
        }
    }
    reach_ = weaponReach - kReachMargin;
}

bool WarriorBrain::react(const BotView& view, BotBody& body)
{
    return block(view, body);
}

float WarriorBrain::weaponValue(const BotView& view) const
{
    float value = 0;
    for (const auto& i : view.inventory) {
        if (i.kind == ItemKind::Weapon && i.equipped) {
            value = std::max(value, i.value);
        }
    }
    return value;
}

std::uint8_t WarriorBrain::swingCost(const BotView& view) const
{
    for (const auto& i : view.inventory) {
        if (i.kind == ItemKind::Weapon && i.equipped) {
            return i.swingStamina;
        }
    }
    return 0;
}

bool WarriorBrain::block(const BotView& view, BotBody& body)
{
    const bool shield = std::ranges::any_of(view.inventory, [](const OwnItem& i) {
        return i.equipped && i.blocks && i.kind == ItemKind::Armor;
    });
    const bool sword = std::ranges::any_of(view.inventory, [](const OwnItem& i) {
        return i.equipped && i.blocks && i.kind == ItemKind::Weapon;
    });
    if (!shield && !sword) {
        return false;
    }
    // Faces the missile, standing, so the guard turns it.
    const auto turnTo = [&](const SeenMissile& m) {
        // Standing still (state 0x0D) facing it: a greatsword turns it at once, a shield once
        // it's up, a quarter second in (Plr_UpdateState: states 0x0F, 0x10).
        body.setMove(std::nullopt);
        body.setAction(false);
        body.setFacing(directionTo(view.x, view.y, m.x, m.y));
    };
    if (view.frame < blockUntil_) {
        const auto it = std::ranges::find(view.missiles, blocking_, &SeenMissile::netCode);
        if (it != view.missiles.end()) {
            turnTo(*it);
            return true;
        }
        blockUntil_ = 0; // turned, or it hit something else
    }
    // Long enough to get the guard up before it arrives, no longer: blocking costs ground.
    // How many frames ahead to look for a missile: a shield needs longer to come up than a greatsword.
    const float window = (shield ? 14.0f : 6.0f) + view.lagFrames;
    const SeenMissile* m = incoming(view, window, 22);
    if (!m) {
        return false;
    }
    // Frames until it reaches us: the block is held until 4 frames past that, at most 20.
    const auto& t = missiles_.at(m->netCode);
    const float speed2 = t.vx * t.vx + t.vy * t.vy;
    const float arrives = ((view.x - m->x) * t.vx + (view.y - m->y) * t.vy) / speed2;
    blocking_ = m->netCode;
    blockUntil_ = view.frame + std::min<std::uint32_t>(static_cast<std::uint32_t>(arrives) + 4, 20);
    turnTo(*m);
    return true;
}

void WarriorBrain::fight(const BotView& view, BotBody& body, const SeenPlayer& enemy)
{
    const float d = distance(view.x, view.y, enemy.x, enemy.y);
    const bool clear = clearBetween(view, enemy.x, enemy.y);
    body.setCursor(static_cast<int>(enemy.x), static_cast<int>(enemy.y));
    // A blow or a charge coming at us is stepped out of, to one side and back (first: it beats
    // finishing a turn to charge).
    const bool theirBlow = isStrike(enemy.anim) && d <= reach_ + 30;
    const bool theirCharge = enemy.anim == kChargeAnim && d <= 260;
    if (view.frame < dodgeUntil_) {
        body.setAction(false);
        body.setMove(dodgeDirection_);
        runCursor(view, body, dodgeDirection_);
        return;
    }
    // A blow or charge just starting (it wasn't last frame): only then is it dodged.
    const bool fresh = (theirBlow || theirCharge) && !enemyWasAttacking_;
    enemyWasAttacking_ = theirBlow || theirCharge;
    const bool ourBlow = view.frame - swingAt_ < kSwingHold || isStrike(view.anim);
    if (fresh && !ourBlow && chance(skill_.dodgeChance)) { // (never out of our own swing)
        // A swing: back and aside, out of its arc. A charge: straight across its line. Never
        // into lava or flames: the other side, or no dodge.
        const std::uint8_t toward = directionTo(view.x, view.y, enemy.x, enemy.y);
        const int side = chance(50) ? 1 : -1;
        // A charge: a quarter turn across its line. A swing: back (half a turn), 40 aside.
        const auto way = theirCharge ? safeWay(view, toward, side * 64)
                                     : safeWay(view, static_cast<std::uint8_t>(toward + 128), side * 40);
        if (!way) {
            enemyWasAttacking_ = true; // nowhere safe to go: take it
            return melee(view, body, enemy, d);
        }
        dodgeDirection_ = *way;
        dodgeUntil_ = view.frame + (theirCharge ? 12 : 8);
        body.setAction(false);
        body.setMove(dodgeDirection_);
        runCursor(view, body, dodgeDirection_);
        return;
    }
    if (useAbilities(view, body, enemy, d, clear)) {
        return;
    }
    if (d <= reach_ + 40) {
        melee(view, body, enemy, d);
        return;
    }
    body.setAction(false);
    approach(view, body, enemy, reach_ - 8);
}

void WarriorBrain::melee(const BotView& view, BotBody& body, const SeenPlayer& enemy, float d)
{
    nav_.reset();
    const std::uint8_t toward = directionTo(view.x, view.y, enemy.x, enemy.y);
    // A swing takes a while to come round again; heavy weapons longer (estimates: the strike
    // animations run about half a second, the hammer's longer).
    const std::uint32_t recover = (weaponValue(view) >= 100 ? 24u : 16u) + static_cast<std::uint32_t>(skill_.slowdown);
    // And the breath for it: a swing takes the weapon's stamina (a Longsword 70, a WarHammer 100,
    // back at 3 a frame), and with less there's no swing at all (playerInputAttack).
    if (view.frame - swingAt_ >= recover && view.stamina >= swingCost(view)) {
        if (d <= reach_) {
            // In reach and ready: stop and turn to them, then swing the next frame. A blow goes
            // the way we already face: Action in the same input as the turn swings the old way
            // (after a dodge, away from them).
            body.setMove(std::nullopt);
            // (The next think, whatever the server's frame did: it can stand still between thinks
            // or jump by its update interval, and waiting on it froze bots in front of enemies.)
            if (strikeAimAt_ != 0 && thinks() - strikeAimAt_ <= 2) {
                body.setFacing(strikeFacing_);
                body.setAction(true); // (Action for a frame starts the blow)
                swingAt_ = view.frame;
                strikeAimAt_ = 0;
                attacked(view);
                return;
            }
            strikeFacing_ = aimAt(view, enemy.x, enemy.y);
            strikeAimAt_ = thinks();
            body.setFacing(strikeFacing_);
            body.setAction(false);
            return;
        }
        // Ready but a step short: in.
        body.setAction(false);
        body.setMove(toward);
        runCursor(view, body, toward);
        hop(view, body, toward);
        return;
    }
    body.setAction(false);
    if (view.frame - swingAt_ < kSwingHold || isStrike(view.anim)) {
        // Our blow lands partway through its swing, the way we face then: stand and keep
        // turned to them until it has (moving would turn us the way we go, and it'd land
        // there). The animation comes back from the server late, so the time counts too.
        body.setMove(std::nullopt);
        body.setFacing(toward);
        return;
    }
    // Between blows: round them at the edge of reach, not standing in front of theirs, now one
    // way, now the other.
    if (view.frame >= circleFlipAt_) {
        circleSide_ = chance(50) ? 1 : -1;
        circleFlipAt_ = view.frame + nox::util::uniform_int_distribution<std::uint32_t>(30, 75)(rng_);
    }
    int turn = 64; // round them
    if (d < reach_ + 2) {
        turn = 100; // too close: out a little as we go
    } else if (d > reach_ + 26) {
        turn = 36; // drifting off: in a little
    }
    std::uint8_t direction = static_cast<std::uint8_t>(toward + circleSide_ * turn);
    if (view.nav && !view.nav->empty()) {
        // A wall or an obelisk that way: round the other way.
        const auto [ux, uy] = unitOf(direction);
        if (blockedAt(view, view.x + 20 * ux, view.y + 20 * uy) || hazardAt(view, view.x + 20 * ux, view.y + 20 * uy)) {
            circleSide_ = -circleSide_;
            direction = static_cast<std::uint8_t>(toward + circleSide_ * turn);
        }
    }
    body.setMove(direction);
    // A near cursor: walk round them, a step at a time, not run (past 100 the server runs us).
    const auto [ux, uy] = unitOf(direction);
    body.setCursor(static_cast<int>(view.x + 50 * ux), static_cast<int>(view.y + 50 * uy));
}

bool WarriorBrain::useAbilities(const BotView& view, BotBody& body, const SeenPlayer& enemy, float distance, bool clear)
{
    if (!skill_.useAbilities || view.playerClass != 0) {
        return false;
    }
    // abilityReady is indexed by Ability id.
    const auto ready = [&](std::uint8_t a) { return view.abilityReady[a]; };
    // Berserker Charge runs the way we face and can't be turned once going
    // (Plr_UpdateState ignores Orientation while it's active), so it's aimed where the enemy will be when
    // it gets there: our input's way to the server, the frame turning, then the run at
    // kChargeSpeed. It turns the frame before, then goes if that still holds.
    const auto chargePoint = [&] { return lead(enemy, view.lagFrames + 1 + distance / kChargeSpeed); };
    const auto chargeFits = [&](float px, float py) {
        const float d = std::hypot(px - view.x, py - view.y);
        // (Across lava or flames is fine: a charge is over it too fast to burn much.)
        return ready(Ability::BerserkerCharge) && d >= 60 && d <= 260 && clearBetween(view, px, py);
    };
    if (chargeAimAt_ != 0) {
        const bool now = thinks() - chargeAimAt_ <= 2; // the next think (not the server's frame: see strikeAimAt_)
        chargeAimAt_ = 0;
        const auto [px, py] = chargePoint();
        if (now && chargeFits(px, py)) {
            body.setFacing(directionTo(view.x, view.y, px, py));
            body.useAbility(Ability::BerserkerCharge);
            abilityAt_ = view.frame;
            attacked(view);
            return true;
        }
    }
    // New decisions a few times a second (by time: the server's frames come in jumps), and not
    // right after another ability.
    if (view.frame - abilityCheckAt_ < 10 || view.frame - abilityAt_ < 20) {
        return false;
    }
    abilityCheckAt_ = view.frame;
    if (const auto [px, py] = chargePoint(); chargeFits(px, py) && chance(skill_.abilityChance)) {
        body.setMove(std::nullopt);
        body.setAction(false);
        body.setFacing(directionTo(view.x, view.y, px, py));
        chargeAimAt_ = thinks();
        return true;
    }
    // The harpoon flies at the cursor up to MaxHarpoonFlightDistance (gamedata.bin: 300) and drags
    // them in: thrown from 20 inside that, aimed a little ahead of them too.
    if (ready(Ability::Harpoon) && clear && distance >= 90 && distance <= view.numbers.harpoonFlight - 20 && chance(skill_.abilityChance)) {
        const auto [hx, hy] = lead(enemy, view.lagFrames + 2);
        body.setCursor(static_cast<int>(hx), static_cast<int>(hy));
        body.useAbility(Ability::Harpoon);
        abilityAt_ = view.frame;
        return false;
    }
    // War Cry stops spells within 300 (warriorWarcry): for a caster near us.
    if (ready(Ability::WarCry) && enemy.playerClass != 0 && distance <= 220 && chance(skill_.abilityChance)) {
        body.useAbility(Ability::WarCry);
        abilityAt_ = view.frame;
        return false;
    }
    return false;
}

bool WarriorBrain::underGeared(const BotView& view) const
{
    // Without a GreatSword or a WarHammer (104, 150; the Longsword everyone starts with is 39):
    // those first, before picking fights.
    return std::ranges::none_of(view.inventory, [](const OwnItem& i) { return i.kind == ItemKind::Weapon && i.twoHanded; });
}

void WarriorBrain::tendGear(const BotView& view, BotBody& body)
{
    // A GreatSword or WarHammer, once ours, stays in hand: they take both hands, so a shield
    // would knock it out. Only held or frozen in place (enchantments 5 / 25: nothing to swing
    // at, everything to block) the shield goes on, and the two-hander back once we can move.
    // Enchantment bits (bit n = ENCHANT n).
    constexpr std::uint32_t kHeld = 1u << 5, kFrozen = 1u << 25;
    const bool stuck = (view.buffs & (kHeld | kFrozen)) != 0;
    // The best usable two-hander we carry, if any.
    const OwnItem* twoHander = nullptr;
    for (const auto& i : view.inventory) {
        if (i.kind == ItemKind::Weapon && i.twoHanded && i.usable && (!twoHander || i.value > twoHander->value)) {
            twoHander = &i;
        }
    }
    const auto isShield = [](const OwnItem& i) { return i.kind == ItemKind::Armor && i.blocks; };
    if (twoHander) {
        const OwnItem* shield = nullptr;
        for (const auto& i : view.inventory) {
            if (isShield(i) && i.usable && (!shield || i.value > shield->value)) {
                shield = &i;
            }
        }
        const bool shieldOn = shield && shield->equipped;
        if (stuck && shield && !shieldOn) {
            body.equip(shield->netCode);
            nextItemAction_ = view.frame + 20;
            return;
        }
        if (!stuck && !twoHander->equipped) {
            body.equip(twoHander->netCode); // back to it (a shield came off, or just picked up)
            nextItemAction_ = view.frame + 20;
            return;
        }
    }
    // The best weapon we carry if it beats the one in hand (holding a two-hander: only a better
    // two-hander), else the armour piece that beats most what's worn there (no shield with a
    // two-hander). Each tried once: the server says no to what doesn't fit.
    const bool wielding = std::ranges::any_of(view.inventory, [](const OwnItem& i) { return i.kind == ItemKind::Weapon && i.equipped; });
    const float wielded = wielding ? weaponValue(view) : -1;
    const OwnItem* gear = weaponUpgrade(view, wielded, twoHander != nullptr);
    if (!gear) {
        gear = armorUpgrade(view, twoHander != nullptr);
    }
    if (gear) {
        equipOnce(view, body, *gear);
    }
}

} // namespace nox::bot
