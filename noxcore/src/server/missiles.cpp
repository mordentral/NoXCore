// Missiles on the server (missiles.hpp): bow, wand and chakram shots, and each frame's flight
// of every missile: its UPDATE function, its move (searched in short pieces for bodies and
// door leaves, and by a ray for walls) and the COLLIDE functions of what it meets.
#include "noxcore/server/missiles.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/collide.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/item_effects.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/walls.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

using util::hi;
using util::lo;
using util::X87;

/// Just ahead of a thrower along (fx, fy): its radius + `add` (kept on the stack) out, each
/// coordinate stored (shootBowCrossbow2, playerAttack's chakram, the harpoon's throw).
std::pair<float, float> aheadOf(const ServerObject& o, float add, float fx, float fy)
{
    const X87 r = X87(o.radius) + add;
    return {(r * fx + o.x).f(), (r * fy + o.y).f()};
}

/// A missile aimed at (tx, ty) at its speed (ChakramInMotionUpdate's and its collide's seek): the
/// x difference kept on the stack, the y one stored; the distance + 0.1 (a double) stored.
void aimAt(ServerObject& m, float tx, float ty)
{
    const X87 dx = X87(tx) - m.x;
    const X87 dy = X87(ty) - m.y;
    const float dyf = dy.f();
    const float d = (util::sqrt(dy * dyf + dx * dx) + X87::fromDouble(static_cast<double>(0.1f))).f();
    m.vx = (dx * m.speedCur / d).f();
    m.vy = (X87(dyf) * m.speedCur / d).f();
}

/// A heading's push at a missile's speed: each product stored.
void pushAlong(ServerObject& m, int direction)
{
    const auto [fx, fy] = directionVector(direction);
    m.forceX = (X87(fx) * m.speedCur).f();
    m.forceY = (X87(fy) * m.speedCur).f();
}

/// A missile's velocity mirrored on the wall's diagonal (collideReflect).
void reflectOffWall(ServerObject& m, float nx, float ny)
{
    const float vx = m.vx;
    const float vy = m.vy;
    // Wall faces are diagonals: swapping vx and vy mirrors the velocity on one; the signs follow
    // which diagonal it is (the normal's nx x ny).
    if (0.0f < ny * nx) {
        m.vx = -vy;
        m.vy = -vx;
    } else {
        m.vx = vy;
        m.vy = vx;
    }
}

void tellString(World& world, const ServerObject& player, std::string_view id)
{
    world.broadcasts().push_back({informStringBytes(id), true, player.extent});
}

/// The unit's strength (unitGetStrength): a player's; a monster's 30.
int strengthOf(const ServerObject* o)
{
    if (!o) {
        return 0;
    }
    if (o->player) {
        return static_cast<int>(o->player->strength);
    }
    return (o->classes & oc::Monster) ? 30 : 0;
}

/// Whether a body's shape holds the point, about its NewPos (the shape's point
/// test): a circle's, or strictly inside a box (insideBox: on an edge, out).
bool holds(const ServerObject& o, float px, float py)
{
    if (o.shape == ShapeKind::Circle) {
        // (On the stack against its kept radius squared: a float.)
        const X87 dx = X87(o.newX) - px;
        const X87 dy = X87(o.newY) - py;
        return dx * dx + dy * dy <= X87((X87(o.radius) * o.radius).f());
    }
    if (o.shape == ShapeKind::Box) {
        return insideBox(o, o.newX, o.newY, px, py);
    }
    return false;
}

// The harpoon's gamedata numbers (read every frame it flies).
const things::GameDataKey kMaxHarpoonDistance{"MaxHarpoonDistance"};
const things::GameDataKey kMaxHarpoonFlightDistance{"MaxHarpoonFlightDistance"};
const things::GameDataKey kMinHarpoonDistance{"MinHarpoonDistance"};
const things::GameDataKey kMaxHarpoonExistence{"MaxHarpoonExistence"};
const things::GameDataKey kHarpoonDamage{"HarpoonDamage"};

} // namespace

void turnSpellMissile(World& world, ServerObject& m, const ServerObject& to, std::uint32_t frame)
{
    if (!(m.classes & oc::Missile) || !(m.subclasses & 2u)) {
        return;
    }
    if (m.castBy == 0x73) {  // (Swap)
        if ((m.subclasses & 1u) && !m.gone()) {
            pointFx(world, 0x87, m.x, m.y);
            m.flags |= ObjFlag::Destroyed;
            m.pendingDelete = true;
        }
        return;
    }
    // Only one homing on `to` turns: it goes after its old owner, and is `to`'s now.
    if (m.seekTarget == to.extent) {
        m.seekTarget = m.owner;
        setOwner(world, world.byExtent(to.extent), m); // (unitClearOwner, unitSetOwner)
        m.born = frame;
        playSound(world, 0x40, to);
    }
}

ServerObject* Missiles::bodyAt(ServerObject& m, float fromX, float fromY, float px, float py)
{
    if (m.modSize && m.modSize->hit) {
        return bodyNear(m, fromX, fromY, px, py); // (noxcpp: a mode's hit shape)
    }
    // (the extent list of the cell the piece's end is in, from its
    // head, each through the body test; what it finds overwrites what was found before.)
    ServerObject* found = nullptr;
    world.unitsInCell(px, py, [&](ServerObject& o) {
        if (o.classes & oc::Door) {
            // A door, not a gate (subclass 4), meets whatever's piece of path crosses its leaf,
            // whatever it is; the piece's end then its start for those after it.
            if (!(o.subclasses & 4u)) {
                const auto [lx, ly] = doorLeaf(o.doorDirection);
                if (segmentsCross(fromX, fromY, px, py, o.x, o.y, static_cast<float>(lx) + o.x, static_cast<float>(ly) + o.y)) {
                    px = fromX;
                    py = fromY;
                    found = &o;
                }
            }
            return;
        }
        if (mayMeet(m, o) && holds(o, px, py)) {
            found = &o;
        }
    });
    return found;
}

bool Missiles::mayMeet(const ServerObject& m, const ServerObject& o)
{
    // (neither DESTROYED, 0x20)
    if ((o.classes & oc::Missile) || m.pendingDelete || m.gone() || o.pendingDelete || o.gone() || !m.collides ||
        !o.collides || (o.flags & ObjFlag::NoCollide)) {
        return false;
    }
    if (o.flags & 0x80u) { // MISSILE_HIT
        return true;
    }
    if (((m.flags & 0x11u) && (o.flags & ObjFlag::Airborne)) || ((o.flags & 0x11u) && (m.flags & ObjFlag::Airborne))) {
        // BELOW or SHORT (0x11) things and airborne ones pass over each other.
        return false;
    }
    if (((m.flags | o.flags) & 0x400u) && sameSide(world, o, m)) {
        return false; // (NO_COLLIDE_OWNER: unitsHaveSameTeam, the team or either's owner chain)
    }
    // Otherwise a monster's ordinary missile hits another monster only when that one is its
    // owner's enemy and not of its side.
    const ServerObject* owner = world.byExtent(m.owner);
    return !owner || !(m.classes & oc::Missile) || (m.subclasses & 2) || !(owner->classes & oc::Monster) ||
           !(o.classes & oc::Monster) || (isEnemy(world, *owner, o) && !sameSide(world, o, *owner));
}

ServerObject* Missiles::bodyNear(ServerObject& m, float fromX, float fromY, float px, float py)
{
    // noxcpp's, for a missile a mode gave a hit shape: the stock search widened by its radius. The
    // bodies whose boxes meet the box its shape sweeps over the piece (its shape's box about the
    // piece's start and about its end, together: the grid's search, each once, row by row), each
    // through the same filter; a door's leaf as the stock search has it (the path's centre line
    // crossing it: the swept box holds the crossing, so every leaf crossed is tested, whatever
    // the radius); a body met when its shape grown by the missile's radius holds the piece's end
    // (a circle within its radius + the missile's, a box within the missile's radius of its
    // sides: at radius 0, the stock test; a box-shaped missile counts as a circle of its larger
    // side's half). What it finds overwrites what was found before, as the stock search's: when
    // several are met in one piece, the walk's last (by the cells' order), not the nearest. Walls
    // stay the stock ray's (the centre's path).
    //
    // Its owner's grace: a missile starts just past its owner's edge (a fireball twice its
    // caster's radius ahead), where a grown shape can still reach the owner. Until a piece's end
    // first lies outside the owner's shape grown by the radius, the owner is met only as the stock
    // test meets it (its own shape holding the end: a fireball put at its caster because a wall
    // was too close still bursts on it, as stock). From then on, as any body. The owner cleared is
    // kept by its extent, so a new owner (turned by a shield, changed hands) has its own grace.
    const float r = m.shape == ShapeKind::Box ? std::max(m.boxWidth, m.boxHeight) * 0.5f : m.radius;
    std::minstd_rand unused; // (boxDepth's way out of a box's dead centre: never asked, insideBox having it)
    const auto grownHolds = [&](const ServerObject& o) {
        if (o.shape == ShapeKind::Circle) {
            const X87 dx = X87(o.newX) - px;
            const X87 dy = X87(o.newY) - py;
            const float reach = (X87(o.radius) + r).f();
            return dx * dx + dy * dy <= X87((X87(reach) * reach).f());
        }
        if (o.shape == ShapeKind::Box) {
            float nx = 0;
            float ny = 0;
            return insideBox(o, o.newX, o.newY, px, py) || 0.0f <= boxDepth(o, px, py, r, nx, ny, unused);
        }
        return false;
    };
    const ServerObject* graced = nullptr;
    if (m.owner != 0 && m.modSize->ownerCleared != m.owner) {
        const ServerObject* owner = world.byExtent(m.owner);
        if (owner && grownHolds(*owner)) {
            graced = owner;
        } else {
            m.modSize->ownerCleared = m.owner;
        }
    }
    const MapGrid::Rect atStart = World::collideBoxAt(m, fromX, fromY);
    const MapGrid::Rect atEnd = World::collideBoxAt(m, px, py);
    const MapGrid::Rect box{std::min(atStart.left, atEnd.left), std::min(atStart.top, atEnd.top), std::max(atStart.right, atEnd.right),
                            std::max(atStart.bottom, atEnd.bottom)};
    ServerObject* found = nullptr;
    world.unitsInRect(box.left, box.top, box.right, box.bottom, [&](ServerObject& o) {
        if (o.classes & oc::Door) {
            if (!(o.subclasses & 4u)) {
                const auto [lx, ly] = doorLeaf(o.doorDirection);
                if (segmentsCross(fromX, fromY, px, py, o.x, o.y, static_cast<float>(lx) + o.x, static_cast<float>(ly) + o.y)) {
                    px = fromX;
                    py = fromY;
                    found = &o;
                }
            }
            return;
        }
        if (!mayMeet(m, o)) {
            return;
        }
        if (&o == graced ? holds(o, px, py) : grownHolds(o)) {
            found = &o;
        }
    });
    return found;
}

