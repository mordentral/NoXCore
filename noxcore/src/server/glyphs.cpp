// Glyphs: the Glyph spell's traps, setting them off, Detonate Glyphs and Trigger Glyph (spells.hpp).
#include "noxcore/server/spells.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

using util::hi;
using util::lo;

/// Up its owners to a player, or the last of them (findParentChainPlayer).
ServerObject* chainPlayer(World& world, ServerObject* o)
{
    for (int guard = 0; o && !(o->classes & oc::Player) && o->owner != 0 && guard < 16; ++guard) {
        ServerObject* next = world.byExtent(o->owner);
        if (!next) {
            break;
        }
        o = next;
    }
    return o;
}

/// Whether `parent` is `o` or up its owners (unitHasThatParent).
bool hasParent(const World& world, const ServerObject& o, const ServerObject& parent)
{
    const ServerObject* p = &o;
    for (int guard = 0; p && guard < 16; ++guard) {
        if (p == &parent) {
            return true;
        }
        p = p->owner ? const_cast<World&>(world).byExtent(p->owner) : nullptr;
    }
    return false;
}

bool isGlyph(const ServerObject& o) { return o.trap.has_value() && o.typeName == NameId::Glyph; }

} // namespace

bool Spells::castGlyph(ServerObject& owner, ServerObject& source, float x, float y)
{
    // Spell_CastGlyph: a player's only (anything else: "cast", nothing done).
    if (!owner.player) {
        return true;
    }
    PlayerControl& p = *owner.player;
    // Refused: MSG_INFORM 0 with the reason (5 too many traps or bombers, 7 no spells queued).
    const auto refuse = [&](int why) {
        tell(owner, {0xA9, 0, lo(why), hi(why), 0, 0}); // MSG_INFORM 0
        return false;
    };
    if (static_cast<int>(world.number("MaxTrapCount")) <= p.curTraps) {
        return refuse(5);
    }
    if (p.trapCount == 0) {
        return refuse(7);
    }
    if (!things.findThing("Glyph")) {
        return false;
    }
    ServerObject* made = nullptr;
    if (p.classId == 2) {
        // A conjurer: a Bomber carrying the glyph (a count of its bombers against
        // MaxBomberCount, room among its summons (the guide's fifth, the Bomber), summoned
        // (unitDoSummonAt) within 50 of the source facing as the conjurer does).
        // (A summons failing: the glyph never placed, the queue spent all the same.) 0x16F at the
        // source either way.
        const auto bombers = std::ranges::count_if(world.objects(), [&](const auto& e) {
            return e.second.owner == owner.extent && (e.second.classes & oc::Monster) && (e.second.subclasses & 0x2000u) && !e.second.gone();
        });
        if (world.number("MaxBomberCount") <= static_cast<float>(bombers)) {
            return refuse(5);
        }
        if (!roomFor(world, things, owner, 5)) {
            return false;
        }
        const auto [bx, by] = clearPointNear(world, rng, source.x, source.y, 50.0f);
        Fight fight{world, rng, frame, &things, modifiers};
        if (ServerObject* bomber = summonAt(fight, "Bomber", bx, by, &owner, owner.direction)) {
            ServerObject& g = world.spawn(things, "Glyph", bomber->x, bomber->y);
            g.born = g.timer = frame;
            g.holder = bomber->extent; // (inventoryPutImpl: the bomber's)
            g.owner = bomber->extent;
            bomber->inventory.emplace_back(g.extent);
            made = &g;
        }
        playSound(world, 0x16F, source);
    } else {
        // A wizard: the glyph where the source is, its, facing as it does; the spell's sound at the
        // source.
        ServerObject& g = world.spawn(things, "Glyph", source.x, source.y);
        g.owner = owner.extent;
        g.born = g.timer = frame;
        g.flags |= ObjFlag::Active;
        made = &g;
        playSound(world, spellSound(things, 0x22, 0), source);
    }
    if (!made) {
        p.trapCount = 0;
        return true;
    }
    ServerObject& g = *made;
    g.direction = source.direction;
    // The queued spells that are enabled, the point they go at.
    ServerObject::Trap& t = *g.trap;
    t.count = 0;
    for (const std::uint32_t s : std::span(p.trapSpells).first(p.trapCount)) {
        if (things.findSpell(s)) {
            t.spells[t.count++] = s;
        }
    }
    t.x = x;
    t.y = y;
    t.target = 0;
    p.trapCount = 0;
    g.touch();
    if (p.laysGlyphs()) {
        ++p.curTraps;
    }
    return true;
}

