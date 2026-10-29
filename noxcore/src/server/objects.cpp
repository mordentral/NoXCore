// The UPDATE functions of the things that act on their own (objects.hpp): obelisks, triggers and
// levers, pentagrams, elevators, trap doors, shooting traps, steam pipes, sentry globes, movers,
// barrels, and the spells' effects (fists, meteors, clouds...); objects' deaths (their DIE) and
// the fall every object takes after its update. Players', doors' and missiles' updates live in
// their own files.
#include "noxcore/server/objects.hpp"

#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <random>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

// A 16-bit value's low and high bytes, for the messages.
using util::hi;
using util::lo;
using util::X87;

/// unitRaise.
void raise(ServerObject& o, float z)
{
    o.raise(z); // (unitRaise)
}

/// A teleport pentagram's partner: the pad its map load linked (World::linkTransporters: the first
/// TRANSPORTER of the number its record named), by extent, and still a TRANSPORTER. Looked up each
/// time, so a deleted partner is never found.
ServerObject* partnerOf(World& world, const ServerObject& o)
{
    if (!o.transport || o.transport->link == 0) {
        return nullptr;
    }
    ServerObject* p = world.byExtent(o.transport->link);
    return p && (p->classes & oc::Transporter) ? p : nullptr;
}

} // namespace

void jiggle(World& world, float x, float y, int strength)
{
    // (earthquakeSend) Each player whose camera is within 300 (90000 = 300 squared) feels it
    // (0x97), fading with the squared distance.
    for (auto& [extent, o] : world.objects()) {
        if (!o.player) {
            continue;
        }
        // (All on the x87's stack: FMUL 1/90000, FSUBR 1, FIMUL the strength, __ftol.)
        const X87 dx = X87(x) - o.player->camX; // (from its camera)
        const X87 dy = X87(y) - o.player->camY;
        const X87 d2 = dy * dy + dx * dx;
        if (d2 < X87(90000.0f)) {
            const int felt = util::ftol((X87(1.0f) - d2 * std::bit_cast<float>(0x373A69DCu)) * X87::fromDouble(strength));
            world.broadcasts().push_back(
                {{0x97, static_cast<std::uint8_t>(static_cast<std::int8_t>(felt))},
                 false,
                 o.extent});
        }
    }
}

bool Objects::acts(const ServerObject& o)
{
    // (By the name's id: one switch, not a compare a name.)
    switch (o.updateFn.id()) {
    case NameId::FistUpdate: case NameId::MeteorUpdate: case NameId::MeteorShowerUpdate: case NameId::ToxicCloudUpdate:
    case NameId::ExpireUpdate: case NameId::MoonglowUpdate: case NameId::TelekinesisUpdate: case NameId::FlameCleanseUpdate:
    case NameId::GlyphChainUpdate: case NameId::ElevatorUpdate: case NameId::ElevatorShaftUpdate: case NameId::TriggerUpdate:
    case NameId::ToggleUpdate: case NameId::PentagramUpdate: case NameId::InvisiblePentagramUpdate: case NameId::TrapDoorUpdate:
    case NameId::SwitchUpdate: case NameId::LoopAndDamageUpdate: case NameId::ObeliskUpdate: case NameId::BreakUpdate: case NameId::BreakAndRemoveUpdate:
    case NameId::OpenUpdate: case NameId::OneSecondDieUpdate: case NameId::SelfDestructUpdate: case NameId::WaterBarrelUpdate:
    case NameId::BlackPowderBarrelUpdate: case NameId::BlackPowderBurnUpdate: case NameId::LifetimeUpdate:
    case NameId::SmallToxicCloudUpdate: case NameId::SkullUpdate: case NameId::BlowUpdate: case NameId::SentryGlobeUpdate:
    case NameId::MoverUpdate: case NameId::FlagUpdate: case NameId::BallUpdate: case NameId::CrownUpdate:
    case NameId::MonsterGeneratorUpdate: case NameId::ArachnaphobiaUpdate: case NameId::UndeadKillerUpdate:
        return true;
    default:
        return false;
    }
}

int Objects::rechargeRate(const ServerObject* item) const
{
    // getRechargeRate: an Oblivion staff's OblivionStaffRechargeRate, else its
    // enchantments' (positions 2-3) ReplenishmentEffect, else none.
    if (!item) {
        return 0;
    }
    if ((item->classes & oc::Wand) && (item->subclasses & 0x4000000u)) {
        return static_cast<int>(fight.world.number("OblivionStaffRechargeRate"));
    }
    if (fight.modifiers) {
        for (std::size_t i = 2; i < 4; ++i) {
            if (const auto* m = fight.modifiers->modifier(item->modifiers[i])) {
                if (const auto it = m->effects.find("ReplenishmentEffect"); it != m->effects.end()) {
                    return static_cast<int>(it->second);
                }
            }
        }
    }
    return 0;
}

void Objects::obeliskUpdate(ServerObject& o)
{
    // updateObelisk. Its mana (0-50) goes to each live player of its team
    // (any, if it has none) within 50 and clear of it: a staff in hand (charge under 100)
    // recharged by its rate (a network game's: 1; a network game's costs it 1 and lets it
    // refill), and 1 mana a frame (a quest's: the class's whole MaxMana, never spending it).
    // Unused (no player in reach, whether it gave anything or not), it refills 1 every half second
    // to 50. Each player it spent mana on (a staff's, a player's or both), it looks at its mana
    // once, after both: on a multiple of 8, clients are told (MSG_OBELISK_CHARGE, the image they
    // draw it with), and its sound plays if its last was over half a second ago.
    World& world = fight.world;
    const bool quest = world.quest();
    const bool network = (world.gameFlags & GameFlag::Network) != 0;
    const std::uint32_t frame = fight.frame;
    bool idle = true;
    const auto tellCharge = [&] {
        // (unitNeedSync; its light set by the charge too, by a call that does nothing)
        if (o.value % 8 == 0) {
            o.touch();
        }
    };
    std::vector<ServerObject*> scratch;
    for (ServerObject* pp : world.playerObjects(scratch)) {
        ServerObject& p = *pp;
        if (p.gone() || (p.flags & ObjFlag::Dead)) {
            continue;
        }
        if (o.team != 0 && o.team != p.team) {
            continue;
        }
        const X87 dx = X87(o.x) - p.x; // (the squared distance on the x87's stack)
        const X87 dy = X87(o.y) - p.y;
        if (!(dy * dy + dx * dx < X87(2500.0f)) || !bodiesClear(world, o, p)) {  // within 50
            continue;
        }
        idle = false;
        bool spent = false; // (it spent mana on this player: its charge looked at below)
        if (0 < o.value) {
            ServerObject* weapon = p.player->currentWeapon ? world.byExtent(p.player->currentWeapon) : nullptr;
            int rate = rechargeRate(weapon);
            if (network && !quest) {
                rate = 1;
            }
            if (rate != 0 && weapon && (weapon->classes & oc::Wand) && (weapon->subclasses & 0x47F0000u) && weapon->chargePercent < 100) {  // the staffs (the StaffWands subclasses)
                if (!quest) {
                    spent = true; // (a flag of its own: the obelisk stays in use, not refilling)
                    --o.value;
                } else if (frame % 15u == 0) {
                    playSound(world, 0xE6, o); // (every half second)
                }
                // rechargeItem: its charge up, its charges with it.
                weapon->chargePercent = std::min(weapon->chargePercent + rate, 100);
                const auto charges = static_cast<std::uint8_t>(weapon->maxCharges * weapon->chargePercent / 100);
                if (charges != weapon->charges) {
                    weapon->charges = charges;
                    world.broadcasts().push_back({{0x64, static_cast<std::uint8_t>(weapon->extent), static_cast<std::uint8_t>(weapon->extent >> 8),
                                                   weapon->charges, weapon->maxCharges},
                                                  true,
                                                  p.extent}); // MSG_REPORT_CHARGES
                }
            }
        }
        if (0 < o.value && p.player->mana < p.player->maxMana) {
            int add = 1;
            if (quest) {
                // (noxcpp: a multiclass player's, a mode's: a conjurer's)
                const char* name = p.player->multiclass ? "ConjurerMaxMana"
                                   : p.player->classId == 0 ? "WarriorMaxMana"
                                   : p.player->classId == 1 ? "WizardMaxMana"
                                                            : "ConjurerMaxMana";
                add = static_cast<int>(world.number(name));
            }
            p.player->mana = static_cast<std::uint16_t>(std::min(p.player->mana + add, static_cast<int>(p.player->maxMana)));
            if (!quest) {
                --o.value;
                spent = true;
            } else if (frame % 15u == 0) {
                playSound(world, 0xE6, o);
            }
        }
        // A staff's alone (its holder full, or its last mana gone to the staff) is looked at too:
        // a drained obelisk's 0 always reaches the clients.
        if (spent) {
            tellCharge();
            if (15u < frame - o.timer) {
                playSound(world, 0xE6, o);
                o.timer = frame;
            }
        }
    }
    if (!idle) {
        return;
    }
    if (frame % 15u == 0 && o.value < 50) {
        tellCharge();
        ++o.value;
    }
}