std::pair<float, float> hitRadiusRange(const ServerObject& m)
{
    const bool kept = m.modSize && m.modSize->hit;
    const ShapeKind type = kept ? m.modSize->typeShape : m.shape;
    if (type == ShapeKind::Circle) {
        return {1.0f, 4.0f * (kept ? m.modSize->typeRadius : m.radius)};
    }
    if (type == ShapeKind::Box) {
        const float w = kept ? m.modSize->typeBoxWidth : m.boxWidth;
        const float h = kept ? m.modSize->typeBoxHeight : m.boxHeight;
        return {1.0f, 4.0f * std::max(w, h) * 0.5f};
    }
    return {1.0f, 32.0f};
}

void setHitRadius(World& world, ServerObject& m, float radius)
{
    ServerObject::ModSize& s = m.modSize ? *m.modSize : m.modSize.emplace();
    if (!s.hit) {
        s.typeShape = m.shape;
        s.typeRadius = m.radius;
        s.typeBoxWidth = m.boxWidth;
        s.typeBoxHeight = m.boxHeight;
        s.hit = true;
    }
    if (s.typeShape == ShapeKind::Box) {
        const float k = radius / (std::max(s.typeBoxWidth, s.typeBoxHeight) * 0.5f);
        m.shape = ShapeKind::Box;
        m.boxWidth = s.typeBoxWidth * k;
        m.boxHeight = s.typeBoxHeight * k;
    } else {
        m.shape = ShapeKind::Circle;
        m.radius = radius;
    }
    world.grid().setMissileBox(m, World::collideBoxAt(m, m.x, m.y));
}

void restoreHitShape(World& world, ServerObject& m)
{
    if (!m.modSize || !m.modSize->hit) {
        return;
    }
    ServerObject::ModSize& s = *m.modSize;
    m.shape = s.typeShape;
    m.radius = s.typeRadius;
    m.boxWidth = s.typeBoxWidth;
    m.boxHeight = s.typeBoxHeight;
    s.hit = false;
    if (s.idle()) {
        m.modSize.reset();
    }
    world.grid().setMissileBox(m, World::collideBoxAt(m, m.x, m.y));
}

void setBurstScale(ServerObject& m, float scale)
{
    if (scale == 1.0f) {
        if (m.modSize) {
            m.modSize->burstScale = 1.0f;
            if (m.modSize->idle()) {
                m.modSize.reset();
            }
        }
        return;
    }
    (m.modSize ? *m.modSize : m.modSize.emplace()).burstScale = scale;
}

void setDrawScale(ServerObject& o, float scale)
{
    if (scale == 1.0f) {
        if (o.modSize) {
            o.modSize->drawScale = 1.0f;
            if (o.modSize->idle()) {
                o.modSize.reset();
            }
        }
        return;
    }
    (o.modSize ? *o.modSize : o.modSize.emplace()).drawScale = scale;
}

float drawScaleOf(const ServerObject& o)
{
    return o.modSize ? o.modSize->drawScale : 1.0f;
}

bool Missiles::shoot(ServerObject& shooter, ServerObject& weapon)
{
    // Obj_ShootBowOrCrossbow. It begins with the weapon's "reload counter" (BowUse's
    // one byte of use data): while non-zero, "pattack.c:ReloadingQuiver", a bow's 0x377 (none in
    // a quest) or a crossbow's 0x378, one off it and no shot. Nothing ever makes it non-zero
    // (the type's zeroed template, WeaponCreate, Obj_PlayerEquipWeapon and this function's reload
    // all write 0; the respawn's copy is only a quiver's, 0x82), so that branch never runs and
    // isn't kept here.
    const std::uint32_t bit = things::weaponBitOf(weapon.typeName);
    // The first worn quiver with arrows (or infinite ones), for anyone holding the bow.
    for (const std::uint32_t e : shooter.inventory) {
        ServerObject* o = world.byExtent(e);
        if (o && (o->flags & ObjFlag::Equipped) && things::weaponBitOf(o->typeName) == 2 && (o->charges != 0 || o->infiniteAmmo)) {
            loose(shooter, o, weapon, bit);
            return true;
        }
    }
    // None: in a quest a bow looses a WeakArcherArrow anyway, and goes on to the reload.
    if (world.quest() && bit == 4) {
        loose(shooter, nullptr, weapon, 4);
    }
    // A monster (an NPC) never reloads.
    if (!(shooter.classes & oc::Player) || !shooter.player) {
        return false;
    }
    // Put one on from the inventory, if there is one (playerTryReloadQuiver).
    Items items{world, things, modifiers, rng, frame};
    bool reloaded = false;
    for (const std::uint32_t e : keysOf(shooter.inventory)) {
        ServerObject* o = world.byExtent(e);
        if (o && things::weaponBitOf(o->typeName) == 2) {
            reloaded = items.equipWeapon(shooter, *o, true);
            break;
        }
    }
    // "ReloadQuiver", or "NoQuiver" but for a quest's bow (its weak arrow flew); then the dry
    // shot's sound: a bow's 0x377 (none in a quest), a crossbow's 0x378.
    if (reloaded || !world.quest() || bit != 4) {
        tellString(world, shooter, reloaded ? "pattack.c:ReloadQuiver" : "pattack.c:NoQuiver");
    }
    if (bit == 4) {
        if (!world.quest()) {
            playSound(world, 0x377, shooter);
        }
    } else if (bit == 8) {
        playSound(world, 0x378, shooter);
    }
    setPlayerState(world, shooter, 0xD, frame); // (Plr_SetState)
    return false;
}

void Missiles::loose(ServerObject& shooter, ServerObject* quiver, ServerObject& weapon, std::uint32_t bit)
{
    // shootBowCrossbow2: just ahead of it, if no wall stands between.
    const auto [fx, fy] = directionVector(shooter.direction);
    const auto [sx, sy] = aheadOf(shooter, 4.0f, fx, fy);
    if (!world.traceRay(shooter.x, shooter.y, sx, sy, Trace::Doors | Trace::ThroughWindows)) {
        return;
    }
    ServerObject& arrow = world.spawn(things, !quiver ? "WeakArcherArrow" : bit == 4 ? "ArcherArrow" : "ArcherBolt", sx, sy);
    arrow.owner = shooter.extent;
    arrow.born = frame;
    arrow.flags |= ObjFlag::Active | ObjFlag::Airborne;
    if (quiver) {
        arrow.modifiers = quiver->modifiers;
    }
    shootEffects(modifiers, weapon, arrow); // (the bow's recoil and projectile speed, shootApplyEffects)
    arrow.vx = (X87(fx) * arrow.speedCur).f();
    arrow.vy = (X87(fy) * arrow.speedCur).f();
    arrow.newX = sx;
    arrow.newY = sy;
    arrow.direction = shooter.direction;
    arrow.touch();
    // One fewer (MSG_REPORT_CHARGES), and an empty quiver goes: a player's only.
    if (quiver && !quiver->infiniteAmmo && (shooter.classes & oc::Player) && shooter.player) {
        --quiver->charges;
        world.broadcasts().push_back({{0x64, lo(quiver->extent), hi(quiver->extent), quiver->charges, quiver->maxCharges}, true, shooter.extent});
        if (quiver->charges == 0) {
            Items{world, things, modifiers, rng, frame}.takeOff(shooter, *quiver, true, true);
            // MSG_REPORT_DROP (0x4D): the quiver's extent and type, 16 bits each.
            world.broadcasts().push_back({{0x4D, lo(quiver->extent), hi(quiver->extent), lo(quiver->type), hi(quiver->type)}, true, shooter.extent});
            std::erase(shooter.inventory, quiver->extent);
            quiver->holder = 0;
            quiver->pendingDelete = true;
        }
    }
    playSound(world, bit == 4 ? 0x375 : 0x376, shooter); // (a bow's, a crossbow's)
}

bool Missiles::wandShot(ServerObject& shooter, ServerObject& wand)
{
    if (wand.wandMissile.empty()) {
        return false;
    }
    if (wand.maxCharges != 0 && wand.charges == 0) {
        playSound(world, 0xDE, shooter); // (spent: a click)
        return false;
    }
    // (Its wait less its READINESS, as an unsigned: below none it never fires.)
    if (frame - wand.wandLastShot < static_cast<std::uint32_t>(static_cast<int>(wand.wandDelay) - readiness(wand, modifiers))) {
        return false;
    }
    // The shot starts its radius + 4 ahead of the shooter, plus the shooter's own velocity
    // (useLesserFireballStaff: x's sum stored before the velocity's added, y's kept).
    // A wall between the shooter and there (mapTraceRay, flags 5): it starts at the shooter.
    const auto [fx, fy] = directionVector(shooter.direction);
    const X87 r = X87(shooter.radius) + 4.0f;
    float sx = (X87((r * fx + shooter.x).f()) + shooter.vx).f();
    float sy = (r * fy + shooter.y + shooter.vy).f();
    if (!world.traceRay(shooter.x, shooter.y, sx, sy, Trace::Doors | Trace::ThroughWindows)) {
        sx = shooter.x;
        sy = shooter.y;
    }
    const auto fire = [&](int direction) { // (wandShot)
        if (things.findThing(wand.wandMissile) == nullptr) {
            return;
        }
        ServerObject& m = world.spawn(things, wand.wandMissile, sx, sy);
        m.owner = shooter.extent;
        m.born = frame;
        m.flags |= ObjFlag::Active;
        m.direction = static_cast<std::uint16_t>(direction);
        const auto [dx, dy] = directionVector(direction);
        m.vx = (X87(shooter.vx) + (X87(dx) * m.speedCur).f()).f();
        m.vy = (X87(shooter.vy) + (X87(dy) * m.speedCur).f()).f();
        m.newX = sx;
        m.newY = sy;
        m.touch();
    };
    const int d = shooter.direction & 0xFF;
    fire(d);
    // MULTI_SHOT: two more, 8 (of 256) to either side.
    if (wand.wandMultiShot) {
        fire((d + 8) & 0xFF);
        fire((d - 8) & 0xFF);
    }
    wand.wandLastShot = frame;
    playSound(world, wand.wandSound, shooter);
    if (wand.maxCharges != 0) {
        --wand.charges;
        wand.chargePercent = wand.charges * 100 / wand.maxCharges;
        world.broadcasts().push_back({{0x64, lo(wand.extent), hi(wand.extent), wand.charges, wand.maxCharges}, true, shooter.extent});
    }
    return true;
}

