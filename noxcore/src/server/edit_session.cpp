// The map editor's session on the host (edit_session.hpp).
#include "noxcore/server/edit_session.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/mapedit/mod_files.hpp"
#include "noxcore/mapedit/pending.hpp"
#include "noxcore/mapedit/script_doc.hpp"
#include "noxcore/mapedit/session_doc.hpp"
#include "noxcore/mapedit/sidecar.hpp"
#include "noxcore/mapedit/writer.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/play_from_here.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/script_reload.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/update_writer.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <tuple>
#include <variant>

namespace nox::server {

namespace me = mapedit;
namespace ss = mapedit::session;

namespace {

bool hasObjectParts(const me::Op& op)
{
    return std::ranges::any_of(op.parts, [](const me::OpPart& p) {
        return std::holds_alternative<me::AddObjects>(p) || std::holds_alternative<me::RemoveObjects>(p) ||
               std::holds_alternative<me::SetObjects>(p) || std::holds_alternative<me::MoveObjects>(p);
    });
}

bool hasScriptSource(const me::Op& op)
{
    return std::ranges::any_of(op.parts, [](const me::OpPart& p) { return std::holds_alternative<me::SetScriptSource>(p); });
}

/// A document object and everything under it the same in both copies (or in neither).
bool sameSubtree(const me::MapDocument& a, const me::MapDocument& b, me::ObjId id, int depth = 0)
{
    const auto ia = a.objects.find(id);
    const auto ib = b.objects.find(id);
    if (ia == a.objects.end() || ib == b.objects.end()) {
        return (ia == a.objects.end()) == (ib == b.objects.end());
    }
    if (!(ia->second == ib->second) || depth > 64) {
        return false;
    }
    const std::vector<me::ObjId> ca = a.childrenOf(id);
    if (ca != b.childrenOf(id)) {
        return false;
    }
    return std::ranges::all_of(ca, [&](me::ObjId c) { return sameSubtree(a, b, c, depth + 1); });
}

/// An editor's removal of a live object (a delete, or the first half of a change or of a door's
/// move): the game's delayed delete, with what it holds deleted along with it (the document's
/// object goes with its inventory; the delete alone would have the free drop it all where the
/// holder stood, dropAllItems, and a change would then leave the old contents beside the new
/// object's), and a door's leaf taken off its wall cell (a fresh load of the edited map attaches
/// none there: the published map's wall stays, the blank one made for the door goes). (Doors are
/// held for a publish, as every door type is IMMOBILE: the door part is for one that isn't.)
void removeLive(World& w, const me::MapDocument& published, ServerObject& o)
{
    std::vector<std::uint32_t> todo(o.inventory.begin(), o.inventory.end());
    while (!todo.empty()) {
        const std::uint32_t e = todo.back();
        todo.pop_back();
        ServerObject* item = w.byExtent(e);
        if (!item || item->pendingDelete) {
            continue;
        }
        item->pendingDelete = true; // (freed with its holder, as the free's kept things: no word to anyone)
        todo.insert(todo.end(), item->inventory.begin(), item->inventory.end());
    }
    if ((o.classes & things::ObjectClass::Door) && o.holder == 0) {
        if (const auto cell = w.doorCell(o.extent)) {
            const me::Cell key{static_cast<std::uint8_t>(cell->first), static_cast<std::uint8_t>(cell->second)};
            if (published.walls.contains(key)) {
                if (WallCell* c = w.wall(cell->first, cell->second)) {
                    c->door = 0;
                }
            } else {
                w.walls().erase(*cell);
            }
        }
    }
    deleteObject(w, o);
}

} // namespace

std::uint32_t EditSessionDirector::sandboxModeFor(std::uint32_t mapTypes)
{
    const std::uint32_t modes = mapToGameFlags(mapTypes) & 0x570u; // (arena, elimination, CTF, KotR, flagball)
    if (modes == 0 || (modes & 0x100u)) {
        return 0x100u;
    }
    for (const std::uint32_t m : {0x400u, 0x20u, 0x10u, 0x40u}) {
        if (modes & m) {
            return m;
        }
    }
    return 0x100u;
}

EditSessionDirector::EditSessionDirector(const things::ThingDb& things, Options options)
    : things_(things)
    , options_(std::move(options))
{
    if (me::isStockMapName(options_.mapName)) {
        throw std::runtime_error("an edit session never publishes under a stock map's name: " + options_.mapName);
    }
    const auto path = options_.mapsDir / options_.mapName / (options_.mapName + ".map");
    me::MapDocument opened = me::MapDocument::load(path, things_);
    opened.origin.name = options_.mapName;
    opened.origin.stock = false;
    base_ = me::canonicalSessionDocument(opened, things_);
    working_ = base_;
    baseBytes_ = me::encodeSessionDocument(base_, things_);
    sandboxMode_ = sandboxModeFor(base_.info.gameFlags);
    loadMod();
}

std::uint8_t EditSessionDirector::flags() const
{
    std::uint8_t f = 0;
    if (options_.playersCantDie) f |= ss::kPlayersCantDie;
    if (options_.liveView) f |= ss::kLiveView;
    if (options_.liveCreatures) f |= ss::kLiveCreatures;
    if (options_.autoPublish) f |= ss::kAutoPublish;
    if (playTest_) f |= ss::kPlayTestRunning;
    return f;
}

std::optional<std::uint32_t> EditSessionDirector::liveExtent(me::ObjId id) const
{
    const auto it = live_.find(id);
    if (it == live_.end()) {
        return std::nullopt;
    }
    return it->second;
}

ss::Role EditSessionDirector::roleOf(std::uint8_t peer) const
{
    const auto it = peers_.find(peer);
    return it == peers_.end() ? ss::Role::Player : it->second.role;
}

std::vector<ss::RosterEntry> EditSessionDirector::roster() const
{
    std::vector<ss::RosterEntry> out;
    for (const auto& [peer, p] : peers_) {
        out.push_back({peer, p.joined ? p.role : ss::Role::Player, p.colour, p.stock, p.name});
    }
    return out;
}

bool EditSessionDirector::isOwner(GameHost& host, std::uint8_t peer) const
{
    if (host.isHostPeer(peer)) {
        return true;
    }
    const auto it = peers_.find(peer);
    return it != peers_.end() && !options_.ownerName.empty() && it->second.name == options_.ownerName;
}

// --- Sending -------------------------------------------------------------------------------------

void EditSessionDirector::send(GameHost& host, std::uint8_t peer, std::uint8_t sub, std::span<const std::uint8_t> body)
{
    for (auto& m : ss::frame(sub, body, transfer_)) {
        host.sendToIdentified(peer, std::move(m));
    }
}

void EditSessionDirector::sendJoined(GameHost& host, std::uint8_t sub, std::span<const std::uint8_t> body, std::optional<std::uint8_t> except,
                                     bool editorsOnly)
{
    const auto frames = ss::frame(sub, body, transfer_);
    for (const auto& [peer, p] : peers_) {
        if (!p.joined || (except && peer == *except) || (editorsOnly && p.role < ss::Role::Editor)) {
            continue;
        }
        for (const auto& m : frames) {
            host.sendToIdentified(peer, m);
        }
    }
}

void EditSessionDirector::sendRole(GameHost& host, std::uint8_t peer, const std::string& text)
{
    send(host, peer, ss::kRole, ss::encode(ss::RoleMsg{peers_.at(peer).role, text}));
}

void EditSessionDirector::requestClosed(GameHost& host, std::uint8_t peer, const std::string& name, const std::string& reason)
{
    sendJoined(host, ss::kEditRequest, ss::encode(ss::EditRequest{peer, name, reason, false}), std::nullopt, true);
}

void EditSessionDirector::dropRequest(GameHost& host, std::uint8_t peer, const std::string& text)
{
    Peer& p = peers_.at(peer);
    const std::string reason = *p.request;
    p.request.reset();
    sendRole(host, peer, text);
    requestClosed(host, peer, p.name, reason);
}

void EditSessionDirector::hello(GameHost& host, std::uint8_t peer)
{
    ss::Hello h;
    h.session = options_.sessionId;
    h.version = version_;
    h.editMode = options_.editMode;
    h.flags = flags();
    h.you = peer;
    h.map = options_.mapName;
    send(host, peer, ss::kHello, ss::encode(h));
}

void EditSessionDirector::broadcastHello(GameHost& host)
{
    for (const auto& [peer, p] : peers_) {
        if (p.greeted) {
            hello(host, peer);
        }
    }
}

void EditSessionDirector::broadcastLocks(GameHost& host)
{
    sendJoined(host, ss::kLockTable, locks_.serialize());
}

void EditSessionDirector::logLine(GameHost& host, const std::string& line)
{
    if (options_.log) {
        options_.log(line);
    }
    sendJoined(host, ss::kLog, ss::encodeText(line));
}

// --- Messages ------------------------------------------------------------------------------------

void EditSessionDirector::onMessage(GameHost&, std::uint8_t peer, std::span<const std::uint8_t> m)
{
    inbox_.emplace_back(peer, std::vector<std::uint8_t>(m.begin(), m.end()));
}

bool EditSessionDirector::tick(GameHost& host, std::uint32_t nowMs)
{
    nowMs_ = nowMs;
    const std::uint32_t versionBefore = version_;
    refreshRoster(host);
    if (needRebind_) {
        needRebind_ = false;
        rebind(host);
    }
    while (!inbox_.empty()) {
        auto [peer, bytes] = std::move(inbox_.front());
        inbox_.pop_front();
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            continue;
        }
        if (auto m = it->second.in.feed(bytes)) {
            try {
                handle(host, peer, *m);
            } catch (const std::exception& ex) {
                // (a malformed message from one client: dropped, the session goes on)
                if (options_.log) {
                    options_.log(std::string("a message dropped: ") + ex.what()); // (the host's log adds "session: ")
                }
            }
        }
    }
    retryWaiting(host);
    if (hasMod()) {
        pumpMod(host); // (the mod's file transfers, as each member's queue has room)
    }
    // A parked character back in play by the game's own way (Action, a map change's start):
    // parked no more.
    for (auto& [peer, p] : peers_) {
        if (!p.parked) continue;
        const ServerObject* u = host.unitOf(peer);
        if (u && u->player && !u->player->observing) {
            p.parked = false;
            sendPark(host, peer, {});
        }
    }
    // Idle locks let go.
    if (!locks_.expire(nowMs_, options_.lockTimeoutMs).empty()) {
        broadcastLocks(host);
    }
    // Who's editing, and how (the pencil), when it changed.
    refreshEditors(host);
    // Requests nobody answered.
    for (auto& [peer, p] : peers_) {
        if (p.request && nowMs_ - p.requestAt >= options_.requestTimeoutMs) {
            dropRequest(host, peer, "No answer");
        }
    }
    // A play-test round's own end (its mode's): back to the sandbox.
    if (playTest_ && (host.server().world().gameFlags & GameFlag::GameOver)) {
        endPlayTest(host);
    }
    const std::uint32_t liveEvery =
        options_.liveCreatures ? std::max(options_.liveIntervalMs, options_.liveCreaturesIntervalMs) : options_.liveIntervalMs;
    if (options_.liveView && nowMs_ - lastLiveMs_ >= liveEvery) {
        lastLiveMs_ = nowMs_;
        sendLive(host);
    }
    // The automatic object publish: 2 s after the last object change, at most 10 s after the first
    // while they keep coming.
    if (objectsDirty_ && options_.autoPublish && !playTest_ &&
        (nowMs_ - objectsChangedAt_ >= options_.autoPublishQuietMs || nowMs_ - objectsDirtySince_ >= options_.autoPublishMaxMs)) {
        publish(host, false);
    }
    return version_ != versionBefore;
}