void Objects::trapDoorUpdate(ServerObject& o)
{
    // updateTrapDoor: shut (off), at its time it opens (on, xStatus 4: opening;
    // its time 5 s on); open, a fresh one (2) or one done opening (4, at that time) is open (8),
    // and one not opening may be sprung again.
    if (!o.pit) {
        o.pit.emplace();
    }
    auto& p = *o.pit;
    const std::uint32_t frame = fight.frame;
    if (!o.enabled()) {
        if (p.opensAt != 0 && p.opensAt <= frame) {
            setOnOff(o, true);
            unsetXStatus(o, 2);
            setXStatus(o, 4);
            p.opensAt += 30u * 5u;
            playSound(fight.world, 0x36A, o);
        }
        return;
    }
    if (o.xStatus & 2u) {
        unsetXStatus(o, 2);
        setXStatus(o, 8);
        return;
    }
    if (!(o.xStatus & 4u)) {
        p.sprung = false;
    } else if (p.opensAt <= frame) {
        unsetXStatus(o, 4);
        setXStatus(o, 8);
    }
}

void Objects::sendFromPentagram(ServerObject& o, bool shown)
{
    // What's on it (getUnitsInRect: collision boxes strictly over the square of
    // its radius about it; a box's radius is 0), but the IMMOBILE (0x420000), to its partner
    // (teleportToMB); the visible one's with a flash (FX 0x89) at both ends.
    World& world = fight.world;
    const ServerObject* to = partnerOf(world, o);
    if (!to) {
        return;
    }
    const float r = o.radius;
    const float left = (X87(o.x) - r).f(); // (each edge FSTP'd)
    const float top = (X87(o.y) - r).f();
    const float right = (X87(o.x) + r).f();
    const float bottom = (X87(o.y) + r).f();
    const float tx = to->x;
    const float ty = to->y;
    // (By the grid, each sent as it's found: one sent relinks at the partner, the walk going on
    // from its new place, as the exe's.)
    world.unitsInRect(left, top, right, bottom, [&](ServerObject& unit) {
        ServerObject* t = &unit;
        if (t->classes & 0x420000u) { // (no 0x20 test)
            return;
        }
        if (shown) {
            pointFx(world, 0x89, t->x, t->y);
            playSound(world, 0x93, *t);
        }
        teleport(world, *t, tx, ty, fight.frame);
        if (shown) {
            pointFx(world, 0x89, t->x, t->y);
            playSound(world, 0x93, *t);
        }
    });
}

void Objects::pentagramUpdate(ServerObject& o)
{
    // updateTeleportPentagram: touched (and enabled, with a partner) it winds up
    // (its frame 1-8, one a frame), sends what's on it at 8, then winds down (frames 1-8 at a
    // slower pace each cycle, four cycles); its partner meanwhile plays the arrival (state 2).
    if (!o.transport) {
        o.transport.emplace();
    }
    auto& t = *o.transport;
    if (t.state == 0) {
        if (t.frame != 0) {
            o.touch();
        }
        t.frame = 0;
    } else if (t.state < 3) {
        if (t.step == t.cycle) {
            ++t.frame;
            o.touch();
            if (t.frame == 9) {
                t.frame = 1;
                ++t.cycle;
            }
            t.step = 0;
        } else {
            ++t.step;
        }
    }
    ServerObject* partner = partnerOf(fight.world, o);
    if (t.state == 0) {
        if (t.touched && partner && o.enabled()) {
            t.state = 1;
            t.cycle = 0;
            t.step = 0;
            o.timer = fight.frame;
            if (!partner->transport) {
                partner->transport.emplace();
            }
            partner->transport->state = 2;
            partner->transport->cycle = 0;
            partner->transport->step = 0;
            partner->timer = fight.frame;
        }
    } else if (t.state == 1) {
        if (t.cycle == 0) {
            if (t.frame == 8 && partner) {
                sendFromPentagram(o, true);
                t.touched = false;
                return;
            }
        } else if (t.cycle == 4) {
            t.state = 0;
            t.touched = false;
            return;
        }
    } else if (t.state == 2 && 3 < t.cycle) {
        t.frame = 0;
        t.state = 0;
        t.touched = false;
        return;
    }
    t.touched = false;
}

void Objects::invisiblePentagramUpdate(ServerObject& o)
{
    // updateInvisiblePentagram: touched (enabled, with a partner), what's on it
    // goes at once, with no flash.
    if (!o.transport) {
        o.transport.emplace();
    }
    auto& t = *o.transport;
    if (t.touched && partnerOf(fight.world, o) && o.enabled()) {
        sendFromPentagram(o, false);
    }
    t.touched = false;
}

namespace {

/// What the server owes about it (its owed bits): every player for the always-sent kinds
/// (0x20400004: IMMOBILE...), else those it's been sent to.
void owe(const ServerObject& o, std::uint32_t& owed)
{
    owed = (o.classes & 0x20400004u) ? 0xFFFFFFFFu : (owed | o.knownBy);
}

/// The sound its UPDATE line names in word `n` (a trigger's or toggle's pressed 1 and released 2,
/// its two sounds), at it; nothing for "Null" or none.
void updateSound(World& world, const ServerObject& o, int n)
{
    const things::ThingDef* t = world.thingDb ? world.thingDb->findThing(o.typeName) : nullptr;
    const auto u = t ? t->attribute("UPDATE") : std::nullopt;
    if (!u) {
        return;
    }
    std::string_view rest = *u;
    for (int i = 0; i <= n; ++i) {
        const auto start = rest.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            return;
        }
        rest.remove_prefix(start);
        const auto end = std::min(rest.find(' '), rest.size());
        if (i == n) {
            playSound(world, rest.substr(0, end), o);
            return;
        }
        rest.remove_prefix(end);
    }
}

/// elevatorAud: an elevator or its shaft starting up or down, by its material
/// (wood 8, metal 0x10 (subclass 0x20 / 0x40 its own), stone 0x20 (subclass 2 its own)).
void elevatorSound(World& world, const ServerObject& o, bool up)
{
    std::uint32_t id = 0;
    if (o.material == 8) {
        id = up ? 0x101 : 0x102;
    } else if (o.material == 0x10) {
        id = (o.subclasses & 0x20u) ? (up ? 0xFD : 0xFE) : (o.subclasses & 0x40u) ? (up ? 0x103 : 0x104) : (up ? 0xFB : 0xFC);
    } else if (o.material == 0x20) {
        id = (o.subclasses & 2u) ? (up ? 0xFF : 0x100) : (up ? 0xF9 : 0xFA);
    }
    playSound(world, id, o);
}

} // namespace

void markAnimFrame(ServerObject& o, std::uint32_t frame)
{
    // servMarkObjAnimFrame.
    o.touch();
    o.animFrame = frame;
    owe(o, o.animOwed);
}

void setOnOff(ServerObject& o, bool on)
{
    o.touch();
    if (on) {
        o.flags |= ObjFlag::Enabled;
    } else {
        o.flags &= ~ObjFlag::Enabled;
    }
    owe(o, o.enableOwed);
}

void setXStatus(ServerObject& o, std::uint32_t bits)
{
    o.xStatus |= bits;
    if (bits != 1) {
        o.touch();
        owe(o, o.xStatusOwed);
    }
}

void unsetXStatus(ServerObject& o, std::uint32_t bits)
{
    if (bits & o.xStatus) {
        o.xStatus &= ~bits;
        if (bits != 1) {
            o.touch();
            owe(o, o.xStatusOwed);
        }
    }
}

