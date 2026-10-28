// Fighting on the server (combat.hpp): who is whose enemy, an attack's reach and hit, the
// damage functions (players and NPCs, the special objects, the default), armour and blocks,
// poison, a unit's death and a player's respawn.
#include "noxcore/server/combat.hpp"
#include "noxcore/server/item_effects.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/books.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/server/walls.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
using util::X87;

constexpr float kSlopeLow = std::bit_cast<float>(0x3ED37A5Fu);  // (0.41291326: about tan 22.5)
constexpr float kSlopeHigh = std::bit_cast<float>(0x401AF288u); // (2.42104149: about tan 67.5)

/// The sides of a facing each sector is on, by 8-way direction (dy x 3 + 4 + dx)
/// and sector (the facing test's).
constexpr int kSides[9][16] = {
    {0, 9, 6, 0, 5, 1, 4, 0, 10, 8, 2, 0, 0, 0, 0, 0}, {0, 1, 2, 0, 4, 5, 6, 0, 8, 9, 10, 0, 0, 0, 0, 0},
    {0, 5, 10, 0, 6, 4, 2, 0, 9, 1, 8, 0, 0, 0, 0, 0}, {0, 8, 4, 0, 1, 9, 5, 0, 2, 10, 6, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  {0, 4, 8, 0, 2, 6, 10, 0, 1, 5, 9, 0, 0, 0, 0, 0},
    {0, 10, 5, 0, 9, 8, 1, 0, 6, 2, 4, 0, 0, 0, 0, 0}, {0, 2, 1, 0, 8, 10, 9, 0, 4, 6, 5, 0, 0, 0, 0, 0},
    {0, 6, 9, 0, 10, 2, 8, 0, 5, 4, 1, 0, 0, 0, 0, 0},
};

bool creature(const ServerObject& o) { return (o.classes & (oc::Monster | oc::Player)) != 0; }

/// A blow's damage as its target's DAMAGE and the walls take it
/// (playerTraceAttack: FLD, FADD 0.5, __ftol): to the nearest, a half up.

} // namespace

float armorShare(float value, float total, int amount)
{
    if (value <= 0.0f || total <= 0.0f || amount <= 0) {
        return 0.0f;
    }
    // FLD the piece's value, FDIV the whole, FIMUL the amount, FSTP.
    return ((X87(value) / total) * X87::fromDouble(amount)).f();
}

void reflectMissile(ServerObject& m, const ServerObject& by)
{
    // On the x87 as projectileReflect has it: dx and dy stay on the stack, -dy and the length are
    // stored to floats, the parts' products each FSTP'd but the last.
    const X87 dx = X87(m.x) - by.x;
    const X87 dy = X87(m.y) - by.y;
    m.direction = static_cast<std::uint16_t>((m.direction + 0x80) & 0xFF);
    const float negDy = (-dy).f();
    const float len = (util::sqrt(dy * dy + dx * dx) + 0.1f).f();
    // The velocity split along and across the line from `by`; the part along it is reversed.
    const X87 along = (dy * m.vy + dx * m.vx) / len;
    const float alongX = (along * dx / len).f();
    const float alongY = (along * dy / len).f();
    const X87 across = (X87(negDy) * m.vx + dx * m.vy) / len;
    const float acrossX = (across * negDy / len).f();
    m.vx = (X87(acrossX) - alongX).f();
    m.vy = (across * dx / len - alongY).f();
    m.newX = m.lastX; // (projectileReflect: NewPos = PrevPos, its last turn's start)
    m.newY = m.lastY;
    m.touch();
}

namespace {

/// What a raised shield or a parry can't stop (server_handler_PlayerDamage): Fist of Vengeance's fists and the
/// meteor, and when it's what hit, the toxic clouds.
bool unblockable(const ServerObject& o, bool hitter)
{
    const NameId n = o.typeName.id();
    return n == NameId::SmallFist || n == NameId::MediumFist || n == NameId::LargeFist || n == NameId::Meteor ||
           (hitter && (n == NameId::ToxicCloud || n == NameId::SmallToxicCloud));
}

} // namespace

std::pair<int, int> gridDirection(int direction)
{
    constexpr int kThreshold = 6;
    const auto [c, s] = sinCos16(direction);
    const int dx = kThreshold < c ? 1 : (-kThreshold <= c ? 0 : -1);
    const int dy = kThreshold < s ? 1 : (s < -kThreshold ? -1 : 0);
    return {dx, dy};
}

int facingSides(float fx, float fy, int direction, float tx, float ty)
{
    const auto [gx, gy] = gridDirection(direction);
    // (On the x87: dx and dy stored; each line's value FMUL, FSUB, stored to a float, the second
    // tested on the stack as it's stored.)
    const float dx = (X87(tx) - fx).f();
    const float dy = (X87(ty) - fy).f();
    const auto line = [&](float k) { return X87(dx) * k - dy; };
    // Which side of four lines through (fx, fy), at about 22.5 and 67.5 degrees each way, the
    // point is on: a 4-bit code naming one of eight sectors (others fall on a line: sector 0).
    const int code = (line(kSlopeLow).f() < 0 ? 0 : 8) | (line(kSlopeHigh) < X87(0.0f) ? 0 : 4) |
                     (line(-kSlopeHigh).f() < 0 ? 0 : 2) | (0 <= line(-kSlopeLow).f() ? 1 : 0);
    int sector = 0;
    switch (code) {
    case 0: sector = 2; break;
    case 2: sector = 6; break;
    case 3: sector = 4; break;
    case 4: sector = 10; break;
    case 0xB: sector = 5; break;
    case 0xC: sector = 8; break;
    case 0xD: sector = 9; break;
    case 0xF: sector = 1; break;
    default: sector = 0; break;
    }
    // Row: the facing's 8-way direction (dy x 3 + 4 + dx); column: the sector.
    return kSides[gy * 3 + 4 + gx][sector];
}

namespace {

/// Up its owners to a player, or the last of them (findParentChainPlayer).
const ServerObject& parentChain(const World& world, const ServerObject& o)
{
    const ServerObject* p = &o;
    for (int guard = 0; !(p->classes & oc::Player) && p->owner != 0 && guard < 16; ++guard) {  // (at most 16 owners up)
        const auto it = world.objects().find(p->owner);
        if (it == world.objects().end()) {
            break;
        }
        p = &it->second;
    }
    return *p;
}

bool fish(const ServerObject& o) { return o.typeName == NameId::FishSmall || o.typeName == NameId::FishBig; }
bool frog(const ServerObject& o) { return o.typeName == NameId::GreenFrog; }
bool rat(const ServerObject& o) { return o.typeName == NameId::Rat; }

} // namespace

bool isEnemy(const World& world, const ServerObject& a, const ServerObject& b)
{
    // Obj_IsEnemyTo.
    if (&a == &b) {
        return false;
    }
    if (b.monster && (b.monster->status & things::MonsterStatus::Morphed)) { // (a mimic as a chest)
        return false;
    }
    const std::uint32_t ca = a.classes;
    const std::uint32_t cb = b.classes;
    if ((ca & oc::Player) && b.typeName == NameId::Polyp) {
        return true;
    }
    if ((ca & oc::Player) && (cb & oc::MonsterGenerator)) {
        return true;
    }
    // Players and female NPCs (monster subclass 0x20) are never at odds, nor monsters and generators.
    if (((ca & oc::Monster) && (cb & oc::MonsterGenerator)) || ((ca & oc::Player) && (cb & oc::Monster) && (b.subclasses & 0x20u)) ||
        ((ca & oc::Monster) && (a.subclasses & 0x20u) && (cb & oc::Player))) {
        return false;
    }
    if (fish(a) || frog(a)) { // (the fish and the frogs: at odds with all but their own kinds)
        return !fish(b) && !frog(b);
    }
    if (rat(a)) {
        return !rat(b);
    }
    if (fish(b)) {
        return false;
    }
    // Shopkeepers (monster subclass 8) are nobody's enemy.
    if (((cb & oc::Monster) && (b.subclasses & 8u)) || ((ca & oc::Monster) && (a.subclasses & 8u))) {
        return false;
    }
    if (enchanted(a, Enchant::Charming) || enchanted(b, Enchant::Charming)) {
        return false;
    }
    if (b.player && (b.player->statusFlags & 1u)) { // (an observer)
        return false;
    }
    const ServerObject& pa = parentChain(world, a);
    const ServerObject& pb = parentChain(world, b);
    if (&pa == &pb) {
        return false;
    }
    if (pa.team != 0 && pa.team == pb.team) {
        return false;
    }
    if ((pa.classes & oc::Player) && (pb.classes & oc::MonsterGenerator)) {
        return true;
    }
    if ((pa.classes & oc::Monster) && (pb.classes & oc::MonsterGenerator)) {
        return false;
    }
    if (!world.quest() && (a.classes & oc::Monster) && b.typeName == NameId::WillOWisp && b.monster) {
        // The Will-o'-Wisp outside a quest: a monster's enemy only while it fights
        // (checkMobAction).
        for (int i = b.monster->top; 0 <= i; --i) {
            if (b.monster->stack[static_cast<std::size_t>(i)].action == 15) { // (ACTION_FIGHT)
                return true;
            }
        }
        return false;
    }
    // Two monsters with no team (and no player over them) are on the same side.
    if (pa.team == 0 && pb.team == 0 && (pa.classes & oc::Monster) && (pb.classes & oc::Monster)) {
        return false;
    }
    return true;
}