void EditSessionDirector::refreshRoster(GameHost& host)
{
    // Who left: their locks and requests go (release on disconnect).
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!host.players().contains(it->first)) {
            if (!locks_.heldBy(it->first).empty()) {
                locks_.releaseAll(it->first);
                broadcastLocks(host);
            }
            if (it->second.request) {
                requestClosed(host, it->first, it->second.name, *it->second.request);
            }
            // The mod files it held let go too; the versions it wrote shown as the host's (its peer
            // id may be a newcomer's next).
            releaseModHolds(it->first);
            for (auto& [path, e] : mod_) {
                if (e.author == it->first) {
                    e.author = 0;
                    modListDirty();
                }
            }
            const std::string name = it->second.name;
            const bool joined = it->second.joined;
            it = peers_.erase(it);
            if (joined && options_.log) {
                options_.log(name + " left the session");
            }
        } else {
            ++it;
        }
    }
    for (const auto& [peer, hp] : host.players()) {
        auto [it, fresh] = peers_.try_emplace(peer);
        Peer& p = it->second;
        if (fresh) {
            std::set<std::uint8_t> used;
            for (const auto& [q, other] : peers_) {
                if (q != peer) used.insert(other.colour);
            }
            while (used.contains(p.colour) && p.colour < 15) ++p.colour;
        }
        p.name = hp.name;
        p.stock = !host.identified(peer);
        if (!p.greeted && !p.stock && hp.ready) {
            p.greeted = true;
            hello(host, peer);
        }
    }
    auto now = roster();
    if (now != rosterSent_) {
        rosterSent_ = now;
        sendJoined(host, ss::kRoster, ss::encode(now));
    }
}

std::vector<ss::EditorState> EditSessionDirector::editorStates(GameHost& host) const
{
    std::vector<ss::EditorState> out;
    for (const auto& [peer, p] : peers_) {
        if (!p.joined || p.role < ss::Role::Editor) continue;
        ss::EditorState e;
        e.peer = peer;
        const ServerObject* u = host.unitOf(peer);
        if (u && !u->gone()) {
            e.netCode = static_cast<std::uint16_t>(u->extent);
        }
        if (u && !u->gone() && u->player && !u->player->observing) {
            e.activity = ss::Activity::Playing; // (the game in front: the character in play)
        } else if (nowMs_ - p.activeAt >= options_.idleMs) {
            e.activity = ss::Activity::Idle;
        }
        out.push_back(e);
    }
    return out;
}

void EditSessionDirector::refreshEditors(GameHost& host)
{
    auto now = editorStates(host);
    if (now != editorsSent_) {
        editorsSent_ = now;
        sendJoined(host, ss::kEditors, ss::encode(now));
    }
}

void EditSessionDirector::handle(GameHost& host, std::uint8_t peer, const ss::Message& m)
{
    Peer& p = peers_.at(peer);
    if (m.sub == ss::kJoin) {
        // (What it takes: a client before features sends no body, and never gets the mod's messages.)
        p.modFiles = (ss::decodeJoin(m.body) & ss::kFeatureModFiles) != 0;
        if (!p.modFiles) {
            p.mod = {}; // (a Join again without it: nothing more of the mod goes to it)
        }
        join(host, peer);
        return;
    }
    if (!p.joined) {
        return; // (everything else is for members)
    }
    p.activeAt = nowMs_; // (anything a member sends is them at work: the idle clock, Editors)
    const bool owner = p.role == ss::Role::Owner;
    switch (m.sub) {
    case ss::kOpMsg:
        onOp(host, peer, ss::decodeOp(m.body));
        break;
    case ss::kLock:
        onLock(host, peer, ss::decodeLock(m.body));
        break;
    case ss::kPresence:
        if (p.role >= ss::Role::Editor) {
            ss::Presence pr = ss::decodePresence(m.body);
            pr.peer = peer;
            locks_.touch(peer, nowMs_);
            // (Latest wins, unreliably: one frame, as small as it is.)
            const auto frames = ss::frame(ss::kPresence, ss::encode(pr), transfer_);
            for (const auto& [q, other] : peers_) {
                if (q != peer && other.joined) {
                    for (const auto& f : frames) {
                        host.sendToIdentifiedUnreliable(q, f);
                    }
                }
            }
        }
        break;
    case ss::kRequestEdit:
        onRequestEdit(host, peer, ss::decodeText(m.body));
        break;
    case ss::kAnswer:
        if (owner) onAnswer(host, peer, ss::decodeAnswer(m.body));
        break;
    case ss::kRevoke:
        if (owner) {
            const std::uint32_t target = ss::decodeU32(m.body);
            if (target <= 0xFF && peers_.contains(static_cast<std::uint8_t>(target))) {
                revoke(host, static_cast<std::uint8_t>(target), "the host revoked it");
            }
        }
        break;
    case ss::kForceRelease:
        if (owner) {
            locks_.forceRelease(ss::decodeKey(m.body));
            broadcastLocks(host);
        }
        break;
    case ss::kPublish:
        if (owner) {
            const bool full = ss::decodeU32(m.body) == 0;
            if (publish(host, full) && !full && !held_.empty()) {
                logLine(host, std::to_string(held_.size()) + (held_.size() == 1 ? " change" : " changes") +
                                  " to scenery, doors or furniture wait for a full publish (every client draws those from its map)");
            }
        }
        break;
    case ss::kScriptReload:
        if (p.role >= ss::Role::Editor) reloadScript(host, peer, ss::decodeU32(m.body));
        break;
    case ss::kPlayTest:
        if (owner) {
            const ss::PlayTest t = ss::decodePlayTest(m.body);
            if (t.start) {
                startPlayTest(host, t.mode, t.playersCanDie);
            } else {
                endPlayTest(host);
            }
        }
        break;
    case ss::kSettings:
        if (owner) {
            const ss::Settings s = ss::decodeSettings(m.body);
            setSettings(host, s.editMode, s.flags);
        }
        break;
    case ss::kPark:
        onPark(host, peer, ss::decodePark(m.body));
        break;
    case ss::kJumpIn:
        onJumpIn(host, peer, ss::decodeU32(m.body));
        break;
    // The mod project's (only from members that said they take it, and only with a mod).
    case ss::kModFile:
        if (p.modFiles && hasMod()) onModFile(host, peer, ss::decodeModFile(m.body));
        break;
    case ss::kModLock:
        if (p.modFiles && hasMod()) onModLock(host, peer, ss::decodeModLock(m.body));
        break;
    case ss::kModPublish:
        if (owner && p.modFiles && hasMod()) publishMod(host);
        break;
    default:
        break;
    }
}

