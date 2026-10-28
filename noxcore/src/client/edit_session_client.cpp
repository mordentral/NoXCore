// The map editor session's client side (edit_session_client.hpp).
#include "noxcore/client/edit_session_client.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/mapedit/mod_files.hpp"
#include "noxcore/mapedit/session_doc.hpp"
#include "noxcore/mapedit/session_merge.hpp"
#include "noxcore/mapedit/writer.hpp"

#include <algorithm>
#include <fstream>

namespace nox::client {

namespace me = mapedit;
namespace ss = mapedit::session;

EditSessionClient::EditSessionClient(const things::ThingDb& things, Send send, std::filesystem::path publishCache)
    : things_(things)
    , send_(std::move(send))
    , cache_(std::move(publishCache))
{
}

void EditSessionClient::attach(me::MapDocument* external)
{
    me::MapDocument* to = external ? external : &own_;
    if (to == doc_) {
        return;
    }
    *to = *doc_;
    doc_ = to;
    ++revision_;
}

std::string EditSessionClient::nameOf(me::PeerId peer) const
{
    for (const auto& r : roster_) {
        if (r.peer == peer) return r.name;
    }
    return peer == 0 ? std::string("the host") : "editor " + std::to_string(peer);
}

std::uint8_t EditSessionClient::colourOf(me::PeerId peer) const
{
    for (const auto& r : roster_) {
        if (r.peer == peer) return r.colour;
    }
    return static_cast<std::uint8_t>(peer % 16);
}

me::MapDocument EditSessionClient::committed() const
{
    me::MapDocument d = *doc_;
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        me::applyOp(d, it->inverse);
    }
    return d;
}

void EditSessionClient::sendBody(std::uint8_t sub, std::span<const std::uint8_t> body)
{
    for (auto& m : ss::frame(sub, body, transfer_)) {
        send_(std::move(m), true);
    }
}

void EditSessionClient::logText(std::string line)
{
    log_.push_back(line);
    while (log_.size() > 200) log_.pop_front();
    Event e;
    e.kind = Event::Kind::Log;
    e.text = std::move(line);
    push(std::move(e));
}

// --- Messages ------------------------------------------------------------------------------------

void EditSessionClient::setHold(bool hold)
{
    hold_ = hold;
    if (!hold_ && !held_.empty()) {
        for (const auto& m : std::exchange(held_, {})) {
            onMessage(m);
        }
    }
}

void EditSessionClient::onMessage(std::span<const std::uint8_t> message)
{
    if (hold_) {
        held_.emplace_back(message.begin(), message.end());
        return;
    }
    if (auto m = in_.feed(message)) {
        try {
            handle(*m);
        } catch (const std::exception& ex) {
            logText(std::string("A session message couldn't be read: ") + ex.what());
        }
    }
}