ServerObject* objectAtCursor(World& world, const ServerObject& player)
{
    // findObjectAtCursor: the units within 100 of the cursor (unitsGetInCircle,
    // by the grid), each through playerCursorScanFn; the frontmost (its
    // y + z, from 0, strictly more) wins.
    if (!player.player) {
        return nullptr;
    }
    const float cx = player.player->cursorX;
    const float cy = player.player->cursorY;
    ServerObject* best = nullptr;
    float bestFront = 0;
    world.unitsInCircle(cx, cy, 100.0f, [&](ServerObject& o) {
        if (&o == &player || o.gone() || o.pendingDelete || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return;
        }
        if (enchanted(o, Enchant::Invisible) && !enchanted(player, Enchant::Infravision)) {
            return;
        }
        // Only monsters, players, pickups and triggers (or the Polyp) can be picked.
        if (!(o.classes & 0x80000206u) && o.typeName != NameId::Polyp) {
            return;
        }
        // (mapCheck: the bodies in the way, then the ray, flags 9)
        if (!bodiesClear(world, o, player)) {
            return;
        }
        // An observing player isn't picked (its status bit 1).
        if ((o.classes & oc::Player) && o.player && (o.player->statusFlags & 1u)) {
            return;
        }
        // Class 0x200 only while airborne.
        if ((o.classes & 0x200u) && !(o.flags & ObjFlag::Airborne)) {
            return;
        }
        // (On the x87: the top, the radius squared (Math_FloatToInt) and the circle's sides each
        // stored; the cursor's squared distance on the stack against the FILD'd square.)
        const float top = (X87(o.y) - o.z).f();
        if (o.shape == ShapeKind::Circle) {
            const int r2 = util::floatToInt(X87(o.radius) * o.radius);
            if (cy == top) {
                if (!((X87(o.x) - o.radius).f() < cx && cx < (X87(o.radius) + o.x).f())) {
                    return;
                }
            } else {
                const X87 ex = X87(cx) - o.x;
                const X87 ey = X87(cy) - top;
                if (!(ey * ey + ex * ex < X87::fromDouble(r2))) {
                    return;
                }
            }
        } else if (o.shape == ShapeKind::Box) {
            // (the box test about (x, y - z); the rectangles the exe tries after it can never
            // pass: each asks a value both under and over the cursor's)
            if (!insideBox(o, o.x, top, cx, cy)) {
                return;
            }
        } else {
            return;
        }
        // The frontmost (largest y + z) wins.
        const X87 front = X87(o.z) + o.y; // (FLD, FADD, FCOM the best: FSTP'd when it wins)
        if (front <= X87(bestFront)) {
            return;
        }
        bestFront = front.f();
        best = &o;
    });
    return best;
}

bool Fight::attack(ServerObject& a, const Attack& blow)
{
    // A weapon's blow: its ATTACKEFFECTs on it first (playerAttack runs
    // itemApplyAttackEffect before each weapon's playerTraceAttack; item_effects.hpp).
    Attack atk = blow;
    // The record's place, the walls' box's centre: the attacker's position as the record is made
    // (a hammer's, its head's), not where the hit may since have moved it (a script's MoveObject
    // run by the blow).
    const float px = atk.area ? atk.x : a.x;
    const float py = atk.area ? atk.y : a.y;
    if (ServerObject* weapon = atk.weapon ? world.byExtent(atk.weapon) : nullptr) {
        attackEffects(*this, *weapon, &a, atk.damage);
    }
    // playerTraceAttack: the hit, then the walls in the box of its reach about
    // the blow (from a hammer's head, 25 further) take it too, from its weapon or the attacker
    // (mapDamageToWalls).
    ServerObject* struck = nullptr;
    const bool hitAny = strike(a, atk, struck);
    ServerObject* weapon = atk.weapon ? world.byExtent(atk.weapon) : nullptr;
    if (things) {
        const float extra = atk.area ? 25.0f : 0.0f;
        // The wall cells (23 units each) within the box of the blow's reach (each edge FLD, FSUB or
        // FADD twice, FSTP, Math_FloatToInt; the radius FLD the extra, FADD the reach, FSTP).
        const auto cell = [](X87 v) { return util::floatToInt(v) / 23; };
        Walls{world, *things, rng, frame}.damageAround(
            cell(X87(px) - atk.range - extra), cell(X87(py) - atk.range - extra),
            cell(X87(atk.range) + px + extra), cell(X87(atk.range) + py + extra), px, py, (X87(extra) + atk.range).f(),
            roundedDamage(atk.damage), atk.type, weapon ? weapon : &a);
        // The weapon's own wear when it hit something (playerDamageWeapon,
        // ItemDamagePercentage of the blow, (float)0.03 x its float, from the one struck as both
        // source and weapon: none for a hammer's, whose hit picks no one). Players' and NPCs'
        // alike (playerAttack); a monster's other attacks don't come here.
        if (weapon && hitAny) {
            const double amount = static_cast<double>(world.number("ItemDamagePercentage")) * static_cast<double>(atk.damage);
            Items{world, *things, modifiers, rng, frame}.wear(a, *weapon, amount, struck, struck, atk.type, true);
        }
    }
    return hitAny;
}