// --- Parking and jumping in ----------------------------------------------------------------------

namespace {
constexpr const char* kInWall = "That spot is inside a wall";
constexpr const char* kOffMap = "That spot is off the map";
} // namespace

bool EditSessionDirector::parked(std::uint8_t peer) const
{
    const auto it = peers_.find(peer);
    return it != peers_.end() && it->second.parked;
}

void EditSessionDirector::sendPark(GameHost& host, std::uint8_t peer, const std::string& text)
{
    ss::Park m;
    m.on = parked(peer);
    m.text = text;
    send(host, peer, ss::kPark, ss::encode(m));
}

void EditSessionDirector::onPark(GameHost& host, std::uint8_t peer, const ss::Park& m)
{
    Peer& p = peers_.at(peer);
    World& w = host.server().world();
    ServerObject* u = host.unitOf(peer);
    if (!u || !u->player || u->gone()) {
        if (m.on) {
            // (Not in the game yet: it comes in observing, as any joining player; parked so.)
            p.parked = true;
            p.parkSpot = false;
            sendPark(host, peer, {});
        } else {
            sendPark(host, peer, "Your character isn't in the game");
        }
        return;
    }
    if (playTest_) {
        sendPark(host, peer, "A play-test is running: your character stays in it");
        return;
    }
    if (m.on) {
        // The Edit side: the character out of harm's way as the stock observer is (goObserver:
        // invisible, not colliding, free to rejoin with Action); where it stood kept.
        if (!u->player->observing) {
            p.parkSpot = true;
            p.parkX = u->x;
            p.parkY = u->y;
            Fight f = host.server().fight();
            if (!goObserver(f, *u, true, true)) {
                p.parkSpot = false;
                sendPark(host, peer, "Your character can't stand aside now");
                return;
            }
            ++stats_.parks;
        } else if (!p.parked) {
            p.parkSpot = false; // (it was watching already: no spot of its own)
        }
        p.parked = true;
        sendPark(host, peer, {});
        return;
    }
    // Back in play: where the editor's camera is; a spot no one can stand on (in a wall, off the
    // map), where it stood aside; never in play here, at a start as the stock observer joins.
    if (!u->player->observing && !m.spot) {
        p.parked = false;
        sendPark(host, peer, {});
        return;
    }
    std::string why;
    if (m.spot) {
        if (unpark(host, peer, m.x, m.y, why)) {
            sendPark(host, peer, {});
            return;
        }
        if (why != kInWall && why != kOffMap) {
            sendPark(host, peer, why);
            return;
        }
    }
    std::string why2;
    const bool ok = p.parkSpot ? unpark(host, peer, p.parkX, p.parkY, why2) : [&] {
        const auto [sx, sy] = findPlayerStart(w, u, host.server().fight().rng);
        return unpark(host, peer, sx, sy, why2);
    }();
    sendPark(host, peer, !ok ? why2 : why.empty() ? std::string() : why + ": back where you stood aside");
}

bool EditSessionDirector::unpark(GameHost& host, std::uint8_t peer, float x, float y, std::string& why)
{
    Peer& p = peers_.at(peer);
    World& w = host.server().world();
    ServerObject* u = host.unitOf(peer);
    if (!u || !u->player || u->gone()) {
        why = "Your character isn't in the game";
        return false;
    }
    if (u->flags & ObjFlag::Dead) {
        why = "Your character is dead: it comes back when it respawns";
        return false;
    }
    if (u->player->statusFlags & 0x20u) {
        why = "Your character is out of this round";
        return false;
    }
    // On the map and not inside a wall the game has (where the game allows it).
    constexpr float kCell = 23.0f;
    if (!(x >= kCell && y >= kCell && x < 255 * kCell && y < 255 * kCell)) {
        why = kOffMap;
        return false;
    }
    if (!standableSpot(w, x, y)) {
        // In a wall (a door's leaf, a window: any wall cell), or off the floor: the nearest spot a
        // player can stand on and walk to, on the point's side first (Play's camera, Play From
        // Here's cursor, a jump in beside a player in a doorway). Where a character in play
        // stands counts as a place it can walk from.
        const std::pair<float, float> own{u->x, u->y};
        const auto open =
            nearestOpenSpot(w, x, y, 6, u->player->observing ? std::span<const std::pair<float, float>>() : std::span(&own, 1));
        if (!open) {
            why = kInWall;
            return false;
        }
        std::tie(x, y) = *open;
    }
    Fight f = host.server().fight();
    if (u->player->observing) {
        // (Plr_UpdateObserver's rejoin; then the spot asked for instead of a start.)
        rejoinFromObserving(f, *u);
    }
    placePlayerAt(w, *u, x, y, f.frame);
    p.parked = false;
    return true;
}

void EditSessionDirector::onJumpIn(GameHost& host, std::uint8_t peer, std::uint32_t target)
{
    World& w = host.server().world();
    if (playTest_) {
        sendPark(host, peer, "A play-test is running: jump in from the game");
        return;
    }
    const ServerObject* t = nullptr;
    for (const auto& [q, hp] : host.players()) {
        if (hp.extent == target && q != peer) t = w.byExtent(hp.extent);
    }
    if (!t || t->gone() || (t->flags & ObjFlag::Dead)) {
        sendPark(host, peer, "That player isn't in the game now");
        return;
    }
    std::string why;
    if (!unpark(host, peer, t->x, t->y, why)) {
        sendPark(host, peer, why);
        return;
    }
    ++stats_.jumps;
    sendPark(host, peer, {});
}

// --- Problems --------------------------------------------------------------------------------------

void EditSessionDirector::problem(GameHost& host, std::uint32_t key, const std::string& text)
{
    if (text.empty()) {
        if (problems_.erase(key) == 0) return;
        sendJoined(host, ss::kProblem, ss::encode(ss::Problem{key, false, {}}));
        return;
    }
    const ss::Problem pr{key, true, text};
    if (const auto it = problems_.find(key); it != problems_.end() && it->second == pr) {
        return;
    }
    problems_[key] = pr;
    sendJoined(host, ss::kProblem, ss::encode(pr));
    if (options_.log) {
        options_.log("session problem: " + text);
    }
}

void EditSessionDirector::alwaysChanged()
{
    if (options_.alwaysEditorsChanged) {
        options_.alwaysEditorsChanged(options_.alwaysEditors);
    }
}

void EditSessionDirector::join(GameHost& host, std::uint8_t peer)
{
    Peer& p = peers_.at(peer);
    if (p.stock) {
        return;
    }
    p.activeAt = nowMs_;
    if (!p.joined) {
        p.joined = true;
        if (isOwner(host, peer)) {
            p.role = ss::Role::Owner;
        } else if (std::ranges::find(options_.alwaysEditors, p.name) != options_.alwaysEditors.end()) {
            p.role = ss::Role::Editor;
            p.always = true;
        } else {
            p.role = ss::Role::Viewer;
        }
        if (options_.log) {
            options_.log(p.name + " joined the session as " + ss::roleName(p.role));
        }
    }
    p.greeted = true;
    hello(host, peer);
    // The working copy: the published version's document, the log since, the locks, in sync.
    if (baseBody_.empty()) {
        baseBody_ = ss::encode(ss::Base{version_, baseBytes_});
    }
    send(host, peer, ss::kBase, baseBody_);
    catchUp(host, peer);
    send(host, peer, ss::kLockTable, locks_.serialize());
    send(host, peer, ss::kSynced, ss::encodeU32(logIndex()));
    sendRole(host, peer, {});
    rosterSent_.clear(); // (everyone the roster again, the newcomer with them)
    refreshRoster(host);
    editorsSent_.clear(); // (and who's editing)
    refreshEditors(host);
    if (p.role == ss::Role::Owner) {
        for (const auto& [q, other] : peers_) {
            if (other.request) {
                send(host, peer, ss::kEditRequest, ss::encode(ss::EditRequest{q, other.name, *other.request, true}));
            }
        }
    }
    for (const auto& [key, pr] : problems_) {
        send(host, peer, ss::kProblem, ss::encode(pr));
    }
    sendPark(host, peer, {});
    if (hasMod() && p.modFiles) {
        // The mod project: its list, then each file's working content, as its queue has room
        // (pumpMod). A Join again (a catch-up) wants the list alone: a transfer under way goes on.
        p.mod.listDirty = true;
        if (!std::exchange(p.mod.sent, true)) {
            for (const auto& [path, e] : mod_) {
                queueModFile(path, peer);
            }
        }
    }
}

