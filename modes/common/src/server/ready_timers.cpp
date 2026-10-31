// Ready timers (ready_timers.hpp): each holder's weapons counted down natively.
#include "ready_timers.hpp"

#include "foes.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nox::server {

void ReadyTimers::setKind(std::uint32_t kind, const Kind& k)
{
    if (kind >= kinds_.size()) {
        kinds_.resize(kind + 1);
    }
    kinds_[kind] = k;
}

void ReadyTimers::clear()
{
    satellites_.clear();
    kept_.clear();
    shapes_.clear();
    waits_.clear();
}

const ReadyTimers::Shape& ReadyTimers::shape(std::uint32_t kind, int level, int bonus)
{
    const std::uint32_t key = (kind << 16) | (static_cast<std::uint32_t>(level & 0xFF) << 8) | static_cast<std::uint32_t>(bonus & 0xFF);
    const auto it = std::ranges::lower_bound(shapes_, key, {}, &Known::key);
    if (it != shapes_.end() && it->key == key) {
        return it->shape;
    }
    if (!shape_) {
        throw std::logic_error("ReadyTimers: a shape asked with no script to ask");
    }
    return shapes_.insert(it, {key, shape_(kind, level, bonus)})->shape;
}

bool ReadyTimers::holds(World& world, const ServerObject& unit, Wait::Test test, const Wait& w, std::uint32_t except)
{
    using Test = Wait::Test;
    switch (test) {
    case Test::None:
        return false;
    case Test::NoFoe:
        return !detail::foeInSight(world, unit, w.range, w.tested);
    case Test::NoneNear: {
        bool any = false;
        world.unitsInCircle(unit.x, unit.y, w.range, [&](ServerObject& o) {
            any = any || (o.monster && !o.gone() && !o.pendingDelete && !(o.flags & ObjFlag::Dead) && o.health != 0 && isEnemy(world, unit, o));
        });
        return !any;
    }
    case Test::HolderEnchanted:
        return enchanted(unit, w.enchant);
    case Test::KeptAny:
        return kept_.settle(world, unit.extent, w.key) > 0;
    case Test::KeptAtLeast:
        return kept_.settle(world, unit.extent, w.key) >= w.least;
    case Test::OutOfReach: {
        const ServerObject* nearest = nullptr;
        detail::visibleFoes(world, unit, w.range, w.tested, [&](ServerObject* o) {
            nearest = o;
            return false;
        });
        if (!nearest) {
            return false;
        }
        const float dx = nearest->x - unit.x;
        const float dy = nearest->y - unit.y;
        return std::sqrt(dx * dx + dy * dy) - nearest->radius > unit.radius + w.reach;
    }
    case Test::AllUnder: {
        bool open = false;
        detail::visibleFoes(world, unit, w.range, w.tested, [&](ServerObject* o) {
            open = !(o->extent == except || enchanted(*o, w.enchant) || (w.enchant2 >= 0 && enchanted(*o, w.enchant2)));
            return !open;
        });
        return !open;
    }
    }
    return false;
}

std::uint32_t ReadyTimers::fire(GameHost& host, std::uint32_t frame, ServerObject& unit, const Shot& shot, int bonus, std::uint32_t except)
{
    const auto held = std::ranges::find_if(waits_, [&](const Held& h) { return h.holder == unit.extent && h.kind == shot.kind; });
    if (held != waits_.end()) {
        if (held->level == shot.level && held->bonus == bonus) {
            const Wait& w = held->wait;
            World& world = host.server().world();
            if (holds(world, unit, w.test, w, except)) {
                return frame + w.retry;
            }
            if (w.also != Wait::Test::None && holds(world, unit, w.also, w, except)) {
                return frame + w.alsoRetry;
            }
        }
        waits_.erase(held);
    }
    const Result r = fire_(shot);
    if (r.wait.test != Wait::Test::None) {
        waits_.push_back({ObjectRef{unit.extent}, shot.kind, shot.level, bonus, r.wait});
    }
    return r.readyAt;
}

bool ReadyTimers::orbit(GameHost& host, std::uint32_t frame, ServerObject& unit, std::uint32_t kind, int level, std::uint32_t readyAt, int bonus)
{
    World& world = host.server().world();
    Satellites::Group& g = satellites_.group(unit.extent);
    if (!satellites_.settle(world, g, frame)) {
        return frame >= readyAt;
    }
    satellites_.turn(host, unit, g, shape(kind, level, bonus).orbit, frame);
    return false;
}

bool ReadyTimers::keep(GameHost& host, std::uint32_t frame, ServerObject& unit, std::uint32_t kind, int level, std::uint32_t readyAt, int bonus,
                       int& have, int& total)
{
    World& world = host.server().world();
    const Kind& k = kinds_[kind];
    total = kept_.settleGroup(world, unit.extent, k.group, kind, have);
    return have < shape(kind, level, bonus).cap && total < k.most && frame >= readyAt && detail::foeInSight(world, unit, k.gate, k.tested);
}

} // namespace nox::server