void Objects::triggerUpdate(ServerObject& o)
{
    // updateTrigger: touched, it goes down (frame 1, its pressed handler with
    // the toucher); let go, it comes up again a second later (a plain Trigger or a
    // PressurePlate at once: frame 0, its released handler), or stays spent if once only.
    if (!o.trigger) {
        o.trigger.emplace();
    }
    auto& t = *o.trigger;
    const std::uint32_t frame = fight.frame;
    const std::uint32_t hold = o.typeName == "Trigger" || o.typeName == "PressurePlate" ? 0u : 30u;
    if (!o.enabled()) {
        // Off: this frame's touch (1) and the set-up (8) forgotten.
        t.flags &= ~9u;
        return;
    }
    // Not set up yet (flag 8): it starts at rest.
    if (!(t.flags & 8u)) {
        t.state = 0;
        o.timer = frame;
        markAnimFrame(o, 0);
    }
    if (t.state == 0) {
        if (t.flags & 1u) {
            updateSound(fight.world, o, 1); // (TriggerPressed, ButtonPress...)
            markAnimFrame(o, 1);
            if (fight.world.script) {
                fight.world.script->callBlock(t.pressed, t.toucher, o.extent);
            }
            t.state = 1;
            o.timer = frame + hold;
        }
    } else if (t.state == 1 && o.timer < frame && !(t.flags & 1u)) {
        updateSound(fight.world, o, 2); // (TriggerReleased, ButtonRelease...)
        markAnimFrame(o, 0);
        if (fight.world.script) {
            fight.world.script->callBlock(t.released, 0, o.extent);
        }
        t.state = (t.flags & 2u) ? 5 : 0;
    }
    // This frame's touch (1) becomes last frame's (4), else both are cleared; set up (8) from now.
    t.flags = (t.flags & 1u) ? ((t.flags | 4u) & ~1u) | 8u : (t.flags & ~5u) | 8u;
}

void Objects::toggleUpdate(ServerObject& o)
{
    // updateToggle (levers, switches): a touch a second after the last change
    // throws it (frame 1, its pressed handler); once let go, the next touch throws it back
    // (frame 0, its released handler; spent if once only).
    if (!o.trigger) {
        o.trigger.emplace();
    }
    auto& t = *o.trigger;
    const std::uint32_t frame = fight.frame;
    if (!o.enabled()) {
        t.flags &= ~9u;
        return;
    }
    if (!(t.flags & 8u)) {
        t.state = 0;
        o.timer = frame;
        markAnimFrame(o, 0);
    }
    bool changed = false;
    if (t.state == 0) {
        if (o.timer < frame && (t.flags & 1u)) {
            updateSound(fight.world, o, 1); // (SwitchToggle, LeverToggle)
            markAnimFrame(o, 1);
            if (fight.world.script) {
                fight.world.script->callBlock(t.pressed, t.toucher, o.extent);
            }
            t.state = 3;
            changed = true;
        }
    } else if (t.state == 1) {
        if (o.timer < frame && (t.flags & 1u)) {
            updateSound(fight.world, o, 2);
            markAnimFrame(o, 0);
            if (fight.world.script) {
                fight.world.script->callBlock(t.released, 0, o.extent);
            }
            t.state = (t.flags & 2u) ? 5 : 0;
            changed = true;
        }
    } else if (t.state == 3 && o.timer < frame && !(t.flags & 1u)) {
        t.state = 1;
    }
    if (changed) {
        o.timer = frame + 30u;
    }
    // As the trigger's: the touch kept as last frame's (4), set up (8).
    t.flags = (t.flags & 1u) ? (t.flags & ~1u) | 0xCu : (t.flags & ~5u) | 8u;
}

void Objects::fall(World& world, ServerObject& o, std::uint32_t frame)
{
    // updateFallLogic.
    if (o.flags & ObjFlag::InHole) {
        // Down a shaft: no velocity but a pull of 3 to its middle; past 50 below, out at its
        // elevator, 90 up.
        raise(o, (X87(o.z) + o.zVel).f());
        const float zVel = (X87(o.zVel) - 1.0f).f();
        o.forceX = o.forceY = 0;
        o.vx = o.vy = 0;
        o.zVel = zVel;
        // (On the x87: dx kept on the stack; dy stored, and squared as the stack's value times the
        // stored one; the distance stored, tested on the stack.)
        const X87 dx = X87(o.x) - o.homeX;
        const X87 dy53 = X87(o.y) - o.homeY;
        const float dy = dy53.f();
        const X87 d53 = util::sqrt(dy53 * dy + dx * dx);
        const float d = d53.f();
        if (X87(0.0f) < d53) {
            o.forceX = (dx * -3.0f / d).f();
            o.forceY = (X87(dy) * -3.0f / d).f();
        }
        if (o.z < -50.0f) {
            raise(o, 90.0f);
            o.flags &= ~ObjFlag::InHole;
            setPosition(world, o, o.fallToX, o.fallToY, frame);
        }
        return;
    }
    if (o.z == 0 && o.zVel == 0) {
        return;
    }
    // A bouncer (a fist, a thrown piece): gravity 0.5 a frame; at the floor it springs back at
    // bounce / 10 of its speed, and settles once that's under 2.
    if (o.flags & ObjFlag::Bouncing) {
        raise(o, (X87(o.z) + o.zVel).f());
        if (0 <= o.z) {
            o.zVel = (X87(o.zVel) - 0.5f).f();
            return;
        }
        raise(o, 0);
        o.zVel = (-X87(o.zVel) * o.bounce * 0.1f).f(); // (FCHS, FMUL, FMUL, FST: the test below the same on the stack)
        if (o.zVel < 2.0f) {
            raise(o, 0);
            o.zVel = 0;
        }
        return;
    }
    if (o.flags & ObjFlag::OnObject) {  // held up by an elevator
        return;
    }
    // Else gravity 1 a frame while above the floor.
    if (0 < o.z) {
        if (o.zVel <= 0) {
            o.flags |= ObjFlag::Falling;
        }
        raise(o, (X87(o.z) + o.zVel).f());
        o.zVel = (X87(o.zVel) - 1.0f).f();
    }
    if (o.z <= 0) {
        o.flags &= ~ObjFlag::Falling;
        // Coming down it lands and joins the motion (unitHasCollideOrUpdateFn).
        if (o.zVel < 0 && !(o.classes & oc::Missile)) {
            queueMotion(world, o);
            if (o.zVel < -10.0f && (o.classes & oc::Player)) {
                playSound(world, 0x118, o); // (a player landing hard)
            }
        }
        raise(o, 0);
        o.zVel = 0;
    }
}

void Objects::elevatorUpdate(ServerObject& o)
{
    // updateElevator.
    World& world = fight.world;
    const std::uint32_t frame = fight.frame;
    ServerObject* shaft = o.elevatorLink ? world.byExtent(o.elevatorLink) : nullptr;
    const auto moved = [&] { // (unitNeedSync, both)
        o.touch();
        if (shaft) {
            shaft->touch();
        }
    };
    // The states: 0 at rest at the bottom, 3 rising, 2 at rest at the top, 1 sinking; each rest
    // lasts at least a second (30 frames) from elevatorStopped.
    switch (o.elevatorState) {
    case 0:
        if (o.enabled() && 30u < frame - o.elevatorStopped) {
            o.elevatorState = 3;
            elevatorSound(world, o, true);
        }
        break;
    case 1:
        if (o.elevatorLevel < 1) {
            o.elevatorState = 0;
            o.elevatorStopped = frame;
        } else {
            o.elevatorLevel -= 2;  // 2 a frame
        }
        moved();
        if (static_cast<float>(o.elevatorLevel) <= 20.0f) {
            o.flags |= 0x10u; // SHORT
        }
        break;
    case 2:
        if (o.enabled() && 30u < frame - o.elevatorStopped) {
            o.elevatorState = 1;
            elevatorSound(world, o, false);
        }
        break;
    case 3: {
        o.elevatorLevel += 2;
        moved();
        if (20.0f <= static_cast<float>(o.elevatorLevel)) {
            o.flags &= ~0x10u;
        }
        if (32.0f <= static_cast<float>(o.elevatorLevel)) {
            // What's on it within 64 (elevatorFn): within 10 of its height,
            // to the shaft, 64 below; too big for the opening, dropped where it is.
            const auto level = static_cast<float>(o.elevatorLevel);
            // (unitsGetInCircle of 64, by the grid: one sent down relinks, and the
            // walk goes on from its new place, as the exe's)
            world.unitsInCircle(o.x, o.y, 64.0f, [&](ServerObject& t) {
                if (!insideBox(o, o.x, o.y, t.x, t.y) || !shaft || !(std::fabs((X87(t.z) - level).f()) < 10.0f)) { // (no 0x20 test; the height FSTP'd)
                    return;
                }
                bool fits = true;
                if (t.shape == ShapeKind::Box) {
                    fits = t.boxWidth <= shaft->boxWidth && t.boxHeight <= shaft->boxHeight;
                } else if (t.shape == ShapeKind::Circle) {
                    const float across = t.radius + t.radius;
                    fits = across <= shaft->boxWidth && across <= shaft->boxHeight;
                }
                if (fits) {
                    setPosition(world, t, shaft->x, shaft->y, frame);
                    raise(t, level - 64.0f);
                } else {
                    t.flags &= ~ObjFlag::OnObject;
                    raise(t, 0);
                }
            });
        }
        if (0x3F < o.elevatorLevel) {  // 64: the top
            o.elevatorLevel = 0x40;
            o.elevatorState = 2;
            o.elevatorStopped = frame;
        }
        break;
    }
    default:
        break;
    }
}