void EditSessionDirector::catchUp(GameHost& host, std::uint8_t peer)
{
    // A long log: the one op from the base to the working copy instead, made once per log index and
    // checked to give the working copy exactly (else the log after all).
    if (!log_.empty() && (logBytes_ > options_.snapshotLogBytes || log_.size() > options_.snapshotLogOps)) {
        if (snapshotAt_ != logIndex() || snapshotBody_.empty()) {
            snapshotBody_.clear();
            snapshotAt_ = logIndex();
            try {
                if (const auto diff = me::diffOp(base_, working_)) {
                    me::MapDocument check = base_;
                    me::applyOp(check, *diff);
                    check.nextObjId = working_.nextObjId;
                    check.nextPolygonKey = working_.nextPolygonKey;
                    check.nextGroupKey = working_.nextGroupKey;
                    if (me::sameSessionState(check, working_, things_)) {
                        snapshotBody_ = ss::encode(ss::Snapshot{version_, logIndex(), working_.nextObjId, working_.nextPolygonKey, working_.nextGroupKey,
                                                                me::serializeOp(*diff, things_)});
                    } else if (options_.log) {
                        options_.log("the compact snapshot didn't give the working copy: the log is sent");
                    }
                }
            } catch (const std::exception& ex) {
                if (options_.log) options_.log(std::string("no compact snapshot (") + ex.what() + "): the log is sent");
            }
        }
        if (!snapshotBody_.empty()) {
            send(host, peer, ss::kSnapshot, snapshotBody_);
            ++stats_.snapshots;
            return;
        }
    }
    for (std::size_t i = 0; i < log_.size(); ++i) {
        const Entry& e = log_[i];
        send(host, peer, ss::kCommitted, ss::encode(ss::Committed{logBase_ + static_cast<std::uint32_t>(i) + 1, e.author, e.seq, true, e.op}));
    }
}

void EditSessionDirector::reject(GameHost& host, std::uint8_t peer, std::uint32_t seq, ss::Reject reason, const std::string& text)
{
    ++stats_.rejected;
    send(host, peer, ss::kRejected, ss::encode(ss::Rejected{seq, reason, text}));
}

void EditSessionDirector::onOp(GameHost& host, std::uint8_t peer, const ss::OpMsg& m)
{
    Peer& p = peers_.at(peer);
    if (p.role < ss::Role::Editor) {
        reject(host, peer, m.seq, ss::Reject::Permission, "You can't edit in this session");
        return;
    }
    if (playTest_) {
        reject(host, peer, m.seq, ss::Reject::PlayTest, "A play-test is running");
        return;
    }
    me::Op op;
    try {
        op = me::parseOp(m.op, things_);
    } catch (const std::exception& ex) {
        reject(host, peer, m.seq, ss::Reject::Invalid, ex.what());
        return;
    }
    // Placed among what others did meanwhile: its new ids moved off ones taken (objects, extents,
    // waypoints), its references to the ids we moved in its sender's earlier ops (made before it
    // saw their echo) following them, its polygon and group list sets merged per entry onto the
    // lists as they are now.
    me::IdMoves moved;
    me::freshenNewIds(working_, op, &moved);
    std::erase_if(p.moved, [&](const Peer::Moved& x) { return x.index <= m.seen; }); // (it has seen those)
    if (!p.moved.empty()) {
        me::IdMoves earlier;
        for (const auto& x : p.moved) earlier.merge(x.moves);
        me::remapIds(op, earlier);
    }
    me::mergeConcurrent(working_, op, me::MergeRule::Ours, &moved);
    op = me::withoutNotes(std::move(op));
    if (const auto c = locks_.conflict(peer, me::elementsTouched(working_, op))) {
        const auto holder = peers_.find(static_cast<std::uint8_t>(c->second));
        reject(host, peer, m.seq, ss::Reject::Lock,
               c->first.text() + " is being edited by " + (holder != peers_.end() ? holder->second.name : std::string("another editor")));
        return;
    }
    try {
        commit(host, std::move(op), peer, m.seq);
    } catch (const std::exception& ex) {
        reject(host, peer, m.seq, ss::Reject::Invalid, ex.what());
        return;
    }
    if (!moved.empty()) {
        ++stats_.merges;
        p.moved.push_back({logIndex(), std::move(moved)});
    }
}

me::ObjId EditSessionDirector::ownerOf(const me::MapDocument& doc, me::ObjId id) const
{
    for (int depth = 0; depth < 64; ++depth) {
        const auto it = doc.objects.find(id);
        if (it == doc.objects.end()) {
            return 0;
        }
        if (it->second.parent == 0) {
            return id;
        }
        id = it->second.parent;
    }
    return 0;
}

void EditSessionDirector::commit(GameHost& host, me::Op op, std::uint32_t author, std::uint32_t seq)
{
    // The world owners the op changes, as they were (removed, set, moved) and as they are (added).
    std::set<me::ObjId> owners;
    std::set<me::ObjId> movedOnly;
    std::set<me::ObjId> otherwise;
    for (const auto& part : op.parts) {
        if (const auto* r = std::get_if<me::RemoveObjects>(&part)) {
            for (const auto id : r->ids) {
                if (const auto o = ownerOf(working_, id)) owners.insert(o), otherwise.insert(o);
            }
        } else if (const auto* s = std::get_if<me::SetObjects>(&part)) {
            for (const auto& e : s->objects) {
                if (const auto o = ownerOf(working_, e.id)) owners.insert(o), otherwise.insert(o);
            }
        } else if (const auto* mv = std::get_if<me::MoveObjects>(&part)) {
            for (const auto& m : mv->moves) {
                if (const auto o = ownerOf(working_, m.id)) {
                    owners.insert(o);
                    (o == m.id ? movedOnly : otherwise).insert(o);
                }
            }
        }
    }
    me::applyOp(working_, op); // (throws, leaving the working copy as it was)
    for (const auto& part : op.parts) {
        if (const auto* a = std::get_if<me::AddObjects>(&part)) {
            for (const auto& e : a->objects) {
                if (const auto o = ownerOf(working_, e.id)) owners.insert(o), otherwise.insert(o);
            }
        }
    }
    for (const auto o : otherwise) movedOnly.erase(o);
    Entry entry{author, seq, me::serializeOp(op, things_)};
    const std::uint32_t index = logIndex() + 1;
    sendJoined(host, ss::kCommitted, ss::encode(ss::Committed{index, author, seq, false, entry.op}));
    logBytes_ += entry.op.size();
    log_.push_back(std::move(entry));
    ++stats_.committed;
    if (author != 0) {
        locks_.touch(author, nowMs_);
    }
    // (Only held things changed: the world, and so an object-only publish's file, as it was.)
    const bool liveChanged = owners.empty() || applyLive(host, owners, movedOnly);
    pruneHeld();
    if (hasObjectParts(op) && liveChanged) {
        if (!objectsDirty_) objectsDirtySince_ = nowMs_;
        objectsDirty_ = true;
        objectsChangedAt_ = nowMs_;
    }
    if (hasScriptSource(op)) {
        scriptCompiled(host);
    }
}

void EditSessionDirector::onLock(GameHost& host, std::uint8_t peer, const ss::LockMsg& m)
{
    const Peer& p = peers_.at(peer);
    ss::LockMsg answer{m.seq, {}};
    if (p.role >= ss::Role::Editor) {
        const auto before = locks_.serialize();
        const auto refused = locks_.setHeld(peer, m.keys, nowMs_);
        for (const auto& k : m.keys) {
            if (!refused.contains(k)) answer.keys.insert(k);
        }
        if (locks_.serialize() != before) {
            send(host, peer, ss::kLockAnswer, ss::encode(answer));
            broadcastLocks(host);
            return;
        }
    }
    send(host, peer, ss::kLockAnswer, ss::encode(answer));
}

// --- Edit rights ---------------------------------------------------------------------------------

void EditSessionDirector::onRequestEdit(GameHost& host, std::uint8_t peer, const std::string& reason)
{
    Peer& p = peers_.at(peer);
    if (p.role >= ss::Role::Editor) {
        sendRole(host, peer, "You can already edit");
        return;
    }
    if (options_.editMode == ss::EditMode::HostOnly) {
        sendRole(host, peer, "The host isn't taking editors");
        return;
    }
    if (p.request) {
        return; // (one pending at a time)
    }
    if (p.denied && nowMs_ - p.deniedAt < options_.deniedWaitMs) {
        sendRole(host, peer, "Wait a minute before asking again");
        return;
    }
    p.request = reason.substr(0, 200);
    p.requestAt = nowMs_;
    sendJoined(host, ss::kEditRequest, ss::encode(ss::EditRequest{peer, p.name, *p.request, true}), std::nullopt, true);
    if (options_.log) {
        options_.log(p.name + " asks to edit" + (reason.empty() ? std::string() : ": " + reason));
    }
}

