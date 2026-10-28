// The COLLIDE functions: what happens when two things touch (or a thing meets a wall or a
// harming floor). The motion (physics.cpp) collects the frame's touches; dispatchContacts
// runs each thing's COLLIDE function by name (doors, triggers, exits, chests, traps...).
#include "noxcore/server/collide.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/books.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/walls.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
using util::X87;

bool creature(const ServerObject& o) { return (o.classes & (oc::Monster | oc::Player)) != 0; }

/// collide: a touch's effects of `a` on `b`.
void touchEffects(Fight& fight, ServerObject& a, ServerObject& b)
{
    if (enchanted(a, Enchant::Shock) && !(b.flags & (ObjFlag::Dead | ObjFlag::AllowOverlap)) && creature(b) && isEnemy(fight.world, a, b)) {
        const int power = enchantPower(a, Enchant::Shock);
        playSound(fight.world, 0x87, a);
        disenchant(fight.world, a, Enchant::Shock);
        fight.damage(b, &a, a.extent, static_cast<int>(fight.world.number("ShockDamage", static_cast<std::size_t>(power - 1))), 9);
    }
    if ((b.classes & 0x20006u) && !(b.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && !(a.team != 0 && a.team == b.team)) {  // a creature or a generator
        disenchant(fight.world, a, Enchant::Invisible);
    }
    if ((a.classes & oc::Player) && (b.classes & 0x20000u) && !(b.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {  // a generator
        disenchant(fight.world, a, Enchant::Invisible);
    }
}

/// PlayerCollide.
void playerCollide(Fight& fight, ServerObject& obj, ServerObject* other)
{
    World& world = fight.world;
    // Berserker Charge (ability 1) under way: it stops against anything solid.
    if (abilityActive(world, obj, 1)) {
        bool through = false;
        if (other) {
            // It passes through a dead creature or a lighter non-creature that can move, an unlocked
            // door, a BELOW / ALLOW_OVERLAP thing or a missile.
            const bool deadCreature = creature(*other) && other->maxHealth != 0 && other->health == 0;
            if ((!creature(*other) || deadCreature) && !(other->classes & 0x400000u) && !(other->flags & 0x80u) && other->mass <= obj.mass) {  // not IMMOBILE, not flagged MISSILE_HIT (0x80)
                through = true;
            } else if (other->classes & oc::Door) {
                through = other->lock == 0;
            } else if ((other->flags & (ObjFlag::Below | ObjFlag::AllowOverlap)) || (other->classes & oc::Missile)) {
                through = true;
            }
        }
        if (!through) {
            fight.setPlayerState(obj, 0xD);
            // The ground shakes (earthquakeSend, 10): within 300, 10 at the centre, fading to 0 at
            // the edge.
            jiggle(world, obj.x, obj.y, 10);
            endAbility(world, obj, 1);
            bool stunned = false;
            if (!other) {
                // A wall: unless it gives (a wall whose WALL flags lack 1 and 4).
                const WallCell* w = obj.player && obj.player->lastWallX >= 0 ? world.wall(obj.player->lastWallX, obj.player->lastWallY) : nullptr;
                const auto& defs = world.wallDefFlags();
                if (!w || (w->material < defs.size() && (defs[w->material] & 5u))) {
                    stunned = true;
                    playSound(world, 0xAB, obj); // the crash
                    // Its blow to the wall under it: 100, crush.
                    if (fight.things) {
                        // (Math_FloatToInt of NewPos x 1/23: stored, then truncated.)
                        Walls{world, *fight.things, fight.rng, fight.frame}.damage(
                            wallCellOf(obj.newX), wallCellOf(obj.newY), 100, 2, &obj);
                    }
                }
            } else {
                Fight f = fight;
                if (!(other->classes & 0x400000u)) {
                    // An elastic knock: the velocities trade by mass. On the x87 as the exe has it:
                    // the sum and 2 x m2 / sum stored, the other ratios kept on the stack.
                    const float m1 = obj.mass;
                    const float m2 = other->mass;
                    const float sum = (X87(m2) + m1).f();
                    const X87 keep1 = (X87(m1) - m2) / sum;
                    const float give1 = ((X87(m2) + m2) / sum).f();
                    const X87 keep2 = (X87(m2) - m1) / sum;
                    const X87 give2 = (X87(m1) + m1) / sum;
                    const float v1x = obj.vx, v1y = obj.vy, v2x = other->vx, v2y = other->vy;
                    obj.vx = (X87(v1x) * keep1 + X87(give1) * v2x).f();
                    obj.vy = (X87(v1y) * keep1 + X87(give1) * v2y).f();
                    other->vy = (keep2 * v2y + give2 * v1y).f();
                    other->vx = (keep2 * v2x + give2 * v1x).f();
                }
                f.damage(*other, fight.parentPlayer(&obj), obj.extent, static_cast<int>(world.number("BerserkerDamage")), 2);
                if (!(other->classes & 0x20006u)) {  // a creature or a generator doesn't stun it
                    stunned = true;
                }
            }
            if (stunned) {
                enchant(world, obj, Enchant::Held, static_cast<int>(world.number("BerserkerStunDuration")), 5);
                // (The ratio times the health on the x87, stored, truncated.)
                int pain = util::floatToInt(X87(world.number("BerserkerPainRatio")) * X87::fromDouble(obj.health));
                if (pain < 1) {
                    pain = 1;
                }
                fight.takeHealth(obj, pain);
            }
            // Back where it was (Obj_SetPosition at PrevPos: the frame's start).
            obj.x = obj.newX = obj.frameStartX;
            obj.y = obj.newY = obj.frameStartY;
            fight.world.gridRelink(obj, obj.x, obj.y); // (moveUpdateSpecial)
            obj.touch();
        }
    }
    // DEATH passes on by touch when it has less than 14 s left.
    if (other && (other->classes & oc::Player) && !(other->flags & ObjFlag::Dead) && enchanted(obj, Enchant::Death) &&
        enchantTime(obj, Enchant::Death) < 30 * 14) {
        enchant(world, *other, Enchant::Death, 30 * 15, enchantPower(obj, Enchant::Death));
        disenchant(world, obj, Enchant::Death);
    }
}

/// "objcoll.c:..." to a player (SrvNet_SendInformString).
void informString(World& world, const ServerObject& to, std::string_view id, std::uint8_t sign = 0)
{
    // (nothing for an empty id or one past 48 characters)
    if (!to.player || id.empty() || 48 < id.size()) {
        return;
    }
    // {0xA9, 0x0F, sign, the string id..., 0}
    world.broadcasts().push_back({informStringBytes(id, sign), true, to.extent});
}

/// A key used up (deleted, delayedDeleteObject): out of its holder's inventory,
/// its client told (MSG_REPORT_DROP).
void useUpKey(World& world, ServerObject& key)
{
    if (ServerObject* holder = key.holder ? world.byExtent(key.holder) : nullptr) {
        if (holder->player) {
            world.broadcasts().push_back({{0x4D, static_cast<std::uint8_t>(key.extent), static_cast<std::uint8_t>(key.extent >> 8),  // MSG_REPORT_DROP: u16 item, u16 type
                                           static_cast<std::uint8_t>(key.type), static_cast<std::uint8_t>(key.type >> 8)},
                                          true,
                                          holder->extent});
        }
        std::erase(holder->inventory, key.extent);
        key.holder = 0;
    }
    key.flags |= ObjFlag::Destroyed;
    key.pendingDelete = true;
}

/// DoorCollide (collideDoor): something touching a shut door.
void doorCollide(Fight& fight, ServerObject& door, ServerObject* other)
{
    World& world = fight.world;
    if (!other || door.doorDirection != door.doorClosed) {
        return;
    }
    // One "locked" notice for everyone at a time: none within 1501 ms (45 frames) of the last.
    // With it, the lock's sound at the door: 0xF0 a door's, 0xF4 a gate's.
    const auto lockedSound = [&] { playSound(world, (door.subclasses & 4u) ? 0xF4u : 0xF0u, door); };
    const auto notice = [&](std::string_view doorLine, std::string_view gateLine) {
        if (world.lockNoticeAt && fight.frame - *world.lockNoticeAt <= 45u) {
            return;
        }
        lockedSound();
        informString(world, *other, (door.subclasses & 4u) ? gateLine : doorLine);
        world.lockNoticeAt = fight.frame;
    };
    // A magic lock (the Lock spell: its owner, till its timer): only its owner passes.
    if (door.owner != 0) {
        if (door.timer <= fight.frame) {
            door.owner = 0;
        } else if (door.owner != other->extent) {
            notice("objcoll.c:DoorLockedMagic", "objcoll.c:GateLockedMagic");
            return;
        }
    }
    if (door.lock == 0) {
        return;
    }
    if (door.lock == 5) {
        notice("objcoll.c:DoorLockedMechanism", "objcoll.c:GateLockedMechanism");
        return;
    }
    // A key lock, 1-4 (doorGetSomeKey): the toucher's key of its kind, none while
    // the door is magically locked (its owner's too: no shared key either, the function returns
    // first); a player in a quest with the warp window shown and none of its own for
    // a silver lock, the shared one.
    static constexpr std::string_view kKeys[5] = {"", "SilverKey", "GoldKey", "RubyKey", "SapphireKey"};
    ServerObject* key = nullptr;
    if (door.lock <= 4 && door.owner == 0) {
        for (const std::uint32_t e : other->inventory) {
            ServerObject* i = world.byExtent(e);
            if (i && (i->classes & oc::Key) && i->typeName == kKeys[door.lock]) {
                key = i;
                break;
            }
        }
    }
    if (!key && door.owner == 0 && other->player && world.quest() && world.questWarpGui && door.lock == 1) {
        key = questSharedSilverKey(world);
    }
    if (!key && door.lock <= 4 && door.owner == 0 && world.modeHooks) {
        key = world.modeHooks->sharedKey(fight, door, *other, kKeys[door.lock]); // (noxcpp: a mode's shared key)
    }
    if (!key) {
        // MSG_GAUNTLET 0x21, a formatted notice: the line, then the lock's kind.
        if (!(world.lockNoticeAt && fight.frame - *world.lockNoticeAt <= 45u)) {
            lockedSound();
            if (other->player) {
                const std::string_view id = (door.subclasses & 4u) ? "objcoll.c:GateLockedKey" : "objcoll.c:DoorLockedKey";
                // 0x34 bytes: 0xF0 0x21, the line's id (zero-padded), the lock's kind in the last byte.
                std::vector<std::uint8_t> m(0x34, 0);
                m[0] = 0xF0;
                m[1] = 0x21;
                std::copy(id.begin(), id.end(), m.begin() + 2);
                m[0x33] = door.lock;
                world.broadcasts().push_back({std::move(m), true, other->extent});
            }
            world.lockNoticeAt = fight.frame;
        }
        return;
    }
    // Unlocked, with its pair (the door in the next cell along its shut direction whose collision
    // box strictly overlaps the square of 34 about its own cell, getUnitsInRect with
    // fnFindCloseDoors). In a quest first its marker cleared
    // and the warp window's clock from now, the pair's marker too;
    // a shared key's holder told ("GeneralPrint:KeyShared1").
    door.lock = 0;
    if (world.quest()) {
        questOpenDoor(world, door);
        world.questWarpState = static_cast<int>(fight.frame);
    }
    if (const auto cell = world.doorCell(door.extent)) {
        const auto [cx, cy] = *cell;
        std::optional<std::pair<int, int>> pair;
        // Its pair's cell: one diagonal step along its shut direction.
        switch (door.doorClosed) {
        case 0: pair = {cx - 1, cy - 1}; break;
        case 8: pair = {cx + 1, cy - 1}; break;
        case 0x10: pair = {cx + 1, cy + 1}; break;
        case 0x18: pair = {cx - 1, cy + 1}; break;
        default: break; // (the exe's cell is left unset here: no door matches it)
        }
        const float left = static_cast<float>(cx * 23) - 34.0f;
        const float top = static_cast<float>(cy * 23) - 34.0f;
        const float right = static_cast<float>(cx * 23) + 34.0f;
        const float bottom = static_cast<float>(cy * 23) + 34.0f;
        world.unitsInRect(left, top, right, bottom, [&](ServerObject& o) { // (by the grid)
            if (pair && (o.classes & oc::Door) && world.doorCell(o.extent) == pair) { // (no 0x20 test)
                o.lock = 0;
                if (world.quest()) {
                    questOpenDoor(world, o);
                }
            }
        });
    }
    playSound(world, 0xEA, door); // the unlock
    if (const ServerObject* holder = key->holder ? world.byExtent(key->holder) : nullptr;
        holder && holder != other && holder->player && world.quest() && world.questWarpGui) {
        informString(world, *holder, "GeneralPrint:KeyShared1");
    }
    useUpKey(world, *key);
}

/// collideTrigger: a body with mass, of the classes and team it takes, touches
/// it (unless its deciding handler returns 0); off, it forgets.
void triggerCollide(Fight& fight, ServerObject& obj, ServerObject* other)
{
    if (!obj.trigger) {
        obj.trigger.emplace();
    }
    auto& t = *obj.trigger;
    if (!obj.enabled()) {
        t.toucher = 0;
        t.flags &= ~1u;
        return;
    }
    // Ignored: in state 5, a wall, a body without mass, or not of the classes / team it takes.
    if (t.state == 5 || !other || !(0 < other->mass) || (t.allowedClasses != 0 && !(other->classes & t.allowedClasses)) ||
        (t.ignoredClasses != 0 && (other->classes & t.ignoredClasses)) || (t.allowedTeam != 0 && other->team != t.allowedTeam) ||
        (t.ignoredTeam != 0 && other->team == t.ignoredTeam)) {
        return;
    }
    if (fight.world.script && fight.world.script->resolves(t.collided)) { // (its function index not -1)
        // (A once-only deciding handler that has run gives the exe a NULL it reads through:
        // an original bug. Here it doesn't count as pressed.)
        const auto r = fight.world.script->callBlock(t.collided, other->extent, obj.extent);
        if (!r || *r == 0) {
            return;
        }
    }
    t.toucher = other->extent;
    t.flags |= 1u;
}

/// collideTrapDoor: shut, the first step on it (not a door's; not a player
/// treading lightly) runs its handler and sets it to open after its delay; open, what fits the
/// opening and stands over it falls in (IN_HOLE and FALLING: the fall draws it to the pit and
/// lets it out at its landing place).
void trapDoorCollide(Fight& fight, ServerObject& obj, ServerObject* other)
{
    if (!other || (other->classes & oc::Door)) {
        return;
    }
    if (!obj.pit) {
        obj.pit.emplace();
    }
    auto& p = *obj.pit;
    if (!obj.enabled()) {
        if (!p.sprung && !((other->classes & oc::Player) && abilityActive(fight.world, *other, 4))) { // (4: Tread Lightly)
            if (p.delay != 0) {
                p.opensAt = p.delay + fight.frame;
            }
            if (fight.world.script) {
                fight.world.script->callBlock(p.stepped, other->extent, obj.extent);
            }
            p.sprung = true;
        }
        return;
    }
    // Only what fits inside the opening falls in.
    if (other->shape == ShapeKind::Box) {
        if (obj.boxWidth < other->boxWidth || obj.boxHeight < other->boxHeight) {
            return;
        }
    } else if (other->shape == ShapeKind::Circle) {
        const float across = other->radius + other->radius;
        if (obj.boxWidth < across || obj.boxHeight < across) {
            return;
        }
    }
    if (insideBox(obj, obj.x, obj.y, other->x, other->y)) {
        other->flags |= ObjFlag::InHole | ObjFlag::Falling;
        other->fallToX = p.fallX;
        other->fallToY = p.fallY;
        other->homeX = obj.x;
        other->homeY = obj.y;
    }
}

/// chest: what a chest holds, out towards the one who opened it. Three spots
/// on its side (by its facing, subclass 0x100-0x800; none: the opener's side), its size + 19
/// out and 30 to either side, taken farthest from the opener first; one a wall cuts off is at
/// the opener's feet. A lone item goes to the first, several in turn round the three; not
/// those of weight 255 nor creatures (left for the spiral after: dropAllItems).
void chestDrop(Fight& fight, ServerObject& chest, const ServerObject& opener)
{
    World& world = fight.world;
    if (!fight.things || chest.inventory.empty()) {
        return;
    }
    // (On the x87 as the exe has it: each step stored, but where noted.)
    float dx = (X87(opener.x) - chest.x).f();
    float dy = (X87(opener.y) - chest.y).f();
    // Its facing by subclass CHEST_NW / NE / SE / SW (0x100-0x800).
    if (chest.subclasses & 0x100u) {
        dx = -1, dy = -1;
    } else if (chest.subclasses & 0x200u) {
        dx = 1, dy = -1;
    } else if (chest.subclasses & 0x400u) {
        dx = 1, dy = 1;
    } else if (chest.subclasses & 0x800u) {
        dx = -1, dy = 1;
    }
    util::normalizeVector(dx, dy); // (utilNormalizeVector)
    // (a circle's radius, a box's larger side halved)
    const float size = chest.shape == ShapeKind::Circle ? chest.radius
                       : chest.shape == ShapeKind::Box  ? std::max(chest.boxWidth, chest.boxHeight) * 0.5f
                                                        : 0.0f;
    const X87 r = X87(size) + 4.0f + 15.0f; // (kept on the stack)
    const float cx = (r * dx + chest.x).f();
    const float cy = (r * dy + chest.y).f();
    struct Spot {
        float x, y;
    };
    // How far from the opener, squared, on the stack (the y offset's square first).
    const auto dist = [&](const Spot& s) {
        const X87 ox = X87(s.x) - opener.x;
        const X87 oy = X87(s.y) - opener.y;
        return oy * oy + ox * ox;
    };
    // Three spots: straight out, and 30 to either side; sorted farthest from the opener first
    // (the first's and the third's distances stored, the second's kept on the stack).
    Spot a{cx, cy};
    Spot b{(X87(-dy) * 30.0f + cx).f(), (X87(dx) * 30.0f + cy).f()};
    Spot c{(X87(dy) * 30.0f + cx).f(), (X87(-dx) * 30.0f + cy).f()};
    float ad = dist(a).f();
    X87 bd = dist(b);
    const float cd = dist(c).f();
    if (X87(ad) < bd) {
        std::swap(a, b);
        const float was = ad;
        ad = bd.f();
        bd = X87(was);
    }
    if (bd < X87(cd)) {
        std::swap(b, c);
        bd = X87(cd);
    }
    if (X87(ad) < bd) {
        std::swap(a, b);
    }
    std::array<Spot, 3> spots{a, b, c};
    // A spot a wall cuts off becomes the opener's feet.
    for (Spot& s : spots) {
        if (!world.traceRay(chest.x, chest.y, s.x, s.y, Trace::Doors)) {
            s.x = opener.x;
            s.y = opener.y;
        }
    }
    Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    const auto held = chest.inventory;
    const bool lone = held.size() == 1;
    std::size_t next = 0;
    for (const std::uint32_t e : held) {
        ServerObject* item = world.byExtent(e);
        if (!item || item->weight == 0xFF || (item->classes & oc::Monster)) {
            if (lone) {
                return;
            }
            continue;
        }
        item->flags |= ObjFlag::NoCollide;
        items.drop(chest, *item, spots[next].x, spots[next].y);
        if (lone) {
            return;
        }
        next = (next + 1) % 3;  // round the three spots in turn
    }
}

/// collideChest: a player touching a shut chest opens it (its DIE: SpawnObjectDie,
/// dead), and what it holds comes out (chestDrop, then the spiral). A quest's chests (subclass
/// 0xF00) want a silver key, used up; without one, "objcoll.c:ChestLockedSilver" (the doors'
/// one notice timer).
void chestCollide(Fight& fight, ServerObject& chest, ServerObject* other)
{
    World& world = fight.world;
    if (!other || !(other->classes & oc::Player) || (chest.flags & ObjFlag::Dead)) {
        return;
    }
    if (world.quest() && (chest.subclasses & 0xF00u)) {
        ServerObject* key = nullptr;
        for (const std::uint32_t e : other->inventory) {
            ServerObject* i = world.byExtent(e);
            if (i && (i->classes & oc::Key) && i->typeName == "SilverKey") {
                key = i;
                break;
            }
        }
        if (!key) {
            if (!(world.lockNoticeAt && fight.frame - *world.lockNoticeAt <= 45u)) {
                playSound(world, 0x3F4, chest);
                informString(world, *other, "objcoll.c:ChestLockedSilver");
                world.lockNoticeAt = fight.frame;
            }
            return;
        }
        useUpKey(world, *key);
        playSound(world, 0xEA, chest);
    }
    objectDies(fight, chest);
    chestDrop(fight, chest, *other);
    if (fight.things) {
        Items{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.dropAll(chest);
    }
}

/// PickupCollide.
void pickupCollide(Fight& fight, ServerObject& obj, ServerObject* other)
{
    if (!other || (other->classes & oc::Monster) || fight.frame - obj.born < 15u) {  // half a second old
        return;
    }
    if (other->player) {
        return; // (a player only with its pick-up flag: not yet)
    }
    // (Anything else: Obj_TryPickup takes only players and monsters, and monsters are left out
    // here; theirs is PICKUP_OBJECT's and the main AI's.)
}

/// collideExit in a network game: a live player touching an exit (not in a dialog;
/// a warp gate, subclass 2, only while the quest's gates are on; in a quest not one already out
/// or waiting) with a map (a quest's: any) goes there, everyone with it (the host loads that map:
/// World::exitTo); a wizard's glyphs on the ground go first; a quest's (quest.hpp); a solo game's
/// asks the WORKING save, the map after it (Server's tick end).
void exitCollide(Fight& fight, ServerObject& exit, ServerObject* other)
{
    World& world = fight.world;
    if (!other || !other->player) {
        return;
    }
    if (world.quest() && (other->player->exitGate != 0 || other->player->warpGate != 0)) {
        return;
    }
    // Not a warp gate while they're off; no save asked for nor waiting; the player
    // free to go (not NoUpdate nor dead...); not paused; somewhere to go.
    if (((exit.subclasses & 2u) && !world.warpGatesOn) || saveBusy(world) || !mayAct(world, *other, fight.frame) ||
        (world.gameFlags & GameFlag::Paused) || ((exit.exitMap.empty() || exit.exitMap[0] == '\0') && !world.quest())) { // (a C string)
        return;
    }
    if (world.modeHooks && !world.modeHooks->exitTouched(fight, exit, *other)) {
        return; // (noxcpp: a mode holding the exit for now)
    }
    if (other->player->laysGlyphs()) {
        for (auto& [extent, o] : world.objects()) {
            if (o.owner == other->extent && o.typeName == "Glyph" && o.holder == 0 && !o.pendingDelete) {
                o.pendingDelete = true;
                if (other->player->curTraps != 0) {
                    --other->player->curTraps;
                }
            }
        }
    }
    world.cameByExit = true;
    if (world.gameFlags & GameFlag::Solo) {
        // A solo game: the game saved first ("WORKING", the exit's), the map changed after it
        // (Server's tick end: solo.hpp).
        world.saveName = "WORKING";
        requestSave(world, true, exit.extent, 0, fight.frame);
        return;
    }
    if (world.quest()) {
        questExitCollide(fight, exit, *other);
        return;
    }
    world.exitTo = exit.exitMap; // (no sound on this path; the quests' 0x3EB comes with them)
}

/// Its COLLIDE function (the thing's, by name).
void collideOf(Fight& fight, ServerObject& obj, ServerObject* other)
{
    if (obj.player) {
        playerCollide(fight, obj, other);
        return;
    }
    const NameId f = obj.collideFn.id(); // (by the name's id)
    switch (f) {
    case NameId::PickupCollide:
        pickupCollide(fight, obj, other);
        break;
    case NameId::FistCollide:
        // FistCollide: on the ground, its damage (type 2) from its caster.
        if (obj.z <= 0 && other) {
            fight.damage(*other, fight.parentPlayer(&obj), obj.extent, obj.value, 2);
        }
        break;
    case NameId::TeleportWakeCollide: {
        // collideTeleportWake: creatures, food, staves, weapons and armour
        // (0x3001016) follow where it went (in a quest only a player's wake takes them).
        ServerObject* owner = obj.owner ? fight.world.byExtent(obj.owner) : nullptr;
        if (other && !enchanted(*other, Enchant::Anchored) && (!fight.world.quest() || !owner || owner->player) &&
            (other->classes & 0x3001016u)) {
            if (!enchanted(*other, Enchant::Invisible)) {
                pointFx(fight.world, 0x8A, other->x, other->y);
            }
            playSound(fight.world, 0x93, *other);
            teleport(fight.world, *other, obj.destX, obj.destY, fight.frame);
            if (!enchanted(*other, Enchant::Invisible)) {
                pointFx(fight.world, 0x89, other->x, other->y);
            }
            playSound(fight.world, 0x93, *other);
        }
        break;
    }
    case NameId::SoulGateCollide:
        questSoulGateCollide(fight, obj, other);
        break;
    case NameId::AnkhCollide:
        questAnkhCollide(fight, obj, other); // (collideAnkhQuest)
        break;
    case NameId::ManaDrainCollide:
        // collideManadrain (blue flames): a player with mana loses its amount
        // (playerManaSub, not below 0), and its hiss (0xE4) at most every half
        // second: the frame kept in a u16 and compared sign-extended, so past frame 0x8000 it
        // hisses every frame (an original bug, kept).
        if (other && (other->classes & oc::Player) && other->player && other->player->mana != 0) {
            other->player->mana = static_cast<std::uint16_t>(std::max(0, other->player->mana - obj.collideArg));
            const auto since = fight.frame - static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(obj.soundFrame)));
            if (15u < since) { // (gameFPS >> 1)
                playSound(fight.world, 0xE4, obj);
                obj.soundFrame = static_cast<std::uint16_t>(fight.frame);
            }
        }
        break;
    case NameId::ChestCollide:
        chestCollide(fight, obj, other);
        break;
    case NameId::ExitCollide:
        exitCollide(fight, obj, other);
        break;
    case NameId::FlagCollide:
        flagCollide(fight, obj, other); // (modes.hpp)
        break;
    case NameId::BallCollide:
        ballCollide(fight, obj, other);
        break;
    case NameId::HomeBaseCollide:
        homeBaseCollide(fight, obj, other);
        break;
    case NameId::CrownCollide:
        crownCollide(fight, obj, other);
        break;
    case NameId::MonsterCollide:
    case NameId::MimicCollide:
        // collideMimic: an enemy creature (not dead) touching a mimic, unless it's
        // fighting already under its top (monsterIsActionScheduled), has it under
        // attack (now) and fighting where that is.
        if (obj.collideFn == NameId::MimicCollide && other && obj.monster && !(other->flags & ObjFlag::Dead) && (other->classes & (oc::Player | oc::Monster)) &&
            isEnemy(fight.world, obj, *other)) {
            const auto& m = *obj.monster;
            bool fighting = false;
            // Is FIGHT anywhere on its stack?
            for (int i = m.top - 1; 0 <= i; --i) {
                fighting = fighting || m.stack[static_cast<std::size_t>(i)].action == MonsterAction::Fight;
            }
            if (!fighting) {
                Monsters ai{fight};
                if (auto* it = ai.push(obj, Dependency::UnderAttack)) {
                    it->args[0] = fight.frame;
                }
                ai.pushFight(obj, *other, fight.frame);
            }
        }
        // collideMonsterEventProc: its collision script (caller what touched it).
        if (obj.monster && fight.world.script) {
            fight.world.script->callBlock(obj.monster->onCollision, other ? other->extent : 0, obj.extent);
        }
        break;
    case NameId::BombCollide: {
        // collideBomb: (in the campaign, not while its first player's held still)
        // its collision script; then a creature (not dead) not of its team sets it off: its
        // glyph's target, and it dies (999).
        World& world = fight.world;
        if (world.gameFlags & GameFlag::Solo) {
            // In the campaign: nothing while the first player is frozen (NoUpdate).
            for (const auto& [e, p] : world.objects()) {
                if (p.player) {
                    if (p.flags & ObjFlag::NoUpdate) {
                        return;
                    }
                    break;
                }
            }
        }
        if (obj.monster && world.script) {
            world.script->callBlock(obj.monster->onCollision, other ? other->extent : 0, obj.extent);
        }
        if (other && obj.monster && (other->classes & (oc::Player | oc::Monster)) && !(obj.team != 0 && obj.team == other->team) &&
            !(other->flags & ObjFlag::Dead)) {
            obj.monster->bombTarget = other->extent;
            fight.takeHealth(obj, 999);
        }
        break;
    }
    case NameId::UndeadKillerCollide:
        // collideUndeadKiller: a wall ends it; an undead monster takes the spell's
        // points (type 6, from its caster's player): what its health is while there's more (on
        // it goes), else what's left (and it's gone); none left, gone.
        if (!other) {
            obj.pendingDelete = true;
        } else if ((other->classes & oc::Monster) && (other->subclasses & 0x40u)) {  // UNDEAD
            auto& list = fight.world.durationSpells();
            const auto it = std::ranges::find_if(list, [&](const DurationSpell& s) { return s.serial == static_cast<std::uint32_t>(obj.value); });
            if (it == list.end()) {
                obj.pendingDelete = true;
                return;
            }
            const int hp = other->health;
            const int points = it->value;
            if (hp < points) {
                fight.damage(*other, fight.parentPlayer(&obj), obj.extent, hp, 6);
                it->value = points - hp;
            } else if (points != 0) {
                fight.damage(*other, fight.parentPlayer(&obj), obj.extent, points, 6);
                obj.pendingDelete = true;
                it->value -= points;
            } else {
                obj.pendingDelete = true;
            }
        }
        break;
    case NameId::MonsterGeneratorCollide:
        generatorTouched(fight, obj, other); // (monsters.hpp)
        break;
    case NameId::SignCollide:
        // collideSign: a player touching it (or working it: the Use hand) has its
        // USE run (ReadUse / WarpReadUse: books.hpp).
        if (other && (other->classes & oc::Player)) {
            readSign(fight.world, fight.frame, *other, obj);
        }
        break;
    case NameId::AwardSpellCollide:
        awardSpellCollide(fight, obj, other); // (collideSpellPedestal: books.hpp)
        break;
    case NameId::AudioEventCollide:
        // AudioEventCollide: a player touching it, at most every second, plays its
        // sound (its CollideData's: a rock rolling, SmallRockMove).
        if (other && (other->classes & oc::Player) && obj.timer + 30u < fight.frame) {
            obj.timer = fight.frame;
            playSound(fight.world, static_cast<std::uint32_t>(obj.collideArg), obj);
        }
        break;
    case NameId::OwnCollide:
        // OwnCollide (the mana crystal): the first player to touch it owns it.
        if (other && (other->classes & oc::Player) && obj.owner != other->extent && obj.owner == 0) {
            obj.timer = fight.frame;
            obj.owner = other->extent;
        }
        break;
    case NameId::BearTrapCollide:
    case NameId::PoisonGasTrapCollide:
        // collideBearTrap / collidePoisonGasTrap: what may set off
        // a trap (a glyph's rule) does. A bear trap shuts (a ClosedBearTrap, its
        // owner's, in its place) and holds it (HELD and ANCHORED, 3 s, power 5); a gas trap
        // leaves a ToxicCloud, its owner's, for ToxicCloudLifetime s. The trap goes.
        if (other && fight.things && Spells{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame}.setsOff(obj, *other)) {
            const bool bear = obj.collideFn == NameId::BearTrapCollide;
            if (ServerObject* made = fight.world.createAt(*fight.things, bear ? "ClosedBearTrap" : "ToxicCloud", obj.x, obj.y, fight.frame)) {
                made->owner = obj.owner;
                obj.pendingDelete = true;
                if (bear) {
                    enchant(fight.world, *other, Enchant::Held, 90, 5);
                    enchant(fight.world, *other, Enchant::Anchored, 90, 5);
                    playSound(fight.world, 0x34E, obj); // (at the trap, gone this frame)
                } else {
                    made->value = util::floatToInt(X87(fight.world.number("ToxicCloudLifetime")) * X87::fromDouble(30)); // (x the frame rate, an int)
                    playSound(fight.world, 0x34F, obj);
                }
            }
        }
        break;
    case NameId::DieCollide:
        // collideDie (the Polyp): a creature not of its side bursts it.
        if (other && !sameSide(fight.world, obj, *other) && creature(*other)) {
            obj.flags |= ObjFlag::Dead;
            objectDies(fight, obj);
        }
        break;
    case NameId::TrapDoorCollide:
        trapDoorCollide(fight, obj, other);
        break;
    case NameId::PentagramCollide:
        // collidePentagram: touched (its update sends what's on it).
        if (!obj.transport) {
            obj.transport.emplace();
        }
        obj.transport->touched = true;
        break;
    case NameId::TriggerCollide:
        triggerCollide(fight, obj, other);
        break;
    case NameId::DoorCollide:
        doorCollide(fight, obj, other);
        break;
    case NameId::GlyphCollide:
        // collideGlyph: what may set it off does. (A glyph never
        // moves into a wall; the exe would read through the null there.)
        if (other && fight.things) {
            Spells spells{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame};
            if (spells.setsOff(obj, *other)) {
                spells.glyphGoesOff(obj, other);
            }
        }
        break;
    case NameId::DamageCollide:
        // DamageCollide: its amount halved a frame (1: every other frame), of its
        // type, from its caster.
        if (other && other->maxHealth != 0) {
            const int amount = obj.collideArg == 1 && (fight.frame & 1u) ? 1 : obj.collideArg >> 1;
            fight.damage(*other, fight.parentPlayer(&obj), obj.extent, amount, obj.collideType);
        }
        break;
    case NameId::ModeCollide: // (noxcpp: a mode's own object, its COLLIDE the mode's)
        if (fight.world.modeHooks) {
            fight.world.modeHooks->collide(fight, obj, other);
        }
        break;
    default:
        break;
    }
    // (DefaultCollide, ElevatorCollide: nothing. The rest: not yet.)
}

} // namespace

void collideFunction(Fight& fight, ServerObject& obj, ServerObject* other)
{
    collideOf(fight, obj, other);
}

void dispatchContacts(Fight& fight, const std::vector<Contact>& contacts)
{
    World& world = fight.world;
    // Each touch: a's COLLIDE and touch effects on b, then b's on a.
    for (const Contact& c : contacts) {
        ServerObject* a = world.byExtent(c.a);
        if (!a || a->gone()) {
            continue;
        }
        ServerObject* b = (c.b == kWallContact || c.b == kFloorContact) ? nullptr : world.byExtent(c.b);
        if (c.b != kFloorContact) {
            collideOf(fight, *a, b);
            if (b) {
                touchEffects(fight, *a, *b);
            }
        }
        // (After the motion's queue was emptied: who's queued here moves next frame.)
        if (c.b == kFloorContact) {
            fight.damage(*a, nullptr, 0, 2, 0xC); // (a harming floor tile: fire)
            queueMotion(world, *a);               // (it goes on meeting the floor)
        } else if (b) {
            if (!b->gone()) {
                collideOf(fight, *b, a);
                touchEffects(fight, *b, *a);
            }
            // A pair with an ALLOW_OVERLAP member (a plate, a spike, a pentagram...): the other
            // goes on meeting it, at rest or not (the first's flag first: both, the second).
            if (a->flags & ObjFlag::AllowOverlap) {
                queueMotion(world, *b);
            } else if (b->flags & ObjFlag::AllowOverlap) {
                queueMotion(world, *a);
            }
        }
    }
}

} // namespace nox::server