bool Fight::strike(ServerObject& a, const Attack& atk, ServerObject*& struck)
{
    // The hit itself.
    bool hitAny = false;
    struck = nullptr;
    const auto hit = [&](ServerObject& t) {
        // Not itself, not off the attack's facing sides, not DEAD or NO_COLLIDE (0x40), and
        // ALLOW_OVERLAP ones only when the attack allows them.
        if (&t == &a || !(atk.sides & facingSides(a.x, a.y, a.direction, t.x, t.y)) || (t.flags & (ObjFlag::Dead | 0x40u)) ||
            (!atk.hitsOverlapping && (t.flags & ObjFlag::AllowOverlap))) {
            return;
        }
        if (t.material != 0x4000u) { // (a thing of no MATERIAL isn't a hit: the miss sound)
            hitAny = true;
        }
        if (world.traceRay(a.x, a.y, t.x, t.y, Trace::Doors | Trace::ThroughWindows)) {
            preHitEffects(*this, t, &a, atk.weapon ? world.byExtent(atk.weapon) : nullptr); // (playerPreAttackEffects)
            damage(t, &a, atk.weapon, roundedDamage(atk.damage), atk.type);
            // A solo game's player striking a thing (not a creature) with health left, not deleted
            // (0x20: ours pendingDelete): a puff where it stands (MSG_FX_DAMAGE_POOF,
            // netSendPointFx(0x8B)).
            if (world.solo() && (a.classes & oc::Player) && !(t.classes & oc::Monster) && t.maxHealth != 0 &&
                !(t.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && !t.pendingDelete) {
                pointFx(0x8B, t.x, t.y);
            }
            if (atk.weapon == 0 && a.player && a.player->punch == 0x19 && joinsMotion(t)) {
                // The hook knocks it back (objectApplyForce: 20).
                applyForce(world, t, a.x, a.y, 20.0f);
            }
        }
    };
    // The circle search's test (its callback, on the x87): dx stored; dy stored too but squared as
    // the stack's value times the stored one; the depth on the stack.
    const auto depthIn = [&](ServerObject& t, float px, float py, float r) -> X87 {
        if (t.shape == ShapeKind::Box) {
            float nx = 0;
            float ny = 0;
            return X87(boxDepth(t, px, py, r, nx, ny, rng));
        }
        const float dx = (X87(px) - t.x).f();
        const X87 dy53 = X87(py) - t.y;
        const float dy = dy53.f();
        const X87 centre = util::sqrt(dy53 * dy + X87(dx) * dx);
        return X87(r) - (t.shape == ShapeKind::Circle ? centre - t.radius : centre);
    };
    if (atk.area) {
        // A hammer: all within its reach of its head.
        const float r = atk.range;
        // (By the grid, its box each edge FSTP'd: each struck as it's found.)
        world.unitsInRect((X87(atk.x) - r).f(), (X87(atk.y) - r).f(), (X87(r) + atk.x).f(), (X87(r) + atk.y).f(),
                          [&](ServerObject& t) { // (no 0x20 test: a deleted one struck too)
            if (X87(0.0f) < depthIn(t, atk.x, atk.y, r)) {
                hit(t);
            }
        });
        return hitAny;
    }
    // The nearest in reach.
    ServerObject* best = nullptr;
    float bestDist = atk.range;
    const float r = atk.range;
    const auto [fx, fy] = directionVector(a.direction);
    world.unitsInRect((X87(a.x) - r).f(), (X87(a.y) - r).f(), (X87(r) + a.x).f(), (X87(r) + a.y).f(), [&](ServerObject& t) { // (by the grid)
        // (The search's depth first, for every one found: the attacker's own too, whose box, were
        // it one, draws a random way out from its centre)
        float nx = 0;
        float ny = 0;
        if (!(X87(0.0f) < depthIn(t, a.x, a.y, r))) {
            return;
        }
        if (&t == &a) { // (the reach test's 0x8049, not 0x20)
            return;
        }
        // Not DEAD, NO_COLLIDE (0x40), ALLOW_OVERLAP or BELOW.
        if (t.flags & (ObjFlag::Dead | 0x40u | ObjFlag::AllowOverlap | ObjFlag::Below)) {
            return;
        }
        // A thing that isn't a creature must not be SHORT (0x10), unless it's MISSILE_HIT (0x80).
        if (!(creature(t) || !(t.flags & 0x10u) || (t.flags & 0x80u))) {
            return;
        }
        if (!isEnemy(world, a, t) && creature(t)) {
            return;
        }
        if (!canInteract(world, a, t, 1) || !(0 < bestDist)) {
            return; // (unitCanInteractWith(attacker, it, 1): not blind, its sneak, bodies, walls)
        }
        // (On the x87: dx stored, dy stored and squared as the stack's value times the stored one,
        // the distance kept on the stack, FSTP'd to the best distance only when it wins.)
        const float vx = (X87(t.x) - a.x).f();
        const X87 vy53 = X87(t.y) - a.y;
        const float vy = vy53.f();
        X87 d = util::sqrt(vy53 * vy + X87(vx) * vx);
        // Within 60 degrees of its facing (the cosine over 0.5).
        if (d == X87(0.0f) || !(X87(0.5f) < X87(vy) / d * fy + X87(vx) / d * fx)) {
            return;
        }
        if (t.shape == ShapeKind::Circle) {
            d = d - t.radius;
        } else if (t.shape == ShapeKind::Box) {
            const float depth = boxDepth(t, a.x, a.y, bestDist, nx, ny, rng);
            if (depth < 0) {
                return;
            }
            d = X87(bestDist) - depth;
        }
        if (d < X87(0.0f)) {
            d = 0.0f;
        }
        // The nearest wins, but a monster beats a non-monster, and the first monster picked is never
        // replaced (as the exe does it).
        const bool bestIsMonster = best && (best->classes & oc::Monster);
        if ((d < X87(bestDist) || (best && !bestIsMonster && (t.classes & oc::Monster))) && !bestIsMonster) {
            bestDist = d.f();
            best = &t;
        }
    });
    struck = best;
    if (best) {
        hit(*best);
    }
    return hitAny;
}

namespace {

/// What hits hand to hand: with nothing in hand a player or a monster that
/// fights so (subclass 0x10); else a weapon not a bow, quiver, chakram or the like, or a staff
/// being swung, or a monster itself.
bool handToHand(const ServerObject& src, const ServerObject* weapon)
{
    if (!weapon) {
        return (src.classes & oc::Player) || ((src.classes & oc::Monster) && (src.subclasses & 0x10u));
    }
    if (weapon->classes & oc::Wand) {
        if (!(weapon->subclasses & 0x47F0000u) || weapon->staffMelee) {
            return true;
        }
    } else if ((weapon->classes & oc::Weapon) && !(weapon->subclasses & 0x47F00FEu)) {
        return true;
    }
    return (weapon->classes & oc::Monster) != 0;
}

/// A war hammer (a weapon of subclass 0x4000).
bool hammer(const ServerObject* weapon)
{
    return weapon && (weapon->classes & oc::Weapon) && (weapon->subclasses & 0x4000u);
}

} // namespace

ServerObject* Fight::parentPlayer(ServerObject* o)
{
    // (findParentChainPlayer: up the owners to a player, or the last)
    for (int guard = 0; o && !(o->classes & oc::Player) && o->owner != 0 && guard < 16; ++guard) {
        ServerObject* next = world.byExtent(o->owner);
        if (!next) {
            break;
        }
        o = next;
    }
    return o;
}

float Fight::protection(const ServerObject& t, int e, const char* table) const
{
    // (checkFireProtect / checkElectrProtect) Worn items'
    // protecting enchantments (item_effects.hpp), at most a half; the enchantment's table at its
    // power on top; at most 0.6.
    float items = wornProtection(world, modifiers, t, e == Enchant::ProtectFromFire ? things::EffectFn::FireProtectEngage : things::EffectFn::LightningProtectEngage);
    if (0.5f <= items) { // (fire's over 0.5, electricity's from it: the same)
        items = 0.5f;
    }
    float prot = items;
    if (enchanted(t, e)) {
        // (FLD the table's float, FADD the items' local: returned on the stack, the caller's FST
        // chops it; the cap's FCOM on the stack's value comes to the same.)
        prot = (X87(world.number(table, static_cast<std::size_t>(enchantPower(t, e) - 1))) + items).f();
    }
    return std::min(prot, 0.6f);
}

int Fight::carryDamage(ServerObject& t, float amount)
{
    // playerDecrementHPMana: the fraction left over carried to the next (a
    // player's or a monster's damageCarry): FLD the amount (its float argument), FADD the carry,
    // FSTP; Math_FloatToInt; the carry the amount less its whole part (exact).
    if (t.player) {
        amount = (X87(amount) + t.player->damageCarry).f();
    } else if ((t.classes & oc::Monster) && t.monster) {
        amount = (X87(amount) + t.monster->damageCarry).f();
    }
    const int whole = util::chop(amount); // (Math_FloatToInt, chopped)
    if (t.player) {
        t.player->damageCarry = (X87(amount) - X87::fromDouble(whole)).f();
    } else if ((t.classes & oc::Monster) && t.monster) {
        t.monster->damageCarry = (X87(amount) - X87::fromDouble(whole)).f();
    }
    return whole;
}

void Fight::wearArmor(ServerObject& t, ServerObject* src, ServerObject* obj3, int amount, int type)
{
    // playerDamageItems: each worn piece its share of what the armour took, by
    // its value over the whole (itemApplyDefendEffect: by its effectiveness; on
    // the x87, chopped to the float equipDamage takes); a player's whole
    // or an NPC's; worn from the blow's source and weapon, by its type.
    const bool npc = !t.player && (t.classes & oc::Monster) && (t.subclasses & 0x10u) && t.monster;
    if ((!t.player && !npc) || !things || !modifiers) {
        return;
    }
    const float total = t.player ? t.player->armorValue : t.monster->armorValue;
    for (const std::uint32_t e : std::vector<std::uint32_t>(t.inventory)) {
        ServerObject* item = world.byExtent(e);
        if (!item || item->holder != t.extent || !(item->classes & oc::Armor) || !(item->flags & ObjFlag::Equipped)) {
            continue;
        }
        const float share = armorShare(armorValueOf(modifiers, *item), total, amount);
        Items{world, *things, modifiers, rng, frame}.wear(t, *item, share, src, obj3, type, false);
    }
}

float Fight::metalFactor(const ServerObject& t) const
{
    // Worn metal armour (not of Material7) adds its value, a plate's 0.15
    // (subclass 0x2000000), to 1: each FADD to the float local, FSTP (chopped).
    float f = 1.0f;
    const things::ModifierDef* material7 = modifiers ? modifiers->findModifier("Material7") : nullptr;
    for (const std::uint32_t e : t.inventory) {
        const ServerObject* item = const_cast<World&>(world).byExtent(e);
        if (!item || !(item->classes & oc::Armor) || !(item->material & 0x10u) || !(item->flags & ObjFlag::Equipped)) {
            continue;
        }
        if (material7 && modifiers->modifier(item->modifiers[1]) == material7) {
            continue;
        }
        if (item->subclasses & 0x2000000u) {
            f = (X87(0.15f) + f).f();
        } else if (const things::EquipDef* def = modifiers ? modifiers->findArmor(item->typeName) : nullptr) {
            f = (X87(def->armorValue) + f).f();
        }
    }
    return f;
}

int Fight::damage(ServerObject& t, ServerObject* source, std::uint32_t weapon, int amount, int type)
{
    ServerObject* obj3 = weapon ? world.byExtent(weapon) : nullptr;
    // (noxcpp: a mode watching hits on it may change the amount or cancel the hit.)
    if (world.modeHooks && (t.modeEvents & ModeEvent::Damage) && !world.modeHooks->damage(*this, t, source, obj3, amount, type)) {
        return 0;
    }
    if (t.player || t.damageFn == NameId::PlayerDamage) { // (the NPC's DAMAGE too)
        return playerDamage(t, source, obj3, amount, type);
    }
    if (t.damageFn == NameId::BallDamage) {
        return 0; // (BallDamage: the flagball takes none)
    }
    // (No health to lose is no reason to stop: DefaultDamage runs whole for whatever its caller
    // struck, only unitDamageClear asks for HealthData's Max. A story creature kept
    // at none (MonsterXfer's unknown361), a shopkeeper, a door, a sign is still marked hurt (its
    // IsHit script), the two materials' sound plays, invisibility goes...)
    if (t.damageFn == NameId::WeaponDamage) {
        // WeaponDamage: a weapon or wand, only while held (or by lava, 0xC).
        if (!(t.classes & (oc::Weapon | oc::Wand)) || (t.holder == 0 && type != 0xC)) {
            return 0;
        }
        return defaultDamage(t, source, obj3, amount, type);
    }
    if (t.damageFn == NameId::ArmorDamage) {
        // damageArmor: armour, only while held (or by lava); a fall's twice on
        // metal; none at all for 0.
        if (!(t.classes & oc::Armor) || (t.holder == 0 && type != 0xC)) {
            return 0;
        }
        if (type == 2 && (t.material & 0x10u)) {
            amount *= 2;
        }
        if (amount == 0) {
            return 0;
        }
        return defaultDamage(t, source, obj3, amount, type);
    }
    if (t.damageFn == NameId::SkeletonDamage) {
        // SkeletonDamage (the skeletons): INVULNERABLE as DefaultDamage; a blow from the front
        // (checkReflectShield's side 1, from where its weapon or source was) while its shield is up
        // (BLOCK_ATTACK past half its animation's frames) is stopped, 0x36E.
        if (enchanted(t, Enchant::Invulnerable)) {
            // The invulnerable "ping" at most every fourth frame.
            if ((frame & 3u) == 0) {
                playSound(world, 0x47, t);
            }
            return 1;
        }
        if (source) {
            const ServerObject& from = obj3 ? *obj3 : *source;
            if ((facingSides(t.x, t.y, t.direction, from.lastX, from.lastY) & 1) && t.monster && 0 <= t.monster->top &&
                t.monster->stack[static_cast<std::size_t>(t.monster->top)].action == MonsterAction::BlockAttack &&
                (t.monster->animFrames >> 1) < t.monster->animFrame) {
                playSound(world, 0x36E, t);
                return 1;
            }
        }
        return defaultDamage(t, source, obj3, amount, type);
    }
    if (t.damageFn == NameId::MechGolemDamage) {
        // MechGolemDamage: electricity twice.
        if (type == 9 || type == 0x11) {
            amount *= 2;
        }
        return defaultDamage(t, source, obj3, amount, type);
    }
    if (t.damageFn == NameId::FlammableDamage) {
        // damageFlammable (black powder barrels): fire, lava or a blast ends it.
        if (type == 1 || type == 0xC || type == 7) {
            amount = 9999999;
        }
    } else if (t.damageFn == NameId::BlackPowderDamage) {
        // damageBlackPowder: only blades, fire, crushing and lava touch it, and
        // set it alight at once.
        if (type != 1 && type != 0xC && type != 2 && type != 0) {
            return 0;
        }
        amount = 999999;
    } else if (t.damageFn == NameId::MonsterGeneratorDamage) {
        // damageMonsterGen: not when broken; its sparks (0xF0 0x1A, towards the
        // blow, 22 out) at most once in 20 frames (or each 30th); its hurt handler when it lost
        // health; its look by what's left (xStatus 0x100 under 2/3, 0x200 under 1/3).
        if (t.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
            return 0;
        }
        if (20u < frame - t.hurtFrame || frame % 30 == 0) {
            float px = t.x;
            float py = t.y;
            if (obj3) {
                // (On the x87: each offset + 0.01 stored, utilNormalizeVector, x 22 + the place
                // stored; a zero length's NaN as the game's.)
                float vx = (X87(obj3->x) - t.x + 0.01f).f();
                float vy = (X87(obj3->y) - t.y + 0.01f).f();
                util::normalizeVector(vx, vy);
                px = (X87(vx) * 22.0f + t.x).f();
                py = (X87(vy) * 22.0f + t.y).f();
            }
            const auto ix = static_cast<std::uint16_t>(static_cast<int>(px));
            const auto iy = static_cast<std::uint16_t>(static_cast<int>(py));
            world.broadcasts().push_back({{0xF0, 0x1A, static_cast<std::uint8_t>(ix), static_cast<std::uint8_t>(ix >> 8), static_cast<std::uint8_t>(iy),
                                           static_cast<std::uint8_t>(iy >> 8)},
                                          false, 0});
            playSound(world, 0x3E9, t);
        }
        const std::uint16_t before = t.health;
        const int r = defaultDamage(t, source, obj3, amount, type);
        if (t.health < before && t.generator && world.script) {
            world.script->callBlock(t.generator->onHurt, source ? source->extent : 0, t.extent);
        }
        if (!(t.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            // (0.333 and 0.666 as the exe's floats: FILD, FMUL, FSTP, Math_FloatToInt)
            const int third = util::floatToInt(X87::fromDouble(t.maxHealth) * std::bit_cast<float>(0x3EAA7EFAu));
            const int twoThirds = util::floatToInt(X87::fromDouble(t.maxHealth) * std::bit_cast<float>(0x3F2A7EFAu));
            if (third < t.health) {
                if (twoThirds >= t.health && !(t.xStatus & 0x100u)) {
                    t.xStatus |= 0x100u;
                    t.touch();
                }
            } else {
                t.xStatus &= ~0x100u;
                if (!(t.xStatus & 0x200u)) {
                    t.xStatus |= 0x200u;
                }
                t.touch();
            }
        }
        return r;
    } // (StoneDamage is DefaultDamage's.)
    return defaultDamage(t, source, obj3, amount, type);
}

int Fight::playerDamage(ServerObject& t, ServerObject* src, ServerObject* obj3, int dmg, int type)
{
    // PlayerDamage: a player's, or an NPC's (a monster of subclass 0x10, with its worn bits
    // and its armour value); anything else takes none.
    if (t.flags & (ObjFlag::NoUpdate | ObjFlag::Dead)) {
        return 0;
    }
    if (enchanted(t, Enchant::Invulnerable)) {
        if ((frame & 3u) == 0) {
            playSound(world, 0x47, t);
        }
        return 1;
    }
    // A solo game's player is never hurt by its own doing (but type 0xF).
    if (world.solo() && parentPlayer(src) == &t && (t.classes & oc::Player) && type != 0xF) {
        return 0;
    }
    ServerObject* hitter = obj3 ? obj3 : src;
    PlayerControl* p = t.player ? &*t.player : nullptr;
    ServerObject::Monster* npc = nullptr;
    if (!p) {
        if (!(t.classes & oc::Monster) || !(t.subclasses & 0x10u) || !t.monster) {
            return 0;
        }
        npc = &*t.monster;
    }
    const std::uint32_t weaponBits = p ? p->weaponEquip : npc->weaponFlags;
    const std::uint32_t armorBits = p ? p->armorEquip : npc->armorFlags;
    const float armor = p ? p->armorValue : npc->armorValue;
    if (p && (p->statusFlags & 1u)) { // (an observer: status bit 1)
        return 0;
    }
    // What hurt it, for the kill feed (a player's) or an NPC's (Monster::killKind / killType).
    const auto hurtKind = [&](std::uint32_t kind, std::uint32_t what) {
        if (p) {
            p->killKind = static_cast<std::uint8_t>(kind);
            p->killType = static_cast<std::uint16_t>(what);
        } else {
            npc->killKind = kind;
            npc->killType = what;
        }
    };
    if (p) {
        p->killKind = 0;
    } else {
        npc->killKind = 0;
    }
    // Watching a creature: back to itself (playerObserveClear).
    if (p && possessed(world, t)) {
        observeClear(*this, t);
    }
    // A reflective shield turns a missile, and a ray (types 0x10, 0x11), met at its front.
    if (enchanted(t, Enchant::ReflectiveShield) && hitter) {
        const bool front = facingSides(t.x, t.y, t.direction, hitter->x, hitter->y) & 1;
        if (!(hitter->classes & oc::Missile)) {
            if ((type == 0x10 || type == 0x11) && front) {
                if (type == 0x10) {
                    pointFx(0x84, t.x, t.y); // MSG_FX_VIOLET_SPARKS
                }
                playSound(world, 0x7A, t);
                return 0;
            }
        } else if (front) {
            reflectMissile(*hitter, t);
            // (The exe keeps the missile's own owner, its UpdateData's, apart from the object's:
            // turned first here, from the one owner we keep.)
            if (hitter->subclasses & 2u) {
                turnSpellMissile(world, *hitter, t, frame);
            }
            if (!(hitter->subclasses & 0x40u)) {
                setOwner(world, &t, *hitter); // (unitClearOwner / unitSetOwner)
            }
            if (type == 0x10) {
                pointFx(0x84, t.x, t.y);
            }
            playSound(world, 0x7A, t);
            return 0;
        }
    }
    if (src) {
        // Blocks: a blow from the front (checkReflectShield's side 1, from where the hitter was, a held
        // weapon's from its wielder) stopped by a raised shield, a great sword's parry or a
        // staff's block (sounds 0x36E, 0x37A, 0x37E). The blocker takes nothing; what blocked it
        // wears. A player's by its state; an NPC's shield while its action is BLOCK_ATTACK
        // (mobActionGet), its parries while idle, waiting, guarding, facing or
        // blocking, the block pushed (monsterAction(0x17)).
        // The kill feed's weapon: a thing that isn't the attacker (or a punch's or a fall's
        // attacker, types 10 and 2), by its type.
        if (!obj3) {
            if (type == 10 || type == 2) {
                hurtKind(1, src->type);
            }
        } else if (src != obj3) {
            hurtKind(1, obj3->type);
        }
        if (type != 0xF && !unblockable(*hitter, obj3 != nullptr)) {
            ServerObject& from = *hitter;
            const ServerObject* at = from.holder != 0 ? world.byExtent(from.holder) : &from;
            const float fromX = at ? at->frameStartX : from.x;
            const float fromY = at ? at->frameStartY : from.y;
            const bool missile = (from.classes & oc::Missile) != 0;
            if (facingSides(t.x, t.y, t.direction, fromX, fromY) & 1) {
                Monsters monsters{*this};
                const int action = npc && 0 <= npc->top ? npc->stack[static_cast<std::size_t>(npc->top)].action : -1;
                const bool npcReady = npc && (action == MonsterAction::Idle || action == MonsterAction::Wait || action == MonsterAction::Guard ||
                                              action == MonsterAction::FaceLocation || action == MonsterAction::FaceObject ||
                                              action == MonsterAction::FaceAngle || action == MonsterAction::WeaponBlock);
                // What wears for it: the first worn thing in the
                // inventory of those SUBCLASS bits, whatever its class (a shield's 2 is a quiver's too).
                const auto wearing = [&](std::uint32_t subclassMask) -> ServerObject* {
                    for (const std::uint32_t e : t.inventory) {
                        ServerObject* i = world.byExtent(e);
                        if (i && (i->flags & ObjFlag::Equipped) && (i->subclasses & subclassMask)) {
                            return i;
                        }
                    }
                    return nullptr;
                };
                const auto wear = [&](ServerObject* item, bool weapon) {
                    // The shield's wear (equipDamage) or the weapon's
                    // (playerDamageWeapon): ItemDamageFromBlockPercentage of it,
                    // from the source and the weapon (or the source). Broken: Plr_SetState, a
                    // monster's action popped.
                    const double amount = static_cast<double>(world.number("ItemDamageFromBlockPercentage")) * dmg;
                    if (item && things &&
                        Items{world, *things, modifiers, rng, frame}.wear(t, *item, amount, src, obj3 ? obj3 : src, type, weapon)) {
                        if (p) {
                            setPlayerState(t, 0xD);
                        } else {
                            monsters.pop(t);
                        }
                    }
                };
                const auto turnBack = [&](std::uint32_t keepSubclasses) {
                    if (missile && !(from.subclasses & keepSubclasses)) {
                        reflectMissile(from, t);
                        if (!(from.subclasses & 2)) {
                            setOwner(world, &t, from); // (it's the blocker's now: ClearOwner, SetOwner)
                        }
                    }
                };
                // (Plr_SetState: already in it, its animation goes on.)
                const auto parry = [&](std::uint8_t state) { setPlayerState(t, state); };
                const auto npcParry = [&] {
                    if (action != MonsterAction::WeaponBlock) {
                        monsters.push(t, MonsterAction::WeaponBlock);
                    }
                };
                const bool shieldUp = p ? p->state == 0x10 : action == MonsterAction::BlockAttack;
                // A raised shield (the worn armour's shield bits) stops all but electricity (9, 0x11); a great
                // sword (weapon bit 0x400) parries; a staff blocks blows.
                if (shieldUp && (armorBits & 0x3000000u)) {
                    if (type != 9 && type != 0x11) {
                        playSound(world, 0x36E, t); // the shield's
                        turnBack(0x70);
                        wear(wearing(2u), false);
                        return 0;
                    }
                } else if ((weaponBits & 0x400u) && (missile || type == 0 || type == 0xB)) {
                    if (p ? (p->state == 0xD || (p->state >= 0x12 && p->state <= 0x14)) : npcReady) {
                        turnBack(0);
                        playSound(world, 0x37A, t); // the great sword's parry
                        if (p) {
                            parry(static_cast<std::uint8_t>(nox::util::uniform_int_distribution<int>(0x12, 0x14)(rng)));
                        } else {
                            npcParry();
                        }
                        wear(wearing(0x400u), true);
                        return 0;
                    }
                } else if ((weaponBits & 0x7FF8000u) && (type == 0 || type == 0xB) && !missile) {
                    if (p ? (p->state == 0xD || p->state == 0x15) : npcReady) {
                        playSound(world, 0x37E, t); // the staff's block
                        if (p) {
                            parry(0x15);
                        } else {
                            npcParry();
                        }
                        wear(wearing(0x7FF8000u), true);
                        return 0;
                    }
                }
            }
        }
    }
    // Armour, by the damage type (PlayerDamage's cases): blows and the like take (1 - armour),
    // falls (2) (1 - armour / 2), fire (1, 0xC) none off but wear it whole, magic (9, 0x11) more
    // through metal; the fraction carried over (playerDecrementHPMana); worn pieces wear
    // what the armour took. The kill feed's damage type if nothing else is (an NPC's flash).
    int final = dmg;
    bool feed = true;
    // (Each amount on the x87: FLD 1, FSUB the armour (FMUL 0.5 first, then FSUBR 1, for a fall),
    // FIMUL the damage, FSTP to playerDecrementHPMana's float argument.)
    const X87 hits = X87::fromDouble(dmg);
    // Damage types past 0x11 skip the armour and the kill feed.
    if (type <= 0x11) {
        switch (type) {
        case 0: case 3: case 7: case 8: case 10: case 11:
            final = carryDamage(t, ((X87(1.0f) - armor) * hits).f());
            wearArmor(t, src, obj3, dmg - final, type);
            break;
        case 2:
            final = carryDamage(t, ((X87(1.0f) - X87(armor) * 0.5f) * hits).f());
            wearArmor(t, src, obj3, dmg - final, 2);
            break;
        case 1: case 12:
            wearArmor(t, src, obj3, dmg, type);
            break;
        case 9: case 0x11:
            final = carryDamage(t, (X87(metalFactor(t)) * hits).f());
            wearArmor(t, src, obj3, dmg, type);
            break;
        case 13:
            feed = false;
            break;
        default: // (4, 5, 6, 14, 15, 16)
            break;
        }
        if (feed && (p ? p->killKind == 0 : npc->killKind == 0)) {
            hurtKind(2, static_cast<std::uint32_t>(type));
        }
        if (dmg > 0 && final == 0) {
            final = 1;
        }
    }
    // The engine's god mode (the engine flags' 0x20, the console's "set god"): a player is
    // spared the rest. A quest's damage to players.
    if (world.godMode && (t.classes & oc::Player)) {
        return 1;
    }
    if (world.quest()) {
        // (FLD the multiplier, FIMUL, FSTP, Math_FloatToInt)
        const int scaled = util::floatToInt(X87(world.questDamage) * X87::fromDouble(final));
        final = final > 0 && scaled < 1 ? 1 : scaled;
    } else if (world.modeDamage > 0.0f && (t.classes & oc::Player)) {
        // (noxcpp: a mode's scaling of the damage players take, the quest's way)
        const int scaled = util::floatToInt(X87(world.modeDamage) * X87::fromDouble(final));
        final = final > 0 && scaled < 1 ? 1 : scaled;
    }
    return defaultDamage(t, src, obj3, final, type);
}

int Fight::defaultDamage(ServerObject& t, ServerObject* src, ServerObject* obj3, int dmg, int type)
{
    // DefaultDamage.
    if (enchanted(t, Enchant::Invulnerable)) {
        if ((frame & 3u) == 0) {
            playSound(world, 0x47, t);
        }
        return 1;
    }
    // A monster: its blow's account cleared (killKind); fire (1, 0xC, 7, 0xE, 6) sets it burning.
    if (t.monster) {
        t.monster->killKind = 0;
        if (type == 1 || type == 0xC || type == 7 || type == 0xE || type == 6) {
            t.monster->status |= things::MonsterStatus::OnFire;
        }
    }
    if (t.flags & ObjFlag::Dead) {
        if (t.typeName == "Zombie" || t.typeName == "VileZombie") { // (its rising: with the zombies)
            t.hurtBy = obj3 ? obj3->extent : src ? src->extent : 0;
            t.hurtType = static_cast<std::uint8_t>(type);
            t.hurtFrame = frame;
        }
        return 1;
    }
    // Its own side doesn't hurt it (but the host's gameplay flag 1); its own doing does, but
    // in a quest.
    if (!(world.gameplayFlags & 1u)) {
        if (ServerObject* pp = parentPlayer(src); pp && creature(*pp) && !isEnemy(world, t, *pp)) {
            if (&t != pp || world.quest()) {
                return 1;
            }
        }
    }
    if (src && obj3 && !isEnemy(world, t, *src) && creature(t) && handToHand(*src, obj3) && !hammer(obj3)) {
        return 1;
    }
    if (t.flags & ObjFlag::NoUpdate) {
        return 1;
    }
    // Shock: one striking it hand to hand takes the charge (ShockDamage's fifth, whatever its
    // power: an original bug) and is stunned.
    if (src && enchanted(t, Enchant::Shock) && creature(*src) && obj3 && handToHand(*src, obj3)) {
        playSound(world, 0x87, *src); // (at the striker)
        disenchant(world, t, Enchant::Shock);
        damage(*src, &t, 0, static_cast<int>(world.number("ShockDamage", 4)), 9);
        if (src->player) {
            setPlayerState(*src, 0x17);
        }
    }
    // A monster's immunities (subclass 0x200: poison, 0x400: fire (a half of type 7), 0x800:
    // electricity).
    if (t.classes & oc::Monster) {
        if ((t.subclasses & 0x200u) && type == 5) {
            return 1;
        }
        if (t.subclasses & 0x400u) {
            if (type == 1 || type == 0xC) {
                return 1;
            }
            if (type == 7) {
                dmg /= 2;
            }
        } else if ((t.subclasses & 0x800u) && (type == 9 || type == 0x11)) {
            return 1;
        }
    }
    if (type == 1 || type == 0xC || type == 7) {
        const float prot = protection(t, Enchant::ProtectFromFire, "FireSpellProtection");
        if (prot != 0.0f && (frame & 3u) == 0) {
            playSound(world, 0x68, t);
        }
        // (FST the protection to a float; FLD 1, FSUB it, FIMUL, FSTP, Math_FloatToInt)
        dmg = util::floatToInt((X87(1.0f) - prot) * X87::fromDouble(dmg));
        if (dmg == 0) {
            dmg = 1;
        }
    }
    if (type == 9 || type == 0x11) {
        const float prot = protection(t, Enchant::ProtectFromElectricity, "ElectricitySpellProtection");
        if (prot != 0.0f && (frame & 3u) == 0) {
            playSound(world, 0x6C, t);
        }
        dmg = util::floatToInt((X87(1.0f) - prot) * X87::fromDouble(dmg));
        if (dmg == 0) {
            dmg = 1;
        }
        // (A player's shock twitch: not yet.) An NPC's field523 = 2.
        if (!t.player && (t.classes & oc::Monster) && (t.subclasses & 0x10u) && t.monster) {
            t.monster->field523 = 2;
        }
    }
    // Where the blow came from (Pos132: its source's, or a thrown thing's, last position; a
    // weapon's or armour's (0x1001000) its wielder's); a monster notes a blow by hand (types 10
    // and 2 from the source itself, or anything carried) and what dealt it. Hurt, it's seen:
    // invisibility goes (without a source only for type 0xC).
    if (!src) {
        t.hurtFromX = t.hurtFromY = 0;
        if (type == 0xC) {
            disenchant(world, t, Enchant::Invisible);
        }
    } else {
        if (!obj3) {
            t.hurtFromX = src->lastX;
            t.hurtFromY = src->lastY;
            if ((type == 10 || type == 2) && t.monster) {
                t.monster->killKind = 1;
                t.monster->killType = src->type;
            }
        } else {
            const ServerObject& from = (obj3->classes & 0x1001000u) ? *src : *obj3;
            t.hurtFromX = from.lastX;
            t.hurtFromY = from.lastY;
            if (obj3 != src && t.monster) {
                t.monster->killKind = 1;
                t.monster->killType = obj3->type;
            }
        }
        disenchant(world, t, Enchant::Invisible);
    }
    // (A player's or a monster of subclass 0x10's worn enchantments' DEFENDEFFECTs,
    // itemApplyDefendEffect2: none in the shipped modifier.bin.)
    t.hurtBy = obj3 ? obj3->extent : src ? src->extent : 0;
    t.hurtType = static_cast<std::uint8_t>(type);
    t.hurtFrame = frame;
    if (t.monster) {
        t.monster->status |= things::MonsterStatus::Injured; // (0x200: hurt this frame)
        if (t.monster->killKind == 0) {
            t.monster->killKind = 2;
            t.monster->killType = static_cast<std::uint32_t>(type);
        }
    }
    // A weapon's or wand's that struck it (item_effects.hpp).
    if (obj3 && (obj3->classes & 0x1001000u)) {
        preDamageEffects(*this, t, src, *obj3, dmg); // (itemApplyPreDamageEffect)
    }
    damageSound(world, modifiers, t, src, obj3);
    // Vampirism: its attacker drinks VampirismCoeff of it (at least 1).
    if (src && creature(t) && enchanted(*src, Enchant::Vampirism)) {
        if (obj3) {
            playSound(world, 0xA3, *obj3); // (at the weapon or missile)
        }
        const float coeff = world.number("VampirismCoeff", static_cast<std::size_t>(enchantPower(*src, Enchant::Vampirism) - 1));
        int heal = util::floatToInt(X87(coeff) * X87::fromDouble(dmg)); // (FLD, FIMUL, FSTP, Math_FloatToInt)
        if (static_cast<std::int16_t>(heal) == 0) {
            heal = 1;
        }
        heal &= 0xFFFF;
        adjustHealth(world, *src, heal);
        const auto i16 = [](float v) { return static_cast<std::uint16_t>(static_cast<int>(v)); };
        const std::uint16_t v[5] = {i16(src->x), i16(src->y), i16(t.x), i16(t.y), static_cast<std::uint16_t>(heal)};
        std::vector<std::uint8_t> fx = {0xA2}; // MSG_FX_VAMPIRISM
        for (const std::uint16_t w : v) {
            fx.push_back(static_cast<std::uint8_t>(w));
            fx.push_back(static_cast<std::uint8_t>(w >> 8));
        }
        world.broadcasts().push_back({std::move(fx), false, 0});
    }
    ballHitCarrier(*this, src, t, dmg); // (gameballOnPlayerDamage: modes.hpp)
    // A blow over 19 makes a player recoil, unless it's attacking (state 1) or in state 0xF.
    if (t.player && 19 < dmg && t.player->state != 1 && t.player->state != 0xF) {
        setPlayerState(t, 0x1E); // recoiling
    }
    // A solo or quest game's field guide bonus: a player striking a
    // monster whose guide it has, FieldGuideDamageBonus times it, a half up (x87, chopped to the
    // float Math_FloatToInt takes: no different for a whole number).
    // Game flags 0x1800: a solo or quest game.
    if ((world.gameFlags & 0x1800u) && src && (src->classes & oc::Player) && src->player && (t.classes & oc::Monster)) {
        if (const int guide = guideCreatureIndex(t.typeName); guide != 0 && src->player->fieldGuide[static_cast<std::size_t>(guide)]) {
            dmg = static_cast<int>(static_cast<double>(world.number("FieldGuideDamageBonus")) * dmg + 0.5);
        }
    }
    // A monster (or a monster's) striking an enemy: its strike sound's timer starts.
    if (src) {
        ServerObject* mob = (src->classes & oc::Monster) ? src : src->owner ? world.byExtent(src->owner) : nullptr;
        if (mob && (mob->classes & oc::Monster) && mob->monster && isEnemy(world, t, *mob) && mob->monster->hurtSoundAt == 0) {
            mob->monster->hurtSoundAt = frame;
        }
    }
    // A shield (the Shield spell's) takes half of it, and all when it fails (unitShieldReduceDamage).
    if (enchanted(t, Enchant::Shield) && type != 5) {
        ServerObject* by = obj3 ? obj3 : src;
        if (type != 0xF || src != &t) {
            shieldTakes(t, dmg, type, by);
        }
        if (dmg == 0) {
            return 0;
        }
    }
    takeHealth(t, dmg);
    return 1;
}

void Fight::shieldTakes(ServerObject& t, int& dmg, int type, ServerObject* by)
{
    // unitShieldReduceDamage: its sound first (0x83). (The shield's flash,
    // netSendShieldFx: not yet.)
    playSound(world, 0x83, t);
    if (static_cast<int>(t.health) <= dmg) {
        // Through it: left on 2, unless a raised shield faces the blow.
        if (!t.player || t.player->state != 0x10 || !by || !(facingSides(t.x, t.y, t.direction, by->x, by->y) & 1)) {
            t.health = 2;
            t.touch();
            dmg = 0;
        } else {
            dmg = 1;
        }
        shieldWears(t, 999999);
        t.hurtBy = by ? by->extent : 0;
        t.hurtType = static_cast<std::uint8_t>(type);
        t.hurtFrame = frame;
        return;
    }
    dmg /= 2;
    if (dmg == 0) {
        dmg = 1;
    }
    shieldWears(t, dmg);
}

void Fight::shieldWears(ServerObject& t, int amount)
{
    // unitShield: the Shield spell on it loses `amount` of its strength, and
    // ends at none; without one the enchantment goes.
    for (DurationSpell& s : world.durationSpells()) {
        // spell 0x33 is the Shield spell; its record's value is the strength left.
        if (s.target == t.extent && s.spell == 0x33) {
            const ServerObject* target = world.byExtent(s.target);
            if (target && !(target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && 0 < s.value - amount) {
                s.value -= amount;
                return;
            }
            cancelDurationSpell(world, s);
            return;
        }
    }
    disenchant(world, t, Enchant::Shield);
}

void Fight::pointFx(std::uint8_t op, float x, float y)
{
    nox::server::pointFx(world, op, x, y); // (netSendPointFx: to the players who see the point; everyone here)
}

void Fight::setPlayerState(ServerObject& u, std::uint8_t state)
{
    server::setPlayerState(world, u, state, frame); // (Plr_SetState: player.hpp)
}

void Fight::setStatus(ServerObject& player, std::uint32_t flags, bool on)
{
    if (!player.player) {
        return;
    }
    std::uint32_t& s = player.player->statusFlags;
    s = on ? (s | flags) : (s & ~flags);
    // Status bits 0x423 are the ones clients see: MSG_REPORT_CLIENT_STATUS (0x6A), the player's
    // extent (16 bits), then the reported bits (32).
    if (flags & 0x423u) {
        const std::uint32_t r = s & 0x423u;
        world.broadcasts().push_back({{0x6A, static_cast<std::uint8_t>(player.extent), static_cast<std::uint8_t>(player.extent >> 8),
                                       static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(r >> 8), static_cast<std::uint8_t>(r >> 16),
                                       static_cast<std::uint8_t>(r >> 24)},
                                      true, 0});
    }
}

namespace {

/// A creature's poison told (Obj_ReducePoison, removePoison,
/// setSomePoisonData): in a solo game an NPC's (subclass 0x10) to the host's
/// player (slot 0x1F), else a subclass 0x80 one's to its owner; SrvNet_SendReportObjectPoison
/// (SrvNet_SendReportObjectPoison): MSG_REPORT_OBJECT_POISON {u16 its net code, u8 `value`} to that player alone.
/// (The exe reads a monster owner's data as a player's, an original bug: ours tells players
/// alone.)
void reportObjectPoison(World& world, const ServerObject& creature, std::uint8_t value)
{
    if (!(creature.classes & oc::Monster)) {
        return;
    }
    const ServerObject* to = nullptr;
    if ((world.gameFlags & GameFlag::Solo) && (creature.subclasses & 0x10u)) {
        to = hostPlayer(world);
    } else if (creature.subclasses & 0x80u) {
        to = creature.owner ? world.byExtent(creature.owner) : nullptr;
    }
    if (!to || !to->player || !(to->classes & oc::Player)) {
        return;
    }
    const std::uint16_t code = netCodeOf(creature);
    world.broadcasts().push_back({{0xDA, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), value}, true, to->extent});
}

} // namespace

void Fight::setPoison(ServerObject& t, int level)
{
    if (t.poison == 0 && 0 < level && t.maxHealth != 0) {
        t.poisonedAt = frame;
    }
    t.poison = static_cast<std::uint8_t>(level);
    if (t.player) {
        setStatus(t, 0x400, level != 0);
    } else {
        reportObjectPoison(world, t, level != 0 ? 1 : 0);
    }
    t.poisonTimer = level == 0 ? 0 : 1000;
}

bool Fight::applyPoison(ServerObject& t, int amount, int most)
{
    if ((t.flags & ObjFlag::NoUpdate) || enchanted(t, Enchant::Invulnerable)) {
        return false;
    }
    if (t.player && (t.player->statusFlags & 1u)) {
        return false;
    }
    if ((t.classes & oc::Monster) && (t.subclasses & 0x200u)) {
        return false;
    }
    // Its protection (getPoisonDmg: worn items', item_effects.hpp, at most 0.7;
    // the enchantment's table on top; at most 0.9) is its chance in 100 to resist.
    float items = wornProtection(world, modifiers, t, things::EffectFn::PoisonProtectEngage);
    if (0.7f < items) {
        items = 0.7f;
    }
    // (The enchantment's FLD and FADD stay on the stack, capped there, and come back in ST0; the
    // caller's FMUL 100 works on that, FSTP, Math_FloatToInt.)
    X87 prot = items;
    if (enchanted(t, Enchant::ProtectFromPoison)) {
        prot = X87(world.number("PoisonSpellProtection", static_cast<std::size_t>(enchantPower(t, Enchant::ProtectFromPoison) - 1))) + items;
    }
    if (X87(0.9f) < prot) {
        prot = 0.9f;
    }
    const int resist = util::floatToInt(prot * 100.0f);
    if (nox::util::uniform_int_distribution<int>(0, 100)(rng) < resist) {
        if (t.player) {
            world.broadcasts().push_back({informStringBytes("Health.c:ResistPoison"), true, t.extent});
        }
        return false;
    }
    const int before = t.poison;
    int level = before + amount;
    // Over the cap: the cap, unless it was already above it (then unchanged).
    if (most < level) {
        level = before <= most ? most : before;
    }
    if (level != before) {
        setPoison(t, level);
        playSound(world, 100, t);
    }
    if (before == 0 && 0 < level && t.maxHealth != 0) {
        t.poisonedAt = frame;
    }
    return true;
}

void Fight::reducePoison(ServerObject& t, int amount)
{
    if (amount < t.poison) {
        t.poison = static_cast<std::uint8_t>(t.poison - amount);
        return;
    }
    t.poison = 0;
    t.poisonedAt = 0;
    if (t.player) {
        setStatus(t, 0x400, false);
        world.broadcasts().push_back({informStringBytes("Health.c:PoisonFade"), true, t.extent});
    } else {
        reportObjectPoison(world, t, 0);
    }
}

void Fight::removePoison(ServerObject& t)
{
    if (t.poison == 0) {
        return;
    }
    t.poison = 0;
    t.poisonedAt = 0;
    if (t.player) {
        setStatus(t, 0x400, false);
    } else {
        reportObjectPoison(world, t, 0);
    }
}

void Fight::unitTail(ServerObject& o)
{
    // (updateUnits's last pass, after the speed: physics.cpp) Every 1000 frames a slowed one's
    // slowness and its poison fall a step.
    if ((o.slowed != 0 || o.poison != 0) && 0 < o.poisonTimer && --o.poisonTimer == 0) {
        if (o.slowed != 0) {
            --o.slowed;
        }
        if (o.poison != 0 && !(o.flags & ObjFlag::Dead)) {
            reducePoison(o, 1);
        }
        o.poisonTimer = 1000;
    }
    stepEnchantments(o, *this);
    // The poison bites (1, a quest's 2, of type 5): 2 s after it began, then every frame past
    // level 8, else every 128 >> (level - 1).
    if (o.poison != 0 && o.maxHealth != 0 && o.health != 0) {
        // Every 128 >> (level - 1) frames: level 1 each 128, level 8 each frame.
        if ((o.poisonedAt == 0 || 60u < frame - o.poisonedAt) && (8 < o.poison || frame % (0x80u >> ((o.poison - 1) & 31)) == 0)) {
            damage(o, nullptr, 0, world.quest() ? 2 : 1, 5);
        }
    }
}

void Fight::takeHealth(ServerObject& t, int amount)
{
    // unitDamageClear: its health down, and at 0 dead (DEATH's enchantment
    // off; its DIE function). (A warrior's harpoon breaks: with the harpoon.)
    if (t.maxHealth == 0) {
        return;
    }
    if (world.godMode && (t.classes & oc::Player)) {
        return; // (the engine flags' 0x20: a player in god mode loses none)
    }
    // NoX Core's map-editor play-test sandbox (World::sandboxPlayersCantDie; off in every
    // ordinary game): a player's health stops at 1, the hit still shown.
    if (world.sandboxPlayersCantDie && t.player && amount >= t.health) {
        amount = t.health > 1 ? t.health - 1 : 0;
    }
    if (t.player && t.player->usesAbilities() && t.player->harpoonTarget != 0) {
        releaseHarpoon(world, t, true); // (a warrior hurt lets its harpoon go)
    }
    if (world.modeHooks && (t.modeEvents & ModeEvent::Damaged)) {
        world.modeHooks->damaged(*this, t, std::min<int>(amount, t.health)); // (noxcpp: told a mode)
    }
    if (amount < t.health) {
        t.health = static_cast<std::uint16_t>(t.health - amount);
        t.touch();
    } else {
        t.health = 0;
        t.touch();
        if (!(t.flags & ObjFlag::Dead)) {
            t.flags |= ObjFlag::Dead;
            disenchant(world, t, Enchant::Death);
            // A solo game's kill XP (Obj_AwardSoloKillXP), not for a zombie; then its DIE.
            if (t.typeName != "Zombie" && t.typeName != "VileZombie") {
                awardKillXP(*this, t);
            }
            if (!t.player && world.modeHooks && (t.modeEvents & ModeEvent::Death)) {
                world.modeHooks->objectKilled(*this, t, world.byExtent(t.hurtBy)); // (noxcpp: told a mode)
            }
            if (t.player) {
                playerDies(t);
            } else if (t.monster) {
                Monsters{*this}.dies(t); // (monsterCallDieFn)
            } else if (!(t.classes & oc::Monster)) {
                objectDies(*this, t);
            }
        }
    }
    if (t.classes & oc::Monster) {
        informOwnerHealth(world, t);
    }
}

void Fight::playerDies(ServerObject& u)
{
    // Plr_Die. A solo game's save asked for is off.
    PlayerControl& p = *u.player;
    // (noxcpp: a mode may keep a solo game's death from being the game's: its save and its things.)
    const bool modeKeeps = (world.gameFlags & GameFlag::Solo) && world.modeHooks && !world.modeHooks->playerDeath(*this, u, DeathStep::Dies);
    if ((world.gameFlags & GameFlag::Solo) && !modeKeeps) {
        requestSave(world, false, 0, 0, frame);
    }
    ServerObject* hurter = world.byExtent(u.hurtBy);
    const ServerObject* k = parentPlayer(hurter);
    const auto victim = static_cast<std::uint16_t>(u.extent);
    // The kill feed, in a network game (INFORM 0x0E: killer, assist, victim, what killed it):
    // a monster by its type; a thing a monster owns (ObjOwner: its arrow) by the
    // monster's; else what the player noted.
    // The assist: the last player to cast on it, within 10 s, not the killer nor itself.
    std::uint16_t assist = 0;
    if (p.assistBy != 0 && frame - p.assistFrame < 30u * 10u) {
        const ServerObject* a = world.byExtent(p.assistBy);
        if (a && a->player && !a->gone() && a != k && a != &u) {
            assist = static_cast<std::uint16_t>(a->extent);
        }
    }
    if (world.network()) {
        const std::uint16_t killer = k && (k->classes & oc::Player) ? static_cast<std::uint16_t>(k->extent) : 0;
        std::uint16_t weapon = p.killType;
        std::uint8_t kind = p.killKind;
        if (hurter && (hurter->classes & oc::Monster)) {
            kind = 1;
            weapon = hurter->type;
        } else if (hurter && !(hurter->classes & oc::Player)) {
            const ServerObject* owner = hurter->owner ? world.byExtent(hurter->owner) : nullptr;
            if (owner && !(owner->classes & oc::Player) && (owner->classes & oc::Monster)) {
                kind = 1;
                weapon = owner->type;
            }
        }
        world.broadcasts().push_back({killFeedMessage(killer, assist, victim, weapon, kind), false});
        p.killKind = 0;
        // A kill by Berserker Charge (put down to damage type 2): ready again for its warrior.
        if (kind == 2 && weapon == 2 && k) {
            resetAbility(world, *const_cast<ServerObject*>(k), 1);
        }
    }
    // Its death cry: 299 (0x12B) by the last blow's type 0x10, else 0x141 / 0x14B (male / female:
    // a female player is a PlayerFemale).
    playSound(world, u.hurtType == 0x10 ? 299u : u.typeName == "PlayerFemale" ? 0x14Bu : 0x141u, u);
    u.flags |= ObjFlag::Dead;
    setPlayerState(u, 3); // dying
    p.punch = 0;
    p.chant.reset(); // (its spell, SpellCastStart) and the trap being built
    p.castStart = 0;
    p.hearsOwnPhonemes = false;
    p.trapSpells = {};
    p.trapCount = 0;
    // The mode's scoring (modes.hpp): its killer's top owner, the assist's unit. Past
    // elimination's lives its spawned things go (playerRemoveSpawnedStuff).
    scoreDeath(*this, u, const_cast<ServerObject*>(k), assist ? world.byExtent(assist) : nullptr);
    if (world.modeHooks) {
        // (noxcpp: told a mode, with the killer and assist the scoring had.)
        world.modeHooks->playerKilled(*this, u, const_cast<ServerObject*>(k), assist ? world.byExtent(assist) : nullptr);
    }
    if ((world.gameFlags & GameFlag::Elimination) && world.lessonLimits[1] != 0 && world.lessonLimits[1] <= p.deaths) {
        removeSpawned(world, u);
    }
    u.flags |= 0x10u; // (SHORT)
    // Its things on the ground (not in a quest), its clothing aside. (noxcpp: a mode's
    // World::keepThingsOnDeath keeps them as a quest does, but a carried flag or crown.)
    const bool keepThings = things && !world.quest() && !modeKeeps && world.keepThingsOnDeath;
    if (things && !world.quest() && !modeKeeps) {
        Items{world, *things, modifiers, rng, frame}.dropAll(u, keepThings);
    }
    // MSG_PLAYER_DIED to everyone (SrvNet_BroadcastPlayerDied).
    world.broadcasts().push_back({{0xE8, static_cast<std::uint8_t>(victim), static_cast<std::uint8_t>(victim >> 8)}, true});
    if (keepThings) {
        // (noxcpp: every client has just forgotten what it wears, as outside a quest it would
        // be on the ground: told again, after.)
        Items{world, *things, modifiers, rng, frame}.reportWornAgain(u);
    }
    // No mana, no enchantments, its abilities and spells cancelled, then its trade.
    p.mana = 0;
    p.assistBy = 0;
    setEnchantments(u, 0);
    cancelAbilities(world, u);
    for (DurationSpell& s : world.durationSpells()) { // (playerCancelSpells)
        if (s.caster == u.extent) {
            cancelDurationSpell(world, s);
        }
    }
    clearEnchantments(u);
    cancelTrade(*this, u); // (shopCancelSession, its trade cleared)
    if (world.quest()) {
        questDeath(*this, u); // (its lives, or game over)
    }
}

void adjustHealth(World& world, ServerObject& o, int amount)
{
    // No healing in elimination's sudden death.
    if ((world.gameFlags & 0x4000000u) || o.maxHealth == 0 || o.health >= o.maxHealth) {
        return;
    }
    o.health = static_cast<std::uint16_t>(std::min<int>(o.health + amount, o.maxHealth));
    o.touch();
    if (o.classes & oc::Monster) {
        informOwnerHealth(world, o);
    }
}

void informOwnerHealth(World& world, const ServerObject& o)
{
    // mobInformOwnerHP: a player's creature, its player told its health
    // (SrvNet_SendReportUnitHealth: MSG_REPORT_UNIT_HEALTH 0x41, its code and half its health,
    // reliable).
    const ServerObject* owner = o.owner ? world.byExtent(o.owner) : nullptr;
    if (!owner || !(owner->classes & oc::Player) || o.maxHealth == 0) {
        return;
    }
    const std::uint32_t code = netCodeOf(o);
    world.broadcasts().push_back({{0x41, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(o.health >> 1)},
                                  true, owner->extent});
}

void Fight::respawn(ServerObject& u, bool keepItems)
{
    PlayerControl& p = *u.player;
    // Plr_Respawn: not while a quest's game over waits for its client (till
    // its MSG_GAUNTLET 3).
    if (world.quest() && p.questGameOverAt != 0) {
        return;
    }
    // playerMakeDefItems (heal 1): its poison gone, whole again, its abilities,
    // camper's watch, gates, buffs, traps and harpoon cleared, its flags and state reset.
    removePoison(u);
    u.health = u.maxHealth;
    cancelAbilities(world, u);
    camperReset(world, u, frame);
    p.exitGate = p.warpGate = 0;
    clearEnchantments(u);
    p.trapSpells = {};
    p.trapCount = 0;
    p.harpoonBolt = p.harpoonTarget = 0;
    // Clears DEAD, SHORT, ALLOW_OVERLAP, AIRBORNE, IN_HOLE and ON_OBJECT.
    u.flags &= 0xFFEB3FE7u;
    p.previousState = p.state;
    p.state = 0xD;
    p.stateFrame = frame;
    p.animFrame = 0;
    p.punch = 0;
    u.hurtBy = 0;
    p.healthSent = -1; // (MSG_REPORT_TOTAL_HEALTH: the host sends its health)
    p.mana = p.maxMana; // (and its mana, MSG_REPORT_TOTAL_MANA)
    p.manaSent = -1;
    p.chant.reset();
    p.castStart = 0; // (SpellCastStart)
    p.hearsOwnPhonemes = false;
    // Its outfit (and MSG_PLAYER_RESPAWN to everyone). A quest's (MakeDefItems' third 1) keeps what
    // it has, MSG_PLAYER_RESPAWN's flag 0; its lives told it again (its own livesSent entry to 0xFA),
    // "GeneralPrint:Respawn".
    if (world.quest()) {
        const auto netCode = static_cast<std::uint16_t>(u.extent);
        world.broadcasts().push_back({{0xE9, static_cast<std::uint8_t>(netCode), static_cast<std::uint8_t>(netCode >> 8),
                                       static_cast<std::uint8_t>(frame), static_cast<std::uint8_t>(frame >> 8),
                                       static_cast<std::uint8_t>(frame >> 16), static_cast<std::uint8_t>(frame >> 24), 0xFF, 0},
                                      true});
        p.livesSent[p.index & 31u] = 0xFA;
        world.broadcasts().push_back({informStringBytes("GeneralPrint:Respawn"), true, u.extent});
    } else if (keepItems || world.keepThingsOnDeath) {
        // (noxcpp: what it holds kept, as a quest's: MSG_PLAYER_RESPAWN's flag 0; a mode's
        // World::keepThingsOnDeath likewise for every respawn)
        const auto netCode = static_cast<std::uint16_t>(u.extent);
        world.broadcasts().push_back({{0xE9, static_cast<std::uint8_t>(netCode), static_cast<std::uint8_t>(netCode >> 8),
                                       static_cast<std::uint8_t>(frame), static_cast<std::uint8_t>(frame >> 8),
                                       static_cast<std::uint8_t>(frame >> 16), static_cast<std::uint8_t>(frame >> 24), 0xFF, 0},
                                      true});
    } else if (things) {
        Items{world, *things, modifiers, rng, frame}.defaultItems(u);
    } else {
        const auto netCode = static_cast<std::uint16_t>(u.extent);
        world.broadcasts().push_back({{0xE9, static_cast<std::uint8_t>(netCode), static_cast<std::uint8_t>(netCode >> 8),
                                       static_cast<std::uint8_t>(frame), static_cast<std::uint8_t>(frame >> 8),
                                       static_cast<std::uint8_t>(frame >> 16), static_cast<std::uint8_t>(frame >> 24), 0xFF, 1},
                                      true});
    }
    // respawnPlayerImpl (in a WOL game only with the host's Bones option on): its bones where it
    // fell, eleven pieces laid out by its facing (by the direction's dy * 3 + 4 + dx; the middle
    // has none), each gone in 10-20 s, in a WOL network game flagged 0x40.
    if (things && (!world.wolGame || world.leaveBones)) {
        static constexpr const char* kPieces[11] = {"Skull",        "RibCage",      "Pelvis",        "LeftLowerLeg",
                                                    "LeftUpperLeg", "LeftLowerArm", "LeftUpperArm",  "RightLowerLeg",
                                                    "RightUpperLeg", "RightLowerArm", "RightUpperArm"};
        static constexpr const char* kFacing[9] = {"NW", "N", "NE", "W", nullptr, "E", "SW", "S", "SE"};
        // Each facing's 11 pieces as (x, y) offsets from where it fell, in kPieces' order.
        static constexpr float kAt[9][22] = {
            {13, 33, 6, 25, 1, 20, -15, 14, -7, 21, -2, 59, -1, 31, -5, 2, -4, 11, 25, 23, 15, 23},
            {-4, 37, -2, 27, 0, 20, -5, 6, -6, 17, -18, 32, -12, 27, 9, 3, 2, 11, 15, 37, 7, 31},
            {-19, 28, -8, 22, 0, 19, 6, 7, -2, 14, -21, 18, -15, 18, 21, 13, 6, 14, -5, 42, -6, 31},
            {22, 21, 12, 20, 1, 19, -16, 24, -3, 24, 16, 33, 11, 27, -18, 10, -8, 14, 18, 9, 14, 14},
            {},
            {-20, 15, -10, 16, -1, 19, 17, 14, 4, 13, -14, 7, -9, 11, 22, 28, 7, 17, -22, 32, -15, 24},
            {16, 9, 9, 12, 1, 18, -8, 32, 2, 25, 22, 20, 15, 20, -22, 23, -6, 18, 3, 3, 6, 8},
            {3, 3, 1, 10, 0, 18, 8, 32, 7, 22, 14, 9, 11, 11, -11, 34, -2, 23, -12, 5, -5, 8},
            {-11, 5, -6, 11, -1, 18, 17, 25, 7, 17, 1, 3, 1, 8, 7, 36, 4, 22, -24, 16, -14, 14},
        };
        const auto [dx, dy] = gridDirection(u.direction);
        const int facing = dy * 3 + 4 + dx;
        const float atX = u.x;
        const float atY = u.y;
        for (int i = 0; i < 11 && kFacing[facing]; ++i) {
            const std::string type = std::string("Corpse") + kPieces[i] + kFacing[facing];
            if (!things->findThing(type)) {
                break; // (newObjectWithTypeInd failing: the rest not made)
            }
            ServerObject* piece = world.createAt(*things, type, kAt[facing][i * 2] + atX, kAt[facing][i * 2 + 1] + atY, frame);
            if (!piece) {
                break;
            }
            // Game flag 0x2000 is a network game; piece flag 0x40 is NO_COLLIDE.
            if (world.wolGame && (world.gameFlags & 0x2000u)) {
                piece->flags |= 0x40u;
            }
            piece->decayAt = frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(10, 20)(rng)) * 30u;
            piece->touch();
        }
    }
    // At a player start (a quest's at its checkpoint), still (Obj_SetPosition); in
    // team KotR its team's crown when nobody holds it; with game flag 0x2000, 5 s invulnerable (buff
    // 23).
    const ServerObject* gate = world.quest() && p.soulGate ? world.byExtent(p.soulGate) : nullptr;
    // (noxcpp: a mode may name the spot itself; then no start is sought, nor its random draw.)
    const std::optional<std::pair<float, float>> modeAt = world.modeHooks ? world.modeHooks->respawnPoint(*this, u) : std::nullopt;
    const auto [x, y] = modeAt ? *modeAt : gate ? nearSoulGate(*this, *gate) : findPlayerStart(world, &u, rng);
    setPosition(world, u, x, y, frame); // (Obj_SetPosition: its sync, relink, motion, harpoon, 0xEA, camera)
    u.vx = u.vy = 0;
    u.forceX = u.forceY = 0;
    playSound(world, world.quest() ? 0x3EEu : 0x94u, u); // (there)
    takeTeamCrown(*this, u);
    // Game flag 0x2000: a network game.
    if (world.gameFlags & 0x2000u) {
        enchant(world, u, Enchant::Invulnerable, 30 * 5, 5);
    }
}

std::pair<float, float> findPlayerStart(World& world, const ServerObject* unit, std::minstd_rand& rng)
{
    // Map_FindPlayerStart: the enabled PlayerStarts its team may use (one
    // without a team, or its own); the one farthest from its nearest enemy player, or a random
    // one with no enemies about; none usable: the last PlayerStart seen, else (2000, 2000).
    if (!unit) {
        return {0.0f, 0.0f};
    }
    const auto usable = [&](const ServerObject& o) {
        return o.typeName == NameId::PlayerStart && !o.gone() && (o.flags & ObjFlag::Enabled) && (unit->team == 0 || o.team == 0 || o.team == unit->team);
    };
    int count = 0;
    const ServerObject* last = nullptr;
    for (const auto& [extent, o] : world.objects()) {
        if (o.typeName == NameId::PlayerStart && !o.gone()) {
            last = &o;
            if (usable(o)) {
                ++count;
            }
        }
    }
    if (count == 0) {
        return last ? std::pair{last->x, last->y} : std::pair{2000.0f, 2000.0f};
    }
    // The start whose nearest enemy player is farthest away; with no enemies anywhere, a random one.
    // (The players by World::playerUnits: those a walk of every object finds, in its order.)
    float farthest = 0;
    const ServerObject* best = nullptr;
    bool alone = true;
    const auto players = world.playerUnits();
    for (const auto& [extent, o] : world.objects()) {
        if (!usable(o)) {
            continue;
        }
        float nearest = 1e7f;
        for (const ServerObject* bp : players) {
            const ServerObject& b = *bp;
            if (&b == unit || !isEnemy(world, *unit, b)) {
                continue;
            }
            // (The squared distance on the x87, FSTP'd to the nearest when under it.)
            const X87 dx = X87(o.x) - b.x;
            const X87 dy = X87(o.y) - b.y;
            if (const X87 d2 = dy * dy + dx * dx; d2 < X87(nearest)) {
                nearest = d2.f();
            }
            alone = false;
        }
        if (farthest < nearest) {
            best = &o;
            farthest = nearest;
        }
    }
    if (!alone && best) {
        return {best->x, best->y};
    }
    int pick = nox::util::uniform_int_distribution<int>(0, count - 1)(rng);
    for (const auto& [extent, o] : world.objects()) {
        if (usable(o)) {
            if (pick == 0) {
                return {o.x, o.y};
            }
            --pick;
        }
    }
    return {2000.0f, 2000.0f};
}

} // namespace nox::server
