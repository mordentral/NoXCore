// Satellites (satellites.hpp): things turning about a unit, harming as they go.
#include "satellites.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nox::server {

const Satellites::Group* Satellites::find(std::uint32_t centre) const
{
    const auto it = groups_.find(centre);
    return it == groups_.end() ? nullptr : &it->second;
}

void Satellites::start(std::uint32_t centre, std::vector<std::uint32_t> things, std::uint32_t frame, std::uint32_t life, std::uint32_t harmAfter,
                       std::uint16_t type, const Harm& harm)
{
    Group& g = groups_[ObjectRef{centre}];
    g.things.assign(things.begin(), things.end());
    g.born = frame;
    g.life = life;
    g.harmAfter = harmAfter;
    g.type = type;
    g.harm = harm;
}

bool Satellites::settle(World& world, Group& g, std::uint32_t frame)
{
    std::erase_if(g.things, [&](std::uint32_t e) {
        const ServerObject* b = world.byExtent(e);
        return !b || b->gone() || b->pendingDelete || b->type != g.type;
    });
    // Its life over: put away.
    if (!g.things.empty() && g.life < frame - g.born) {
        for (const std::uint32_t e : g.things) {
            if (ServerObject* b = world.byExtent(e)) {
                deleteObject(world, *b);
            }
        }
        g.things.clear();
    }
    return !g.things.empty();
}

void Satellites::turn(GameHost& host, ServerObject& centre, Group& g, const Shape& shape, std::uint32_t frame)
{
    World& world = host.server().world();
    // The harm, about each in turn where it stands, from it, the centre spared.
    if (g.harmAfter < frame - g.born) {
        Fight fight = host.server().fight();
        Missiles harm{world, things_, fight.modifiers, fight.rng, fight.frame};
        for (const std::uint32_t e : g.things) {
            if (ServerObject* b = world.byExtent(e)) {
                harm.damageAt(b->x, b->y, g.harm.reach, g.harm.full, g.harm.damage, g.harm.type, b, &centre, true);
            }
        }
    }
    // Round it goes.
    g.angle += shape.step;
    if (g.angle > 2.0f * std::numbers::pi_v<float>) {
        g.angle -= 2.0f * std::numbers::pi_v<float>;
    }
    for (std::size_t i = 0; i < g.things.size(); ++i) {
        ServerObject* b = world.byExtent(g.things[i]);
        if (!b) {
            continue;
        }
        const float a = g.angle + 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(shape.count);
        b->vx = b->vy = 0;
        setPosition(world, *b, centre.x + std::cos(a) * shape.radius, centre.y + std::sin(a) * shape.radius, frame);
    }
}

void Satellites::letGo(World& world, const Group& g)
{
    for (const std::uint32_t e : g.things) {
        if (ServerObject* o = world.byExtent(e)) {
            o->pendingDelete = true;
        }
    }
}

void Satellites::putAway(World& world)
{
    for (const auto& [centre, g] : groups_) {
        letGo(world, g);
    }
    groups_.clear();
}

} // namespace nox::server