void Objects::shaftUpdate(ServerObject& o)
{
    // updateElevatorShaft: it follows its elevator's state.
    World& world = fight.world;
    const ServerObject* elevator = o.elevatorLink ? world.byExtent(o.elevatorLink) : nullptr;
    if (!elevator) {
        return;
    }
    // Each frame it joins the motion (unitHasCollideOrUpdateFn), to hold up or
    // swallow what's over it, moving or not (physics.cpp shaftContact).
    queueMotion(world, o);
    if (elevator->elevatorState == 1) {
        if (elevator->elevatorLevel < 0x21) {
            // What's on its top (fnElevatorShaft) rides down on the elevator.
            const auto level = static_cast<float>(elevator->elevatorLevel);
            // (unitsGetInCircle of 64, by the grid; as the elevator's)
            world.unitsInCircle(o.x, o.y, 64.0f, [&](ServerObject& t) {
                if (insideBox(o, o.x, o.y, t.x, t.y) && std::fabs((t.z + 64.0f) - level) < 10.0f) { // (no 0x20 test)
                    setPosition(world, t, elevator->x, elevator->y, fight.frame);
                    raise(t, level);
                }
            });
        }
        if (o.elevatorState != 1) {
            elevatorSound(world, o, false); // (newly going down)
        }
    } else if (elevator->elevatorState == 3 && o.elevatorState != 3) {
        elevatorSound(world, o, true); // (newly going up)
    }
    o.elevatorState = elevator->elevatorState;
}

namespace {

/// BoulderDie's rocks and their bounces (the exe's two tables), taken in turn.
constexpr std::array<const char*, 12> kRocks{"BigRock",   "MediumRock", "SmallRock", "MediumRock", "SmallRock", "SmallRock",
                                             "MediumRock", "SmallRock", "SmallRock", "SmallRock",  "SmallRock", "SmallRock"};
constexpr std::array<std::uint8_t, 12> kRockBounce{1, 3, 5, 3, 5, 5, 3, 5, 5, 5, 5, 5};

} // namespace

std::pair<float, float> clearPointNear(const World& world, std::minstd_rand& rng, float x, float y, float r)
{
    const float step = r * 0.015625f;
    float angle = util::randomFloat(rng, -3.1415927f, 3.1415927f).f(); // (randomFloat, stored)
    for (int i = 0; i < 64; ++i) {
        // (On the x87: the angle's sum FST'd, its FCOS of the stack's value, its FSIN of the stored
        // one (ours the library's in double, near enough to the x87's); FMUL the reach, FADD, FSTP.
        // The turn is the float 0x3FF1463B, 0.6 pi.)
        const X87 turned = X87(angle) + 1.88495576f;
        angle = turned.f();
        const float px = (X87::fromDouble(util::cosNearest(turned.d())) * r + x).f();
        const float py = (X87::fromDouble(util::sinNearest(static_cast<double>(angle))) * r + y).f();
        if (world.traceRay(x, y, px, py, Trace::Doors)) {
            return {px, py};
        }
        r = (X87(r) - step).f();
    }
    return {x, y};
}

void objectDies(Fight& fight, ServerObject& o)
{
    // Its DIE function (the exe's obj->Death), or, with none, it's deleted
    // (delayedDeleteObject).
    World& world = fight.world;
    const things::ThingDb* things = fight.things;
    const std::string_view f = o.dieFn;
    if (f == "CreateObjectDie" || f == "SpawnObjectDie") {
        // dieCreateObject / dieSpawnObject: its DeathData's type
        // where it was (the pieces of a barrel; "NULL": nothing). The first goes, the second
        // stays, dead (a chest open, a workstation broken: their updates go on).
        if (things) {
            world.createAt(*things, o.dieType, o.x, o.y, fight.frame);
        }
        playSound(world, o.dieSound, o); // (its DIE's sound, after the type)
        if (f == "CreateObjectDie") {
            deleteObject(world, o);
        } else {
            o.flags |= ObjFlag::Dead;
        }
    } else if (f == "PolypDie") {
        // diePolyp: a ToxicCloud for ToxicCloudLifetime seconds where it was.
        if (ServerObject* cloud = things ? world.createAt(*things, "ToxicCloud", o.x, o.y, fight.frame) : nullptr) {
            cloud->value = util::floatToInt(X87(world.number("ToxicCloudLifetime")) * X87::fromDouble(30)); // (FIMUL the FPS, FSTP)
        }
        playSound(world, 0x11C, o);
        o.pendingDelete = true;
    } else if (f == "BoulderDie") {
        // dieBoulder: FX 0x8A, then 20-30 rocks (the twelve in turn, carried
        // over from the last boulder) within 30 where it sees, 10-70 up, falling at 0-2 and
        // bouncing by their kind, pushed off it by 5-20, gone in 45-75 s.
        playSound(world, 0x2F5, o);
        pointFx(world, 0x8A, o.x, o.y);
        const int count = nox::util::uniform_int_distribution<int>(20, 30)(fight.rng);
        for (int i = 0; i < count; ++i) {
            const std::uint32_t kind = world.nextRock;
            const auto [rx, ry] = clearPointNear(world, fight.rng, o.x, o.y, 30.0f);
            ServerObject* rock = things ? world.createAt(*things, kRocks[kind], rx, ry, fight.frame) : nullptr;
            if (!rock) {
                return; // (the exe's: the boulder isn't deleted then)
            }
            raise(*rock, util::randomFloat(fight.rng, 10.0f, 70.0f).f()); // (each draw stored as it comes)
            rock->zVel = util::randomFloat(fight.rng, -2.0f, 0.0f).f();
            rock->flags |= ObjFlag::Bouncing;
            rock->bounce = static_cast<float>(kRockBounce[kind]);
            applyForce(world, *rock, o.x, o.y, util::randomFloat(fight.rng, 5.0f, 20.0f).f());
            rock->decayAt = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(45, 75)(fight.rng)) * 30u;
            world.nextRock = (world.nextRock + 1) % kRocks.size();
        }
        o.pendingDelete = true;
    } else if (f == "WeaponDie" || f == "ArmorDie") {
        // WeaponDie / ArmorDie: its holder told it broke; gone
        // (delayedDeleteObject: out of the inventory at once).
        if (things) {
            Items{world, *things, fight.modifiers, fight.rng, fight.frame}.breakNotice(o);
        }
        deleteObject(world, o);
    } else if (f == "GlyphDie") {
        // dieGlyph: it goes off (with nothing to blame).
        if (things) {
            Spells{world, *things, fight.modifiers, fight.rng, fight.frame}.glyphGoesOff(o, nullptr);
        } else {
            o.pendingDelete = true;
        }
    } else if (f == "GameBallDie") {
        // dieGameBall: a new one at a start, this one gone (no
        // start: it stays).
        spawnBall(fight, &o);
    } else if (f == "MarkerDie") {
        // dieMarker (a teleport mark): out of its player's marks, FX 0x8A, gone.
        if (ServerObject* p = fight.parentPlayer(&o); p && p->player) {
            for (auto& mark : p->player->marks) {
                if (mark == o.extent) {
                    mark = 0;
                    break;
                }
            }
        }
        pointFx(world, 0x8A, o.x, o.y);
        o.pendingDelete = true;
    } else if (f == "MonsterGeneratorDie") {
        generatorDies(fight, o); // (monsters.hpp)
    } else if (f == "ImpEggDie") {
        // dieImpEgg: it stays, no longer in the way. (Its UPDATE, ImpEggUpdate, is
        // in no table of the exe's: it never hatches.) 0x2FC at it.
        playSound(world, 0x2FC, o);
        o.flags |= ObjFlag::NoCollide;
    } else {
        deleteObject(world, o); // (no DIE: delayedDeleteObject, held or not)
    }
}

void Objects::breakUpdate(ServerObject& o, bool breaks)
{
    // updateBreak / updateOpen: whole (xStatus 2) till it's dead
    // (broken, opened), then breaking (4: a breakable no longer in the way, NO_COLLIDE) for 2
    // s, then broken (8), and it updates no more (unitRemoveFromUpdatable).
    const std::uint32_t frame = fight.frame;
    if (o.xStatus & 2u) {
        if (o.flags & ObjFlag::Dead) {
            unsetXStatus(o, 2);
            setXStatus(o, 4);
            o.timer = frame + 30u * 2u;
            if (breaks) {
                o.flags |= ObjFlag::NoCollide;
            }
        }
    } else if (o.xStatus & 4u) {
        if (o.timer < frame) {
            unsetXStatus(o, 4);
            setXStatus(o, 8);
        }
    } else if (o.xStatus & 8u) {
        fight.world.removeUpdatable(o); // (its UPDATE kept: moved again, it's back for a turn)
    }
}