void EditSessionClient::handle(const ss::Message& m)
{
    switch (m.sub) {
    case ss::kHello: {
        const ss::Hello h = ss::decodeHello(m.body);
        hello_ = h;
        break;
    }
    case ss::kRoster:
        roster_ = ss::decodeRoster(m.body);
        break;
    case ss::kBase: {
        const ss::Base b = ss::decodeBase(m.body);
        *doc_ = me::decodeSessionDocument(b.document, things_);
        published_ = *doc_;
        pending_.clear();
        hasDoc_ = true;
        synced_ = false;
        bySnapshot_ = false;
        logIndex_ = 0;
        ++revision_;
        break;
    }
    case ss::kSnapshot: {
        // A long log's catch-up: the one op from the base to the working copy, and the next ids no
        // op sets.
        if (!hasDoc_) break;
        const ss::Snapshot s = ss::decodeSnapshot(m.body);
        undoPending();
        try {
            me::applyOp(*doc_, me::parseOp(s.op, things_));
        } catch (const std::exception& ex) {
            catchUpAgain(std::string("The session's snapshot didn't apply (") + ex.what() + ")");
            break;
        }
        doc_->nextObjId = s.nextObjId;
        doc_->nextPolygonKey = s.nextPolygonKey;
        doc_->nextGroupKey = s.nextGroupKey;
        redoPending();
        logIndex_ = s.index;
        bySnapshot_ = true;
        ++revision_;
        break;
    }
    case ss::kPark: {
        const ss::Park p = ss::decodePark(m.body);
        parked_ = p.on;
        Event e;
        e.kind = Event::Kind::Park;
        e.text = p.text;
        push(std::move(e));
        break;
    }
    case ss::kProblem: {
        const ss::Problem p = ss::decodeProblem(m.body);
        if (p.open) {
            problems_[p.key] = p;
        } else {
            problems_.erase(p.key);
        }
        Event e;
        e.kind = Event::Kind::Problem;
        e.problem = p;
        push(std::move(e));
        break;
    }
    case ss::kCommitted:
        onCommitted(ss::decodeCommitted(m.body));
        break;
    case ss::kSynced:
        logIndex_ = ss::decodeU32(m.body);
        synced_ = hasDoc_;
        push(Event{.kind = Event::Kind::Synced});
        break;
    case ss::kRejected:
        onRejected(ss::decodeRejected(m.body));
        break;
    case ss::kRole: {
        const ss::RoleMsg r = ss::decodeRole(m.body);
        const bool changed = r.role != role_;
        role_ = r.role;
        if (changed && role_ < ss::Role::Editor) {
            // (revoked: our unsent work dropped, our locks gone with the host's table)
            undoPending();
            pending_.clear();
            ++revision_;
        }
        if (changed || !r.text.empty()) {
            Event e;
            e.kind = Event::Kind::Role;
            e.text = r.text;
            push(std::move(e));
        }
        break;
    }
    case ss::kEditRequest: {
        const ss::EditRequest r = ss::decodeEditRequest(m.body);
        std::erase_if(requests_, [&](const auto& q) { return q.peer == r.peer; });
        if (r.open) requests_.push_back(r);
        Event e;
        e.kind = Event::Kind::Request;
        e.request = r;
        push(std::move(e));
        break;
    }
    case ss::kLockTable:
        locks_ = me::LockTable::parse(m.body);
        push(Event{.kind = Event::Kind::Locks});
        break;
    case ss::kLockAnswer: {
        const ss::LockMsg a = ss::decodeLock(m.body);
        Event e;
        e.kind = Event::Kind::LockAnswer;
        e.seq = a.seq;
        e.keys = a.keys;
        push(std::move(e));
        break;
    }
    case ss::kPresence: {
        const ss::Presence p = ss::decodePresence(m.body);
        presences_[p.peer] = p;
        break;
    }
    case ss::kPublished:
        onPublished(ss::decodePublished(m.body));
        break;
    case ss::kScriptDiag: {
        const ss::ScriptDiag d = ss::decodeScriptDiag(m.body);
        lastDiag_ = d;
        Event e;
        e.kind = Event::Kind::ScriptDiag;
        e.diag = d;
        push(std::move(e));
        break;
    }
    case ss::kLive:
        live_ = ss::decodeLive(m.body);
        ++liveVersion_;
        break;
    case ss::kEditors:
        editors_ = ss::decodeEditors(m.body);
        break;
    case ss::kLog:
        logText(ss::decodeText(m.body));
        break;
    case ss::kModProject:
        onModProject(ss::decodeModProject(m.body));
        break;
    case ss::kModFile:
        onModFile(ss::decodeModFile(m.body));
        break;
    default:
        break;
    }
}

// --- The mod project -----------------------------------------------------------------------------

std::vector<std::uint8_t> EditSessionClient::joinBody() const
{
    return takesModFiles_ ? ss::encodeJoin(ss::kFeatureModFiles) : std::vector<std::uint8_t>{};
}

