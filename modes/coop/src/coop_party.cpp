// The co-op campaign's exits, deaths and revisited maps (coop.hpp): the party gathers at an exit;
// only a party wipe loses (the others respawn by the checkpoint); a guest's view of a map it loads
// from its own file is put right where the world (a visited map's saved copy) differs.
#include "noxcore/coop/coop.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <set>
#include <string>

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

/// Where an exit leads, as one gathering's key (its map, any case).
std::string destination(std::string_view exitMap)
{
    std::string s = exitMapName(exitMap);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

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

bool CoopDirector::exitTouched(server::Fight& fight, server::ServerObject& exit, server::ServerObject& player)
{
    if (!host_) {
        return true;
    }
    std::vector<server::ServerObject*> living;
    for (server::ServerObject* u : partyUnits()) {
        if (!(u->flags & server::ObjFlag::Dead)) {
            living.push_back(u);
        }
    }
    if (living.size() < 2) {
        gather_.reset();
        return true; // (alone, or the only one alive: as the game)
    }
    const std::uint64_t now = world().ticks;
    // One gathering at a time, by where it leads (a wide way is often several exits side by side);
    // another way's touch waits for it to be over or given up.
    const std::string to = destination(exit.exitMap);
    if (gather_ && gather_->to != to) {
        return false;
    }
    if (!gather_) {
        gather_ = Gather{server::ObjectRef{exit.extent}, to, now, now, {}, false};
        noticeAll(coopmsg::Notice::Info, nameOf(player) + " is at the way to " + exitMapName(exit.exitMap) +
                                             ". Everyone there, or the party goes in 30 seconds (the host: ?go).");
    }
    gather_->standing[server::ObjectRef{player.extent}] = now;
    gather_->lastTouch = now;
    // Everyone alive on it now (touched this tick or the last), the time's up, or the host says go.
    const bool all = std::ranges::all_of(living, [&](const server::ServerObject* u) {
        const auto it = gather_->standing.find(u->extent);
        return it != gather_->standing.end() && now - it->second <= 1u;
    });
    // The host put on it by the script a moment ago (a scene's MoveObject onto the map's way on,
    // under black, often out of the map where nobody could walk back from): the party goes at once,
    // as the solo game's host does.
    const bool scripted = isHostUnit(&player) && scriptMovedAt_ && now - *scriptMovedAt_ <= 2u;
    if (all || goNow_ || scripted || now - gather_->since >= 30u * 30u) {
        scriptMovedAt_.reset();
        gather_.reset();
        goNow_ = false;
        // A player still talking (frozen in it, so not on the exit) has its talk closed as an
        // answer closes it, without the end function: on the next map nothing could close it.
        for (server::ServerObject* u : partyUnits()) {
            if (u->player && u->player->dialogWith != 0) {
                world().broadcasts().push_back({{0xD0, 4}, true, u->extent});
                u->player->dialogWith = 0;
                server::unfreezeUnit(fight, *u, 0);
            }
        }
        return true;
    }
    return false;
}

void CoopDirector::gatherTick()
{
    if (!gather_) {
        goNow_ = false;
        return;
    }
    const std::uint64_t now = world().ticks;
    if (now - gather_->lastTouch > 60u) {
        // (Nobody on it for two seconds: given up.)
        gather_.reset();
        goNow_ = false;
        noticeAll(coopmsg::Notice::Info, "Nobody's at the way on: the party stays.");
        return;
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
    if (it == party_.end() || it->host) {
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

} // namespace nox::coop
