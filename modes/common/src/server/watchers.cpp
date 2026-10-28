// Watchers (watchers.hpp): a mode's frame loops as generic plumbing.
#include "watchers.hpp"

#include "mode_shared.hpp"

#include "noxcore/server/enchant.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>

namespace nox::server {

namespace {

// Whether a player's unit passes a watcher's filter bits.
bool passes(std::uint32_t filter, const ServerObject& unit)
{
    // (Each field read only for its bit: a filter of one reads one.)
    return (!(filter & Watchers::NotObserving) || (unit.player->statusFlags & 1u) == 0) &&
           (!(filter & Watchers::NotBusy) || (unit.player->statusFlags & 3u) == 0) &&
           (!(filter & Watchers::Alive) || unit.health != 0) &&
           (!(filter & Watchers::NotWhole) || unit.health < unit.maxHealth || unit.player->mana < unit.player->maxMana) &&
           (!(filter & Watchers::GameOver) || unit.player->questGameOverAt != 0);
}

} // namespace

int Watchers::group(Admit admit, Handler arrive, int roster)
{
    Group group;
    group.admit = std::move(admit);
    group.arrive = std::move(arrive);
    group.roster = roster;
    groups_.push_back(std::move(group));
    return static_cast<int>(groups_.size()) - 1;
}

std::uint32_t Watchers::zone(int g, std::uint32_t thing, float x, float y, float radius, std::uint32_t filter, Mode mode, std::uint32_t cooldown,
                             Handler handler)
{
    Zone z;
    z.number = nextZone_++;
    z.thing = thing;
    z.point = thing == 0;
    z.x = x;
    z.y = y;
    z.radius = radius;
    z.filter = filter;
    z.mode = mode;
    z.cooldown = cooldown;
    z.handler = std::move(handler);
    groups_.at(static_cast<std::size_t>(g)).zones.push_back(std::move(z));
    return groups_[static_cast<std::size_t>(g)].zones.back().number;
}

Watchers::Zone* Watchers::find(std::uint32_t zone)
{
    for (Group& g : groups_) {
        for (Zone& z : g.zones) {
            if (z.number == zone) {
                return &z;
            }
        }
    }
    return nullptr;
}

void Watchers::retarget(std::uint32_t zone, std::uint32_t thing)
{
    if (Zone* z = find(zone)) {
        z->thing = thing;
        z->point = false;
    }
}

void Watchers::rearm(std::uint32_t zone)
{
    if (Zone* z = find(zone)) {
        z->firedAt = 0;
    }
}

void Watchers::reset(int g)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.zones.clear();
    group.seen.clear();
    group.through.clear();
    group.zones.shrink_to_fit();
}

void Watchers::clear()
{
    groups_.clear();
}

void Watchers::gather(GameHost& host, Roster& out, const Admit& admit)
{
    out.clear();
    World& world = host.server().world();
    for (const auto& [peer, p] : host.players()) {
        ServerObject* unit = world.byExtent(p.extent);
        if (unit && unit->player && p.ready && (!admit || admit(p, *unit))) {
            out.push_back({peer, &p, unit});
        }
    }
}

int Watchers::roster(Admit admit)
{
    rosters_.push_back({std::move(admit), {}});
    return static_cast<int>(rosters_.size()) - 1;
}

void Watchers::gatherRosters(GameHost& host)
{
    for (Shared& r : rosters_) {
        gather(host, r.members, r.admit);
    }
}

const Watchers::Roster& Watchers::membersOf(int g, GameHost& host)
{
    const Group& group = groups_.at(static_cast<std::size_t>(g));
    if (group.roster >= 0) {
        return rosters_.at(static_cast<std::size_t>(group.roster)).members;
    }
    gather(host, scratch_, group.admit);
    return scratch_;
}