bool Spells::setsOff(const ServerObject& glyph, const ServerObject& by) const
{
    // In the campaign, not while its (first) player is held still (NO_UPDATE).
    if (world.gameFlags & GameFlag::Solo) {
        for (const auto& [extent, o] : world.objects()) {
            if (o.player) {
                if (o.flags & ObjFlag::NoUpdate) {
                    return false;
                }
                break;
            }
        }
    }
    // Not its own side's; in a co-op game a player's not a player's.
    World& w = const_cast<World&>(world);
    bool goes = true;
    if (sameSide(world, glyph, by)) {
        goes = false;
    } else if (world.gameFlags & GameFlag::Coop) {
        const ServerObject* a = chainPlayer(w, const_cast<ServerObject*>(&glyph));
        const ServerObject* b = chainPlayer(w, const_cast<ServerObject*>(&by));
        if ((a->classes & oc::Player) && (b->classes & oc::Player)) {
            goes = false;
        }
    }
    // A warrior treading lightly passes over.
    return goes && !abilityActive(world, by, 4);
}

void Spells::glyphGoesOff(ServerObject& glyph, ServerObject* by)
{
    // Not twice.
    if (glyph.gone() || !glyph.trap) {
        return;
    }
    ServerObject* caster = chainPlayer(world, &glyph);
    // Gone (delayedDeleteObject: out of whatever holds it).
    if (glyph.holder != 0) {
        if (ServerObject* h = world.byExtent(glyph.holder)) {
            std::erase(h->inventory, glyph.extent);
        }
        glyph.holder = 0;
    }
    glyph.flags |= ObjFlag::Destroyed;
    glyph.pendingDelete = true;
    if (caster->player && caster->player->laysGlyphs() && caster->player->curTraps != 0) {
        --caster->player->curTraps;
    }
    // At what set it off, else the nearest within GlyphRange (its search: not of its
    // side, not observing, a creature, not dead, on its facing's sides (all), it can see and
    // reach it).
    ServerObject::Trap& t = *glyph.trap;
    // (Destroyed already, but the search doesn't look at its own flags.)
    ServerObject* target = by ? by : nearestCreature(world, glyph, world.number("GlyphRange"));
    t.target = target ? target->extent : 0;
    // Each spell (one needing a target (spell flag 1) only when something set it off) the
    // caster's to cast, from the glyph: a player's at its power, else at 2.
    for (std::uint8_t i = 0; i < t.count; ++i) {
        const std::uint32_t spell = t.spells[i];
        const things::SpellDef* def = things.findSpell(spell);
        if (def && (def->flags & 1u) && !by) {
            continue;
        }
        if (whoseSpell(*caster, spell) != 0) {
            continue;
        }
        cast(spell, *caster, target, t.x, t.y, caster->player ? power(*caster, spell) : 2, &glyph);
    }
    // MSG_FX 0x81 and 0x28 at it; the glyphs about it (a 100 box, a clear ray, flags 5) go off next
    // frame (their update set to GlyphChainUpdate by hand).
    pointFx(0x81, glyph.x, glyph.y);
    playSoundAt(world, 0x28, glyph.x, glyph.y);
    // (getUnitsInRect over the 100 box, by the grid)
    world.unitsInRect(glyph.x - 100.0f, glyph.y - 100.0f, glyph.x + 100.0f, glyph.y + 100.0f, [&](ServerObject& o) {
        // (not 0x20, ours' pendingDelete)
        if (&o != &glyph && !o.pendingDelete && isGlyph(o) && world.traceRay(glyph.x, glyph.y, o.x, o.y, Trace::Doors | Trace::ThroughWindows)) {
            o.updateFn = "GlyphChainUpdate";
            o.updates = true;
            world.addUpdatable(o); // (unitAddToUpdatable)
        }
    });
}