void EditSessionDirector::onAnswer(GameHost& host, std::uint8_t, const ss::Answer& a)
{
    if (a.peer > 0xFF) return;
    const auto it = peers_.find(static_cast<std::uint8_t>(a.peer));
    if (it == peers_.end() || !it->second.joined) return;
    Peer& p = it->second;
    const std::string reason = p.request.value_or(std::string());
    p.request.reset();
    requestClosed(host, it->first, p.name, reason);
    if (a.decision == 0) {
        p.denied = true;
        p.deniedAt = nowMs_;
        sendRole(host, it->first, "The host said no");
        logLine(host, p.name + " was not given edit rights");
        return;
    }
    grant(host, it->first, a.decision == 2);
}

void EditSessionDirector::grant(GameHost& host, std::uint8_t peer, bool always)
{
    Peer& p = peers_.at(peer);
    if (p.role == ss::Role::Owner) return;
    p.role = ss::Role::Editor;
    p.denied = false;
    p.always = always;
    p.activeAt = nowMs_; // (a new editor isn't idle)
    if (always && std::ranges::find(options_.alwaysEditors, p.name) == options_.alwaysEditors.end()) {
        options_.alwaysEditors.push_back(p.name);
        alwaysChanged();
    }
    sendRole(host, peer, "You can now edit");
    logLine(host, p.name + " can now edit");
    refreshRoster(host);
    refreshEditors(host);
}

void EditSessionDirector::revoke(GameHost& host, std::uint8_t peer, const std::string& why)
{
    Peer& p = peers_.at(peer);
    if (p.role != ss::Role::Editor) return;
    p.role = ss::Role::Viewer;
    if (std::erase(options_.alwaysEditors, p.name) != 0) {
        alwaysChanged();
    }
    if (!locks_.heldBy(peer).empty()) {
        locks_.releaseAll(peer);
        broadcastLocks(host);
    }
    releaseModHolds(peer); // (the mod files it held too)
    sendRole(host, peer, "Your edit rights were revoked");
    logLine(host, p.name + "'s edit rights were revoked (" + why + ")");
    refreshRoster(host);
}

void EditSessionDirector::setSettings(GameHost& host, ss::EditMode mode, std::uint8_t f)
{
    options_.editMode = mode;
    const bool cantDie = (f & ss::kPlayersCantDie) != 0;
    if (cantDie != options_.playersCantDie) {
        options_.playersCantDie = cantDie;
        if (!playTest_) host.setPlayersCantDie(cantDie);
        logLine(host, cantDie ? "Players can't die" : "Players can die");
    }
    options_.liveView = (f & ss::kLiveView) != 0;
    options_.liveCreatures = (f & ss::kLiveCreatures) != 0;
    options_.autoPublish = (f & ss::kAutoPublish) != 0;
    if (mode == ss::EditMode::HostOnly) {
        for (auto& [peer, p] : peers_) {
            if (p.request) {
                dropRequest(host, peer, "The host isn't taking editors");
            }
        }
    }
    broadcastHello(host);
}

// --- The running game ----------------------------------------------------------------------------

void EditSessionDirector::rebind(GameHost& host)
{
    live_.clear();
    waiting_.clear();
    World& w = host.server().world();
    std::map<std::uint32_t, me::ObjId> byExtent;
    for (const auto& [id, o] : working_.objects) {
        if (o.parent == 0 && o.header.extent != 0) {
            byExtent.try_emplace(o.header.extent, id);
        }
    }
    for (const std::uint32_t e : w.loadOrder) {
        if (const ServerObject* o = w.byExtent(e)) {
            if (const auto it = byExtent.find(o->fileExtent); it != byExtent.end()) {
                live_.try_emplace(it->second, e);
            }
        }
    }
}

bool EditSessionDirector::createLive(GameHost& host, me::ObjId id)
{
    const auto it = working_.objects.find(id);
    if (it == working_.objects.end() || it->second.parent != 0) {
        return false;
    }
    const things::ThingDef* t = things_.findThing(it->second.type);
    if (!t || clientOnlyType(*t)) {
        // (Never asked for one: the clients load those from their own map, so applyLive holds
        // them for a publish first. A type thing.bin hasn't waits the same way.)
        waiting_.insert(id);
        return false;
    }
    // The object and its inventory as a map's records (the writer's), read and placed as the
    // map load does.
    me::MapDocument mini = me::MapDocument::blank();
    std::vector<me::ObjId> todo{id};
    while (!todo.empty()) {
        const me::ObjId at = todo.back();
        todo.pop_back();
        mini.objects.emplace(at, working_.objects.at(at));
        for (const me::ObjId child : working_.childrenOf(at)) {
            todo.push_back(child);
        }
    }
    mini.nextObjId = working_.nextObjId;
    std::vector<std::uint32_t> roots;
    try {
        const map::MapFile file = me::toMapFile(mini, things_);
        roots = host.server().world().placeLiveRecords(map::decodeSection<map::ObjectTOC>(file), map::decodeSection<map::ObjectData>(file),
                                                       things_, host.server().clock().frame());
    } catch (const std::exception& ex) {
        if (options_.log) {
            options_.log(std::string("an object couldn't go live: ") + ex.what());
        }
        roots.clear();
    }
    if (roots.empty()) {
        waiting_.insert(id);
        return false;
    }
    live_[id] = roots.front();
    waiting_.erase(id);
    ++stats_.liveAdds;
    return true;
}

bool EditSessionDirector::holds(me::ObjId id) const
{
    return me::heldForPublish(base_, working_, id, things_);
}

void EditSessionDirector::pruneHeld()
{
    std::erase_if(held_, [&](me::ObjId id) { return sameSubtree(base_, working_, id); });
}

bool EditSessionDirector::applyLive(GameHost& host, const std::set<me::ObjId>& owners, const std::set<me::ObjId>& moved)
{
    World& w = host.server().world();
    const std::uint32_t frame = host.server().clock().frame();
    bool anyLive = false;
    for (const me::ObjId id : owners) {
        // A thing every client draws from its own map, or what it holds: the World keeps it as
        // published (where it stands, blocks and lights, a removed one too) till a full publish's
        // map change brings the working copy's, for the clients and the World together.
        if (holds(id)) {
            held_.insert(id);
            waiting_.erase(id);
            ++stats_.heldEdits;
            continue;
        }
        anyLive = true;
        const auto doc = working_.objects.find(id);
        const bool inDoc = doc != working_.objects.end() && doc->second.parent == 0;
        const auto it = live_.find(id);
        ServerObject* o = it != live_.end() ? w.byExtent(it->second) : nullptr;
        if (o && (o->pendingDelete || o->gone())) {
            o = nullptr;
        }
        if (!inDoc) {
            waiting_.erase(id);
            problem(host, id, {});
            if (o && o->holder == 0) {
                removeLive(w, base_, *o); // (the game's delayed delete: the clients are told it's gone)
                ++stats_.liveRemoves;
            }
            live_.erase(id);
            continue;
        }
        if (it == live_.end()) {
            createLive(host, id);
            continue;
        }
        if (!o) {
            waiting_.erase(id); // (used up in play: gone till the next publish)
            continue;
        }
        // (A door moved is taken off its wall and made again where it now is, as the load attaches
        // it: the position set alone would leave its leaf on the old cell. Every door in thing.bin
        // is IMMOBILE, so held above: this is for a door type that isn't, never half-attached.)
        if (moved.contains(id) && !(o->classes & things::ObjectClass::Door)) {
            if (o->holder == 0) {
                // (Obj_SetPosition, as a script's MoveObject: the clients see it move)
                setPosition(w, *o, doc->second.header.x, doc->second.header.y, frame);
                ++stats_.liveMoves;
            } else {
                waiting_.insert(id);
            }
            continue;
        }
        if (o->holder != 0 || (o->flags & ObjFlag::Dead)) {
            waiting_.insert(id); // (carried, or mid-death: changed when it can be)
            continue;
        }
        // Scripts (and creatures' script links) hold objects by script id (Object("name") gives
        // it; a global keeps it): the one made again takes the old one's when its record names
        // none (a new object's: the counter's, a fresh one at each make), so they find it again.
        const std::int32_t oldScriptId = o->scriptId;
        const std::vector<std::pair<std::string, std::int32_t>> oldItems = itemScriptIds(w, *o);
        const std::string typeName = o->typeName;
        removeLive(w, base_, *o);
        live_.erase(id);
        if (createLive(host, id)) {
            ++stats_.liveRecreates;
            --stats_.liveAdds;
            repointScriptIds(host, id, oldScriptId, oldItems);
        } else if (oldScriptId != 0) {
            problem(host, id,
                    typeName + " (object " + std::to_string(id) + ") waits to be made again (a publish): scripts holding its script id " +
                        std::to_string(oldScriptId) + " find nothing until then");
        }
    }
    return anyLive;
}