bool Missiles::throwChakram(ServerObject& thrower, ServerObject& weapon)
{
    const bool round = things::weaponBitOf(weapon.typeName) == 0x40;  // (weapon bit 0x40)
    // (playerAttack) Its radius + 4 ahead, if no wall is between (the ray's flags: 5 for
    // a round one, 4 for a fan).
    const auto [fx, fy] = directionVector(thrower.direction);
    const auto [sx, sy] = aheadOf(thrower, 4.0f, fx, fy);
    if (!world.traceRay(thrower.x, thrower.y, sx, sy, round ? Trace::Doors | Trace::ThroughWindows : Trace::ThroughWindows)) {
        playSound(world, 0x143, thrower); // (no room to throw)
        return false;
    }
    if (!things.findThing(round ? "RoundChakramInMotion" : "FanChakramInMotion")) {
        return false;
    }
    ServerObject& m = world.spawn(things, round ? "RoundChakramInMotion" : "FanChakramInMotion", sx, sy);
    m.owner = thrower.extent;
    m.team = thrower.team;
    m.born = frame;
    m.flags |= ObjFlag::Active | ObjFlag::Airborne;
    m.modifiers = weapon.modifiers;
    m.vx = (X87(fx) * m.speedCur).f();
    m.vy = (X87(fy) * m.speedCur).f();
    m.newX = sx;
    m.newY = sy;
    m.direction = thrower.direction;
    m.touch();
    playSound(world, 0x37B, thrower);
    if (round) {
        // It goes itself: out of the thrower's inventory (taken off, MSG_REPORT_DROP), riding in
        // the one in flight, which starts out with 4 bounces and its thrower's place. The one in
        // flight owns it now, not the thrower (unitClearOwner, then inventoryPutImpl's
        // unitSetOwner), so only that one blocks a second under server option 2.
        Items{world, things, modifiers, rng, frame}.takeOff(thrower, weapon, true, true);
        world.broadcasts().push_back({{0x4D, lo(weapon.extent), hi(weapon.extent), lo(weapon.type), hi(weapon.type)}, true, thrower.extent});
        std::erase(thrower.inventory, weapon.extent);
        weapon.holder = m.extent;
        setOwner(world, &m, weapon);
        m.inventory.emplace_back(weapon.extent);
        m.chakram = ServerObject::Chakram{4, 2, {}, {}, thrower.x, thrower.y};  // (4 bounces, leg 2: outward)
        return true;
    }
    // A fan chakram looses one of its charges (AmmoUse), gone at none.
    if (!weapon.infiniteAmmo) {
        --weapon.charges;
        if (weapon.charges == 0) {
            Items{world, things, modifiers, rng, frame}.takeOff(thrower, weapon, true, true);
            world.broadcasts().push_back({{0x4D, lo(weapon.extent), hi(weapon.extent), lo(weapon.type), hi(weapon.type)}, true, thrower.extent});
            std::erase(thrower.inventory, weapon.extent);
            weapon.holder = 0;
            weapon.pendingDelete = true;
        } else {
            world.broadcasts().push_back({{0x64, lo(weapon.extent), hi(weapon.extent), weapon.charges, weapon.maxCharges}, true, thrower.extent});
        }
    }
    return true;
}

void Missiles::chakramDrops(ServerObject& m, ServerObject& chakram, bool dropped)
{
    // Either way out of the missile's inventory (Obj_RemoveFromInventory: nobody's) and into the
    // world where the missile is.
    Items items{world, things, modifiers, rng, frame};
    if (dropped) {
        // Obj_DropAt: in a network game but a quest not picked up at once; its WeaponDrop, DefaultDrop
        // (decaying in 10 s out of solo and quest games unless it respawns), its drop's sound, then
        // 25 s out of solo and quest games under server option 2.
        items.dropAt(m, chakram, m.x, m.y);
    } else {
        // Off a body: Obj_RemoveFromInventory and createAt alone, no DROP: it doesn't decay.
        items.removeFromInventory(m, chakram);
        items.place(chakram, m.x, m.y);
    }
    m.pendingDelete = true;
}

void Missiles::chakramUpdate(ServerObject& m)
{
    // ChakramInMotionUpdate.
    ServerObject::Chakram& c = *m.chakram;
    ServerObject* chakram = m.inventory.empty() ? nullptr : world.byExtent(m.inventory.front());
    if (!chakram || chakram->gone()) {
        m.pendingDelete = true;
        return;
    }
    // What it last hit is forgotten once that's gone.
    if (c.lastHit != 0) {
        const ServerObject* hit = world.byExtent(c.lastHit);
        if (!hit || hit->gone()) {
            c.lastHit = 0;
        }
    }
    // Thrower gone: lost. Else it notes where the thrower is and, on its way home (leg 0) with the
    // thrower in sight, heads straight for it.
    const ServerObject* owner = world.byExtent(m.owner);
    if (!owner || owner->gone()) {
        c.leg = 1; // lost
        c.target = 0;
    } else {
        c.homeX = owner->x;
        c.homeY = owner->y;
        // (mapCheck: in sight of it, the ray's flags 9; the bodies in the way: not yet)
        bool steer = true;
        if (!world.traceRay(m.x, m.y, owner->x, owner->y, Trace::Doors | Trace::NearWindows)) {
            c.target = 0;
        } else if (c.leg != 0) {
            steer = false;
        } else {
            c.target = owner->extent;
        }
        if (steer && c.leg == 0) {
            const ServerObject* t = c.target ? world.byExtent(c.target) : nullptr;
            if (c.target == 0 || (t && !(t->flags & (ObjFlag::Destroyed | ObjFlag::Dead)))) {
                aimAt(m, c.homeX, c.homeY);
            } else {
                c.target = 0;
                c.leg = 1;
            }
        }
    }
    if (30u * 5u < frame - m.born) { // five seconds out: lost
        c.leg = 1;
        c.target = 0;
    }
}

void Missiles::chakramHits(ServerObject& m, ServerObject* other, float nx, float ny)
{
    // ChakramInMotionCollide.
    ServerObject::Chakram& c = *m.chakram;
    ServerObject* chakram = m.inventory.empty() ? nullptr : world.byExtent(m.inventory.front());
    if (!chakram || chakram->gone()) {
        m.pendingDelete = true;
        return;
    }
    // Turned back at random, half a turn give or take an eighth, from where it was.
    const auto turnBack = [&] {
        // (Its speed stored; its heading the velocity's, rounded: directionOf.)
        const float speed = util::sqrt(X87(m.vx) * m.vx + X87(m.vy) * m.vy).f();
        const int heading = directionOf(m.vx, m.vy);
        const int d = (heading + 0x80 + nox::util::uniform_int_distribution<int>(-0x40, 0x40)(rng)) & 0xFF;
        const auto [dx, dy] = directionVector(d);
        m.vx = (X87(speed) * dx).f();
        m.vy = (X87(speed) * dy).f();
        m.x = m.newX = m.lastX; // (PosVec and NewPos = PrevPos, its last turn's start)
        m.y = m.newY = m.lastY;
    };
    // The nearest enemy in sight within 400, not its thrower nor what it last hit, sought after
    // the bounce.
    const auto seek = [&] {
        const ServerObject* best = nullptr;
        float bestD2 = 1e7f;
        world.unitsInRect((X87(m.x) - 400.0f).f(), (X87(m.y) - 400.0f).f(), (X87(m.x) + 400.0f).f(), (X87(m.y) + 400.0f).f(),
                          [&](const ServerObject& o) { // (getUnitsInRect, by the grid)
            // (A creature or generator, class 0x20006; not 0x8020; not invisible, testUnitBuffs 0;
            // not its thrower nor what it last hit.)
            if (!(o.classes & (oc::Monster | oc::Player | oc::MonsterGenerator)) || (o.flags & (ObjFlag::Destroyed | ObjFlag::Dead)) ||
                o.pendingDelete || enchanted(o, Enchant::Invisible) || o.extent == m.owner || o.extent == c.lastHit || o.holder != 0) {
                return;
            }
            const X87 dx = X87(m.x) - o.x;
            const X87 dy = X87(m.y) - o.y;
            const X87 d2 = dx * dx + dy * dy; // (on the stack; the nearest's kept as a float)
            // (mapCheck(it, the chakram): the bodies in the way, then the ray, flags 9;
            // the exe asks it before the distance, which changes nothing found)
            if (d2 <= X87(160000.0f) && d2 < X87(bestD2) && bodiesClear(world, o, m)) {
                best = &o;
                bestD2 = d2.f();
            }
        });
        if (best) {
            c.leg = 2;
            aimAt(m, best->x, best->y);
        }
    };
    // After a bounce: one fewer, or none left: home (unless already on the way: out again).
    // (Off a body, a lost one falls with 0x37D and no DROP; off a wall, by its DROP.)
    const auto afterBounce = [&](bool hadBounces, bool body) {
        bool contact = true;
        if (!hadBounces) {
            if (c.leg == 0) {  // on its way home: sent out again
                c.leg = 2;
                contact = false;
            }
        } else {
            --c.bounces;
        }
        if (c.leg == 1) {
            if (body) {
                playSound(world, 0x37D, m);
            }
            chakramDrops(m, *chakram, !body); // (lost: down it falls)
            return;
        }
        if (c.bounces == 0 && contact) {
            c.leg = 0;
            c.target = m.owner;
            return;
        }
        seek();
    };
    // A wall: it bounces off (none left: turned back), with its blow to the wall.
    if (!other) {
        if (nx == 0 && ny == 0) {
            return; // (nothing met)
        }
        const bool hadBounces = c.bounces != 0;
        if (!hadBounces) {
            turnBack();
        } else {
            reflectOffWall(m, nx, ny);
        }
        wallBlow(m, 1, 0); // (its blow to the wall: 1 of type 0)
        afterBounce(hadBounces, false);
        return;
    }
    if (other->extent == m.owner) {
        // Caught: back in its thrower's inventory, worn if nothing is.
        std::erase(m.inventory, chakram->extent);
        chakram->holder = 0;
        clearOwner(world, *chakram); // (Obj_RemoveFromInventory; the inventory's put sets it again)
        if (other->player) {
            Items{world, things, modifiers, rng, frame}.receive(*other, *chakram);
        }
        playSound(world, 0x37C, m);
        m.pendingDelete = true;
        return;
    }
    if (m.team != 0 && m.team == other->team) {
        return;
    }
    ServerObject* owner = world.byExtent(m.owner);
    if (!owner || (owner->flags & (ObjFlag::Destroyed | ObjFlag::Dead))) {
        chakramDrops(m, *chakram, true); // (its thrower gone: Obj_DropAt)
        return;
    }
    // Its damage as a weapon's: (strength - required strength) x coefficient + least, from
    // modifier.bin's record by the flying missile's own type; the dead, or without a record,
    // nothing at all (no blow, no bounce).
    const things::EquipDef* def = modifiers ? modifiers->findWeapon(m.typeName) : nullptr;
    if ((other->flags & ObjFlag::Dead) || !def) {
        return;
    }
    const int strength = strengthOf(owner);
    float damage = boltDamage53(world, def, strength).f();
    // Its modifiers' ATTACKEFFECTs and ATTACKPREHITEFFECTs (playerPreAttackEffects: item_effects.hpp).
    Fight fight{world, rng, frame, &things, modifiers};
    attackEffects(fight, m, owner, damage);
    preHitEffects(fight, *other, owner, &m);
    fight.damage(*other, owner, m.extent, roundedDamage(damage), 0);
    if (!(other->flags & (ObjFlag::Destroyed | ObjFlag::Dead))) {
        c.lastHit = other->extent;
    }
    const bool hadBounces = c.bounces != 0;
    if (!hadBounces) {
        turnBack();
    } else {
        reflectMissile(m, *other);
    }
    afterBounce(hadBounces, true);
}