bool Spells::detonateGlyphs(ServerObject& owner, ServerObject& source)
{
    // Within a 300 box of the source, in a clear ray (flags 5), its player's
    // glyphs (or a non-player's: the unowned), one at a time, the last found first. The spell's
    // sound at the caster first.
    playSound(world, spellSound(things, 0x13, 0), owner);
    ServerObject* player = chainPlayer(world, &source);
    for (;;) {
        ServerObject* found = nullptr;
        // (getUnitsInRect over the 300 box, by the grid: the last found)
        world.unitsInRect(source.x - 300.0f, source.y - 300.0f, source.x + 300.0f, source.y + 300.0f, [&](ServerObject& o) {
            if (!isGlyph(o) || o.pendingDelete) { // (0x20: ours' pendingDelete)
                return;
            }
            if (!hasParent(world, o, *player) && (o.owner != 0 || (player->classes & oc::Player))) {
                return;
            }
            if (world.traceRay(source.x, source.y, o.x, o.y, Trace::Doors | Trace::ThroughWindows)) {
                found = &o;
            }
        });
        if (!found) {
            break;
        }
        glyphGoesOff(*found, nullptr);
    }
    return true;
}

void removeSpawned(World& world, ServerObject& player)
{
    // (noxcpp's multiclass player, a mode's: its glyphs and its creatures both.)
    if (player.player && player.player->laysGlyphs()) {
        for (auto& [extent, o] : world.objects()) {
            if (o.typeName == "Glyph" && !o.gone() && o.holder == 0 && hasParent(world, o, player)) {
                pointFx(world, 0x81, o.x, o.y);
                o.flags |= ObjFlag::Destroyed;
                o.pendingDelete = true;
            }
        }
    }
    if (player.player && player.player->keepsCreatures()) {
        // A conjurer's creatures (those it controls, creatureIsMonitored)
        // go, with what they carry, in a puff (MSG_FX 0x81).
        for (auto& [extent, o] : world.objects()) {
            if (o.owner == player.extent && !o.gone() && controls(world, player, o)) {
                for (const std::uint32_t e : keysOf(o.inventory)) {
                    if (ServerObject* i = world.byExtent(e)) {
                        deleteObject(world, *i); // (delayedDeleteObject: out of it at once)
                        i->flags |= ObjFlag::Destroyed;
                    }
                }
                pointFx(world, 0x81, o.x, o.y);
                o.flags |= ObjFlag::Destroyed;
                o.pendingDelete = true;
            }
        }
    }
    for (auto& [extent, o] : world.objects()) {
        if (o.owner == player.extent && (o.classes & oc::Missile) && !o.gone()) {
            o.flags |= ObjFlag::Destroyed;
            o.pendingDelete = true;
        }
    }
}

bool Spells::triggerGlyph(ServerObject& owner)
{
    // Its nearest glyph goes off (GlyphDie); none, the spell fails.
    ServerObject* nearest = nullptr;
    float best = 1e8f;
    for (auto& [extent, o] : world.objects()) {
        if (o.holder != 0 || o.typeName != "Glyph" || !hasParent(world, o, owner)) {
            continue;
        }
        // (The squared distance on the x87's stack, FSTP'd to the best when under it.)
        const util::X87 dx = util::X87(owner.x) - o.x;
        const util::X87 dy = util::X87(owner.y) - o.y;
        if (const util::X87 d2 = dy * dy + dx * dx; d2 < util::X87(best)) {
            best = d2.f();
            nearest = &o;
        }
    }
    if (!nearest) {
        return false;
    }
    playSound(world, spellSound(things, 0x81, 0), owner);
    glyphGoesOff(*nearest, nullptr);
    return true;
}

} // namespace nox::server