void EditSessionClient::onModProject(ss::ModProject m)
{
    // (decodeModProject checked the paths and the caps.) The contents we have kept for the files
    // still there.
    ModState next;
    next.id = std::move(m.id);
    next.name = std::move(m.name);
    for (auto& f : m.files) {
        ModFileState s;
        if (mod_ && mod_->id == next.id) {
            if (const auto it = mod_->files.find(f.path); it != mod_->files.end()) {
                s.bytes = std::move(it->second.bytes);
                s.bytesVersion = it->second.bytesVersion;
                s.arrivals = it->second.arrivals;
            }
        }
        s.info = std::move(f);
        const std::string path = s.info.path;
        next.files.emplace(path, std::move(s));
    }
    mod_ = std::move(next);
    ++modRevision_;
    push(Event{.kind = Event::Kind::ModProject});
}

void EditSessionClient::onModFile(ss::ModFile m)
{
    if (!mod_) {
        return;
    }
    const auto it = mod_->files.find(m.path);
    if (it == mod_->files.end()) {
        return; // (not a file of the list: let go)
    }
    // Kept only as the list says it is: its working version, deleted or its size (the list's caps
    // bound what's kept; a stale or oversized one is let go, the right one comes after it).
    const ss::ModFileInfo& info = it->second.info;
    if (m.version != info.working || m.deleted != (info.working == 0) || (!m.deleted && m.bytes.size() != info.size)) {
        return;
    }
    if (m.deleted) {
        it->second.bytes.reset();
        it->second.bytesVersion = 0;
    } else {
        it->second.bytes = std::move(m.bytes);
        it->second.bytesVersion = m.version;
    }
    ++it->second.arrivals;
    ++modRevision_;
    Event e;
    e.kind = Event::Kind::ModFile;
    e.text = m.path;
    push(std::move(e));
}

bool EditSessionClient::modReady() const
{
    return mod_ && std::ranges::all_of(mod_->files, [](const auto& f) { return f.second.have(); });
}

void EditSessionClient::modSave(const std::string& path, std::span<const std::uint8_t> bytes)
{
    if (!mod_ || !me::modPathValid(path) || bytes.size() > ss::kMaxModFile) {
        return;
    }
    ss::ModFile f;
    f.path = path;
    const auto it = mod_->files.find(path);
    f.version = it == mod_->files.end() ? 0 : it->second.info.working;
    f.bytes.assign(bytes.begin(), bytes.end());
    sendBody(ss::kModFile, ss::encode(f));
}

void EditSessionClient::modDelete(const std::string& path)
{
    if (!mod_) {
        return;
    }
    const auto it = mod_->files.find(path);
    if (it == mod_->files.end() || it->second.info.working == 0) {
        return;
    }
    ss::ModFile f;
    f.path = path;
    f.version = it->second.info.working;
    f.deleted = true;
    sendBody(ss::kModFile, ss::encode(f));
}

void EditSessionClient::modLock(const std::string& path, bool on, bool force)
{
    if (!mod_ || !mod_->files.contains(path)) {
        return;
    }
    sendBody(ss::kModLock, ss::encode(ss::ModLock{path, on, force}));
}

void EditSessionClient::modPublish()
{
    if (mod_) {
        sendBody(ss::kModPublish, {});
    }
}

void EditSessionClient::catchUpAgain(const std::string& why)
{
    // Our unsent work dropped and the document started over from the host's copy (a join again).
    logText(why + ": catching up again");
    pending_.clear();
    hasDoc_ = false;
    synced_ = false;
    sendBody(ss::kJoin, joinBody());
}

void EditSessionClient::undoPending()
{
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        me::applyOp(*doc_, it->inverse);
    }
}