ServerObject* Missiles::shooterPlayer(const ServerObject& m)
{
    // (findParentChainPlayer: up the owners)
    ServerObject* o = world.byExtent(m.owner);
    for (int guard = 0; o && !o->player && o->owner != 0 && guard < 8; ++guard) {
        o = world.byExtent(o->owner);
    }
    return o;
}

// By the missile's COLLIDE function's name.
void Missiles::collide(ServerObject& m, ServerObject* other, float nx, float ny)
{
    const NameId f = m.collideFn.id(); // (by the name's id)
    switch (f) {
    case NameId::ArrowCollide:
        arrowHits(m, other);
        break;
    case NameId::SparkExplosionCollide:
        sparkExplosion(m, other);
        break;
    case NameId::SpiderSpitCollide:
        // collideWebbing: it goes on whatever it meets (0x15F at it); a
        // body it touches (no harm, type 2, from its shooter's player) is slowed 4 s (power 3), a
        // player told "objcoll.c:WebbingSlow".
        m.pendingDelete = true;
        if (other) {
            playSound(world, 0x15F, m);
            Fight fight{world, rng, frame, &things, modifiers};
            if (fight.damage(*other, fight.parentPlayer(&m), m.extent, 0, 2) != 0) {
                if (other->classes & (oc::Monster | oc::Player)) {
                    enchant(world, *other, Enchant::Slowed, 30 * 4, 3);
                }
                if (other->player) {
                    world.broadcasts().push_back({informStringBytes("objcoll.c:WebbingSlow"), true, other->extent});
                }
            }
        }
        break;
    case NameId::ProjectileCollide: {
        // collideProjectileGeneric: its damage (an urchin's ThrowingStone
        // UrchinStoneDamage, an ImpShot ImpShotDamage, else CollideData's number), type 11. A wall
        // takes it, and it's gone (nothing met: gone too); a body that takes it (Damage's say,
        // from its shooter's player) ends it, one that doesn't lets it fly on.
        int amount = m.collideArg;
        if (m.typeName == NameId::ThrowingStone) {
            amount = static_cast<int>(world.number("UrchinStoneDamage"));
        } else if (m.typeName == NameId::ImpShot) {
            amount = static_cast<int>(world.number("ImpShotDamage"));
        }
        if (!other) {
            wallBlow(m, amount, 0xB);
            m.pendingDelete = true;
        } else if (Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent, amount, 0xB) != 0) {
            m.pendingDelete = true;
        }
        break;
    }
    case NameId::MonsterArrowCollide: {
        // collideMonsterArrow: its damage (CollideData's first number in a solo
        // game, else its second: an arrow trap's ArrowTrapDamage). A wall takes it (type 11),
        // a live body (impale, from its shooter's player); either way it's gone. The dead it
        // passes.
        const int amount = (world.gameFlags & GameFlag::Solo) ? m.collideArg : m.collideArg2;
        if (!other) {
            wallBlow(m, amount, 0xB);
            m.pendingDelete = true;
        } else if (!(other->flags & ObjFlag::Dead)) {
            Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent, amount, 3);
            m.pendingDelete = true;
        }
        break;
    }
    case NameId::YellowStarShotCollide:
    case NameId::WallReflectCollide:
        starShot(m, other, nx, ny);
        break;
    case NameId::ChakramInMotionCollide:
        if (m.chakram) {
            chakramHits(m, other, nx, ny);
        } else {
            m.pendingDelete = true; // (one without its chakram's record: as the other missiles, below)
        }
        break;
    case NameId::BoomCollide:
        boom(m, other, nx, ny);
        break;
    case NameId::HarpoonCollide:
        harpoonHits(m, other);
        break;
    case NameId::PixieCollide:
        pixieHits(m, other, nx, ny);
        break;
    case NameId::SpellProjectileCollide:
        spellFlyHits(m, other, nx, ny);
        break;
    case NameId::DeathBallFragmentCollide:
        // collideDeathBallFragment: off a wall it bounces (collideReflect),
        // 0x25 at it, 20 crush to the cell its NewPos is in (damageToMap), and flies
        // on; a body takes 20 crush (from its player) and it's gone; met nothing at all (a
        // Counterspell), gone.
        if (!other && (nx != 0 || ny != 0)) {
            reflectOffWall(m, nx, ny);
            playSound(world, 0x25, m);
            cellBlow(m, m.newX, m.newY, 20, 2);
            return;
        }
        if (other) {
            Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent, 20, 2);
        }
        m.pendingDelete = true;
        break;
    case NameId::DeathBallCollide:
        // collideDeathBall: off a wall it bounces, its blow
        // (DeathBallCollideDamage, crush) to the wall; off a door's leaf too, back where it was;
        // anything else takes DeathBallCollideDamage (crush) and it goes on. A bounce off a wall
        // 0x25, off a leaf 0x11B, at it.
        if (!other) {
            if (nx != 0 || ny != 0) {
                reflectOffWall(m, nx, ny);
                playSound(world, 0x25, m);
                wallBlow(m, static_cast<int>(world.number("DeathBallCollideDamage")), 2);
            }
        } else if (other->classes & oc::Door) {
            // Its velocity mirrored off the leaf's normal (the leaf turned, the
            // side it came from), its move undone (NewPos = PrevPos).
            m.newX = m.lastX; // (PrevPos, its last turn's start)
            m.newY = m.lastY;
            const auto [lx, ly] = doorLeaf(other->doorDirection);
            float px = static_cast<float>(-ly);
            float py = static_cast<float>(lx);
            if (X87(0.0f) < X87(static_cast<float>(lx)) * (X87(other->y) - m.lastY) + X87(px) * (X87(other->x) - m.lastX)) {
                px = -px;
                py = -py;
            }
            // (The length + 0.1 and the along part stay on the stack; the across part and three
            // of the four pieces are stored, the across y piece kept for the last sum.)
            const X87 d = util::sqrt(X87(px) * px + X87(py) * py) + 0.1f;
            const X87 along = (X87(m.vx) * px + X87(m.vy) * py) / d;
            const float across = ((X87(px) * m.vy + X87(-py) * m.vx) / d).f();
            const float ax = (along * px / d).f();
            const float ay = (along * py / d).f();
            const float bx = (X87(across) * -py / d).f();
            m.vx = (X87(bx) - ax).f();
            m.vy = (X87(across) * px / d - ay).f();
            playSound(world, 0x11B, m);
        } else {
            Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent,
                                                                static_cast<int>(world.number("DeathBallCollideDamage")), 2);
        }
        break;
    default:
        m.pendingDelete = true; // (the other missiles: not yet; gone on whatever they meet)
        break;
    }
}

void Missiles::sparkExplosion(ServerObject& m, ServerObject* other)
{
    // fireballCollide. A reflective shield met in front (from where it is) turns
    // it: reflected, the shield-bearer's now, 0x7A at it; no burst.
    if (other && enchanted(*other, Enchant::ReflectiveShield) && (facingSides(other->x, other->y, other->direction, m.x, m.y) & 1)) {
        reflectMissile(m, *other);
        setOwner(world, other, m); // (unitClearOwner / unitSetOwner)
        playSound(world, 0x7A, *other);
        return;
    }
    // A quest's player doesn't burst on a team-mate.
    if (world.quest() && other && (other->classes & oc::Player)) {
        const ServerObject* p = shooterPlayer(m);
        if (p && (p->classes & oc::Player) && !isEnemy(world, *p, *other)) {
            return;
        }
    }
    const int size = m.collideArg;
    const float cx = m.x;
    const float cy = m.y;
    // The push's reach and the harm's (each stored as the float argument it is): times a mode's
    // burst scale (noxcpp's, ServerObject::ModSize; none for the game's), the product stored.
    float pushReach = (X87::fromDouble(size) * 0.6666667f).f();
    float reach = (X87::fromDouble(size) * 0.33333334f).f();
    if (m.modSize && m.modSize->burstScale != 1.0f) {
        pushReach = (X87(pushReach) * m.modSize->burstScale).f();
        reach = (X87(reach) * m.modSize->burstScale).f();
    }
    // The push: 50, out to 2/3 of its size.
    pushAround(cx, cy, pushReach, 0.0f, 50.0f, &m);
    const int damage = size >> 1;
    if (other) {
        Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent, damage, 1);
    }
    // Around it: a third of its size, full within 15 (a solo game's: 0), from the
    // fireball itself.
    damageAround(cx, cy, reach, (world.gameFlags & GameFlag::Solo) ? 0.0f : 15.0f, damage, 1, &m, other);
    // The burst for the clients (netSparkExplosionFx: MSG 0x93 x, y, size).
    const auto ix = static_cast<std::uint32_t>(std::clamp(cx, 0.0f, 65535.0f));
    const auto iy = static_cast<std::uint32_t>(std::clamp(cy, 0.0f, 65535.0f));
    world.broadcasts().push_back({{0x93, lo(ix), hi(ix), lo(iy), hi(iy), static_cast<std::uint8_t>(size)}, false, 0});
    playSound(world, 0x2A, m);
    makeScorch(world, things, cx, cy, 2, rng, frame);
    m.pendingDelete = true;
}

void Missiles::damageAround(float x, float y, float reach, float full, int damage, int type, ServerObject* source, const ServerObject* except,
                            bool spareSource)
{
    // The units in the square of the larger of reach and full about it (getUnitsInRect, by the
    // grid), each struck as it's found (which asks nothing of its health: DefaultDamage
    // runs on whatever is in reach).
    const float m = full <= reach ? reach : full;
    // (The box's corners stored.)
    const float left = (X87(x) - m).f();
    const float top = (X87(y) - m).f();
    const float right = (X87(m) + x).f();
    const float bottom = (X87(m) + y).f();
    Fight fight{world, rng, frame, &things, modifiers};
    world.unitsInRect(left, top, right, bottom, [&](ServerObject& o) {
        if ((spareSource && &o == source) || &o == except) { // (no 0x20 test: a deleted one struck too)
            return;
        }
        // (The distance on the stack against the reach, then stored; the falloff on the stack,
        // chopped by __ftol.)
        const X87 dx = X87(o.x) - x;
        const X87 dy = X87(o.y) - y;
        const X87 d = util::sqrt(dx * dx + dy * dy);
        const float df = d.f();
        // (Within the larger of the two, its harm falling off past `full` to it.)
        if (d <= X87(m) && world.traceRay(x, y, o.x, o.y, Trace::Doors)) {
            const int dealt = full <= df ? util::ftol((X87(1.0f) - (X87(df) - full) / (X87(m) - full)) * X87::fromDouble(damage)) : damage;
            fight.damage(o, source, 0, dealt, type);
        }
    });
    // Then the walls in the box about it (mapDamageToWalls: its cells, the
    // corners chopped to whole numbers and divided by 23).
    Walls{world, things, rng, frame}.damageAround(static_cast<int>(left) / 23, static_cast<int>(top) / 23, static_cast<int>(right) / 23,
                                                  static_cast<int>(bottom) / 23, x, y, reach, damage, type, source);
}