void Watchers::evaluate(int g, GameHost& host, std::uint32_t frame)
{
    const Roster& roster = membersOf(g, host);
    Group& group = groups_.at(static_cast<std::size_t>(g));
    World& world = host.server().world();
    // (Arrivals forgotten as they leave the host: looked for only as the host's players change, each
    // join known by its own number, its name set before it.)
    if (group.arrive) {
        bool same = group.joins.size() == host.players().size();
        if (same) {
            auto join = group.joins.begin();
            for (const auto& [peer, p] : host.players()) {
                if (*join++ != p.joinSerial) {
                    same = false;
                    break;
                }
            }
        }
        if (!same) {
            group.joins.clear();
            for (const auto& [peer, p] : host.players()) {
                group.joins.push_back(p.joinSerial);
            }
            std::erase_if(group.seen, [&](const std::string& name) {
                return std::ranges::none_of(host.players(), [&](const auto& e) { return e.second.name == name; });
            });
        }
    }
    // Each zone's place this pass (a thing's where it is now; gone: nowhere).
    for (Zone& z : group.zones) {
        z.anyHit = false;
        if ((z.filter & ByPeer) && !z.insideIds.empty()) {
            std::erase_if(z.insideIds, [&](std::uint32_t peer) { return !host.players().contains(static_cast<std::uint8_t>(peer)); });
        }
        if (z.point) {
            z.there = true;
            z.px = z.x;
            z.py = z.y;
        } else if (const ServerObject* o = z.thing != 0 ? world.byExtent(z.thing) : nullptr; o && !o->gone()) {
            z.there = true;
            z.px = o->x;
            z.py = o->y;
        } else {
            z.there = false;
        }
    }
    for (const Member& m : roster) {
        const HostedPlayer& p = *m.player;
        ServerObject* unit = m.unit;
        const std::uint8_t peer = m.peer;
        const Event base{Event::Kind::Fire, 0, peer, &p, unit};
        if (group.arrive && group.seen.insert(p.name).second) {
            Event e = base;
            e.kind = Event::Kind::Arrive;
            group.arrive(e);
        }
        for (Zone& z : group.zones) {
            const bool within = z.there && passes(z.filter, *unit) && detail::within(*unit, z.px, z.py, z.radius);
            switch (z.mode) {
            case Mode::EachEnter: {
                if (z.filter & (ByUnit | ByPeer)) {
                    // (Kept by unit or peer: two players of one name each their own.)
                    if (!within && z.insideIds.empty()) {
                        break;
                    }
                    const std::uint32_t id = (z.filter & ByPeer) ? std::uint32_t{peer} : unit->extent;
                    Event e = base;
                    e.zone = z.number;
                    if (within && z.insideIds.insert(id).second) {
                        e.kind = Event::Kind::Enter;
                        z.handler(e);
                    } else if (!within && z.insideIds.erase(id) != 0) {
                        e.kind = Event::Kind::Leave;
                        z.handler(e);
                    }
                    break;
                }
                if (!within && z.inside.empty()) {
                    break; // (no one within: nothing to leave)
                }
                Event e = base;
                e.zone = z.number;
                if (within && !z.inside.contains(p.name)) {
                    z.inside.insert(p.name);
                    if (z.filter & SpellsChanged) {
                        z.spells[p.name] = unit->player->spellLevels;
                    }
                    e.kind = Event::Kind::Enter;
                    z.handler(e);
                } else if (!within && z.inside.erase(p.name) != 0) {
                    z.spells.erase(p.name);
                    e.kind = Event::Kind::Leave;
                    z.handler(e);
                } else if (within && (z.filter & SpellsChanged)) {
                    auto& seen = z.spells[p.name];
                    if (seen != unit->player->spellLevels) {
                        seen = unit->player->spellLevels;
                        e.kind = Event::Kind::Changed;
                        z.handler(e);
                    }
                }
                break;
            }
            case Mode::AnyCooldown:
                if (within && (z.firedAt == 0 || z.cooldown < frame - z.firedAt)) {
                    z.firedAt = frame;
                    Event e = base;
                    e.zone = z.number;
                    z.handler(e);
                }
                break;
            case Mode::Any:
                if (within) {
                    z.anyHit = true;
                    z.anyEvent = base;
                    z.anyEvent.zone = z.number;
                }
                break;
            }
        }
    }
    for (Zone& z : group.zones) {
        if (z.anyHit) {
            z.handler(z.anyEvent);
        }
    }
}
int Watchers::list(std::uint32_t ends, Handler handler)
{
    lists_.push_back({ends, std::move(handler), {}});
    return static_cast<int>(lists_.size()) - 1;
}