void EditSessionClient::redoPending()
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        try {
            it->inverse = me::applyOp(*doc_, it->op);
            ++it;
        } catch (const std::exception& ex) {
            Event e;
            e.kind = Event::Kind::RolledBack;
            e.seq = it->seq;
            e.text = ex.what();
            push(std::move(e));
            it = pending_.erase(it);
        }
    }
}

void EditSessionClient::onCommitted(const ss::Committed& c)
{
    if (!hasDoc_) {
        return;
    }
    const me::PeerId me_ = self();
    // Ours, first in line and as we sent it: already in the working copy.
    if (c.author == me_ && c.author != 0 && !pending_.empty() && pending_.front().seq == c.seq && pending_.front().bytes == c.op) {
        Event e;
        e.kind = Event::Kind::Acked;
        e.seq = c.seq;
        e.author = c.author;
        e.op = std::move(pending_.front().op);
        e.inverse = std::move(pending_.front().inverse);
        pending_.pop_front();
        logIndex_ = c.index;
        push(std::move(e));
        return;
    }
    me::Op op = me::parseOp(c.op, things_);
    undoPending();
    const bool ours = c.author == me_ && c.author != 0 && !pending_.empty() && pending_.front().seq == c.seq;
    if (!ours) {
        // (Who changed what: a selective undo names them.)
        for (const auto& k : me::elementsTouched(*doc_, op)) lastAuthor_[k] = c.author;
    }
    me::Op inverse;
    try {
        inverse = me::applyOp(*doc_, op);
    } catch (const std::exception& ex) {
        // (never, while every member applies the host's ops in its order)
        catchUpAgain(std::string("Out of step with the host (") + ex.what() + ")");
        return;
    }
    logIndex_ = c.index;
    Event e;
    e.author = c.author;
    e.seq = c.seq;
    if (ours) {
        // Ours, changed on the way (new ids, a per-entry merge): our later ops, made before we
        // knew, follow the ids the host gave, as the host moves them in those ops too.
        const me::IdMoves moves = me::movesBetween(pending_.front().sent, op);
        pending_.pop_front();
        if (!moves.empty()) {
            for (auto& p : pending_) {
                me::remapIds(p.op, moves);
                me::remapIds(p.sent, moves);
                p.bytes = me::serializeOp(me::withoutNotes(p.op), things_);
            }
        }
        e.kind = Event::Kind::Acked;
        e.inverse = std::move(inverse);
    } else {
        e.kind = Event::Kind::Committed;
    }
    e.op = std::move(op);
    redoPending();
    ++revision_;
    push(std::move(e));
}

void EditSessionClient::onRejected(const ss::Rejected& r)
{
    const auto it = std::ranges::find_if(pending_, [&](const PendingOp& p) { return p.seq == r.seq; });
    if (it != pending_.end()) {
        undoPending(); // (the list as it was: `it` still stands)
        pending_.erase(it);
        redoPending();
        ++revision_;
    }
    Event e;
    e.kind = Event::Kind::Rejected;
    e.seq = r.seq;
    e.text = r.text;
    push(std::move(e));
}