void Objects::breakAndRemoveUpdate(ServerObject& o)
{
    // updateBreakAndRemove: BreakUpdate's, by xStatus exactly 2, 4 or 8 (a switch,
    // not its bits), and at 8 off the list and deleted (delayedDeleteObject).
    // (No type in thing.bin names it: only its entry in the name table names it.)
    const std::uint32_t frame = fight.frame;
    switch (o.xStatus) {
    case 2:
        if (o.flags & ObjFlag::Dead) {
            unsetXStatus(o, 2);
            setXStatus(o, 4);
            o.timer = frame + 30u * 2u;
            o.flags |= ObjFlag::NoCollide;
        }
        break;
    case 4:
        if (o.timer < frame) {
            unsetXStatus(o, 4);
            setXStatus(o, 8);
        }
        break;
    case 8:
        fight.world.removeUpdatable(o);
        deleteObject(fight.world, o);
        break;
    default:
        break;
    }
}

void Objects::blackPowderBarrelUpdate(ServerObject& o)
{
    // updateBlackPowderBarrel (the barrel's pieces): 1-5 frames after it's made
    // it blows: 30 of explosion (type 7) within 100, full within 30, a push of 60, and four
    // small or medium flames 10-25 off where it sees, burning 5-20 s; a second on it's gone.
    World& world = fight.world;
    const std::uint32_t frame = fight.frame;
    if (o.timer == o.born) {
        o.timer = static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(1, 5)(fight.rng)) + frame;
        return;
    }
    if (o.timer == frame) {
        Missiles missiles{world, things, fight.modifiers, fight.rng, frame};
        missiles.damageAt(o.x, o.y, 100.0f, 30.0f, 30, 7, &o, nullptr, false);
        missiles.pushFrom(o.x, o.y, 100.0f, 30.0f, 60.0f);
        for (int i = 0; i < 4; ++i) {
            const int kind = nox::util::uniform_int_distribution<int>(0, 1)(fight.rng);
            const float r = (util::randomFloat(fight.rng, 0.0f, 15.0f) + 10.0f).f(); // (randomFloat's stack value + 10, stored)
            const auto [dx, dy] = directionVector(nox::util::uniform_int_distribution<int>(0, 255)(fight.rng));
            const float px = (X87(r) * dx + o.x).f();
            const float py = (X87(r) * dy + o.y).f();
            if (world.traceRay(o.x, o.y, px, py, Trace::Doors)) {
                if (ServerObject* flame = world.createAt(things, kind == 0 ? "SmallFlame" : "MediumFlame", px, py, frame)) {
                    flame->decayAt = frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(5, 20)(fight.rng)) * 30u;
                }
            }
        }
        return;
    }
    if (30u <= frame - o.born) {
        o.pendingDelete = true;
    }
}

