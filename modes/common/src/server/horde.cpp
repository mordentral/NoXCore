// A horde (horde.hpp): creatures steered at the players and reaped natively.
#include "horde.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <bit>
#include <optional>

namespace nox::server {

Horde::Member* Horde::find(std::uint32_t extent)
{
    for (auto m = members_.rbegin(); m != members_.rend(); ++m) {
        if (m->extent == extent) {
            return &*m;
        }
    }
    return nullptr;
}

void Horde::steer(GameHost& host, std::uint32_t frame, const std::vector<ServerObject*>& players)
{
    if (players.empty()) {
        return;
    }
    World& world = host.server().world();
    std::optional<Fight> fight;
    const float cull2 = rules_.cull * rules_.cull;
    const float close2 = rules_.close * rules_.close;
    for (Member& f : members_) {
        if (frame < f.steerAt) {
            continue;
        }
        f.steerAt = frame + rules_.period + (rules_.spread != 0 ? f.extent % rules_.spread : 0);
        ServerObject* o = world.byExtent(f.extent);
        if (!o || !o->monster || o->gone() || (o->flags & ObjFlag::Dead) || !o->monster->initialised) {
            continue;
        }
        // The nearest player (squared distances).
        ServerObject* nearest = nullptr;
        float best = 0;
        for (ServerObject* u : players) {
            const float d2 = (u->x - o->x) * (u->x - o->x) + (u->y - o->y) * (u->y - o->y);
            if (!nearest || d2 < best) {
                nearest = u;
                best = d2;
            }
        }
        if (best > cull2) {
            if (f.comesBack) {
                if (far_) {
                    far_(host, f, *o, *nearest);
                }
                continue;
            }
            deleteObject(world, *o);
            continue;
        }
        auto& m = *o->monster;
        // The action at the top of its AI's stack: one busy fighting, fleeing or dying is left alone.
        const int top = m.stack[static_cast<std::size_t>(std::max(0, m.top))].action;
        switch (top) {
        case MonsterAction::Fight:
        case MonsterAction::MeleeAttack:
        case MonsterAction::MissileAttack:
        case MonsterAction::CastSpellOnObject:
        case MonsterAction::CastSpellOnLocation:
        case MonsterAction::CastDurationSpell:
        case MonsterAction::BlockAttack:
        case MonsterAction::BlockFinish:
        case MonsterAction::WeaponBlock:
        case MonsterAction::Dodge:
        case MonsterAction::Dying:
        case MonsterAction::Dead:
        case MonsterAction::GetUp:
        case MonsterAction::Flee:
        case MonsterAction::Retreat:
        case MonsterAction::Confused:
            continue; // (busy with what it's at: its own AI)
        default:
            break;
        }
        // One with an enemy of its own, or already near a player, is left to its AI.
        if (m.enemy != 0 || best < close2) {
            continue;
        }
        if (!fight) {
            fight.emplace(host.server().fight());
        }
        Monsters ai{*fight};
        // Its stack replaced: walk far to the player; the Report under it then pops and runs its
        // end-of-waypoint script.
        ai.clear(*o);
        if (auto* it = ai.push(*o, MonsterAction::Report)) {
            it->args[0] = MonsterAction::FarMoveTo;
        }
        if (auto* it = ai.push(*o, MonsterAction::FarMoveTo)) {
            it->args[0] = std::bit_cast<std::uint32_t>(nearest->x);
            it->args[1] = std::bit_cast<std::uint32_t>(nearest->y);
            it->args[2] = 0;
        }
    }
}

void Horde::reap(GameHost& host)
{
    World& world = host.server().world();
    // (The fallen told after the pass, in order, as Watchers' lists are: a handler may then add to
    // or clear the horde. One an earlier handler deleted is dropped untold, as it was in the pass.)
    std::vector<Member> fallen;
    std::erase_if(members_, [&](const Member& f) {
        ServerObject* o = world.byExtent(f.extent);
        if (!o || o->gone() || o->pendingDelete || !o->monster) {
            return true;
        }
        if (!(o->flags & ObjFlag::Dead)) {
            // Its most health held (a rescale elsewhere would put an unhurt one to other numbers).
            if (o->maxHealth != f.most && f.most != 0) {
                if (o->health >= o->maxHealth) {
                    o->health = f.most;
                }
                o->maxHealth = f.most;
                o->touch();
            }
            return false;
        }
        if (fell_) {
            fallen.push_back(f);
        }
        return true;
    });
    for (const Member& f : fallen) {
        ServerObject* o = world.byExtent(f.extent);
        if (o && !o->gone() && !o->pendingDelete && o->monster && fell_) {
            fell_(host, f, *o);
        }
    }
}

} // namespace nox::server