std::vector<std::pair<std::string, std::int32_t>> EditSessionDirector::itemScriptIds(World& w, const ServerObject& holder)
{
    std::vector<std::pair<std::string, std::int32_t>> out;
    std::vector<std::uint32_t> todo(holder.inventory.rbegin(), holder.inventory.rend());
    while (!todo.empty()) {
        const std::uint32_t e = todo.back();
        todo.pop_back();
        const ServerObject* item = w.byExtent(e);
        if (!item) continue;
        out.emplace_back(item->typeName, item->scriptId);
        todo.insert(todo.end(), item->inventory.rbegin(), item->inventory.rend());
    }
    return out;
}

void EditSessionDirector::repointScriptIds(GameHost& host, me::ObjId id, std::int32_t oldScriptId,
                                           const std::vector<std::pair<std::string, std::int32_t>>& oldItems)
{
    World& w = host.server().world();
    const auto it = live_.find(id);
    ServerObject* n = it != live_.end() ? w.byExtent(it->second) : nullptr;
    if (!n) {
        return;
    }
    const auto recordId = [](const ServerObject& o) { return o.record ? static_cast<std::int32_t>(o.record->header.scriptId) : 0; };
    const std::int32_t own = recordId(*n);
    if (oldScriptId != 0 && (own == 0 || own == oldScriptId)) {
        if (n->scriptId != oldScriptId) {
            n->scriptId = oldScriptId;
            ++stats_.scriptRepoints;
        }
        problem(host, id, {});
    } else if (oldScriptId != 0 && own != oldScriptId) {
        problem(host, id,
                n->typeName + " (object " + std::to_string(id) + ") now has script id " + std::to_string(own) +
                    " (its record's): scripts holding its old id " + std::to_string(oldScriptId) + " find nothing");
    }
    // Its items, in the same order and of the same types, keep theirs as well.
    std::vector<std::uint32_t> todo(n->inventory.rbegin(), n->inventory.rend());
    std::size_t i = 0;
    while (!todo.empty()) {
        const std::uint32_t e = todo.back();
        todo.pop_back();
        ServerObject* item = w.byExtent(e);
        if (!item) continue;
        if (i < oldItems.size() && oldItems[i].first == item->typeName && oldItems[i].second != 0 && recordId(*item) == 0) {
            item->scriptId = oldItems[i].second;
        }
        ++i;
        todo.insert(todo.end(), item->inventory.rbegin(), item->inventory.rend());
    }
    // Two objects answering to one script id: a script finds either.
    for (const auto& [extent, o] : w.objects()) {
        if (&o != n && !o.gone() && !o.pendingDelete && o.scriptId == n->scriptId && n->scriptId != 0) {
            problem(host, id,
                    n->typeName + " (object " + std::to_string(id) + ") shares script id " + std::to_string(n->scriptId) + " with " + o.typeName +
                        ": a script holding it may find either");
            break;
        }
    }
}

void EditSessionDirector::retryWaiting(GameHost& host)
{
    if (waiting_.empty()) {
        return;
    }
    World& w = host.server().world();
    std::set<me::ObjId> again;
    for (const me::ObjId id : std::set<me::ObjId>(waiting_)) {
        const auto doc = working_.objects.find(id);
        if (doc == working_.objects.end()) {
            waiting_.erase(id);
            continue;
        }
        const auto it = live_.find(id);
        if (it == live_.end()) {
            continue; // (client-only or unplaceable: waits for a publish)
        }
        const ServerObject* o = w.byExtent(it->second);
        if (!o || o->pendingDelete || o->gone()) {
            waiting_.erase(id); // (used up meanwhile)
            continue;
        }
        if (o->holder == 0 && !(o->flags & ObjFlag::Dead)) {
            waiting_.erase(id);
            again.insert(id);
        }
    }
    if (!again.empty()) {
        applyLive(host, again, {});
    }
}

void EditSessionDirector::sendLive(GameHost& host)
{
    if (std::ranges::none_of(peers_, [](const auto& p) { return p.second.joined; })) {
        return;
    }
    World& w = host.server().world();
    const auto coord = [](float v) { return static_cast<std::int16_t>(std::clamp(v, -32000.0f, 32000.0f)); };
    std::vector<ss::LiveEntry> live;
    for (const auto& [peer, hp] : host.players()) {
        const ServerObject* u = w.byExtent(hp.extent);
        if (!u || u->gone()) continue;
        ss::LiveEntry e;
        e.id = static_cast<std::uint16_t>(hp.extent);
        e.player = true;
        e.stock = !host.identified(peer);
        e.team = u->team;
        e.x = coord(u->x);
        e.y = coord(u->y);
        e.name = hp.name;
        // What the live layer draws them with: their animation bytes as their update record has
        // them (SrvNet_SendPlayerObj: playerRecord), their worn class bits.
        e.type = u->type;
        e.anim = directionNibble(u->direction);
        if (u->player) {
            e.state = playerAnimation(*u, e.frame, &w);
            e.armor = u->player->armorEquip;
            e.weapon = u->player->weaponEquip;
            e.observer = (u->player->statusFlags & 1u) != 0;
        }
        live.push_back(std::move(e));
    }
    if (options_.liveCreatures) {
        for (const auto& [extent, o] : w.objects()) {
            if (live.size() >= 400) break;
            if (!(o.classes & things::ObjectClass::Monster) || o.holder != 0 || o.gone() || o.pendingDelete || (o.flags & ObjFlag::Dead)) {
                continue;
            }
            ss::LiveEntry e;
            e.id = static_cast<std::uint16_t>(extent);
            e.player = false;
            e.team = o.team;
            e.x = coord(o.x);
            e.y = coord(o.y);
            e.name = o.typeName;
            e.type = o.type;
            e.anim = directionNibble(o.direction);
            if (o.monster) { // (SrvNet_SendComplexObj: its action's animation state and frame)
                e.anim = static_cast<std::uint8_t>(e.anim | monsterAnimState(o));
                e.frame = o.monster->animFrame;
            }
            live.push_back(std::move(e));
        }
    }
    // To each viewer whose link keeps up: one behind on its acks gets the next.
    const auto frames = ss::frame(ss::kLive, ss::encode(live), transfer_);
    for (const auto& [peer, p] : peers_) {
        if (!p.joined) {
            continue;
        }
        if (host.queuedReliable(peer) > options_.liveQueueLimit) {
            ++stats_.liveSkipped;
            continue;
        }
        for (const auto& m : frames) {
            host.sendToIdentified(peer, m);
        }
    }
}

// --- Scripts -------------------------------------------------------------------------------------

namespace {
ss::ScriptDiag diagOf(const script::CompileResult& r, std::uint8_t kind)
{
    ss::ScriptDiag d;
    d.kind = kind;
    d.ok = r.ok();
    std::size_t errors = 0;
    for (const auto& x : r.diagnostics) {
        const bool error = x.severity == script::Diagnostic::Severity::Error;
        errors += error ? 1 : 0;
        d.diagnostics.push_back({x.loc.line, x.loc.column, error, x.message});
    }
    d.summary = r.ok() ? "The script compiles (" + std::to_string(r.diagnostics.size()) + " warnings)"
                       : "The script has " + std::to_string(errors) + (errors == 1 ? " error" : " errors");
    return d;
}
} // namespace

void EditSessionDirector::scriptCompiled(GameHost& host)
{
    if (!working_.scriptSource) {
        return;
    }
    const script::CompileResult r = me::compileForDocument(working_, *working_.scriptSource);
    sendJoined(host, ss::kScriptDiag, ss::encode(diagOf(r, 0)), std::nullopt, true);
}