void EditSessionClient::onPublished(const ss::Published& p)
{
    if (hello_) {
        hello_->version = p.version;
    }
    if (p.full && hasDoc_) {
        // The host's new base is its working copy as every member holds it (session_doc.hpp):
        // ours the same, from our committed state.
        undoPending();
        *doc_ = me::canonicalSessionDocument(*doc_, things_);
        published_ = *doc_;
        bool have = false;
        // (The name becomes a folder and file names in the cache: one that isn't a plain name
        // would reach outside it, so nothing is written, and the map downloads as any other.)
        const bool plainName = isPlainFileName(p.name);
        if (!cache_.empty() && plainName) {
            // (Already there with the host's CRC: the host's own client, whose cache is the maps
            // folder the host published into.)
            try {
                have = map::loadMapFile(cache_ / p.name / (p.name + ".map")).crc == p.crc;
            } catch (const std::exception&) {
            }
        }
        if (!cache_.empty() && plainName && !have) {
            try {
                const auto files = me::encodeFiles(*doc_, things_);
                const auto dir = cache_ / p.name;
                std::filesystem::create_directories(dir);
                std::ofstream map(dir / (p.name + ".map"), std::ios::binary | std::ios::trunc);
                map.write(reinterpret_cast<const char*>(files.map.data()), static_cast<std::streamsize>(files.map.size()));
                std::ofstream nxz(dir / (p.name + ".nxz"), std::ios::binary | std::ios::trunc);
                nxz.write(reinterpret_cast<const char*>(files.nxz.data()), static_cast<std::streamsize>(files.nxz.size()));
                ++publishesWritten_;
            } catch (const std::exception& ex) {
                logText(std::string("The published map couldn't be written here (it will download): ") + ex.what());
            }
        }
        redoPending();
        ++revision_;
    }
    Event e;
    e.kind = Event::Kind::Published;
    e.published = p;
    push(std::move(e));
}

// --- Actions -------------------------------------------------------------------------------------

void EditSessionClient::join()
{
    joinSent_ = true;
    sendBody(ss::kJoin, joinBody()); // (what we take: the mod project's messages)
}