void Watchers::watch(int l, std::uint32_t thing, Name type, float x, float y)
{
    lists_.at(static_cast<std::size_t>(l)).things.push_back({thing, type, x, y});
}

void Watchers::unwatchAll(int l)
{
    lists_.at(static_cast<std::size_t>(l)).things.clear();
}

std::size_t Watchers::watching(int l) const
{
    return lists_.at(static_cast<std::size_t>(l)).things.size();
}

void Watchers::evaluateList(int l, World& world)
{
    List& list = lists_.at(static_cast<std::size_t>(l));
    for (std::size_t i = 0; i < list.things.size();) {
        Watched& w = list.things[i];
        const ServerObject* o = world.byExtent(w.thing);
        const std::uint32_t ends = list.ends;
        const bool ended = !o ? (ends & Missing) != 0
                              : ((ends & Gone) && o->gone()) || ((ends & Dead) && (o->flags & ObjFlag::Dead)) || ((ends & NoHealth) && o->health == 0) ||
                                    ((ends & NotMonster) && !o->monster) || ((ends & OtherType) && o->typeName != w.type) ||
                                    ((ends & Deleted) && o->pendingDelete) || ((ends & Destroyed) && (o->flags & ObjFlag::Destroyed));
        if (!ended) {
            if (o) {
                w.x = o->x; // (where it stands: pushed about, it ends where it was)
                w.y = o->y;
            }
            ++i;
            continue;
        }
        WatchEvent e;
        e.id = w.thing;
        e.x = w.x;
        e.y = w.y;
        list.things.erase(list.things.begin() + static_cast<std::ptrdiff_t>(i));
        list.handler(e);
    }
}

int Watchers::finder(Handler handler)
{
    finders_.push_back({std::move(handler)});
    return static_cast<int>(finders_.size()) - 1;
}

void Watchers::seek(int f, Name type, float x, float y, float radius)
{
    Finder& finder = finders_.at(static_cast<std::size_t>(f));
    finder.seeking = true;
    finder.type = type;
    finder.x = x;
    finder.y = y;
    finder.radius = radius;
}

void Watchers::evaluateFind(int f, World& world)
{
    Finder& finder = finders_.at(static_cast<std::size_t>(f));
    if (!finder.seeking) {
        return;
    }
    for (auto& [extent, o] : world.objects()) {
        if (o.typeName == finder.type && !o.gone() && detail::within(o, finder.x, finder.y, finder.radius)) {
            finder.seeking = false;
            WatchEvent e;
            e.id = extent;
            e.unit = &o;
            e.x = o.x;
            e.y = o.y;
            finder.handler(e);
            return;
        }
    }
}

void Watchers::keeping(int g, Handler handler)
{
    groups_.at(static_cast<std::size_t>(g)).keeping = std::move(handler);
}

void Watchers::keep(int g, const std::string& name, Keep keep)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.kept[name].keep = std::move(keep);
    group.keptByPeer.fill({});
}

void Watchers::unkeepAll(int g)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.kept.clear();
    group.everyone = false;
    group.keptByPeer.fill({});
}

void Watchers::keepEveryone(int g, Keep keep)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.everyone = true;
    group.keepAll = std::move(keep);
    group.keptByPeer.fill({});
}

void Watchers::restat(int g, const std::string& name)
{
    auto& kept = groups_.at(static_cast<std::size_t>(g)).kept;
    if (const auto it = kept.find(name); it != kept.end()) {
        it->second.seen = false;
    }
}