void EditSessionDirector::reloadScript(GameHost& host, std::uint8_t peer, std::uint32_t mode)
{
    if (!working_.scriptSource) {
        send(host, peer, ss::kScriptDiag, ss::encode(ss::ScriptDiag{1, false, {}, "No script source to reload"}));
        return;
    }
    const script::CompileResult r = me::compileForDocument(working_, *working_.scriptSource);
    if (!r.ok()) {
        ss::ScriptDiag d = diagOf(r, 1);
        d.summary = "Not reloaded: " + d.summary;
        sendJoined(host, ss::kScriptDiag, ss::encode(d), std::nullopt, true);
        return;
    }
    // Everyone's working copy gets the bytecode the source compiles to (the host's op).
    if (me::Op op = me::compiledScriptOp(working_, *r.file); !op.empty()) {
        commit(host, std::move(op), 0, 0);
    }
    Server& server = host.server();
    if (!runningDebug_ && !runningDebugTried_) {
        runningDebugTried_ = true;
        // The running script's names: its source's compile (the base's), else its decompiled
        // source's when that compiles back to the same bytes.
        std::optional<script::DebugInfo> candidate;
        if (base_.scriptSource) {
            const auto c = me::compileForDocument(base_, *base_.scriptSource);
            if (c.ok()) candidate = c.debug;
        } else {
            const auto d = me::decompileDocumentScript(base_);
            if (d.identical) {
                const auto c = me::compileForDocument(base_, d.source);
                if (c.ok()) candidate = c.debug;
            }
        }
        if (candidate && dev::debugMatches(server.scriptFile(), *candidate)) {
            runningDebug_ = candidate;
        }
    }
    dev::ReloadReport report;
    const bool hot = mode == 0 && runningDebug_ && dev::debugMatches(server.scriptFile(), *runningDebug_);
    report = hot ? dev::hotReload(server, *r.file, *runningDebug_, r.debug) : dev::restartScripts(server, *r.file);
    if (report.applied) {
        runningDebug_ = r.debug;
    }
    ss::ScriptDiag d;
    d.kind = 1;
    d.ok = report.applied;
    for (const auto& n : report.notes) {
        d.diagnostics.push_back({n.loc.line, n.loc.column, n.severity == dev::ReloadNote::Severity::Warning, n.message});
    }
    d.summary = (hot ? "Hot reload: " : "Restart scripts: ") + report.summary();
    if (!report.applied && !report.notes.empty()) {
        d.summary = "Not reloaded: " + report.notes.front().message;
    }
    sendJoined(host, ss::kScriptDiag, ss::encode(d), std::nullopt, true);
    const auto it = peers_.find(peer);
    logLine(host, (it != peers_.end() ? it->second.name : std::string("The host")) + (report.applied ? " reloaded the script for everyone" : " tried to reload the script"));
}

// --- Publish and play-test -----------------------------------------------------------------------

bool EditSessionDirector::publish(GameHost& host, bool full)
{
    me::Sidecar side;
    side.origin = options_.mapName;
    me::storeScriptInSidecar(side, working_.scriptSource, runningDebug_ ? &*runningDebug_ : nullptr);
    objectsDirty_ = false;
    try {
        if (full) {
            const auto paths = me::saveDocument(working_, things_, options_.mapsDir, options_.mapName, &side);
            base_ = me::canonicalSessionDocument(working_, things_);
            working_ = base_;
            baseBytes_ = me::encodeSessionDocument(base_, things_);
            baseBody_.clear();
            logBase_ += static_cast<std::uint32_t>(log_.size());
            log_.clear();
            logBytes_ = 0;
            snapshotBody_.clear();
            ++version_;
            ++stats_.fullPublishes;
            waiting_.clear();
            held_.clear(); // (the map change below brings every held edit, to the World and the clients alike)
            // (The map loads again: every object's script id is the new load's; nothing held.)
            std::vector<std::uint32_t> open;
            for (const auto& [key, pr] : problems_) open.push_back(key);
            for (const auto key : open) {
                problem(host, key, {});
            }
            runningDebug_.reset();
            runningDebugTried_ = false;
            const std::uint32_t crc = map::loadMapFile(paths.map).crc;
            sendJoined(host, ss::kPublished, ss::encode(ss::Published{version_, crc, options_.mapName, true}));
            logLine(host, "Published version " + std::to_string(version_) + " (a map change)");
            // The map change, as the console's "load": stock clients download the new file.
            host.loadMapNamed(options_.mapName, playTest_ ? options_.playTestMode : sandboxMode_);
            needRebind_ = true;
        } else {
            // The published walls, floor and script with the objects as the World has them:
            // the working copy's, held things as published, so a late joiner
            // draws what it collides with.
            me::MapDocument doc = base_;
            doc.objects = me::playedObjects(base_, working_, things_);
            doc.nextObjId = working_.nextObjId;
            doc.ensureSection(map::ObjectTOC::kName);
            doc.ensureSection(map::ObjectData::kName);
            const auto paths = me::saveDocument(doc, things_, options_.mapsDir, options_.mapName, &side);
            host.refreshMapFile();
            ++stats_.objectPublishes;
            const std::uint32_t crc = map::loadMapFile(paths.map).crc;
            sendJoined(host, ss::kPublished, ss::encode(ss::Published{version_, crc, options_.mapName, false}));
        }
    } catch (const std::exception& ex) {
        logLine(host, std::string("Publish failed: ") + ex.what());
        return false;
    }
    return true;
}

bool EditSessionDirector::startPlayTest(GameHost& host, std::uint32_t mode, bool playersCanDie)
{
    if (playTest_) {
        return false;
    }
    playTest_ = true;
    options_.playTestMode = mode & 0x17F0u ? mode & 0x17F0u : sandboxMode_;
    host.setPlayersCantDie(!playersCanDie);
    host.setSandboxNoScoring(false); // (the round's real mode scores)
    if (!log_.empty()) {
        if (!publish(host, true)) {
            playTest_ = false;
            host.setPlayersCantDie(options_.playersCantDie);
            host.setSandboxNoScoring(true);
            return false;
        }
    } else {
        host.loadMapNamed(options_.mapName, options_.playTestMode);
        needRebind_ = true;
    }
    logLine(host, "Play-test started");
    broadcastHello(host);
    return true;
}

void EditSessionDirector::endPlayTest(GameHost& host)
{
    if (!playTest_) {
        return;
    }
    playTest_ = false;
    host.setPlayersCantDie(options_.playersCantDie);
    host.setSandboxNoScoring(true);
    host.server().world().gameFlags &= ~GameFlag::GameOver;
    host.loadMapNamed(options_.mapName, sandboxMode_);
    needRebind_ = true;
    logLine(host, "Play-test ended: back to editing");
    broadcastHello(host);
}

// --- The mod project ---------------------------------------------------------------------------

void EditSessionDirector::loadMod()
{
    if (options_.modFolder.empty()) {
        return;
    }
    // Its id is its folder's name, as a mod's (a-z, 0-9 and _).
    const std::string id = options_.modFolder.filename().string();
    const bool goodId = !id.empty() && id.size() <= 32 &&
                        std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
    me::ModFolderFiles read = goodId ? me::readModFolder(options_.modFolder) : me::ModFolderFiles{{}, "the folder's name isn't a mod's id"};
    if (!read.error.empty()) {
        modError_ = read.error;
        if (options_.log) options_.log("the mod isn't carried: " + modError_);
        return;
    }
    modId_ = id;
    modName_ = id;
    if (const auto it = read.files.find("mode.json"); it != read.files.end()) {
        // Its name as mode.json gives it (the lists' label; the id when it has none).
        try {
            const me::Sidecar json = me::Sidecar::fromJson(std::string_view(reinterpret_cast<const char*>(it->second.data()), it->second.size()));
            if (const auto name = json.text("name"); name && !name->empty()) {
                modName_ = name->substr(0, 32);
            }
        } catch (const std::exception&) {
        }
    }
    for (auto& [path, bytes] : read.files) {
        ModEntry e;
        e.publishedVersion = e.workingVersion = ++modVersions_;
        e.published = bytes;
        e.working = std::move(bytes);
        packModEntry(path, e);
        mod_.emplace(path, std::move(e));
    }
    if (options_.log) options_.log("the mod " + modId_ + " carried (" + std::to_string(mod_.size()) + " files)");
}

void EditSessionDirector::packModEntry(const std::string& path, ModEntry& e)
{
    // The file's message body, made once per working version (joins and copy-backs reuse it).
    ss::ModFile f;
    f.path = path;
    f.deleted = !e.working;
    f.version = e.working ? e.workingVersion : 0;
    if (e.working) {
        f.bytes = *e.working;
    }
    e.body = ss::encode(f);
    ++stats_.modPacks;
}

ss::ModProject EditSessionDirector::modProject() const
{
    ss::ModProject m;
    m.id = modId_;
    m.name = modName_;
    for (const auto& [path, e] : mod_) {
        ss::ModFileInfo f;
        f.path = path;
        f.size = e.working ? static_cast<std::uint32_t>(e.working->size()) : 0;
        f.published = e.published ? e.publishedVersion : 0;
        f.working = e.working ? e.workingVersion : 0;
        f.holder = e.holder;
        f.author = e.author;
        m.files.push_back(std::move(f));
    }
    return m;
}

void EditSessionDirector::modListDirty(std::optional<std::uint8_t> only)
{
    // (Latest wins: the list goes out at most once a tick, while the member's queue has room.)
    modListBody_.clear();
    for (auto& [peer, p] : peers_) {
        if (p.joined && p.modFiles && (!only || peer == *only)) {
            p.mod.listDirty = true;
        }
    }
}

void EditSessionDirector::queueModFile(const std::string& path, std::optional<std::uint8_t> only)
{
    if (!mod_.contains(path)) {
        return;
    }
    // One entry a path: a file wanted again while it waits goes once, at its version then.
    for (auto& [peer, p] : peers_) {
        if (p.joined && p.modFiles && (!only || peer == *only) && p.mod.wanted.insert(path).second) {
            p.mod.want.push_back(path);
        }
    }
}