void Missiles::pushAround(float x, float y, float reach, float full, float force, const ServerObject* source)
{
    // (mapPushUnitsAround) The units in the square of `reach` about it and the
    // missiles in its cells (getUnitsInRectAdv, by the grid), each that can be moved
    // (isObjectMovable: not DEAD, DESTROYED, NO_COLLIDE or ALLOW_OVERLAP, not
    // IMMOBILE) in a clear ray (unitPushAroundFn) pushed as it's found; all but a missile join the motion.
    (void)source; // (not spared: the exe's callback doesn't ask)
    const float most = (X87(force) * 10.0f).f();
    const float outer = full <= reach ? reach : full;
    world.unitsInRectAdv((X87(x) - reach).f(), (X87(y) - reach).f(), (X87(reach) + x).f(), (X87(reach) + y).f(), [&](ServerObject& o) {
        if ((o.flags & 0x8068u) || o.pendingDelete || (o.classes & 0x400000u) || !world.traceRay(x, y, o.x, o.y, 0)) {
            return;
        }
        // (unitPushAroundFn: x's difference stored, y's kept for its square; the distance + 0.1
        // on the stack against the reach, then stored; the force and force / mass stored.)
        const float dx = (X87(o.x) - x).f();
        const X87 dy53 = X87(o.y) - y;
        const float dy = dy53.f();
        const X87 d53 = util::sqrt(dy53 * dy + X87(dx) * dx) + 0.1f;
        const float d = d53.f();
        if (d53 <= X87(outer)) {
            const float f = full < d ? ((X87(1.0f) - (X87(d) - full) / (X87(outer) - full)) * most).f() : most;
            const float perMass = (X87(f) / o.mass).f();
            o.forceX = (X87(perMass) * dx / d + o.forceX).f();
            o.forceY = (X87(perMass) * dy / d + o.forceY).f();
            if (!(o.classes & oc::Missile)) {
                queueMotion(world, o); // (unitHasCollideOrUpdateFn)
            }
        }
    });
}

ServerObject* Missiles::seekTarget(float x, float y, const ServerObject& missile, float range, const ServerObject* owner)
{
    // (over the units in the square: creatures, not dead or gone, enemy to the
    // missile, within range, in its sight and its owner's; the nearest.)
    ServerObject* best = nullptr;
    float bestD2 = 1e8f;
    const float range2 = (X87(range) * range).f(); // (each stored: the reach squared, the box)
    world.unitsInRect((X87(x) - range).f(), (X87(y) - range).f(), (X87(x) + range).f(), (X87(y) + range).f(), [&](ServerObject& o) { // (spellFlySearchTarget: getUnitsInRect, by the grid)
        if (&o == &missile || !(o.classes & (oc::Monster | oc::Player)) || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) ||
            o.pendingDelete || o.holder != 0) { // (0x8020: ours' 0x20 pendingDelete)
            return;
        }
        if (owner && (&o == owner || !isEnemy(world, *owner, o))) {
            return;
        }
        const X87 dx = X87(x) - o.x;
        const X87 dy = X87(y) - o.y;
        const float d2 = (dx * dx + dy * dy).f(); // (stored)
        if (range2 < d2 || bestD2 <= d2) {
            return;
        }
        // (unitCanInteractWith: the ray, flags 9; the invisible and the blind
        // (unitCanSee) and the bodies in the way: not yet)
        if (!world.traceRay(missile.x, missile.y, o.x, o.y, Trace::Doors | Trace::NearWindows) ||
            (owner && !world.traceRay(owner->x, owner->y, o.x, o.y, Trace::Doors | Trace::NearWindows))) {
            return;
        }
        best = &o;
        bestD2 = d2;
    });
    return best;
}

void Missiles::magicMissileUpdate(ServerObject& m)
{
    // MagicMissileUpdate: its owner gone or 3 s out, it bursts.
    ServerObject* owner = world.byExtent(m.owner);
    if (!owner || owner->gone() || 30u * 3u < frame - m.born) {
        collide(m, nullptr, 0, 0);
        m.pendingDelete = true;
        return;
    }
    if (m.seekTarget != 0) {
        const ServerObject* t = world.byExtent(m.seekTarget);
        if (!t || (t->flags & (ObjFlag::Destroyed | ObjFlag::Dead))) {
            m.seekTarget = 0;
        }
    }
    // No target: one is sought every 8th frame (spellFlySearchTarget about the missile, a
    // hostile spell's: the enemy under its owner's cursor first, else the nearest enemy).
    if (m.seekTarget == 0 && (frame & 7) == 0) {
        const ServerObject* t = searchTarget(world, nullptr, &m, 0x20, 600.0f, 0, owner);
        m.seekTarget = t && t->extent != m.owner ? t->extent : 0;
    }
    if (const ServerObject* t = m.seekTarget ? world.byExtent(m.seekTarget) : nullptr) {
        // A 42nd of a turn (of 256) a frame, towards whichever side it's on (the cross product
        // on the stack).
        const auto [fx, fy] = directionVector(m.direction & 0xFF);
        const int turn = X87(0.0f) <= (X87(t->y) - m.y) * fx - (X87(t->x) - m.x) * fy ? 0x2A : -0x2A;
        m.direction = static_cast<std::uint16_t>((m.direction + turn) & 0xFF);
    }
    pushAlong(m, m.direction & 0xFF);
    m.drag = 0.8f;
}