void Watchers::evaluateKeep(int g, GameHost& host, std::uint32_t frame, std::uint32_t serial)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    if (group.kept.empty() && !group.everyone) {
        return;
    }
    const Roster& roster = membersOf(g, host);
    World& world = host.server().world();
    for (const Member& m : roster) {
        const HostedPlayer& p = *m.player;
        ServerObject* unit = m.unit;
        const std::uint8_t peer = m.peer;
        // (Its keep by name, the lookup kept for the player's join with this unit: a join's number
        // is never given again, where a record's address and a unit's number are.)
        auto& cached = group.keptByPeer[peer];
        Group::Kept* kept = cached.join == p.joinSerial && cached.unit == unit->extent ? cached.kept : nullptr;
        if (!kept) {
            auto found = group.kept.find(p.name);
            if (found == group.kept.end() && group.everyone) {
                found = group.kept.emplace(p.name, Group::Kept{group.keepAll}).first;
            }
            kept = found != group.kept.end() ? &found->second : nullptr;
            cached = {p.joinSerial, unit->extent, kept};
        }
        if (!kept) {
            continue;
        }
        auto& k = *kept;
        const Keep& keep = k.keep;
        PlayerControl& pc = *unit->player;
        const bool inPlay = (pc.statusFlags & 1u) == 0 && unit->health != 0 && !(unit->flags & ObjFlag::Dead);
        WatchEvent e;
        e.peer = peer;
        e.player = &p;
        e.unit = unit;
        if (keep.stats && (!k.seen || k.maxHealth != unit->maxHealth || k.maxMana != pc.maxMana || k.strength != pc.strength)) {
            e.kind = Event::Kind::Stats;
            group.keeping(e);
            k.seen = true;
            k.maxHealth = unit->maxHealth;
            k.maxMana = pc.maxMana;
            k.strength = pc.strength;
        }
        for (std::size_t i = 0; i < keep.enchants.size() && inPlay; ++i) {
            const int en = keep.enchants[i].first;
            if (!enchanted(*unit, en) || enchantTime(*unit, en) != 0) { // (missing, or timed: made for good)
                enchant(world, *unit, en, 0, keep.enchants[i].second);
            }
        }
        for (const int en : keep.strip) {
            if (enchanted(*unit, en) && enchantTime(*unit, en) == 0) {
                disenchant(world, *unit, en);
            }
        }
        if (keep.beat != 0 && inPlay && (frame - keep.beatFrom) % keep.beat == keep.beatAt && k.beatFired != frame) {
            k.beatFired = frame;
            e.kind = Event::Kind::Beat;
            group.keeping(e);
        }
        if (keep.once && inPlay && k.onceAt != serial) {
            k.onceAt = serial;
            e.kind = Event::Kind::Once;
            group.keeping(e);
        }
    }
}

void Watchers::firsts(int g, std::uint32_t filter, Handler handler)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.firstsFilter = filter;
    group.firsts = std::move(handler);
}

void Watchers::forget(int g, const std::string& name)
{
    groups_.at(static_cast<std::size_t>(g)).first.erase(name);
}

void Watchers::forgetAll(int g)
{
    groups_.at(static_cast<std::size_t>(g)).first.clear();
}

void Watchers::evaluateFirsts(int g, GameHost& host)
{
    const Roster& roster = membersOf(g, host);
    Group& group = groups_.at(static_cast<std::size_t>(g));
    for (const Member& m : roster) {
        const HostedPlayer& p = *m.player;
        ServerObject* unit = m.unit;
        const std::uint8_t peer = m.peer;
        if (group.first.contains(p.name) || ((group.firstsFilter & NotObserving) && (unit->player->statusFlags & 1u) != 0)) {
            continue;
        }
        group.first.insert(p.name);
        WatchEvent e;
        e.kind = Event::Kind::First;
        e.peer = peer;
        e.player = &p;
        e.unit = unit;
        group.firsts(e);
    }
}

void Watchers::exits(int g, std::vector<std::uint32_t> gates, Handler handler)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.gates = std::move(gates);
    group.exits = std::move(handler);
}