std::optional<std::uint32_t> EditSessionClient::submit(const me::Op& op, me::Op* inverse, std::string* why)
{
    const auto fail = [&](std::string text) -> std::optional<std::uint32_t> {
        if (why) *why = std::move(text);
        return std::nullopt;
    };
    if (op.empty()) return fail("nothing to do");
    if (!synced_) return fail("not in step with the session yet");
    if (role_ < ss::Role::Editor) return fail("you can't edit in this session (ask the host: Request edit)");
    if (playTestRunning()) return fail("a play-test is running");
    if (const auto c = locks_.conflict(self(), me::elementsTouched(*doc_, op))) {
        return fail(c->first.text() + " is being edited by " + nameOf(c->second));
    }
    PendingOp p;
    try {
        p.inverse = me::applyOp(*doc_, op);
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
    p.seq = nextSeq_++;
    p.op = me::withoutNotes(op);
    p.bytes = me::serializeOp(p.op, things_);
    // What the host needs to place it among others' ops made meanwhile (new waypoints, the keyed
    // lists it replaced) and the log index we had.
    p.sent = p.op;
    me::annotateForHost(p.sent, p.inverse);
    if (inverse) *inverse = p.inverse;
    sendBody(ss::kOpMsg, ss::encode(ss::OpMsg{p.seq, me::serializeOp(p.sent, things_), logIndex_}));
    const std::uint32_t seq = p.seq;
    pending_.push_back(std::move(p));
    ++revision_;
    return seq;
}

me::PeerId EditSessionClient::lastChangedBy(const me::ElementKey& key) const
{
    const auto it = lastAuthor_.find(key);
    return it == lastAuthor_.end() ? 0 : it->second;
}

EditSessionClient::Selective EditSessionClient::submitSelective(const me::Op& apply, const me::Op& expect)
{
    Selective out;
    std::vector<me::UndoSkip> skips;
    me::Op cut;
    try {
        cut = me::selectiveOp(*doc_, apply, expect, &skips);
    } catch (const std::exception& ex) {
        out.why = ex.what();
        return out;
    }
    out.skipped = skips.size();
    if (!skips.empty()) {
        // "2 left as they are: wall 30,40 (changed since by Bob), object 12 (deleted since)"
        std::string list;
        for (std::size_t i = 0; i < skips.size() && i < 3; ++i) {
            const me::PeerId by = lastChangedBy(skips[i].key);
            if (!list.empty()) list += ", ";
            list += skips[i].what() + " (" + skips[i].why + (by != 0 && by != self() ? " by " + nameOf(by) : std::string()) + ")";
        }
        if (skips.size() > 3) list += " and " + std::to_string(skips.size() - 3) + " more";
        out.note = std::to_string(skips.size()) + " left as " + (skips.size() == 1 ? "it is: " : "they are: ") + list;
    }
    if (cut.empty()) {
        out.nothingLeft = true;
        return out;
    }
    out.seq = submit(cut, nullptr, &out.why);
    return out;
}

void EditSessionClient::park(bool on, std::optional<std::pair<float, float>> spot)
{
    ss::Park p;
    p.on = on;
    p.spot = spot.has_value();
    if (spot) {
        p.x = spot->first;
        p.y = spot->second;
    }
    sendBody(ss::kPark, ss::encode(p));
}

void EditSessionClient::jumpIn(std::uint16_t target)
{
    sendBody(ss::kJumpIn, ss::encodeU32(target));
}

std::set<me::ElementKey> EditSessionClient::requestLocks(const std::set<me::ElementKey>& keys)
{
    const auto refused = locks_.setHeld(self(), keys, nowMs_);
    std::set<me::ElementKey> asked;
    for (const auto& k : keys) {
        if (!refused.contains(k)) asked.insert(k);
    }
    sendBody(ss::kLock, ss::encode(ss::LockMsg{++lockSeq_, asked}));
    return refused;
}

void EditSessionClient::takeOver(const me::ElementKey& key)
{
    // ForceRelease, then our request with it: the host takes them in this order.
    forceRelease(key);
    locks_.forceRelease(key);
    std::set<me::ElementKey> keys = locks_.heldBy(self());
    keys.insert(key);
    requestLocks(keys);
}

void EditSessionClient::requestEdit(const std::string& reason)
{
    sendBody(ss::kRequestEdit, ss::encodeText(reason));
}

void EditSessionClient::answer(me::PeerId peer, std::uint8_t decision)
{
    std::erase_if(requests_, [&](const auto& q) { return q.peer == peer; });
    sendBody(ss::kAnswer, ss::encode(ss::Answer{peer, decision}));
}

void EditSessionClient::revoke(me::PeerId peer)
{
    sendBody(ss::kRevoke, ss::encodeU32(peer));
}

void EditSessionClient::forceRelease(const me::ElementKey& key)
{
    sendBody(ss::kForceRelease, ss::encodeKey(key));
}

void EditSessionClient::publish(bool full)
{
    sendBody(ss::kPublish, ss::encodeU32(full ? 0 : 1));
}

void EditSessionClient::reloadScript(bool hot)
{
    sendBody(ss::kScriptReload, ss::encodeU32(hot ? 0 : 1));
}

void EditSessionClient::playTest(bool start, std::uint32_t mode, bool playersCanDie)
{
    sendBody(ss::kPlayTest, ss::encode(ss::PlayTest{start, mode, playersCanDie}));
}

void EditSessionClient::settings(ss::EditMode mode, std::uint8_t flags)
{
    sendBody(ss::kSettings, ss::encode(ss::Settings{mode, flags}));
}

void EditSessionClient::presence(float x, float y, std::uint8_t tool, float radius, bool inView)
{
    presenceWanted_ = ss::Presence{self(), x, y, tool, radius, inView};
}

void EditSessionClient::tick(std::uint32_t nowMs)
{
    nowMs_ = nowMs;
    if (!presenceWanted_ || role_ < ss::Role::Editor || nowMs - presenceAt_ < 100) {
        return;
    }
    const auto& w = *presenceWanted_;
    if (presenceSent_ && presenceSent_->x == w.x && presenceSent_->y == w.y && presenceSent_->tool == w.tool && presenceSent_->radius == w.radius &&
        presenceSent_->inView == w.inView) {
        return;
    }
    presenceSent_ = w;
    presenceAt_ = nowMs;
    const auto body = ss::encode(w);
    for (auto& m : ss::frame(ss::kPresence, body, transfer_)) {
        send_(std::move(m), false);
    }
}

} // namespace nox::client