void Missiles::harpoonUpdate(ServerObject& m)
{
    // HarpoonUpdate.
    ServerObject* owner = m.owner ? world.byExtent(m.owner) : nullptr;
    if (!owner || !owner->player) {
        return;
    }
    PlayerControl& p = *owner->player;
    if (owner->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
        releaseHarpoon(world, *owner, true);
        return;
    }
    // Homing: it lets go when its target dies; not holding one yet, it picks one near the cursor.
    if (m.seekTarget != 0) {
        const ServerObject* t = world.byExtent(m.seekTarget);
        if (!t || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            releaseHarpoon(world, *owner, true);
            return;
        }
    } else if (p.harpoonTarget == 0) {
        // The nearest enemy to the cursor within MaxHarpoonDistance, if ahead of the bolt.
        const ServerObject* t = seekTarget(p.cursorX, p.cursorY, m, world.number(kMaxHarpoonDistance), owner);
        if (t && !(facingSides(m.x, m.y, m.direction, t->x, t->y) & 1)) {
            t = nullptr;
        }
        m.seekTarget = t ? t->extent : 0;
    }
    // In flight with a target: its velocity aimed straight at it.
    if (m.seekTarget != 0 && p.harpoonTarget == 0) {
        if (const ServerObject* t = world.byExtent(m.seekTarget)) {
            // (Each difference stored in the velocity; utilNormalizeVector; then each times the
            // speed, stored. At the target's very centre the game's 0 / 0 gives NaNs: OB-239; ours
            // keeps the velocity.)
            float dx = (X87(t->x) - m.x).f();
            float dy = (X87(t->y) - m.y).f();
            if (dx != 0 || dy != 0) {
                util::normalizeVector(dx, dy);
                m.vx = (X87(m.speedCur) * dx).f();
                m.vy = (X87(m.speedCur) * dy).f();
            }
        }
    }
    // In flight it snaps past MaxHarpoonFlightDistance. Holding one: it snaps when too far, too old,
    // stuck, out of sight or the target's gone; pulled in nearer than MinHarpoonDistance, it's let go.
    const X87 dist = edgeDistance53(m, *owner); // (calcDistance: its stack value compared at once)
    if (p.harpoonTarget == 0) {
        if (X87(world.number(kMaxHarpoonFlightDistance)) < dist) {
            releaseHarpoon(world, *owner, true);
            return;
        }
    } else {
        ServerObject* t = world.byExtent(p.harpoonTarget);
        if (X87(world.number(kMaxHarpoonDistance)) < dist) {
            releaseHarpoon(world, *owner, true);
            return;
        }
        if (dist < X87(world.number(kMinHarpoonDistance))) {
            releaseHarpoon(world, *owner, false);
            return;
        }
        if (world.number(kMaxHarpoonExistence) < static_cast<float>(frame - p.harpoonThrown)) {
            releaseHarpoon(world, *owner, true);
            return;
        }
        if (!t) {
            releaseHarpoon(world, *owner, true);
            return;
        }
        if (30u < frame - p.harpoonChecked) { // (stuck: not moved a unit in a second)
            p.harpoonChecked = frame;
            const X87 mx = X87(p.harpoonTargetX) - t->x;
            const X87 my = X87(p.harpoonTargetY) - t->y;
            if (mx * mx + my * my < X87(1.0f)) { // (on the stack)
                releaseHarpoon(world, *owner, true);
                return;
            }
            p.harpoonTargetX = t->x;
            p.harpoonTargetY = t->y;
        }
        if (!world.traceRay(owner->x, owner->y, t->x, t->y, Trace::Doors | Trace::NearWindows) ||
            (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            releaseHarpoon(world, *owner, true);
            return;
        }
        // It rides its target.
        m.forceX = m.forceY = 0;
        m.vx = m.vy = 0;
        m.x = m.newX = m.frameStartX = t->x;
        m.y = m.newY = m.frameStartY = t->y;
        m.direction = t->direction;
    }
    if (p.harpoonThrown == 0) {
        // The rope (MSG_FX_DURATION_SPELL 7: the warrior and the bolt; unset buffer if either's
        // missing, as the break's: an original bug).
        world.broadcasts().push_back({{0x9E, 7, 0, lo(owner->extent), hi(owner->extent), lo(m.extent), hi(m.extent)}, true, 0});
        p.harpoonThrown = frame;
    }
}

void Missiles::harpoonHits(ServerObject& m, ServerObject* other)
{
    // HarpoonCollide.
    ServerObject* owner = m.owner ? world.byExtent(m.owner) : nullptr;
    if (!owner || !owner->player) {
        m.pendingDelete = true;
        return;
    }
    PlayerControl& p = *owner->player;
    if (other) {
        if ((other->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || other == owner) {
            return;
        }
        Fight fight{world, rng, frame, &things, modifiers};
        const int took = fight.damage(*other, fight.parentPlayer(&m), m.extent, static_cast<int>(world.number(kHarpoonDamage)), 0xB);
        // It holds an enemy, or (gameplay flag 1) any creature, when the blow landed.
        if (took != 0 &&
            (isEnemy(world, *owner, *other) || ((world.gameplayFlags & 1u) && (other->classes & (oc::Monster | oc::Player))))) {
            p.harpoonTarget = other->extent;
            p.harpoonTargetX = other->x;
            p.harpoonTargetY = other->y;
            p.harpoonChecked = frame;
            m.flags |= ObjFlag::NoCollide;
            if (other->player) {
                other->player->assistBy = owner->extent;
                other->player->assistFrame = frame;
            }
            playSound(world, 999, *owner);
            return;
        }
        defaultDamageSound(world, *other, &m);
    }
    // A wall: its blow to the wall under it (HarpoonDamage, type 11). Let go.
    if (!other) {
        cellBlow(m, m.newX, m.newY, static_cast<int>(world.number(kHarpoonDamage)), 0xB);
    }
    p.harpoonTarget = 0;
    endAbility(world, *owner, 3);
    m.pendingDelete = true;
    p.harpoonBolt = 0;
}

void Missiles::boom(ServerObject& m, ServerObject* other, float nx, float ny)
{
    // BoomCollide, with gamedata's MagicMissile numbers.
    static const things::GameDataKey kDamageKey{"MagicMissileDamage"}, kSplashKey{"MagicMissileSplashDamage"},
        kRangeKey{"MagicMissileRange"}, kPushRangeKey{"MagicMissilePushRange"}, kForceKey{"MagicMissileForce"};
    const int kDamage = static_cast<int>(world.number(kDamageKey));
    const int kSplash = static_cast<int>(world.number(kSplashKey));
    float kRange = world.number(kRangeKey);
    float kPushRange = world.number(kPushRangeKey);
    if (m.modSize && m.modSize->burstScale != 1.0f) {
        // (noxcpp: a mode's burst scale, ServerObject::ModSize; none for the game's.)
        kRange = (X87(kRange) * m.modSize->burstScale).f();
        kPushRange = (X87(kPushRange) * m.modSize->burstScale).f();
    }
    const float kForce = world.number(kForceKey);
    const auto fx = [&] {
        const auto ix = static_cast<std::uint32_t>(std::clamp(m.x, 0.0f, 65535.0f));
        const auto iy = static_cast<std::uint32_t>(std::clamp(m.y, 0.0f, 65535.0f));
        world.broadcasts().push_back({{0x86, lo(ix), hi(ix), lo(iy), hi(iy)}, false, 0}); // MSG_FX_LESSER_EXPLOSION
    };
    // (A quest game's team-mates: not hit.)
    if (other && (world.gameFlags & 0x1000u)) {
        const ServerObject* p = shooterPlayer(m);
        if (p && p->player && other->player && !isEnemy(world, *p, *other)) {
            return;
        }
    }
    fx();
    if (!other) {
        if (nx != 0 || ny != 0) {
            // Off a wall at half speed, its heading the new way; its blow (MagicMissileDamage,
            // type 7) to the wall.
            reflectOffWall(m, nx, ny);
            m.direction = static_cast<std::uint16_t>(directionOf(m.vx, m.vy));
            m.vx *= 0.5f;
            m.vy *= 0.5f;
            wallBlow(m, kDamage, 7);
            return;
        }
    } else {
        // A player's worn inversion turns it (checkInversionEffect), and its
        // reflective shield met in front (changeOwner), 0x7A at the player.
        if (other->player && inverts(world, modifiers, *other)) {
            turnSpellMissile(world, m, *other, frame);
            return;
        }
        if (other->player && enchanted(*other, Enchant::ReflectiveShield) &&
            (facingSides(other->x, other->y, other->direction, m.x, m.y) & 1)) {
            turnSpellMissile(world, m, *other, frame);
            playSound(world, 0x7A, *other);
            return;
        }
        Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent, kDamage, 7);
        makeScorch(world, things, other->x, other->y, 0, rng, frame); // (sMakeScorch)
    }
    damageAround(m.x, m.y, kRange, 5.0f, kSplash, 7, &m, nullptr);
    pushAround(m.x, m.y, kPushRange, kPushRange, kForce, &m);
    playSound(world, 0x54, m);
    m.pendingDelete = true;
}

void Missiles::starShot(ServerObject& m, ServerObject* other, float nx, float ny)
{
    if (!other) {
        if (nx != 0 || ny != 0) { // a wall: it glances off
            reflectOffWall(m, nx, ny);
        }
        return;
    }
    if (m.team != 0 && m.team == other->team) {
        return;
    }
    Fight fight{world, rng, frame, &things, modifiers};
    if (fight.damage(*other, shooterPlayer(m), m.extent, m.collideArg, 11) != 0) {
        m.pendingDelete = true;
    }
}

namespace {

/// Turns its heading (Direction2) `turn` of 256 towards (vx, vy).
void steer(ServerObject& m, float vx, float vy, int turn)
{
    const auto [fx, fy] = directionVector(m.direction);
    int d = m.direction;
    // The cross product's sign tells which side of the heading (vx, vy) lies (on the stack).
    if (X87(fx) * vy - X87(fy) * vx < X87(0.0f)) {
        d -= turn;
        while (d < 0) {
            d += 0x100;
        }
    } else {
        d += turn;
        while (0x100 <= d) {
            d -= 0x100;
        }
    }
    m.direction = static_cast<std::uint16_t>(d);
}

} // namespace

bool projectileWallNormal(const World& world, const RayHit& hit, float fromX, float fromY, float& nx, float& ny)
{
    // The side of the cell its move set out from, by the cell's diagonals (the offsets chopped: 0
    // west, 1 north, 2 east, 3 south); the quarter of the cell the ray met it in (1: east and
    // north halves, 2: west and north, 4: east and south, 8: west and south; the hit's whole
    // units within the cell against 11.5).
    const int ix = util::floatToInt(X87(fromX) - static_cast<float>(hit.cellX * 23));
    const int iy = util::floatToInt(X87(fromY) - static_cast<float>(hit.cellY * 23));
    const bool pastSlash = 22 - ix <= iy;
    const int side = iy < ix ? (pastSlash ? 2 : 1) : (pastSlash ? 3 : 0);
    const auto withinCell = [](float v) { return static_cast<float>((util::floatToInt(X87(v)) % 23) & 0xFF); };
    const float qx = withinCell(hit.x);
    const float qy = withinCell(hit.y);
    std::uint8_t quarter = 0;
    if (11.5f <= qx) {
        quarter = 11.5f <= qy ? 4 : 1;
    }
    if (qx <= 11.5f) {
        quarter |= 11.5f <= qy ? 8 : 2;
    }
    constexpr float P = kShortRootHalf;
    constexpr float N = -kShortRootHalf;
    const auto set = [&](float x, float y) {
        nx = x;
        ny = y;
        return true;
    };
    // The X's arms as the table picks them from each side (the other shapes share them).
    const auto west = [&] { return set(N, (quarter & 2) ? P : N); };
    const auto north = [&] { return (quarter & 1) ? set(N, N) : set(P, N); };
    const auto east = [&] { return set(P, (quarter & 1) ? P : N); };
    const auto south = [&] { return (quarter & 4) ? set(N, P) : set(P, P); };
    switch (world.wallShape(hit.cellX, hit.cellY, 0x40)) {
    case 0:
        return side <= 1 ? set(N, N) : set(P, P);
    case 1:
        return side == 1 || side == 2 ? set(P, N) : set(N, P);
    case 2:
        return side == 0 ? west() : side == 1 ? north() : side == 2 ? east() : south();
    case 3:
        return side == 0 ? west() : side == 1 ? north() : set(P, P);
    case 4:
        return side == 1 ? north() : side == 2 ? east() : set(N, P);
    case 5:
        return side == 2 ? east() : side == 3 ? south() : set(N, N);
    case 6:
        return side == 0 ? west() : side == 3 ? south() : set(P, N);
    case 7:
        return side == 1 ? north() : (quarter & 1) ? set(P, P) : set(N, P);
    case 8:
        return side == 2 ? east() : set(N, (quarter & 1) ? N : P);
    case 9:
        return side == 3 ? south() : (quarter & 4) ? set(P, N) : set(N, N);
    case 10:
        return side == 0 ? west() : (quarter & 2) ? set(P, N) : set(P, P);
    default:
        return false;
    }
}

ServerObject* Missiles::pixieTarget(const ServerObject& m)
{
    // The pixie's prey: each one in reach tested in turn.
    const float range = world.quest() ? 640.0f : 250.0f;
    ServerObject* best = nullptr;
    float bestD = 1e7f;
    const float range2 = (X87(range) * range).f();
    world.unitsInCircle(m.x, m.y, range, [&](ServerObject& o) { // (unitsGetInCircle, by the grid)
        // (Its circle's test: on the stack against the radius squared, stored.)
        const X87 cx = X87(m.x) - o.x;
        const X87 cy = X87(m.y) - o.y;
        if (&o == &m || o.holder != 0 || !(cx * cx + cy * cy < X87(range2))) { // (no 0x20 test)
            return;
        }
        if (!(o.classes & (oc::Monster | oc::Player | oc::MonsterGenerator)) || !isEnemy(world, m, o) || (o.flags & ObjFlag::Dead) ||
            !canInteract(world, m, o)) {
            return;
        }
        // (x's difference stored, y's kept for its square; the length stored, + 0.001 stored; a
        // villain's third on the stack against the nearest, kept as a float.)
        const float dx = (X87(o.x) - m.x).f();
        const X87 dy = X87(o.y) - m.y;
        const float len = util::sqrt(dy * dy.f() + X87(dx) * dx).f();
        X87 d = X87((X87(len) + 0.001f).f());
        if (enchanted(o, Enchant::Villain)) {
            d = d * 0.33333334f;
        }
        if (d < X87(bestD)) {
            bestD = d.f();
            best = &o;
        }
    });
    return best;
}

void Missiles::pixieUpdate(ServerObject& m)
{
    // updatePixie.
    static const things::GameDataKey kReturnTimeout{"PixieReturnTimeout"};
    const std::uint32_t giveUp = static_cast<std::uint32_t>(world.number(kReturnTimeout)) * 30u;  // seconds to frames
    if (m.expiresAt != 0 && m.expiresAt < frame) {
        m.pendingDelete = true;
        return;
    }
    ServerObject* target = m.seekTarget ? world.byExtent(m.seekTarget) : nullptr;
    if (target && (target->flags & (ObjFlag::Destroyed | ObjFlag::Dead))) {
        target = nullptr;
    }
    if (m.flags & ObjFlag::Enabled) { // a fresh look for prey 4 times a second
        if (30u / 4u < frame - m.timer) {
            target = pixieTarget(m);
            if (target && target->extent == m.owner) {
                target = nullptr;
            }
            m.timer = frame;
        }
    } else {
        target = nullptr;
    }
    ServerObject* owner = m.owner ? world.byExtent(m.owner) : nullptr;
    // A frozen-out (NO_UPDATE) player's pixies seek nothing.
    if (owner && owner->player && (owner->flags & 2u)) {
        target = nullptr;
    }
    m.seekTarget = target ? target->extent : 0;
    // Prey: turned 0x20 of 256 a frame towards it.
    // (Each way to steer by: the differences stored.)
    const auto towards = [&](float x, float y, int turn) { steer(m, (X87(x) - m.x).f(), (X87(y) - m.y).f(), turn); };
    if (target) {
        towards(target->x, target->y, 0x20);
    } else {
        // Its fellows with the same owner within 200 draw it in; then home to its
        // owner (or where it was made) while it sees it.
        world.missilesInCircle(m.x, m.y, 200.0f, [&](ServerObject& o) { // (getMissilesInCircle, by the grid)
            // (Its circle's test: on the stack against 200 squared.)
            const X87 cx = X87(m.x) - o.x;
            const X87 cy = X87(m.y) - o.y;
            if (&o != &m && (o.classes & oc::Missile) && o.typeName == NameId::Pixie && o.owner == m.owner && // (no 0x20 test)
                cx * cx + cy * cy <= X87(40000.0f)) {
                towards(o.x, o.y, 0x10);
            }
        });
        const float hx = owner ? owner->x : m.homeX;
        const float hy = owner ? owner->y : m.homeY;
        if (world.traceRay(m.x, m.y, hx, hy, Trace::Doors | Trace::NearWindows)) {
            towards(hx, hy, 0x19);
        }
    }
    // Its push along its heading, at its speed; drag 0.9.
    pushAlong(m, m.direction);
    m.drag = 0.9f;
    // Every other 8 frames: out of its owner's sight too long, it's brought back to it
    // (teleportPixie).
    if ((frame & 8u) && owner) {
        if (bodiesClear(world, m, *owner)) {
            m.seenOwnerAt = frame;
        }
        if (giveUp < frame - m.seenOwnerAt) {
            teleportPixie(world, m, *owner); // (its PrevPos too)
            m.touch();
            m.seenOwnerAt = frame;
        }
    }
}

void Missiles::pixieHits(ServerObject& m, ServerObject* other, float nx, float ny)
{
    // collidePixie.
    if (other) {
        const ServerObject* owner = m.owner ? world.byExtent(m.owner) : nullptr;
        if (isEnemy(world, m, *other) && (other->classes & (oc::Monster | oc::Player | oc::MonsterGenerator)) &&
            !(other->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && (!owner || !owner->player || !(owner->flags & 2u))) {
            // A player's worn inversion (checkInversionEffect), or a reflective
            // shield it meets in front (0x7A at it), makes it the player's (changeOwner).
            const auto turned = [&] {
                if ((m.subclasses & 2u) && m.castBy != 0x73 && m.seekTarget == other->extent) {
                    m.seekTarget = m.owner;
                    setOwner(world, other, m);
                    m.born = frame;
                    playSound(world, 0x40, *other); // (changeOwner's)
                }
            };
            if (other->player && inverts(world, modifiers, *other)) {
                turned();
                return;
            }
            if (other->player && enchanted(*other, Enchant::ReflectiveShield) &&
                (facingSides(other->x, other->y, other->direction, m.x, m.y) & 1)) {
                turned();
                playSound(world, 0x7A, *other);
                return;
            }
            Fight{world, rng, frame, &things, modifiers}.damage(*other, shooterPlayer(m), m.extent, m.collideArg, 0xB);
            playSound(world, 0x60, m);
            m.pendingDelete = true;
        }
        return;
    }
    if (nx == 0 && ny == 0) {
        m.pendingDelete = true;
        return;
    }
    // Off a wall: its heading the new way, a step on along it, and its blow (its damage, type
    // 11) to the wall under where that leaves it.
    reflectOffWall(m, nx, ny);
    m.direction = static_cast<std::uint16_t>(directionOf(m.vx, m.vy));
    m.newX = (X87(m.vx) + m.newX).f();
    // (y's sum is stored but its cell is taken from the sum still on the stack.)
    const X87 y = X87(m.vy) + m.newY;
    m.newY = y.f();
    Walls{world, things, rng, frame}.damage(wallCellOf(m.newX), wallCellOf(y), m.collideArg, 0xB, &m);
}

void Missiles::deathBallUpdate(ServerObject& m)
{
    // updateDeathBall.
    const std::uint32_t age = frame - m.born;
    // (Not on every 10th frame.)
    if (frame % 10u != 0) {
        // Two in reach of each other with nothing between: both go, a green bolt
        // between them (MSG 0x98). (Their scorches: not yet.)
        static const things::GameDataKey kCancelRange{"DeathBallCancelRange"};
        const float reach = world.number(kCancelRange);
        bool met = false;
        world.missilesInCircle(m.x, m.y, reach, [&](ServerObject& o) { // (getMissilesInCircle, by the grid)
            // (getMissilesInCircle's test: on the stack against the reach squared, stored.)
            const X87 dx = X87(m.x) - o.x;
            const X87 dy = X87(m.y) - o.y;
            if (&o == &m || o.typeName != NameId::DeathBall || !(o.classes & oc::Missile) || o.holder != 0 ||
                X87((X87(reach) * reach).f()) < dx * dx + dy * dy ||
                !bodiesClear(world, m, o)) {
                return;
            }
            met = true;
            if (!o.pendingDelete && !(o.flags & ObjFlag::Destroyed)) {
                const auto x0 = static_cast<std::uint16_t>(static_cast<int>(m.x));
                const auto y0 = static_cast<std::uint16_t>(static_cast<int>(m.y));
                const auto x1 = static_cast<std::uint16_t>(static_cast<int>(o.x));
                const auto y1 = static_cast<std::uint16_t>(static_cast<int>(o.y));
                // MSG 0x98: the two balls' points (16 bits each), then 10 and 0.
                world.broadcasts().push_back({{0x98, lo(x0), hi(x0), lo(y0), hi(y0), lo(x1), hi(x1), lo(y1), hi(y1), 10, 0}, false, 0});
                o.pendingDelete = true;
            }
        });
        if (met) {
            m.pendingDelete = true;
        }
    }
    if (10u < age) { // the harm about it: DeathBallNearbyDamage (crush) out to DeathBallOutRadius
        static const things::GameDataKey kOut{"DeathBallOutRadius"}, kIn{"DeathBallInRadius"}, kNearby{"DeathBallNearbyDamage"};
        damageAround(m.x, m.y, world.number(kOut), world.number(kIn), static_cast<int>(world.number(kNearby)), 2, &m, nullptr);
    }
    if (30u * 4u < age) {
        m.pendingDelete = true;
    }
}

void Missiles::wallBlow(ServerObject& m, int amount, int type)
{
    if (wallCell) {
        Walls{world, things, rng, frame}.damage(wallCell->first, wallCell->second, amount, type, &m);
    }
}

void Missiles::cellBlow(ServerObject& m, float x, float y, int amount, int type)
{
    Walls{world, things, rng, frame}.damage(wallCellOf(x), wallCellOf(y), amount, type, &m);
}

void Missiles::spellFlyUpdate(ServerObject& m)
{
    // spellFlyUpdate: Targeted / UnTargetedSpellLifetime frames, then MSG_FX 0x96
    // and cast on the nearest creature within 50, from its owner, and gone.
    // (Its owner and first caster forgotten once gone; it flies on.)
    ServerObject* owner = m.owner ? world.byExtent(m.owner) : nullptr;
    if (owner && owner->gone()) {
        owner = nullptr;
    }
    if (const ServerObject* first = m.flyCaster ? world.byExtent(m.flyCaster) : nullptr; !first || first->gone()) {
        m.flyCaster = 0;
    }
    const auto lifetime = static_cast<std::uint32_t>(world.number(m.seekTarget != 0 ? "TargetedSpellLifetime" : "UnTargetedSpellLifetime"));
    if (lifetime < frame - m.born) {
        pointFx(world, 0x96, m.x, m.y);
        ServerObject* first = m.flyCaster ? world.byExtent(m.flyCaster) : nullptr;
        if (ServerObject* t = nearestCreature(world, m, 50.0f); t && owner && first) {
            Spells{world, things, modifiers, rng, frame}.cast(m.castBy, *owner, t, t->x, t->y, m.value, owner, first);
        }
        m.flags |= ObjFlag::Destroyed;
        m.pendingDelete = true;
        return;
    }
    if (const ServerObject* t = m.seekTarget ? world.byExtent(m.seekTarget) : nullptr; !t || (t->flags & (ObjFlag::Destroyed | ObjFlag::Dead))) {
        m.seekTarget = 0;
    }
    // No target: one sought four times a second (and at once), for its spell's flags, by its owner.
    if (m.seekTarget == 0 && (30u / 4u < frame - m.timer || m.timer == m.born)) {
        const things::SpellDef* def = things.findSpell(m.castBy);
        const ServerObject* t = searchTarget(world, nullptr, &m, def ? def->flags : 0, 600.0f, 0, owner);
        m.seekTarget = t ? t->extent : 0;
        m.timer = frame;
    }
    // Pushed at its speed towards its target, else along its way; drag 0.9.
    // (The length + 0.1, a double, stored to a float and back; x's difference stored, y's kept
    // for its square.)
    float dx = m.vx;
    float dy = m.vy;
    X87 dy53 = X87(m.vy);
    if (const ServerObject* t = m.seekTarget ? world.byExtent(m.seekTarget) : nullptr) {
        dx = (X87(t->x) - m.x).f();
        dy53 = X87(t->y) - m.y;
        dy = dy53.f();
    }
    const float d = (util::sqrt(dy53 * dy + X87(dx) * dx) + X87::fromDouble(static_cast<double>(0.1f))).f();
    m.drag = 0.9f;
    m.forceX = (X87(dx) * m.speedCur / d).f();
    m.forceY = (X87(dy) * m.speedCur / d).f();
}

void Missiles::spellFlyHits(ServerObject& m, ServerObject* other, float nx, float ny)
{
    // spellFlyCollide.
    if (!other) {
        if (nx != 0 || ny != 0) {
            reflectOffWall(m, nx, ny); // (collideReflect)
        }
        return;
    }
    if (other->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
        return;
    }
    if (other->player) {
        PlayerControl& p = *other->player;
        const bool front = facingSides(other->x, other->y, other->direction, m.x, m.y) & 1;
        if (p.state == 0x10 && front) { // a raised shield: back it goes, the shield's (0x36E at it)
            playSound(world, 0x36E, *other);
            reflectMissile(m, *other);
            turnSpellMissile(world, m, *other, frame);
            return;
        }
        if (p.state == 0xD && (p.weaponEquip & 0x400u) && front) { // a great sword's parry (0x37A at it)
            playSound(world, 0x37A, *other);
            reflectMissile(m, *other);
            turnSpellMissile(world, m, *other, frame);
            p.previousState = p.state;
            p.state = static_cast<std::uint8_t>(nox::util::uniform_int_distribution<int>(0x12, 0x14)(rng));
            p.stateFrame = frame;
            p.animFrame = 0; // (the exe its animation's last frame: the next update's step replaces it)
            // (No return: the exe goes on to the checks below, and a projectile turned is no
            // longer after it, so it's reflected again, back the way it came:
            // an original bug.)
        }
        if (inverts(world, modifiers, *other)) { // (a worn inversion, checkInversionEffect)
            turnSpellMissile(world, m, *other, frame);
            return;
        }
        if (enchanted(*other, Enchant::ReflectiveShield) && front) { // made the shield's (0x7A at it)
            turnSpellMissile(world, m, *other, frame);
            playSound(world, 0x7A, *other);
            return;
        }
    }
    if (m.seekTarget != other->extent) {
        reflectMissile(m, *other); // (off whatever isn't its target)
        return;
    }
    // Its target: the spell cast on it from the projectile, by its first caster and its owner.
    // (The exe passes the target alone where the spell's point follows: what's after it on the
    // stack; its place here.)
    ServerObject* owner = m.owner ? world.byExtent(m.owner) : nullptr;
    ServerObject* first = m.flyCaster ? world.byExtent(m.flyCaster) : nullptr;
    if (owner && first) {
        Spells{world, things, modifiers, rng, frame}.cast(m.castBy, *owner, other, other->x, other->y, m.value, &m, first);
    }
    m.flags |= ObjFlag::Destroyed;
    m.pendingDelete = true;
}

void Missiles::fly()
{
    // The units update's second walk (updateUnits): the missiles' list, newest first, each not
    // DESTROYED nor NO_UPDATE (0x22): its update, its move, what it meets, then back in the grid
    // where it is (moveUpdateSpecial: one deleted meanwhile only off it). (Owned or
    // not: one whose shooter died flies on and runs out as any, ProjectileUpdate having no owner
    // check; the stock maps place no missiles.)
    world.grid().eachMissile([&](ServerObject& m) {
        if (m.gone() || m.pendingDelete || m.holder != 0 || (m.flags & ObjFlag::NoUpdate)) {
            return;
        }
        // (Its box: the move's objectUnkUpdateCoords after its Update, about where
        // its move sets out from (flyOne's move); the link makes none for a missile. One deleted
        // before its move isn't linked again, its box unread.)
        flyOne(&m);
        world.gridRelink(m, m.x, m.y, &flyBox_);
    });
}

void Missiles::flyOne(ServerObject* m)
{
    {
        // Its UPDATE. (One it deletes still makes its move below, meeting nothing: updateUnits tests
        // 0x22 only before it, the trace skips 0x60.)
        if (m->updateFn == NameId::PixieUpdate) {
            pixieUpdate(*m);
        } else if (m->updateFn == NameId::DeathBallUpdate) {
            deathBallUpdate(*m);
        } else if (m->updateFn == NameId::ChakramInMotionUpdate && m->chakram) {
            chakramUpdate(*m);
        } else if (m->updateFn == NameId::MagicMissileUpdate) {
            magicMissileUpdate(*m);
        } else if (m->updateFn == NameId::HarpoonUpdate) {
            harpoonUpdate(*m);
        } else if (m->updateFn == NameId::SpellProjectileUpdate) {
            spellFlyUpdate(*m);
        } else if (m->updateFn == NameId::DeathBallFragmentUpdate) {
            // updateDeathBallFragment: past 10 frames, 20 crush within 25 of it a
            // frame (mapDamageUnitsAround, from it); gone past 2 s.
            const std::uint32_t age = frame - m->born;
            if (10u < age) {
                damageAround(m->x, m->y, 25.0f, 0.0f, 20, 2, m, nullptr);
            }
            if (30u * 2u < age) {
                m->pendingDelete = true;
            }
        } else if (m->updateFn == NameId::HomingProjectileUpdate) {
            // (HomingProjectileUpdate: nothing; it flies till it meets something, no ProjectileUpdate timeout.)
        } else if (40u < frame - m->born) {
            // ProjectileUpdate: 40 frames, then its collide with nothing, and gone.
            collide(*m, nullptr, 0, 0);
            m->pendingDelete = true;
        }
        // Its move (PosVec its NewPos, where the move sets out from): its velocity, its
        // push added and at its drag; its box about here.
        flyBox_ = World::collideBoxAt(*m, m->x, m->y);
        // (The units update's move for a missile: x's velocity stored but its sum kept on the
        // stack for the new place; y's new place from the stored velocity.)
        const X87 vx = (X87(m->forceX) + m->vx) * m->drag;
        m->vx = vx.f();
        m->vy = ((X87(m->forceY) + m->vy) * m->drag).f();
        m->newX = (vx + m->x).f();
        m->newY = (X87(m->vy) + m->y).f();
        // What it meets on the way (not DESTROYED or NO_COLLIDE, 0x60;
        // projectileTraceHit).
        if (!(m->flags & 0x60u) && !m->pendingDelete) {
            // (x's difference stored, y's kept for its square; the length squared on the stack.)
            const float dx = (X87(m->newX) - m->x).f();
            const X87 dy53 = X87(m->newY) - m->y;
            const float dy = dy53.f();
            const X87 len2 = dy53 * dy + X87(dx) * dx;
            ServerObject* body = nullptr;
            float fromX = m->x; // (the piece's start: what it met is told where it came from)
            float fromY = m->y;
            // A move longer than 6 is searched in equal pieces of at most 6 (36 is 6 squared); the
            // first piece that meets a body ends the search.
            if (X87(36.0f) < len2) {
                // (The square root at 53 bits, Math_DoubleToInt; each piece's step stored.)
                const int pieces = static_cast<int>(util::sqrt(len2 * (1.0f / 36.0f)).d()) + 1;
                const float stepX = (X87(dx) / X87::fromDouble(pieces)).f();
                const float stepY = (X87(dy) / X87::fromDouble(pieces)).f();
                for (int i = 0; i < pieces; ++i) {
                    const float px = (X87(fromX) + stepX).f();
                    const float py = (X87(fromY) + stepY).f();
                    if ((body = bodyAt(*m, fromX, fromY, px, py))) {
                        break;
                    }
                    fromX = px;
                    fromY = py;
                }
            } else {
                body = bodyAt(*m, m->x, m->y, m->newX, m->newY);
            }
            RayHit wall;
            bool walled = false;
            float wallNx = 0;
            float wallNy = 0;
            if (!world.traceRay(m->x, m->y, m->newX, m->newY, Trace::Doors | Trace::ThroughWindows, &wall)) {
                // (projectileTraceHit) A wall in the way: its cell kept for the COLLIDE's blow to
                // it, its normal from the table, the move undone (NewPos = Pos) even when the
                // table has no normal (then it's as if no wall were met).
                wallCell = std::pair{wall.cellX, wall.cellY};
                walled = projectileWallNormal(world, wall, m->x, m->y, wallNx, wallNy);
                m->newX = m->x;
                m->newY = m->y;
            }
            if (body && walled) {
                // (Both squares on the stack.)
                const X87 bx = X87(m->x) - body->x;
                const X87 by = X87(m->y) - body->y;
                const X87 wx = X87(m->x) - wall.x;
                const X87 wy = X87(m->y) - wall.y;
                if (!(by * by + bx * bx < wy * wy + wx * wx)) {
                    body = nullptr; // (the wall's nearer)
                }
            }
            if (body) {
                // Its COLLIDE, then the body's with the way reversed; a fist meets
                // nothing at all.
                const std::string_view t = body->typeName;
                if (t != "SmallFist" && t != "MediumFist" && t != "LargeFist") {
                    // The normal handed to its COLLIDE: from the body towards where this piece set out.
                    const float nx = (X87(fromX) - body->x).f();
                    const float ny = (X87(fromY) - body->y).f();
                    collide(*m, body, nx, ny);
                    if (!body->gone()) {
                        Fight fight{world, rng, frame, &things, modifiers};
                        collideFunction(fight, *body, m);
                    }
                }
            } else if (walled) {
                collide(*m, nullptr, wallNx, wallNy);
            }
            wallCell.reset();
        }
        // PrevPos its PosVec as the collide left it (the move's start, or where a
        // chakram's turn back put it), PosVec its end, the push spent: deleted on the way or
        // not.
        m->lastX = m->x;
        m->lastY = m->y;
        m->x = m->frameStartX = m->newX;
        m->y = m->frameStartY = m->newY;
        m->forceX = m->forceY = 0;
        if (!m->pendingDelete) {
            m->touch();
        }
    }
}

void Missiles::arrowHits(ServerObject& arrow, ServerObject* other)
{
    // ArrowCollide, with its weapon record (modifier.bin, by the arrow's own type): without one it
    // does nothing at all (no blow, not ended), first of all.
    const things::EquipDef* def = modifiers ? modifiers->findWeapon(arrow.typeName) : nullptr;
    if (!def) {
        return;
    }
    ServerObject* owner = world.byExtent(arrow.owner);
    if (owner && owner == other) {
        return;
    }
    const int strength = owner ? strengthOf(owner) : 30;
    if (!other) {
        // A wall: its blow (its damage, type 11) to the wall the ray met; gone either way. (The
        // figure chopped by __ftol as it comes back on the stack.)
        wallBlow(arrow, util::ftol(boltDamage53(world, def, strength)), 0xB);
        arrow.pendingDelete = true;
        return;
    }
    // A quest's player's arrow passes a player not its enemy.
    if (world.quest() && (other->classes & oc::Player)) {
        const ServerObject* p = shooterPlayer(arrow);
        if (p && (p->classes & oc::Player) && !isEnemy(world, *p, *other)) {
            return;
        }
    }
    if (other->flags & ObjFlag::Dead) {
        return;
    }
    float damage = boltDamage53(world, def, strength).f();
    // Its modifiers' (its quiver's, and its bow's recoil) ATTACKEFFECTs and ATTACKPREHITEFFECTs
    // (itemApplyAttackEffect, playerPreAttackEffects: item_effects.hpp).
    Fight fight{world, rng, frame, &things, modifiers};
    attackEffects(fight, arrow, owner, damage);
    preHitEffects(fight, *other, owner, &arrow);
    const int took = fight.damage(*other, owner, arrow.extent, roundedDamage(damage), 11);
    if (arrow.typeName == "ArcherBolt") {
        if (other->maxHealth != 0 && other->health == 0) {
            return; // (a bolt flies on through the dead)
        }
    } else if (took == 0) {
        return;
    }
    arrow.pendingDelete = true;
}

} // namespace nox::server