void Watchers::evaluateExits(int g, GameHost& host)
{
    const Roster& roster = membersOf(g, host);
    Group& group = groups_.at(static_cast<std::size_t>(g));
    for (const Member& m : roster) {
        const HostedPlayer& p = *m.player;
        ServerObject* unit = m.unit;
        const std::uint8_t peer = m.peer;
        const std::uint32_t gate = unit->player->exitGate;
        const auto it = std::ranges::find(group.gates, gate);
        if (gate == 0 || it == group.gates.end() || group.through.contains(unit->extent)) {
            continue;
        }
        group.through.insert(unit->extent);
        WatchEvent e;
        e.kind = Event::Kind::Exit;
        e.peer = peer;
        e.player = &p;
        e.unit = unit;
        e.zone = static_cast<std::uint32_t>(it - group.gates.begin());
        group.exits(e);
    }
}

void Watchers::conditions(int g, Conditions c)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.conditions = std::move(c);
    group.anyoneDone = group.afterDone = group.allDone = false;
    group.allSince = 0;
}

void Watchers::rearmConditions(int g, std::uint32_t afterFrom)
{
    Group& group = groups_.at(static_cast<std::size_t>(g));
    group.conditions.afterFrom = afterFrom;
    group.anyoneDone = group.afterDone = group.allDone = false;
    group.allSince = 0;
}

void Watchers::evaluateConditions(int g, GameHost& host, std::uint32_t frame)
{
    // (The group looked up again after each handler: its handlers may watch more.)
    const auto at = [&]() -> Group& { return groups_.at(static_cast<std::size_t>(g)); };
    const Roster& roster = membersOf(g, host);
    if (roster.empty()) {
        if (at().conditions.all && !at().allDone) {
            at().allSince = 0;
        }
        return;
    }
    WatchEvent e;
    e.peer = roster.front().peer;
    e.player = roster.front().player;
    e.unit = roster.front().unit;
    if (at().conditions.anyone && !at().anyoneDone) {
        at().anyoneDone = true;
        e.kind = Event::Kind::Anyone;
        const Handler h = at().conditions.anyone;
        h(e);
    }
    if (at().conditions.after && !at().afterDone && at().conditions.afterFrames < frame - at().conditions.afterFrom) {
        at().afterDone = true;
        e.kind = Event::Kind::After;
        const Handler h = at().conditions.after;
        h(e);
    }
    if (at().conditions.all) {
        const std::uint32_t filter = at().conditions.allFilter;
        if (std::ranges::all_of(roster, [&](const Member& m) { return passes(filter, *m.unit); })) {
            if (at().allSince == 0) {
                at().allSince = frame;
                e.kind = Event::Kind::Begin;
                const Handler h = at().conditions.all;
                h(e);
            } else if (!at().allDone && at().conditions.allFrames < frame - at().allSince) {
                at().allDone = true;
                e.kind = Event::Kind::Held;
                const Handler h = at().conditions.all;
                h(e);
            }
        } else if (!at().allDone) {
            at().allSince = 0;
        }
    }
}

int Watchers::timer(Handler handler)
{
    timers_.push_back({std::move(handler)});
    return static_cast<int>(timers_.size()) - 1;
}

void Watchers::every(int t, std::uint32_t beat, std::uint32_t from, std::uint32_t at)
{
    Timer& timer = timers_.at(static_cast<std::size_t>(t));
    timer.beat = beat;
    timer.from = from;
    timer.at = at;
}

void Watchers::due(int t, std::uint32_t frame)
{
    Timer& timer = timers_.at(static_cast<std::size_t>(t));
    timer.dueArmed = true;
    timer.dueAt = frame;
}

void Watchers::stop(int t)
{
    Timer& timer = timers_.at(static_cast<std::size_t>(t));
    timer.beat = 0;
    timer.dueArmed = false;
}

void Watchers::evaluateTimer(int t, std::uint32_t frame)
{
    Timer& timer = timers_.at(static_cast<std::size_t>(t));
    WatchEvent e;
    if (timer.beat != 0 && (frame - timer.from) % timer.beat == timer.at && timer.beatFired != frame) {
        timer.beatFired = frame;
        e.kind = Event::Kind::Beat;
        timer.handler(e);
    }
    if (timer.dueArmed && frame > timer.dueAt) {
        timer.dueArmed = false;
        e.kind = Event::Kind::Due;
        timer.handler(e);
    }
}

} // namespace nox::server