bool Objects::skullSees(const ServerObject& o)
{
    // In the square of 350 about it (collision boxes), a live creature that's its
    // enemy, clear of it (mapCheck: no shadow-casting body or wall between) and
    // in front of it (unitIsAttackReachable).
    World& world = fight.world;
    bool sees = false;
    world.unitsInRect(o.x - 350.0f, o.y - 350.0f, o.x + 350.0f, o.y + 350.0f, [&](const ServerObject& t) { // (by the grid)
        if (t.gone() || !(t.classes & (oc::Monster | oc::Player)) || (t.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || t.pendingDelete) {
            return;
        }
        if (isEnemy(world, o, t) && bodiesClear(world, t, o) && (facingSides(o.x, o.y, o.direction, t.x, t.y) & 1)) {
            sees = true;
        }
    });
    return sees;
}

void Objects::skullUpdate(ServerObject& o)
{
    // updateShootingTrap (arrow traps, skulls): on, it looks for a target once a
    // second (newly seen: it may shoot at once), and while it sees one shoots every second: its
    // missile (createArrowTrapProjectile) its radius + 4 ahead, owned by it, at
    // the missile's speed along its facing; an arrow trap's arrow does ArrowTrapDamage, and it
    // flashes (MSG 0xA1: where, 1 or 2 by the trap). Off, it starts afresh when turned on.
    World& world = fight.world;
    if (!o.shooter) {
        o.shooter.emplace();
    }
    auto& s = *o.shooter;
    if (!o.enabled()) {
        s.wasOn = false;
        return;
    }
    if (!s.wasOn) {
        s.look = 0;
        s.reload = 0;
    }
    s.wasOn = true;
    if (s.look == 0) {
        if (skullSees(o)) {
            if (!s.sees) {
                s.sees = true;
                s.reload = 0;
            }
        } else {
            s.sees = false;
        }
        s.look = 30;
    }
    if (s.sees && s.reload == 0) {
        const auto [dx, dy] = directionVector(o.direction);
        // (createArrowTrapProjectile on the x87: the reach kept on the stack; each spot and
        // velocity FSTP'd)
        const X87 r = X87(o.radius) + 4.0f;
        if (ServerObject* m = world.createAt(things, s.missile, (r * dx + o.x).f(), (r * dy + o.y).f(), fight.frame)) {
            m->owner = o.extent;
            m->direction = o.direction;
            m->vx = (X87(dx) * m->speedCur).f();
            m->vy = (X87(dy) * m->speedCur).f();
            if (o.typeName == "ArrowTrap1" || o.typeName == "ArrowTrap2") {
                // (Both of its CollideData's numbers: the solo game's and the others'.)
                m->collideArg = m->collideArg2 = static_cast<int>(world.number("ArrowTrapDamage"));
            }
            if (s.missile == "MercArcherArrow") {
                playSound(world, 0x379, o);
            }
        }
        if (o.typeName == "ArrowTrap1" || o.typeName == "ArrowTrap2") { // (sendArrowTrapFX)
            const auto ix = static_cast<std::uint16_t>(static_cast<int>(o.x));
            const auto iy = static_cast<std::uint16_t>(static_cast<int>(o.y));
            world.broadcasts().push_back({{0xA1, lo(ix), hi(ix), lo(iy), hi(iy), static_cast<std::uint8_t>(o.typeName == "ArrowTrap1" ? 1 : 2)}, false, 0});
        }
        s.reload = 30;
    }
    if (s.look != 0) {
        --s.look;
    }
    if (s.reload != 0) {
        --s.reload;
    }
}

void Objects::blowUpdate(ServerObject& o)
{
    // (The exe's float constants, bit for bit.)
    const float kTan15 = std::bit_cast<float>(0x3E892A30u);
    const float kBlowSteep = std::bit_cast<float>(0x3EBF1412u);
    const float kTan30 = std::bit_cast<float>(0x3F13C9EFu);
    const float kBlowDiagonal = std::bit_cast<float>(0x3E315B57u);
    // updateBlow (steam pipes): on, what's in the 400 square on its side
    // (getUnitsInRectAdv; by its facing's grid way, the rect's corner at the pipe:
    // west of it if the way's x is negative, else east; north if its y is negative, else
    // south), not IMMOBILE nor gone, nearer than 400 (+ 0.1), within its blast's angle and
    // reachable from it (unitCanInteractWith), is pushed along its facing by
    // (400 - distance)^3 x 5e-7 / its mass. The angle by the facing: east or
    // west, |dy| / |dx| at most tan 15 (0.2679); north or south, at most 0.3732, so a pipe
    // pointing north or south pushes what's level with it, and one pointing diagonally
    // (between 0.5773 and 0.1732) never pushes, an original bug. By the grid, the missiles
    // in the rect's cells after (pushed as any). Nothing it pushes joins the motion
    // (doesn't call unitHasCollideOrUpdateFn): one at rest gets the push only if its turn in the units
    // update is after the pipe's.
    World& world = fight.world;
    if (!o.enabled()) {
        return;
    }
    const auto [gx, gy] = gridDirection(o.direction);
    const float left = gx < 0 ? (X87(o.x) - 400.0f).f() : o.x;
    const float right = gx < 0 ? o.x : (X87(o.x) + 400.0f).f();
    const float top = gy < 0 ? (X87(o.y) - 400.0f).f() : o.y;
    const float bottom = gy < 0 ? o.y : (X87(o.y) + 400.0f).f();
    const auto [fx, fy] = directionVector(o.direction);
    world.unitsInRectAdv(left, top, right, bottom, [&](ServerObject& t) {
        if (t.gone() || t.pendingDelete || (t.classes & oc::Immobile)) {
            return;
        }
        // (On the x87: dx stored; dy stored too but squared as the stack's value times the stored
        // one; the distance stored, tested on the stack; the ratio on the stack.)
        const float dx = (X87(t.x) - o.x).f();
        const X87 dy53 = X87(t.y) - o.y;
        const float dy = dy53.f();
        const X87 d53 = util::sqrt(dy53 * dy + X87(dx) * dx) + 0.1f;
        const float d = d53.f();
        if (!(d53 < X87(400.0f))) {
            return;
        }
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        const X87 ratio = X87(ay) / ax;
        bool within = false;
        // The facing's grid way as one index (dy x 3 + dx + 4): 3 west, 5 east, 1 north, 7 south,
        // 4 no way (everything), the others the diagonals.
        switch (gy * 3 + 4 + gx) {
        case 3: within = dx < 0 && ratio <= X87(kTan15); break;
        case 5: within = dx > 0 && ratio <= X87(kTan15); break;
        case 1: within = dy < 0 && ratio <= X87(kBlowSteep); break;
        case 7: within = dy > 0 && ratio <= X87(kBlowSteep); break;
        case 4: within = true; break;
        default: { // the diagonals
            const bool side = (gx < 0 ? dx < 0 : dx > 0) && (gy < 0 ? dy < 0 : dy > 0);
            within = side && !(ratio < X87(kTan30)) && ratio <= X87(kBlowDiagonal);
            break;
        }
        }
        if (!within || !canInteract(world, o, t, 0)) {
            return;
        }
        // (FLD 400, FSUB the distance, cubed and by 5e-7 FSTP'd; over the mass kept)
        const X87 left400 = X87(400.0f) - d;
        const X87 push = X87((left400 * left400 * left400 * 5e-7f).f()) / t.mass;
        t.forceX = (push * fx + t.forceX).f();
        t.forceY = (push * fy + t.forceY).f();
    });
}

void Objects::sentryUpdate(ServerObject& o)
{
    // updateSentryGlobe: on, its beam (600 along its way, cut by walls: ray
    // flags 5) ends at Pos39 (sent to the players whose view it crosses, MSG_FX_SENTRY_RAY:
    // game_host.cpp); the way turns its speed a frame; what the beam passes over within its
    // radius (the beam's test: in the beam's box, not BELOW or NO_COLLIDE, not SHORT but
    // a quest's live monster, with HealthData, at 0 too) takes 500 of type 0x10 from the globe's player.
    // (No self-skip, as the beam's test: a globe has no HEALTH in thing.bin, nor does SentryXfer give one,
    // so its own HealthData test passes over it.)
    // Off, its way goes back to its start.
    World& world = fight.world;
    if (!o.sentry) {
        o.sentry.emplace();
    }
    auto& s = *o.sentry;
    if (!o.enabled()) {
        s.angle = s.start;
        return;
    }
    // (FLD the angle, FCOS / FSIN on the stack (ours the library's double, near enough to the
    // x87's), FMUL 600, FADD, FSTP)
    const float x1 = (X87::fromDouble(util::cosNearest(static_cast<double>(s.angle))) * 600.0f + o.x).f();
    const float y1 = (X87::fromDouble(util::sinNearest(static_cast<double>(s.angle))) * 600.0f + o.y).f();
    RayHit hit;
    if (!world.traceRay(o.x, o.y, x1, y1, Trace::Doors | Trace::ThroughWindows, &hit)) {
        o.homeX = hit.x;
        o.homeY = hit.y;
    } else {
        o.homeX = x1;
        o.homeY = y1;
    }
    s.angle = (X87(s.speed) + s.angle).f();
    const float ex = o.homeX;
    const float ey = o.homeY;
    const float left = std::min(o.x, ex);
    const float right = std::max(o.x, ex);
    const float top = std::min(o.y, ey);
    const float bottom = std::max(o.y, ey);
    // (getUnitsInRect over the beam's box, by the grid; each struck as it's found)
    world.unitsInRect(left, top, right, bottom, [&](ServerObject& t) {
        if (t.classes & oc::Missile) { // (no 0x20 test; a missile is in no extent list anyway)
            return;
        }
        if (t.flags & (ObjFlag::Below | ObjFlag::NoCollide)) {
            return;
        }
        if ((t.flags & 0x10u) && (!world.quest() || !(t.classes & oc::Monster) || (t.flags & ObjFlag::Dead))) {
            return; // (SHORT)
        }
        if (!t.hasHealthData()) { // (HealthData, at 0 too: a story creature kept at none, a shopkeeper)
            return;
        }
        float fx = 0;
        float fy = 0;
        if (pointOnLine(o.x, o.y, ex, ey, t.x, t.y, fx, fy)) {
            // (all on the x87's stack)
            const X87 dx = X87(t.x) - fx;
            const X87 dy = X87(t.y) - fy;
            if (dy * dy + dx * dx < X87(t.radius) * t.radius) {
                fight.damage(t, fight.parentPlayer(&o), o.extent, 500, 0x10);
                playSound(world, 0x12A, t);
            }
        }
    });
}

void makeScorch(World& world, const things::ThingDb& things, float x, float y, int size, std::minstd_rand& rng, std::uint32_t frame)
{
    // sMakeScorch: a scorch mark of its size (0 ScorchMarkFloorSmallA, 1
    // ...MediumA, 2 ...LargeB) on the floor, gone in 10-20 s (a quest's 5-8).
    static constexpr const char* kScorch[3] = {"ScorchMarkFloorSmallA", "ScorchMarkFloorMediumA", "ScorchMarkFloorLargeB"};
    if (size < 0 || 2 < size) {
        return;
    }
    if (ServerObject* s = world.createAt(things, kScorch[size], x, y, frame)) {
        const int seconds = world.quest() ? nox::util::uniform_int_distribution<int>(5, 8)(rng) : nox::util::uniform_int_distribution<int>(10, 20)(rng);
        s->decayAt = frame + static_cast<std::uint32_t>(seconds) * 30u;
    }
}

void scriptMoveTo(World& world, ServerObject& o, std::int32_t waypoint)
{
    // server_scriptMoveTo: not the dead; a monster walks there (monsters.hpp's
    // scriptMove, from the script host);
    // a mover sets off for it, as do the movers carrying it (moverGoTo: on,
    // still, starting, back on the updatable list).
    if ((o.flags & ObjFlag::Dead) || (o.classes & things::ObjectClass::Monster)) {
        return;
    }
    const auto goTo = [&](ServerObject& m) {
        setOnOff(m, true);
        m.vx = m.vy = 0;
        if (!m.mover) {
            m.mover.emplace();
        }
        m.mover->state = 0;
        m.mover->start = waypoint;
        m.updateFn = "MoverUpdate";
        m.updates = true;
        queueMotion(world, m); // (objectSetOn's unitHasCollideOrUpdateFn)
        world.addUpdatable(m); // (unitAddToUpdatable)
    };
    if (o.typeName == "Mover") {
        goTo(o);
        return;
    }
    // (Its movers by the map number each carries: the thing's Extent, its map number, 0 for one made
    // in play.)
    for (auto& [extent, m] : world.objects()) {
        if (m.typeName == "Mover" && m.mover && m.mover->carried == o.fileExtent) {
            goTo(m);
        }
    }
}

std::int32_t createMover(World& world, const things::ThingDb& things, ServerObject& o, std::int32_t waypoint, float speed, std::uint32_t frame)
{
    // A Mover made where `o` is (createAt), its UpdateData holding
    // the object, the waypoint, the speed (__ftol: chopped) and state 0; still; on
    // (objectSetOn) and on the updatable list. Its carried
    // extent MoverUpdate and Move go by, stays 0: at its first turn it
    // leaves the list, having moved nothing (an original bug). Ours keeps the extent only.
    ServerObject* m = world.createAt(things, "Mover", o.x, o.y, frame);
    if (!m) {
        return 0;
    }
    if (!m->mover) {
        m->mover.emplace();
    }
    m->mover->carried = 0;
    m->mover->start = waypoint;
    m->mover->speed = static_cast<int>(speed);
    m->mover->state = 0;
    m->vx = m->vy = 0;
    setOnOff(*m, true); // (objectSetOn: a Mover is none of 0x4000 / 0x10042000, nor a missile)
    queueMotion(world, *m);
    world.addUpdatable(*m); // (unitAddToUpdatable)
    return m->scriptId;
}

void Objects::moverUpdate(ServerObject& o)
{
    // unitUpdateMover: it carries its object (put at its own spot each frame)
    // along a waypoint path. Starting (0, on): it goes to its object, heads for its first
    // waypoint at its speed x 0.25; moving (1, on), once its velocity (both parts non-zero:
    // an original bug) shows it past its waypoint, it takes the next: none, it's done (3)
    // and its object is put on the waypoint; one link, that; several, one at random that isn't
    // where it came from; then its velocity points at the waypoint at its speed. Off it pauses
    // (2), on again it goes on from its object. Done, or its object gone, it updates no more.
    World& world = fight.world;
    if (!o.mover) {
        o.mover.emplace();
    }
    auto& m = *o.mover;
    const auto stop = [&] {
        o.updateFn.clear();
        o.updates = false;
        world.removeUpdatable(o); // (unitRemoveFromUpdatable)
    };
    // Its thing by its map number, at an update that has none (netGetUnitByExtent: the first on
    // the map of that number), kept; one no longer ACTIVE, or destroyed, let go (its number kept,
    // so the next start finds the thing of that number again).
    if (m.carried == 0) {
        stop();
        return;
    }
    if (m.carriedObject == 0) {
        const ServerObject* found = world.byMapNumber(m.carried);
        if (!found) {
            stop();
            return;
        }
        m.carriedObject = found->extent;
    }
    ServerObject* carried = world.byExtent(m.carriedObject);
    if (!carried || !(carried->flags & ObjFlag::Active) || carried->pendingDelete) {
        m.carriedObject = 0;
        stop();
        return;
    }
    switch (m.state) {
    case 0:
        if (!o.enabled()) {
            return;
        }
        if (world.waypoint(m.start)) {
            setPosition(world, o, carried->x, carried->y, fight.frame);
            m.state = 1;
            m.target = m.start;
            m.previous = 0;
            o.speedBase = o.speedCur = static_cast<float>(m.speed) * 0.25f;
        } else {
            m.state = 3;
        }
        return;
    case 1: {
        if (!o.enabled()) {
            m.state = 2;
            return;
        }
        const ServerWaypoint* to = world.waypoint(m.target);
        if (!to) {
            return; // (the exe reads through the null)
        }
        // Past the waypoint: the velocity no longer points toward it.
        if (o.vx != 0 && o.vy != 0 && (X87(to->x) - o.x) * o.vx + (X87(to->y) - o.y) * o.vy <= X87(0.0f)) { // (on the stack)
            if (to->links.empty()) {
                m.state = 3;
                setPosition(world, *carried, to->x, to->y, fight.frame);
            } else if (to->links.size() == 1) {
                m.previous = m.target;
                m.target = to->links[0];
            } else {
                std::int32_t next = 0;
                do {
                    next = to->links[static_cast<std::size_t>(
                        nox::util::uniform_int_distribution<int>(0, static_cast<int>(to->links.size()) - 1)(fight.rng))];
                } while (next == m.previous);
                m.previous = m.target;
                m.target = next;
            }
        }
        if (m.state != 1) {
            return;
        }
        setPosition(world, *carried, o.x, o.y, fight.frame);
        const ServerWaypoint* head = world.waypoint(m.target);
        if (!head) {
            return;
        }
        // (On the x87: dx kept on the stack; dy stored, and squared as the stack's value times the
        // stored one; the length stored.)
        const X87 dx = X87(head->x) - o.x;
        const X87 dy53 = X87(head->y) - o.y;
        const float dy = dy53.f();
        const float d = (util::sqrt(dy53 * dy + dx * dx) + 0.1f).f();
        o.vx = (dx * o.speedCur / d).f();
        o.vy = (X87(dy) * o.speedCur / d).f();
        return;
    }
    case 2:
        if (o.enabled()) {
            setPosition(world, o, carried->x, carried->y, fight.frame);
            m.state = 1;
        }
        return;
    default:
        stop();
        return;
    }
}

namespace {

/// updateSmallToxicCloud / updateToxicCloud: every 5-10 frames the creatures and players within
/// `reach` it sees (ray flags 5) are touched by it (type 5: no harm from the small one, 3-10 drawn
/// for each as it's found from the big one) and its caster's enemies poisoned (1); its count
/// (value) down each frame, gone at 0.
void toxicCloudUpdate(Fight& fight, ServerObject& o, float reach, bool harms)
{
    World& world = fight.world;
    if (o.timer < fight.frame) {
        ServerObject* from = fight.parentPlayer(&o);
        // (unitsGetInCircle, by the grid; each as it's found: toxicCloudPoison)
        world.unitsInCircle(o.x, o.y, reach, [&](ServerObject& t) {
            if ((t.classes & (oc::Monster | oc::Player)) && world.traceRay(o.x, o.y, t.x, t.y, Trace::Doors | Trace::ThroughWindows)) { // (no 0x20 test)
                fight.damage(t, from, o.extent, harms ? nox::util::uniform_int_distribution<int>(3, 10)(fight.rng) : 0, 5);
                if (from && isEnemy(world, *from, t)) {
                    fight.applyPoison(t, 1, 1);
                }
            }
        });
        o.timer = static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(5, 10)(fight.rng)) + fight.frame;
    }
    if (o.value != 0) {
        --o.value;
    }
    if (o.value == 0) {
        o.pendingDelete = true;
    }
}

} // namespace

