// Gatherables (gather.hpp): things lying about that players take up.
#include "gather.hpp"

#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <cmath>

namespace nox::server {

void Gatherables::cap(World& world)
{
    while (items_.size() > rules_.most) {
        if (ServerObject* o = world.byExtent(items_.front().extent)) {
            deleteObject(world, *o);
        }
        items_.erase(items_.begin());
    }
}

void Gatherables::evaluate(GameHost& host, std::uint32_t frame, std::span<const Taker> takers)
{
    World& world = host.server().world();
    // (The taken told after the pass, in order, as Watchers' lists are: a handler may then lay or
    // clear this block's own.)
    struct Took {
        Item item;
        std::size_t taker;
        float x;
        float y;
    };
    std::vector<Took> took;
    const auto take = [&](const Item& g, std::size_t taker, float x, float y) {
        if (taken_) {
            took.push_back({g, taker, x, y});
        }
    };
    std::erase_if(items_, [&](const Item& g) {
        ServerObject* o = world.byExtent(g.extent);
        if (!o || o->gone() || o->pendingDelete) {
            return true;
        }
        if (rules_.life < frame - g.born || (rules_.dropHeld && o->holder != 0)) {
            deleteObject(world, *o);
            return true;
        }
        if (rules_.way == Way::Touch) {
            for (std::size_t i = 0; i < takers.size(); ++i) {
                const ServerObject& u = *takers[i].unit;
                const float reach = takers[i].reach;
                if ((u.x - o->x) * (u.x - o->x) + (u.y - o->y) * (u.y - o->y) < reach * reach) {
                    const float px = o->x;
                    const float py = o->y;
                    deleteObject(world, *o);
                    take(g, i, px, py);
                    return true;
                }
            }
            return false;
        }
        // Drift: the nearest whose reach it's within.
        std::size_t best = takers.size();
        float bestD2 = 0;
        for (std::size_t i = 0; i < takers.size(); ++i) {
            const ServerObject& u = *takers[i].unit;
            const float reach = takers[i].reach;
            const float d2 = (u.x - o->x) * (u.x - o->x) + (u.y - o->y) * (u.y - o->y);
            if (d2 < reach * reach && (best == takers.size() || d2 < bestD2)) {
                best = i;
                bestD2 = d2;
            }
        }
        if (best == takers.size()) {
            return false;
        }
        const ServerObject& u = *takers[best].unit;
        const float d = std::sqrt(bestD2);
        if (d < rules_.take) {
            const float px = o->x;
            const float py = o->y;
            deleteObject(world, *o);
            take(g, best, px, py);
            return true;
        }
        // Drawn in, faster as it nears.
        const float step = std::min(d, rules_.base + (frame - g.born) % rules_.jitter + rules_.pull / std::max(d, rules_.near));
        setPosition(world, *o, o->x + (u.x - o->x) / d * step, o->y + (u.y - o->y) / d * step, frame);
        return false;
    });
    for (const Took& t : took) {
        if (taken_) {
            taken_(t.item, t.taker, t.x, t.y);
        }
    }
}

} // namespace nox::server