void EditSessionDirector::setModHolder(ModEntry& e, std::uint8_t holder)
{
    if ((e.holder != 0) != (holder != 0)) {
        holder != 0 ? ++modHeld_ : --modHeld_;
    }
    e.holder = holder;
    if (holder != 0) {
        e.heldAt = nowMs_;
    }
}

void EditSessionDirector::releaseModHolds(std::uint8_t peer)
{
    if (modHeld_ == 0) {
        return;
    }
    bool any = false;
    for (auto& [path, e] : mod_) {
        if (e.holder == peer) {
            setModHolder(e, 0);
            any = true;
        }
    }
    if (any) {
        modListDirty();
    }
}

void EditSessionDirector::modLog(GameHost& host, const std::string& line)
{
    // (To the host's log and the members that take the mod: never a line for a client from before.)
    if (options_.log) {
        options_.log(line);
    }
    for (const auto& [peer, p] : peers_) {
        if (p.joined && p.modFiles) {
            send(host, peer, ss::kLog, ss::encodeText(line));
        }
    }
}

std::size_t EditSessionDirector::modWanted(std::uint8_t peer) const
{
    const auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.mod.want.size() + (it->second.mod.frames.size() > it->second.mod.frameAt ? 1 : 0);
}

void EditSessionDirector::pumpMod(GameHost& host)
{
    for (auto& [peer, p] : peers_) {
        if (!p.joined || !p.modFiles) {
            continue;
        }
        ModOut& o = p.mod;
        while (host.queuedReliable(peer) < options_.modQueueLimit) {
            if (o.listDirty) {
                // The list first (its files' contents are checked against it on arrival).
                if (modListBody_.empty()) {
                    modListBody_ = ss::encode(modProject());
                }
                send(host, peer, ss::kModProject, modListBody_);
                o.listDirty = false;
                continue;
            }
            if (o.frameAt < o.frames.size()) {
                host.sendToIdentified(peer, std::move(o.frames[o.frameAt++]));
                continue;
            }
            if (o.want.empty()) {
                break;
            }
            // The next wanted file, framed now from its working version as it stands.
            const std::string path = std::move(o.want.front());
            o.want.pop_front();
            o.wanted.erase(path);
            if (const auto it = mod_.find(path); it != mod_.end()) {
                o.frames = ss::frame(ss::kModFile, it->second.body, transfer_);
                o.frameAt = 0;
            }
        }
        if (o.frameAt >= o.frames.size() && !o.frames.empty()) {
            o.frames.clear();
            o.frameAt = 0;
        }
    }
    // Idle holds let go (as the lock table's); nothing to look at when nothing is held.
    if (modHeld_ != 0) {
        bool changed = false;
        for (auto& [path, e] : mod_) {
            if (e.holder != 0 && nowMs_ - e.heldAt >= options_.lockTimeoutMs) {
                setModHolder(e, 0);
                changed = true;
            }
        }
        if (changed) {
            modListDirty();
        }
    }
}

void EditSessionDirector::onModFile(GameHost& host, std::uint8_t peer, const ss::ModFile& m)
{
    Peer& p = peers_.at(peer);
    const auto refuse = [&](const std::string& why) {
        send(host, peer, ss::kLog, ss::encodeText(m.path + ": not saved: " + why));
        queueModFile(m.path, peer); // (its copy back to the session's, once however often it's asked)
    };
    if (p.role < ss::Role::Editor) {
        // (A viewer's copy is the session's still: the line alone.)
        send(host, peer, ss::kLog, ss::encodeText(m.path + ": not saved: you can't edit (ask the host)"));
        return;
    }
    auto it = mod_.find(m.path);
    if (it == mod_.end()) {
        if (m.deleted || m.version != 0) {
            return; // (a delete of a file that isn't there: nothing to do)
        }
        if (mod_.size() >= ss::kMaxModFiles) {
            refuse("the mod has " + std::to_string(ss::kMaxModFiles) + " files already");
            return;
        }
        // A new path mustn't be another's in another case, or a file where another has a folder.
        for (const auto& [path, e] : mod_) {
            if (me::modPathsClash(path, m.path)) {
                send(host, peer, ss::kLog, ss::encodeText(m.path + ": not saved: it clashes with " + path));
                return;
            }
        }
    } else {
        const ModEntry& e = it->second;
        if (e.holder != 0 && e.holder != peer) {
            refuse(peers_.contains(e.holder) ? peers_.at(e.holder).name + " is editing it" : "another editor holds it");
            return;
        }
        if (m.version != (e.working ? e.workingVersion : 0)) {
            refuse("it changed since (" + (e.author && peers_.contains(static_cast<std::uint8_t>(e.author)) ? peers_.at(static_cast<std::uint8_t>(e.author)).name : std::string("the host")) + ")");
            return;
        }
    }
    // The caps for the whole mod with the change.
    std::size_t total = m.deleted ? 0 : m.bytes.size();
    for (const auto& [path, e] : mod_) {
        if (path != m.path && e.working) total += e.working->size();
    }
    if (total > ss::kMaxModBytes) {
        refuse("the mod would be over " + std::to_string(ss::kMaxModBytes >> 20) + " MB");
        return;
    }
    ModEntry& e = mod_[m.path];
    if (options_.log) options_.log(p.name + (m.deleted ? " deleted the mod's " : " saved the mod's ") + m.path);
    if (m.deleted) {
        e.working.reset();
        if (!e.published) {
            setModHolder(e, 0);
            mod_.erase(m.path); // (a new file never published: simply gone)
            modListDirty();
            return;
        }
    } else {
        e.working = m.bytes;
        e.workingVersion = ++modVersions_;
    }
    e.author = peer;
    if (e.holder == peer) {
        e.heldAt = nowMs_;
    }
    packModEntry(m.path, e);
    modListDirty();
    queueModFile(m.path);
}

void EditSessionDirector::onModLock(GameHost& host, std::uint8_t peer, const ss::ModLock& m)
{
    const auto it = mod_.find(m.path);
    if (it == mod_.end() || peers_.at(peer).role < ss::Role::Editor) {
        return;
    }
    ModEntry& e = it->second;
    const std::uint8_t before = e.holder;
    if (m.on) {
        if (e.holder == 0 || e.holder == peer || (m.force && isOwner(host, peer))) {
            setModHolder(e, peer);
        }
    } else if (e.holder == peer) {
        setModHolder(e, 0);
    }
    if (e.holder != before) {
        modListDirty();
    } else if (m.on && e.holder != peer) {
        modListDirty(peer); // (refused: the list as it is, to the one who asked)
    }
}

bool EditSessionDirector::publishMod(GameHost& host)
{
    if (!hasMod()) {
        return false;
    }
    int written = 0;
    bool ok = true;
    for (auto it = mod_.begin(); it != mod_.end();) {
        ModEntry& e = it->second;
        const bool same = e.working ? (e.published && e.publishedVersion == e.workingVersion) : !e.published;
        if (same) {
            ++it;
            continue;
        }
        std::string error;
        if (e.working ? me::writeModFile(options_.modFolder, it->first, *e.working, error) : me::removeModFile(options_.modFolder, it->first, error)) {
            ++written;
            if (!e.working) {
                setModHolder(e, 0);
                it = mod_.erase(it);
                continue;
            }
            e.published = e.working;
            e.publishedVersion = e.workingVersion;
        } else {
            ok = false;
            modLog(host, "Mod publish: " + it->first + ": " + error);
        }
        ++it;
    }
    modListDirty();
    modLog(host, "Published the mod " + modId_ + " (" + std::to_string(written) + (written == 1 ? " file)" : " files)"));
    return ok;
}

std::filesystem::path setUpEditSession(EditSessionDirector& director, GameHost::Options& o, const std::string& hostName)
{
    o.serverName = EditSessionDirector::listedName(hostName);
    o.mapCycleOn = false; // (the session's map plays on: no cycle, no vote)
    o.mapVote = false;
    o.quest = false;
    o.solo = false;
    o.gameFlags = (o.gameFlags & ~0x17F0u) | director.sandboxMode();
    o.lessonLimits.fill(0); // (no score or time limit: no end of round)
    o.timeLimits.fill(0);
    o.playersCantDie = director.options().playersCantDie;
    o.sandboxNoScoring = true; // (kills, flags, goals don't count while editing; play-test rounds score)
    o.mapsDir = director.options().mapsDir;
    o.prepareMap = [&director](const std::filesystem::path&, const World&) { director.mapLoading(); };
    o.noxcppMessages[static_cast<std::uint8_t>(net::Op::NoxcppEdit)] = [&director](GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m) {
        director.onMessage(host, peer, m);
    };
    const auto& name = director.options().mapName;
    return director.options().mapsDir / name / (name + ".map");
}

} // namespace nox::server