// Runs its UPDATE by name (acts() lists the names handled here).
void Objects::update(ServerObject& o)
{
    World& world = fight.world;
    const NameId f = o.updateFn.id(); // (by the name's id: numbers compared, not text)
    switch (f) {
    case NameId::MoverUpdate:
        moverUpdate(o);
        break;
    case NameId::MonsterGeneratorUpdate:
        generatorUpdate(fight, o); // (monsters.hpp)
        break;
    case NameId::ArachnaphobiaUpdate:
        // updateArachnaphobia: a SmallSpider (its owner's) each 1-5 frames, for
        // 3 seconds.
        if (o.timer < fight.frame && fight.things) {
            if (ServerObject* spider = world.createAt(*fight.things, "SmallSpider", o.x, o.y, fight.frame)) {
                spider->owner = o.owner;
            }
            o.timer = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(1, 5)(fight.rng));
        }
        if (30u * 3u < fight.frame - o.born) {
            o.pendingDelete = true;
        }
        break;
    case NameId::UndeadKillerUpdate: {
        // updateUndeadKiller: gone with its spell's cancelling, or 70 frames on.
        const auto& list = world.durationSpells();
        const auto it = std::ranges::find_if(list, [&](const DurationSpell& s) { return s.serial == static_cast<std::uint32_t>(o.value); });
        if ((it != list.end() && it->cancelled) || 0x46u < fight.frame - o.timer) {  // 0x46: 70 frames
            o.pendingDelete = true;
        }
        break;
    }
    case NameId::FlagUpdate:
        flagUpdate(fight, o); // (modes.hpp)
        break;
    case NameId::BallUpdate:
        ballUpdate(fight, o);
        break;
    case NameId::CrownUpdate:
        crownUpdate(fight, o);
        break;
    case NameId::SkullUpdate:
        skullUpdate(o);
        break;
    case NameId::BlowUpdate:
        blowUpdate(o);
        break;
    case NameId::SentryGlobeUpdate:
        sentryUpdate(o);
        break;
    case NameId::BreakUpdate:
    case NameId::OpenUpdate:
        breakUpdate(o, f == NameId::BreakUpdate);
        break;
    case NameId::BreakAndRemoveUpdate:
        breakAndRemoveUpdate(o);
        break;
    case NameId::OneSecondDieUpdate: // updateOneSecondDie
        if (30u <= fight.frame - o.born) {
            o.pendingDelete = true;
        }
        break;
    case NameId::SelfDestructUpdate: // updateSelfDestruct
        if (2u < fight.frame - o.born) {
            o.pendingDelete = true;
        }
        break;
    case NameId::WaterBarrelUpdate: {
        // updateWaterBarrel: 8 frames on, the fires whose edge is within 40 of
        // it go out (those in the square of 40 about it, getUnitsInRect); a
        // second on it's gone.
        const std::uint32_t age = fight.frame - o.born;
        if (age == 8u) {
            bool doused = false;
            world.unitsInRect((X87(o.x) - 40.0f).f(), (X87(o.y) - 40.0f).f(), (X87(o.x) + 40.0f).f(), (X87(o.y) + 40.0f).f(),
                              [&](ServerObject& t) { // (by the grid)
                if (!(t.classes & oc::Fire)) { // (no 0x20 test)
                    return;
                }
                const X87 dx = X87(o.x) - t.x; // (on the x87's stack)
                const X87 dy = X87(o.y) - t.y;
                if (util::sqrt(dy * dy + dx * dx) - t.radius <= X87(40.0f)) {
                    t.pendingDelete = true;
                    doused = true;
                }
            });
            if (doused) {
                playSound(world, 0x11B, o);
            }
        } else if (0x1Du < age) {  // past 29 frames: about a second
            o.pendingDelete = true;
        }
        break;
    }
    case NameId::BlackPowderBarrelUpdate:
        blackPowderBarrelUpdate(o);
        break;
    case NameId::BlackPowderBurnUpdate:
        // updateBlackPowderBurn: 3 frames after it's lit, 1 of fire to what's
        // within 15 (it spreads to the powder about it); 2 s on it's gone.
        if (o.timer == o.born) {
            o.timer = fight.frame + 3u;
        } else if (o.timer == fight.frame) {
            Missiles{world, things, fight.modifiers, fight.rng, fight.frame}.damageAt(o.x, o.y, 15.0f, 15.0f, 1, 1, &o, nullptr, false);
        } else if (60u <= fight.frame - o.born) {
            o.pendingDelete = true;
        }
        break;
    case NameId::LifetimeUpdate:
        // updateLifetime: past its frames (its argument) it dies.
        if (static_cast<std::uint32_t>(o.value) < fight.frame - o.born) {
            o.flags |= ObjFlag::Dead;
            objectDies(fight, o);
        }
        break;
    case NameId::SmallToxicCloudUpdate:
        toxicCloudUpdate(fight, o, 35.0f, false); // (updateSmallToxicCloud: within 35, no harm)
        break;
    case NameId::ObeliskUpdate:
        obeliskUpdate(o);
        break;
    case NameId::SwitchUpdate:
        // updateSwitch (a Spike): off, it's down (frame 1, the first time) and
        // harmless (NO_COLLIDE); on, it harms again, and back on it joins the motion
        // (unitHasCollideOrUpdateFn) to meet what's on it.
        if (!o.enabled()) {
            if (o.animFrame == 0) {
                o.touch();
                o.animFrame = 1;
            }
            o.flags |= ObjFlag::NoCollide;
        } else if (o.collides && (o.flags & ObjFlag::NoCollide)) {
            o.flags &= ~ObjFlag::NoCollide;
            queueMotion(world, o);
        } else {
            o.flags &= ~ObjFlag::NoCollide;
        }
        break;
    case NameId::LoopAndDamageUpdate:
        // LoopAndDamageUpdate (a PeriodicSpike): harmless while off. (SpikeBlockUpdate:
        // nothing at all.)
        if (!o.enabled()) {
            o.flags |= ObjFlag::NoCollide;
        } else {
            o.flags &= ~ObjFlag::NoCollide;
        }
        break;
    case NameId::TrapDoorUpdate:
        trapDoorUpdate(o);
        break;
    case NameId::PentagramUpdate:
        pentagramUpdate(o);
        break;
    case NameId::InvisiblePentagramUpdate:
        invisiblePentagramUpdate(o);
        break;
    case NameId::TriggerUpdate:
        triggerUpdate(o);
        break;
    case NameId::ToggleUpdate:
        toggleUpdate(o);
        break;
    case NameId::ElevatorUpdate:
        elevatorUpdate(o);
        break;
    case NameId::ElevatorShaftUpdate:
        shaftUpdate(o);
        break;
    case NameId::GlyphChainUpdate: // (set by a glyph that went off nearby): it goes off
        Spells{world, things, fight.modifiers, fight.rng, fight.frame}.glyphGoesOff(o, nullptr);
        break;
    case NameId::MoonglowUpdate: { // updateMoonglow: at its owner's cursor for 300 s
        ServerObject* owner = o.owner ? world.byExtent(o.owner) : nullptr;
        if (!owner) {
            o.pendingDelete = true;
            return;
        }
        if (!owner->player) {
            return;
        }
        if ((owner->flags & ObjFlag::Destroyed) || 30u * 300u < fight.frame - o.born) {
            o.pendingDelete = true;
            disenchant(world, *owner, Enchant::Moonglow);
            return;
        }
        // (Whole numbers, inside the map's object grid: 70 squares of 85.)
        const auto cx = static_cast<float>(static_cast<int>(owner->player->cursorX));
        const auto cy = static_cast<float>(static_cast<int>(owner->player->cursorY));
        if (0 <= cx && MapGrid::cellOf(cx) < 70 && 0 <= cy && MapGrid::cellOf(cy) < 70) {
            setPosition(world, o, cx, cy, fight.frame);
        }
        break;
    }
    case NameId::TelekinesisUpdate: { // updateTelekinesis: at its owner's cursor
        // while it sees it (ray flags 5), for 20 s; what it overlaps the motion pushes
        ServerObject* owner = o.owner ? world.byExtent(o.owner) : nullptr;
        if (owner && owner->player && !(owner->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && fight.frame - o.born <= 30u * 20u) {
            const auto cx = static_cast<float>(static_cast<int>(owner->player->cursorX));
            const auto cy = static_cast<float>(static_cast<int>(owner->player->cursorY));
            if (world.traceRay(owner->x, owner->y, cx, cy, Trace::Doors | Trace::ThroughWindows)) {
                setPosition(world, o, cx, cy, fight.frame);
            }
        } else {
            if (owner) {
                playSound(world, spellSound(things, 0x7F, 2), *owner); // (and again as it's taken off)
            }
            o.pendingDelete = true;
            if (owner) {
                disenchant(world, *owner, Enchant::Telekinesis);
            }
        }
        break;
    }
    case NameId::FlameCleanseUpdate: { // updateFlameCleanse
        // Till its time, while its origin sees it (ray flags 0x41) and, after 4 frames, it moved
        // last frame (a wall stops it).
        const bool sees = world.traceRay(o.homeX, o.homeY, o.x, o.y, Trace::Doors | Trace::SightOnly);
        const bool moved = o.lastX != o.x || o.lastY != o.y;
        o.lastX = o.x;
        o.lastY = o.y;
        if (fight.frame < o.timer && sees && (fight.frame - o.born < 4u || moved)) {
            return;
        }
        o.pendingDelete = true;
        break;
    }
    case NameId::ExpireUpdate: // updateExpire: gone out of its span
        if (fight.frame < o.born || o.timer < fight.frame) {
            o.pendingDelete = true;
        }
        break;
    case NameId::FistUpdate: // updateFist
        if (o.z <= 0 && !(o.flags & ObjFlag::Landed)) {
            playSound(world, 0x30, o);
            o.flags |= ObjFlag::Landed;
            pointFx(world, 0x8A, o.radius + o.x, o.y);
            pointFx(world, 0x8A, o.x - o.radius, o.y);
            pointFx(world, 0x8A, o.x, o.radius + o.y);
            jiggle(world, o.x, o.y, 30);
        }
        if (200.0f <= o.z && (o.flags & ObjFlag::Landed)) {
            o.pendingDelete = true;
        }
        if (30u * 3u < fight.frame - o.born) {
            o.pendingDelete = true;
        }
        break;
    case NameId::MeteorUpdate: // meteorExplode
        if (o.z <= 0) {
            playSound(world, 0x57, o);
            jiggle(world, o.x, o.y, 10);
            if (things.findThing("MeteorExplode")) {
                world.spawn(things, "MeteorExplode", o.x, o.y).flags |= ObjFlag::Active;
            }
            ServerObject* from = fight.parentPlayer(&o);
            Missiles{world, things, fight.modifiers, fight.rng, fight.frame}.damageAt(o.x, o.y, 80.0f, 30.0f, o.value, 7, from, nullptr, false);
            o.pendingDelete = true; // (the walls about it too: damageAt)
        }
        break;
    case NameId::MeteorShowerUpdate: // updateMeteorShower
        if (30u * 5u <= fight.frame - o.born) {
            o.pendingDelete = true;
            return;
        }
        if (o.timer <= fight.frame) {
            const int way = nox::util::uniform_int_distribution<int>(0, 255)(fight.rng);
            const X87 r = util::randomFloat(fight.rng, 4.0f, 12.0f); // (randomFloat's stack value, never stored)
            const auto [fx, fy] = directionVector(way);
            const X87 r2 = r * r; // (kept on the x87's stack)
            const float px = (r2 * fx + o.x).f();  // 4-12 squared: 16 to 144 out
            const float py = (r2 * fy + o.y).f();
            if (world.traceRay(o.x, o.y, px, py, Trace::Doors | Trace::NearWindows) && things.findThing("Meteor")) {
                ServerObject& m = world.spawn(things, "Meteor", px, py);
                m.owner = o.extent;
                m.born = fight.frame;
                m.flags |= ObjFlag::Active;
                m.value = o.value;
                m.xStatus |= 0x20; // (its height goes as MSG_FX_DELTAZ_SPELL_START: the client draws the fall)
                raise(m, 255.0f);
                m.zVel = -8.0f;
            }
            o.timer = static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(4, 8)(fight.rng)) + fight.frame;
        }
        break;
    case NameId::ToxicCloudUpdate:
        toxicCloudUpdate(fight, o, 75.0f, true); // (updateToxicCloud: within 75, 3-10 harm)
        break;
    default:
        break;
    }
}

} // namespace nox::server
