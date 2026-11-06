// The co-op campaign's exits, deaths and revisited maps (coop.hpp): the party gathers at an exit;
// only a party wipe loses (the others respawn by the checkpoint); a guest's view of a map it loads
// from its own file is put right where the world (a visited map's saved copy) differs.
#include "noxcore/coop/coop.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace nox::coop {

namespace {

/// The classes a client draws from its own map file (IMMOBILE, CLIENT_PERSIST: netCodeOf's).
constexpr std::uint32_t kClientLoaded = 0x20400000u;

/// A map's name from an exit's string ("Wiz02A.map:FromA" -> "Wiz02A").
std::string exitMapName(std::string_view exitMap)
{
    exitMap = exitMap.substr(0, exitMap.find('\0')); // (a C string)
    exitMap = exitMap.substr(0, exitMap.find(':'));
    return std::string(exitMap.substr(0, exitMap.find('.')));
}

/// Whether a unit is held by a script's freeze (Frozen's lock: a scene, Wiz03A's GotoWiz03b), which
/// closing a talk doesn't let go. A gathering's go waits while the host is: the exit's save waits for
/// a free host, and while it waited a scripted move onto a way on would find the save busy.
bool scriptFrozen(const server::ServerObject& u)
{
    return (u.flags & server::ObjFlag::NoUpdate) && u.player && u.player->freezeLock != 0;
}

/// Where an exit leads, as one gathering's key (its map, any case).
std::string destination(std::string_view exitMap)
{
    std::string s = exitMapName(exitMap);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool sameText(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

/// A map script's scene that leaves the host frozen in the dark where no way on is (OB-423): its
/// MoveObject of the host, frozen, to `waypoint` was meant to take the player through `exit`. With a
/// party in, the party goes through that exit there, as through one such a move lands the host on.
/// Alone the solo game's: the player stays where the script left it.
struct ScriptTrap {
    std::string_view map;      ///< (any case)
    std::string_view waypoint; ///< the script's move's
    std::string_view exit;     ///< the way on it meant: an exit's string (any case)
};

constexpr ScriptTrap kScriptTraps[] = {
    // Wiz03C's GotoWiz03b (its ExitTrig, in a corner of the cave the player comes down into): a copy
    // of Wiz03A's, whose waypoint is on a hidden exit to Wiz03B; here it's out in the rock. The way
    // back up to Wiz03B, the two exits by the landing.
    {"wiz03c", "Wiz03bExitWP", "Wiz03b.map:HideoutRearWP"},
};

/// How near the trap's waypoint the moved host is on it (MoveObject puts it there).
constexpr float kTrapNear = 2.0f;

} // namespace

std::string CoopDirector::nameOf(const server::ServerObject& unit) const
{
    std::string name;
    if (unit.player) {
        for (const char16_t c : unit.player->name) {
            name += c < 0x80 ? static_cast<char>(c) : '?';
        }
    }
    return name.empty() ? std::string("A player") : name;
}

void CoopDirector::noticeAll(coopmsg::Notice kind, std::string_view text)
{
    for (const Member& m : party_) {
        notice(m, kind, text);
    }
}

// ---- the exit's gathering

std::vector<server::ServerObject*> CoopDirector::livingUnits()
{
    std::vector<server::ServerObject*> living = partyUnits();
    std::erase_if(living, [](const server::ServerObject* u) { return (u->flags & server::ObjFlag::Dead) != 0; });
    return living;
}

bool CoopDirector::exitTouched(server::Fight& fight, server::ServerObject& exit, server::ServerObject& player)
{
    if (!host_) {
        return true;
    }
    const std::vector<server::ServerObject*> living = livingUnits();
    if (living.size() < 2) {
        gather_.reset();
        return true; // (alone, or the only one alive: as the game)
    }
    const std::uint64_t now = world().ticks;
    // The host put on it by the script a moment ago (a scene's MoveObject onto the map's way on,
    // under black, often out of the map where nobody could walk back from): the party goes at once,
    // as the solo game's host does.
    if (isHostUnit(&player) && scriptMovedAt_ && now - *scriptMovedAt_ <= 2u) {
        partyGoes(fight, nullptr, false); // (the exit's own collide asks the save)
        return true;
    }
    // One gathering at a time, by where it leads (a wide way is often several exits side by side);
    // another way's touch waits for it to be over or given up.
    const std::string to = destination(exit.exitMap);
    if (gather_ && gather_->to != to) {
        return false;
    }
    if (!gather_) {
        gather_ = Gather{};
        gather_->exit = exit.extent;
        gather_->to = to;
        gather_->since = now;
        noticeAll(coopmsg::Notice::Info, nameOf(player) + " is at the way to " + exitMapName(exit.exitMap) +
                                             ". Everyone there, or the party goes in 30 seconds (the host: ?go).");
    }
    gather_->standing[server::ObjectRef{player.extent}] = now;
    gather_->lastTouch = now;
    if (const std::pair<float, float> p{exit.x, exit.y}; std::ranges::find(gather_->at, p) == gather_->at.end()) {
        gather_->at.push_back(p);
    }
    // Everyone alive at it (each touched it and hasn't walked off since: gatherTick), the time's
    // up, or the host says go; never while the host is down or held by a script's freeze.
    const bool all = std::ranges::all_of(living, [&](const server::ServerObject* u) { return gather_->standing.contains(u->extent); });
    const server::ServerObject* host = server::hostPlayer(world());
    const bool hostUp = host && !(host->flags & server::ObjFlag::Dead) && !scriptFrozen(*host);
    if (hostUp && (all || goNow_ || now - gather_->since >= 30u * 30u)) {
        partyGoes(fight, nullptr, false); // (the exit's own collide asks the save)
        return true;
    }
    return false;
}

bool CoopDirector::walkedOff(const server::ServerObject& unit)
{
    server::World& w = world();
    if (!server::mayAct(w, unit, host_->server().clock().frame()) ||
        (unit.flags & (server::ObjFlag::Falling | server::ObjFlag::InHole | server::ObjFlag::OnObject)) || unit.z > 0.0f) {
        return false; // (held, or carried: where the way on took it, or will)
    }
    return std::ranges::none_of(gather_->at, [&](const std::pair<float, float>& p) {
        return std::hypot(unit.x - p.first, unit.y - p.second) <= kGatherAway;
    });
}

bool CoopDirector::mayGoThrough(const server::ServerObject& exit)
{
    const server::World& w = world();
    const server::ServerObject* host = server::hostPlayer(world());
    return !exit.gone() && !exit.pendingDelete && !server::saveBusy(w) && !(w.gameFlags & server::GameFlag::Paused) &&
           !((exit.subclasses & 2u) && !w.warpGatesOn) && !exit.exitMap.empty() && exit.exitMap[0] != '\0' && host &&
           !(host->flags & server::ObjFlag::Dead);
}

void CoopDirector::partyGoes(server::Fight& fight, server::ServerObject* exit, bool thaw)
{
    server::World& w = world();
    scriptMovedAt_.reset();
    scriptedExit_ = 0;
    gather_.reset();
    goNow_ = false;
    std::vector<server::ObjectKey> glyphLayers;
    for (server::ServerObject* u : partyUnits()) {
        // A player still talking (frozen in it, so not on the exit) has its talk closed as an
        // answer closes it, without the end function: on the next map nothing could close it.
        if (u->player && u->player->dialogWith != 0) {
            w.broadcasts().push_back({{0xD0, 4}, true, u->extent});
            u->player->dialogWith = 0;
            server::unfreezeUnit(fight, *u, 0);
        }
        if (thaw) {
            server::unfreezeUnit(fight, *u, 1); // (with its creatures, as the map change thaws them)
        }
        if (u->player && u->player->laysGlyphs()) {
            glyphLayers.push_back(u->extent);
        }
    }
    // Each one's glyphs on the ground gone, its count of them down (exitCollide's for the one who
    // touched; the others' would stay counted on the next map).
    if (!glyphLayers.empty()) {
        for (auto& [extent, o] : w.objects()) {
            const server::ObjectKey by = o.owner;
            if (o.typeName == "Glyph" && o.holder == 0 && !o.pendingDelete && std::ranges::find(glyphLayers, by) != glyphLayers.end()) {
                o.pendingDelete = true;
                if (server::ServerObject* layer = w.byExtent(by); layer && layer->player && layer->player->curTraps != 0) {
                    --layer->player->curTraps;
                }
            }
        }
    }
    if (exit) {
        // (exitCollide's go in a solo game, as a co-op campaign is: the WORKING save first, the
        // exit's map after it, at the server's tick end.)
        w.cameByExit = true;
        w.saveName = "WORKING";
        server::requestSave(w, true, exit->extent, 0, fight.frame);
    }
}

server::ServerObject* CoopDirector::exitUnder(const server::ServerObject& unit)
{
    // (Whether it's on or off: ExitCollide doesn't ask, nor does the contact that calls it.)
    for (auto& [extent, o] : world().objects()) {
        if (!(o.classes & things::ObjectClass::Exit) || o.gone() || o.pendingDelete) {
            continue;
        }
        bool on = false;
        if (o.shape == server::ShapeKind::Box) {
            on = server::insideBox(o, o.x, o.y, unit.x, unit.y);
        } else {
            on = std::hypot(unit.x - o.x, unit.y - o.y) <= (o.shape == server::ShapeKind::Circle ? o.radius : 0.0f) + unit.radius;
        }
        if (on) {
            return &o;
        }
    }
    return nullptr;
}

void CoopDirector::readScriptTrap()
{
    trapAt_.reset();
    trapExit_ = 0;
    if (!host_) {
        return;
    }
    server::World& w = world();
    const auto trap = std::ranges::find_if(kScriptTraps, [&](const ScriptTrap& t) { return sameText(w.mapName, t.map); });
    if (trap == std::end(kScriptTraps)) {
        return;
    }
    const server::ServerWaypoint* wp = w.waypointMatching(trap->waypoint);
    if (!wp) {
        return;
    }
    // (Of the exits with its string, the nearest the waypoint: side by side, they're one way.)
    const server::ServerObject* best = nullptr;
    float bestD = 0.0f;
    for (const auto& [extent, o] : w.objects()) {
        if (!(o.classes & things::ObjectClass::Exit) || o.gone() || o.pendingDelete ||
            !sameText(std::string_view(o.exitMap).substr(0, std::string_view(o.exitMap).find('\0')), trap->exit)) {
            continue;
        }
        if (const float d = std::hypot(o.x - wp->x, o.y - wp->y); !best || d < bestD) {
            best = &o;
            bestD = d;
        }
    }
    if (best) {
        trapAt_ = std::pair{wp->x, wp->y};
        trapExit_ = best->extent;
    }
}

server::ServerObject* CoopDirector::trapExitFor(const server::ServerObject& host)
{
    if (!trapAt_ || !scriptFrozen(host) || std::hypot(host.x - trapAt_->first, host.y - trapAt_->second) > kTrapNear) {
        return nullptr;
    }
    server::ServerObject* exit = world().byExtent(trapExit_);
    return exit && !exit->gone() && !exit->pendingDelete ? exit : nullptr;
}

void CoopDirector::gatherTick()
{
    if (!host_) {
        return;
    }
    server::World& w = world();
    // An exit a script put the host on (frozen, as Wiz03A's GotoWiz03b leaves it, it never touches
    // it), or the one a known trap's move meant (kScriptTraps): the party through it at once, thawed
    // (the save waits for a free host).
    if (scriptedExit_) {
        server::ServerObject* exit = w.byExtent(scriptedExit_);
        if (!exit || exit->gone()) {
            scriptedExit_ = 0;
        } else if (mayGoThrough(*exit)) {
            server::Fight f = host_->server().fight();
            partyGoes(f, exit, true);
            return;
        }
    }
    if (!gather_) {
        goNow_ = false;
        return;
    }
    const std::uint64_t now = w.ticks;
    // Who touched it and has walked off since isn't at it any more (a touch puts it back).
    for (auto it = gather_->standing.begin(); it != gather_->standing.end();) {
        const server::ServerObject* u = w.byExtent(it->first);
        if (!u || u->gone() || walkedOff(*u)) {
            it = gather_->standing.erase(it);
        } else {
            ++it;
        }
    }
    const std::vector<server::ServerObject*> living = livingUnits();
    const auto at = [&](const server::ServerObject* u) { return gather_->standing.contains(u->extent); };
    if (std::ranges::none_of(living, at) && now - gather_->lastTouch > 60u) {
        // (Everyone who came to it has walked off, none touching it for two seconds: given up.)
        gather_.reset();
        goNow_ = false;
        noticeAll(coopmsg::Notice::Info, "Nobody's at the way on: the party stays.");
        return;
    }
    // Every living player at it, the time's up, or the host's "?go": the party goes (at a solid
    // exit, a shaft or a pit's landing nobody may be touching it just now).
    if (!living.empty() && (std::ranges::all_of(living, at) || goNow_ || now - gather_->since >= 30u * 30u)) {
        server::ServerObject* exit = w.byExtent(gather_->exit);
        if (!exit || exit->gone()) {
            gather_.reset(); // (its exit gone with the map's change of it: nothing to take)
            goNow_ = false;
            return;
        }
        // (Not while a script's freeze holds the host: its scene's end, or its move onto a way on
        // above, decides.)
        const server::ServerObject* host = server::hostPlayer(w);
        if (mayGoThrough(*exit) && host && !scriptFrozen(*host)) {
            server::Fight f = host_->server().fight();
            partyGoes(f, exit, false);
            return;
        }
    }
    if (!gather_->warned && now - gather_->since >= 20u * 30u) {
        gather_->warned = true;
        noticeAll(coopmsg::Notice::Info, "The party goes on in 10 seconds.");
    }
}

// ---- deaths and respawns

bool CoopDirector::playerDeath(server::Fight& fight, server::ServerObject& player, server::DeathStep step)
{
    if (!host_ || party_.size() < 2) {
        return true; // (alone: the solo game's death)
    }
    if (step == server::DeathStep::DeathState) {
        // (The map's PlayerDeath functions: the party's loss alone, once: the last one's.)
        if (wipe_ && player.extent == wipeBy_ && !wipeRan_) {
            wipeRan_ = true;
            return true;
        }
        return false;
    }
    const bool othersAlive = std::ranges::any_of(partyUnits(), [&](const server::ServerObject* u) {
        return u != &player && !(u->flags & server::ObjFlag::Dead);
    });
    if (!othersAlive) {
        // Every one dead at once: the party's lost, as the game's death (the save dropped, the
        // PlayerDeath functions: DeathScreen, the host's load menu).
        wipe_ = true;
        wipeBy_ = player.extent;
        wipeRan_ = false;
        downs_.clear();
        for (const Member& m : party_) {
            if (!m.host) {
                notice(m, coopmsg::Notice::Info, "The party has fallen. Waiting for the host to load a saved game.");
            }
        }
        // The game's loss: its save off (Plr_Die's), but what the last one holds kept (the guests'
        // characters aren't in the host's load: dropped, they'd be gone for good).
        server::requestSave(world(), false, 0, 0, fight.frame);
        return false;
    }
    if (const Member* m = memberOfUnit(player)) {
        downs_[m->peer] = world().ticks + 10u * 30u;
        notice(*m, coopmsg::Notice::Info, "You fell: you'll be back by the party in 10 seconds.");
        for (const Member& o : party_) {
            if (o.peer != m->peer) {
                notice(o, coopmsg::Notice::Info, nameOf(player) + " fell, and will be back in 10 seconds.");
            }
        }
    }
    rosterDirty_ = true;
    return false; // (the pending save kept)
}

void CoopDirector::respawnPlayer(server::ServerObject& unit)
{
    server::World& w = world();
    server::Fight f = host_->server().fight();
    f.respawn(unit, true); // (the game's respawn, keeping what it holds)
    // Its weapon and armour drawn on it again: every client's MSG_PLAYER_DIED took them off it
    // (outside a quest), and the respawn's MSG_PLAYER_RESPAWN puts nothing back.
    if (f.things) {
        server::Items items{w, *f.things, f.modifiers, f.rng, f.frame};
        for (const std::uint32_t e : keysOf(unit.inventory)) {
            if (const server::ServerObject* o = w.byExtent(e); o && (o->flags & server::ObjFlag::Equipped)) {
                items.reportEquip(unit, *o);
            }
        }
    }
    // By the checkpoint (where the host came into the map), else by the host.
    std::optional<std::pair<float, float>> at = checkpoint_;
    if (const server::ServerObject* h = server::hostPlayer(w); !at && h && h != &unit) {
        at = std::pair{h->x, h->y};
    }
    if (at) {
        const auto [x, y] = server::clearPointNear(w, rng_, at->first, at->second, 50.0f);
        server::setPosition(w, unit, x, y, host_->server().clock().frame());
    }
    rosterDirty_ = true;
}

void CoopDirector::respawnDue()
{
    if (wipe_ || downs_.empty()) {
        return;
    }
    const std::uint64_t now = world().ticks;
    for (auto it = downs_.begin(); it != downs_.end();) {
        server::ServerObject* u = host_->unitOf(it->first);
        if (!u || !u->player || !(u->flags & server::ObjFlag::Dead)) {
            it = downs_.erase(it); // (gone, or up again some other way)
            continue;
        }
        if (now < it->second) {
            ++it;
            continue;
        }
        respawnPlayer(*u);
        if (const Member* m = memberOfUnit(*u)) {
            notice(*m, coopmsg::Notice::Info, "Back with the party.");
        }
        it = downs_.erase(it);
    }
}

// ---- revisited maps

void CoopDirector::readPristine(server::Server& server)
{
    pristine_.clear();
    wallIds_.clear();
    goneWalls_.clear();
    const server::World& w = server.world();
    const std::filesystem::path mapsDir = host_ ? host_->mapsDir() : std::filesystem::path{};
    if (mapsDir.empty() || w.mapName.empty()) {
        return;
    }
    const std::filesystem::path own = resolveNoCase(mapsDir / w.mapName / (w.mapName + ".map"));
    std::error_code ec;
    if (!std::filesystem::exists(own, ec)) {
        return;
    }
    try {
        const server::World pristine = server::World::load(map::loadMapFile(own), server.things(), server::GameFlag::Solo);
        for (const auto& [e, o] : pristine.objects()) {
            if ((o.classes & kClientLoaded) && o.fileExtent && o.fileExtent.number() <= 0x7FFFu) {
                pristine_.push_back(o.fileExtent.number());
            }
        }
        // The walls: a saved copy leaves out those broken before (map_state's walls), so the
        // copy's ids (counted from its file, as Map_BreakWallById names them) aren't the file's.
        // Each of ours by its cell to the file's id; the file's breakable ones the copy lacks.
        for (const auto& [cell, wall] : w.walls()) {
            if (const auto it = pristine.walls().find(cell);
                it != pristine.walls().end() && it->second.destructible && it->second.wallId != wall.wallId) {
                wallIds_[wall.wallId] = it->second.wallId;
            }
        }
        for (const auto& cell : pristine.breakableWalls()) {
            if (!w.walls().contains(cell)) {
                if (const auto it = pristine.walls().find(cell); it != pristine.walls().end()) {
                    goneWalls_.push_back(it->second.wallId);
                }
            }
        }
    } catch (const std::exception&) {
        pristine_.clear(); // (an unreadable file: nothing to put right)
        wallIds_.clear();
        goneWalls_.clear();
    }
}

std::optional<std::vector<std::uint8_t>> CoopDirector::forPeer(std::uint8_t peer, std::span<const std::uint8_t> m)
{
    // MSG_DESTROY_WALL to a guest: the wall by the id its own file gives it.
    if (wallIds_.empty() || m.size() < 3 || m[0] != 0x3A) {
        return std::nullopt;
    }
    const auto member = std::ranges::find_if(party_, [&](const Member& x) { return x.peer == peer; });
    if (member == party_.end() || member->host) {
        return std::nullopt;
    }
    const auto id = static_cast<std::uint16_t>(m[1] | m[2] << 8);
    if (auto& own = fileNumbered_[peer]; own.contains(id)) {
        own.erase(own.find(id)); // (ours, by the file's id already: clientReady's)
        return std::nullopt;
    }
    const auto it = wallIds_.find(id);
    if (it == wallIds_.end()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out(m.begin(), m.end());
    out[1] = static_cast<std::uint8_t>(it->second);
    out[2] = static_cast<std::uint8_t>(it->second >> 8);
    return out;
}

void CoopDirector::findHiddenStatics()
{
    hidden_.clear();
    if (pristine_.empty()) {
        return;
    }
    std::set<std::uint32_t> inWorld;
    for (const auto& [e, o] : world().objects()) {
        if ((o.classes & kClientLoaded) && !o.gone() && !o.pendingDelete && o.fileExtent) {
            inWorld.insert(o.fileExtent.number());
        }
    }
    for (const std::uint32_t n : pristine_) {
        if (!inWorld.contains(n)) {
            hidden_.push_back(static_cast<std::uint16_t>(n | 0x8000u));
        }
    }
}

void CoopDirector::clientReady(server::GameHost& host, std::uint8_t peer)
{
    host_ = &host;
    const auto it = std::ranges::find_if(party_, [&](const Member& m) { return m.peer == peer; });
    if (it == party_.end()) {
        return;
    }
    tellTeam(*it);
    if (it->host) {
        return;
    }
    findHiddenStatics();
    // (CliNet_OnPacket's MSG_DESTROY_OBJECT: a net code with the high bit names a static drawable
    // by its map number, set inactive.)
    for (const std::uint16_t code : hidden_) {
        tell(*it, {0x31, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8)});
    }
    // (The walls broken before the copy was saved: MSG_DESTROY_WALL by the file's ids.)
    for (const std::uint16_t id : goneWalls_) {
        fileNumbered_[peer].insert(id);
        tell(*it, {0x3A, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8)});
    }
}

void CoopDirector::tellTeam(const Member& ready)
{
    // (At its ready the other members' MSG_NEW_PLAYER are in, as is its own at theirs: the team
    // message finds the player it names. Stock clients are told nothing new.)
    server::World& w = world();
    const auto membership = [&](const Member& m) -> std::optional<std::vector<std::uint8_t>> {
        const server::ServerObject* u = host_->unitOf(m.peer);
        if (!u || !u->player || u->team == 0 || !server::inTeam(w, *u, u->team)) {
            return std::nullopt;
        }
        const server::ServerTeam* team = server::teamById(w, u->team);
        return team ? std::optional(server::teamMemberMessage(*team, *u)) : std::nullopt;
    };
    const auto own = membership(ready);
    for (const Member& m : party_) {
        if (m.peer == ready.peer) {
            continue;
        }
        if (ready.identified) {
            if (auto theirs = membership(m)) {
                host_->sendToIdentified(ready.peer, std::move(*theirs));
            }
        }
        const auto in = host_->players().find(m.peer);
        if (own && m.identified && in != host_->players().end() && in->second.ready) {
            host_->sendToIdentified(m.peer, *own);
        }
    }
}

} // namespace nox::coop
