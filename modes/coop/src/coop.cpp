// The co-op campaign's director (coop.hpp): the host's set-up, the party as players come and go,
// the presentation layer. The builtins' rules are coop_script.cpp's.
#include "noxcore/coop/coop.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/script/builtins.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/x87.hpp"

#include "mode_shared.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <vector>

namespace nox::coop {

namespace {

/// A builtin's id by its name (none: an id no call has).
std::uint32_t builtinId(std::string_view name)
{
    return script::findBuiltin(name).value_or(0xFFFFFFFFu);
}

} // namespace

using server::detail::widen; // (a line for MSG_TEXT_MESSAGE: the modes' own)

std::string_view className(std::uint8_t playerClass)
{
    switch (playerClass) {
    case 0: return "Warrior";
    case 1: return "Wizard";
    default: return "Conjurer";
    }
}

std::string_view firstMapOf(std::uint8_t playerClass)
{
    switch (playerClass) {
    case 0: return "War01A";
    case 1: return "Wiz01A";
    default: return "Con01A";
    }
}

std::string listedName(std::uint8_t playerClass)
{
    return "Coop " + std::string(className(playerClass));
}

std::string runnerKey(const server::HostedPlayer& p)
{
    std::string account;
    for (const std::uint8_t c : p.accept.account) {
        if (c == 0) {
            break;
        }
        account += static_cast<char>(c);
    }
    return account.empty() ? p.name : account + "/" + p.name;
}

CoopDirector::CoopDirector(Options options)
    : options_(options)
    , rng_(options.seed)
{
    options_.maxPlayers = std::clamp(options_.maxPlayers, 1, 4);
    ids_.frozen = builtinId("Frozen");
    ids_.moveObject = builtinId("MoveObject");
    ids_.journalEntry = builtinId("JournalEntry");
    ids_.journalDelete = builtinId("JournalDelete");
    ids_.journalEdit = builtinId("JournalEdit");
    ids_.hasItem = builtinId("HasItem");
    ids_.awardSpell = builtinId("AwardSpell");
    ids_.isCaller = builtinId("IsCaller");
    ids_.startupScreen = builtinId("StartupScreen");
    ids_.deathScreen = builtinId("DeathScreen");
    ids_.tellStory = builtinId("TellStory");
    ids_.giveXp = builtinId("GiveXp");
    ids_.changeGold = builtinId("ChangeGold");
    ids_.getGold = builtinId("GetGold");
    ids_.pickup = builtinId("Pickup");
}

std::filesystem::path CoopDirector::configure(server::GameHost::Options& o)
{
    o.solo = true;
    o.soloGuests = options_.maxPlayers - 1;
    o.maxPlayers = options_.maxPlayers;
    o.serverName = listedName(options_.campaignClass);
    o.features |= lobby::ServerFeature::CoopCampaign;
    o.onJoin = [this](server::GameHost& host, std::uint8_t peer, server::ServerObject& unit) { joined(host, peer, unit); };
    o.onLeave = [this](server::GameHost& host, std::uint8_t peer, server::ServerObject& unit) { leaving(host, peer, unit); };
    o.noxcppMessages[coopmsg::kOp] = [this](server::GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m) { onMessage(host, peer, m); };
    o.onChat = [this](server::GameHost& host, std::uint8_t peer, std::u16string_view text) { return onChat(host, peer, text); };
    o.onClientReady = [this](server::GameHost& host, std::uint8_t peer) { clientReady(host, peer); };
    o.forPeer = [this](server::GameHost&, std::uint8_t peer, std::span<const std::uint8_t> m) { return forPeer(peer, m); };
    o.onCharacter = [this](server::GameHost& host, std::uint8_t peer, bool loaded) { characterIn(host, peer, loaded); };
    // Stock guests: a quest to them (listed as one: a stock player brings a quest character; told
    // the quest's network flags, never the solo game's 0x800: no solo menu or saves of their own),
    // their characters read and written as a quest's. NoX Core guests are told the world's.
    o.listedMode = server::GameFlag::Quest;
    o.clientGameFlags = [](server::GameHost& host, std::uint8_t peer, const server::HostedPlayer& p, std::uint32_t flags) {
        if ((p.slot & 31u) == 0x1F || host.identified(peer)) {
            return flags; // (the host's own client, a NoX Core guest: the world's)
        }
        return (flags & ~(server::GameFlag::Solo | 0x7F0u)) | kStockGuestFlags;
    };
    return {};
}

bool CoopDirector::onChat(server::GameHost& host, std::uint8_t peer, std::u16string_view text)
{
    // The host's "?skip" (a chapter screen's wait over for everyone) and "?go" (the party on through
    // the exit it's gathering at). Taken, not relayed.
    host_ = &host;
    std::string lower;
    for (const char16_t c : text) {
        lower += (c >= u'A' && c <= u'Z') ? static_cast<char>(c + 32) : c < 0x80 ? static_cast<char>(c) : '?';
    }
    if (lower != "?skip" && lower != "?go") {
        return false;
    }
    const auto it = std::ranges::find_if(party_, [&](const Member& x) { return x.peer == peer; });
    if (it == party_.end() || !it->host) {
        return true;
    }
    if (lower == "?skip" && !reading_.empty()) {
        releaseChapter();
    } else if (lower == "?go" && gather_) {
        goNow_ = true; // (the next touch takes the party)
    } else if (lower == "?go") {
        notice(*it, coopmsg::Notice::Info, "Nobody's at a way on: ?go takes the party once someone stands at one.");
    }
    return true;
}

void CoopDirector::attach(server::GameHost& host)
{
    host_ = &host;
    host.server().setModeHooks(this);
    // Same class only: the lobby refuses every other (TRY_JOIN's class bit: the client's own
    // "class not allowed"), and the game list shows them refused.
    server::GameHost::AccessSettings a = host.accessSettings();
    a.restrictedClasses = static_cast<std::uint8_t>(7u & ~(1u << options_.campaignClass));
    a.maxPlayers = options_.maxPlayers;
    host.setAccessSettings(a);
    readPristine(host.server()); // (the first map's load came before us)
    // The saves: the party's beside the game's (the core's save wrapped); the runners a continued
    // campaign knows.
    coreSave_ = host.server().saveGameHook();
    host.server().setSaveGame([this](const std::string& name, server::ServerObject* exit) { return save(name, exit); });
    readRecords();
}

void CoopDirector::detach(server::GameHost& host)
{
    if (host.server().modeHooks() == this) {
        host.server().setModeHooks(nullptr);
        host.server().setSaveGame(std::exchange(coreSave_, {}));
    }
    host_ = nullptr;
}

server::World& CoopDirector::world()
{
    return host_->server().world();
}

std::vector<server::ServerObject*> CoopDirector::partyUnits()
{
    std::vector<server::ServerObject*> out;
    if (!host_) {
        return out;
    }
    if (server::ServerObject* h = server::hostPlayer(world())) {
        out.push_back(h);
    }
    for (const Member& m : party_) {
        server::ServerObject* u = host_->unitOf(m.peer);
        if (u && u->player && !u->gone() && std::ranges::find(out, u) == out.end()) {
            out.push_back(u);
        }
    }
    return out;
}

bool CoopDirector::isHostUnit(const server::ServerObject* o)
{
    return o && o->player && o->player->index == 0x1F;
}

CoopDirector::Member* CoopDirector::memberOfUnit(const server::ServerObject& unit)
{
    if (!host_) {
        return nullptr;
    }
    for (Member& m : party_) {
        if (host_->unitOf(m.peer) == &unit) {
            return &m;
        }
    }
    return nullptr;
}

void CoopDirector::intoTeam(server::ServerObject& unit)
{
    // (soloTeam puts the host in team 1, "COOP", at each campaign map's load; the guests go with it.)
    server::World& w = world();
    server::addTeamMember(w, 1, unit, true, false, rng_, host_->server().clock().frame());
}

void CoopDirector::joined(server::GameHost& host, std::uint8_t peer, server::ServerObject& unit)
{
    host_ = &host;
    const auto it = host.players().find(peer);
    if (it == host.players().end()) {
        return;
    }
    Member m;
    m.key = runnerKey(it->second);
    m.name = it->second.name;
    m.peer = peer;
    m.extent = unit.extent;
    m.host = isHostUnit(&unit);
    m.identified = host.identified(peer);
    // (By its peer; a same-key entry only when its own peer has gone: a rejoin. Names aren't unique,
    // and a LAN game has no accounts.)
    std::erase_if(party_, [&](const Member& x) { return x.peer == peer || (x.key == m.key && !host.players().contains(x.peer)); });
    party_.push_back(m);
    rosterDirty_ = true;
    if (m.host) {
        return;
    }
    // A runner the campaign knows: its campaign character back before its first frame (its own
    // isn't asked for: Hello's characterKept).
    restoreGuest(party_.back(), unit, false);
    // A guest: with the host in team 1, by the host (a clear point within 50 of it), its screen
    // faded in once it's loaded the map (the map's start faded the others in: a solo game's join
    // sends none).
    server::World& w = world();
    intoTeam(unit);
    if (const server::ServerObject* h = server::hostPlayer(w)) {
        const auto [x, y] = server::clearPointNear(w, rng_, h->x, h->y, 50.0f);
        server::setPosition(w, unit, x, y, host.server().clock().frame());
    }
    w.broadcasts().push_back({{0xE4, 1, 1}, true, unit.extent}); // (MSG_FADE_BEGIN {1, 1})
}

void CoopDirector::leaving(server::GameHost& host, std::uint8_t peer, server::ServerObject& unit)
{
    host_ = &host;
    // The wipe's last one gone before its dying second was over: the map's PlayerDeath functions
    // (the party's loss) run now, as its dead state would have.
    if (wipe_ && !wipeRan_ && unit.extent == wipeBy_ && world().script) {
        wipeRan_ = true;
        const util::RoundingScope rounding(host.server().exactMaths());
        world().script->runEntryPoints("PlayerDeath");
    }
    // Its keys to the host (the party's doors), then its campaign character kept (WORKING's Party):
    // back to it if it comes again (the next save takes it along).
    if (const auto it = std::ranges::find_if(party_, [&](const Member& x) { return x.peer == peer; }); it != party_.end() && !it->host) {
        keysToHost(unit);
        if (!keepGuest(*it, unit).empty()) {
            writeRecords();
        }
    }
    std::erase_if(party_, [&](const Member& x) { return x.peer == peer; });
    std::erase_if(toldAt_, [&](const auto& t) { return t.first.first == unit.extent; });
    downs_.erase(peer);
    fileNumbered_.erase(peer);
    if (gather_) {
        gather_->standing.erase(unit.extent);
    }
    rosterDirty_ = true;
}

void CoopDirector::onMessage(server::GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m)
{
    host_ = &host;
    const auto p = coopmsg::parse(m);
    if (!p) {
        return;
    }
    const auto& [sub, rest] = *p;
    if (sub == coopmsg::kChapterReady) {
        // A guest's chapter screen over: the pause let go once the last is.
        if (reading_.erase(peer) != 0) {
            tellReadyState();
            if (reading_.empty()) {
                releaseChapter();
            }
        }
    } else if (sub == coopmsg::kHostCmd) {
        const auto cmd = coopmsg::parseHostCmd(rest);
        const auto it = std::ranges::find_if(party_, [&](const Member& x) { return x.peer == peer; });
        if (!cmd || it == party_.end() || !it->host) {
            return; // (the host's alone)
        }
        if (*cmd == coopmsg::HostCmd::SkipBriefing && !reading_.empty()) {
            releaseChapter();
        }
    }
}

void CoopDirector::mapLoaded(server::Server& server)
{
    if (!host_) {
        return;
    }
    sceneGathered_ = false; // (a scene the map change ended: the next map's bars gather again)
    // (The teams came along from the last map; the first map's load had no guests. Again, in case.)
    for (const Member& m : party_) {
        if (server::ServerObject* u = host_->unitOf(m.peer); u && u->player && !m.host) {
            intoTeam(*u);
            if (u->flags & server::ObjFlag::Dead) {
                downs_[m.peer] = 0; // (down as the party left: back as soon as the checkpoint's known)
            }
        }
    }
    // A new map (or a saved game's): the party whole again, its checkpoint where the host is once
    // the map's running, no gathering.
    wipe_ = false;
    wipeBy_ = 0;
    wipeRan_ = false;
    fileNumbered_.clear();
    checkpoint_.reset();
    gather_.reset();
    goNow_ = false;
    scriptMovedAt_.reset();
    readPristine(server);
    // The new map's monsters by the party's size.
    copies_.clear();
    sceneViewers_.clear();
    sceneTalker_ = 0;
    healthTold_.clear(); // (each told again on the new map)
    baseHealth_.clear();
    scaledHealth_ = 1.0f;
    rescale(true);
    if (std::exchange(loadPending_, false)) {
        restoreParty(); // (a saved game's map: the party as the save had it)
    }
    rosterDirty_ = true;
}

void CoopDirector::tick(server::GameHost& host, std::uint32_t nowMs)
{
    (void)nowMs;
    host_ = &host;
    // (The game's x87 rounding for what moves units: called between the host's frames, outside its scope.)
    const util::RoundingScope rounding(host.server().exactMaths());
    // Each player, once in (its map loaded): who's the host and the campaign, and the rules'
    // notice (a guest's).
    for (Member& m : party_) {
        const auto it = host.players().find(m.peer);
        if (it == host.players().end() || !it->second.ready || m.welcomed) {
            continue;
        }
        m.welcomed = true;
        m.welcomedAt = world().ticks;
        tellHello(host, m);
        if (!m.host) {
            notice(m, coopmsg::Notice::Rules, kRulesLine);
            joinScene(m); // (a scene under way: its bars, held as the party is)
        }
        rosterDirty_ = true;
    }
    // Same class only, past the lobby too: a guest whose own character (its .plr, loaded after the
    // lobby's check of the class its join named) is of another class is told why, and let go 3 s
    // on, as the vote's kick does.
    std::vector<std::uint8_t> otherClass;
    for (Member& m : party_) {
        const server::ServerObject* u = host.unitOf(m.peer);
        if (m.host || !u || !u->player || u->player->classId == options_.campaignClass) {
            continue;
        }
        if (m.otherClassAt == 0) {
            m.otherClassAt = std::max<std::uint64_t>(world().ticks, 1);
            notice(m, coopmsg::Notice::Info, "This co-op campaign is for " + std::string(className(options_.campaignClass)) + " characters only.");
        } else if (world().ticks - m.otherClassAt >= 3u * 30u) {
            otherClass.push_back(m.peer);
        }
    }
    for (const std::uint8_t peer : otherClass) {
        host.kickPeer(peer); // (leaving: party_ changes)
    }
    // A new player behind the party caught up, once its own character is in (or none came: 10 s).
    for (Member& m : party_) {
        if (m.host || !m.welcomed || m.caughtUp) {
            continue;
        }
        if (m.characterIn || m.restored || world().ticks - m.welcomedAt > 10u * 30u) {
            if (server::ServerObject* u = host.unitOf(m.peer); u && u->player) {
                catchUp(m, *u);
            }
        }
    }
    // The checkpoint: where the host stands as the map first runs (past the load's pause).
    server::World& w = world();
    if (!checkpoint_ && !(w.gameFlags & server::GameFlag::Paused)) {
        if (const server::ServerObject* h = server::hostPlayer(w); h && !(h->flags & server::ObjFlag::Dead)) {
            checkpoint_ = std::pair{h->x, h->y};
        }
    }
    if (checkpoint_) {
        respawnDue();
    }
    gatherTick();
    rescale(false); // (the party's size changed: at once; else the monsters not seen yet, each 5 s)
    tellRoster(host);
    watchDialog();
    watchSceneDialog();
    tellPartyHealth();
    endUnpausedLevelUp();
    // A chapter screen's wait given up after a minute and a half (a guest who never answers).
    if (!reading_.empty() && world().ticks - chapterAt_ > 90u * 30u) {
        releaseChapter();
    }
    // A guest that left while reading isn't waited for.
    std::erase_if(reading_, [&](std::uint8_t peer) { return !host.players().contains(peer); });
    if (readingOf_ != 0 && reading_.empty() && hostDoneReading_) {
        releaseChapter();
    }
}

void CoopDirector::tellHello(server::GameHost& host, const Member& m)
{
    if (!m.identified) {
        return;
    }
    coopmsg::Hello h;
    h.campaignClass = options_.campaignClass;
    h.maxPlayers = static_cast<std::uint8_t>(options_.maxPlayers);
    h.youAreHost = m.host;
    h.characterKept = m.restored;
    if (const server::ServerObject* u = server::hostPlayer(world())) {
        h.hostNetCode = server::netCodeOf(*u);
    }
    host.sendToIdentified(m.peer, coopmsg::helloMessage(h));
}

void CoopDirector::tellRoster(server::GameHost& host)
{
    const auto flagsOf = [](const Member& m, const server::ServerObject& u) {
        return static_cast<std::uint8_t>((m.host ? 1u : 0u) | ((u.flags & server::ObjFlag::Dead) ? 2u : 0u) | (m.identified ? 4u : 0u));
    };
    // (The party read against the roster last told in place, built again only as it differs.)
    if (!rosterDirty_) {
        std::size_t i = 0;
        bool same = true;
        for (const Member& m : party_) {
            const server::ServerObject* u = host.unitOf(m.peer);
            if (!u || !u->player) {
                continue;
            }
            const coopmsg::Roster::Member* x = i < lastRoster_.members.size() ? &lastRoster_.members[i++] : nullptr;
            if (!x || x->netCode != server::netCodeOf(*u) || x->playerClass != u->player->classId || x->level != u->player->level ||
                x->flags != flagsOf(m, *u) || x->respawnIn != 0 || x->name != m.name) {
                same = false;
                break;
            }
        }
        if (same && i == lastRoster_.members.size()) {
            return;
        }
    }
    coopmsg::Roster r;
    for (const Member& m : party_) {
        const server::ServerObject* u = host.unitOf(m.peer);
        if (!u || !u->player) {
            continue;
        }
        coopmsg::Roster::Member x;
        x.netCode = server::netCodeOf(*u);
        x.playerClass = u->player->classId;
        x.level = u->player->level;
        x.flags = flagsOf(m, *u);
        x.name = m.name;
        r.members.push_back(std::move(x));
    }
    if (!rosterDirty_ && r == lastRoster_) {
        return;
    }
    rosterDirty_ = false;
    lastRoster_ = r;
    const auto bytes = coopmsg::rosterMessage(r);
    for (const Member& m : party_) {
        if (m.identified && m.welcomed) {
            host.sendToIdentified(m.peer, bytes);
        }
    }
}

void CoopDirector::notice(const Member& m, coopmsg::Notice kind, std::string_view text)
{
    if (!host_) {
        return;
    }
    if (m.identified) {
        host_->sendToIdentified(m.peer, coopmsg::noticeMessage(kind, text));
        return;
    }
    // A stock client: the server's line (MSG_TEXT_MESSAGE from no one: shown in the middle of its
    // screen for a few seconds, and in its console, while it's in play).
    if (const server::ServerObject* u = host_->unitOf(m.peer)) {
        world().broadcasts().push_back({server::World::textMessage(widen(text), 0), true, u->extent});
    }
}

void CoopDirector::hostOnly(server::ServerObject& guest, std::uint32_t what)
{
    const Member* m = memberOfUnit(guest);
    if (!m || m->host) {
        return;
    }
    // (At most once in 10 s for the same thing: a trigger is touched every frame it's stood on.)
    const std::uint32_t now = world().frameNow;
    auto& at = toldAt_[{guest.extent, what}];
    if (at != 0 && now - at < 300u) {
        return;
    }
    at = now == 0 ? 1u : now;
    notice(*m, what == 0xFFFFFFFFu ? coopmsg::Notice::TalkRefused : coopmsg::Notice::HostOnly,
           what == 0xFFFFFFFFu ? kTalkRefusedLine : kHostOnlyLine);
}

void CoopDirector::talkTaken(server::ServerObject& player, const server::ServerObject& talker)
{
    const Member* m = memberOfUnit(player);
    if (!m) {
        return;
    }
    const std::uint32_t now = world().frameNow;
    auto& at = toldAt_[{player.extent, 0xFFFFFFFEu}];
    if (at != 0 && now - at < 90u) {
        return;
    }
    at = now == 0 ? 1u : now;
    notice(*m, coopmsg::Notice::TalkRefused, nameOf(talker) + std::string(kTalkTakenLine));
}

bool talkPlaysScene(const script::ScriptFile& file, std::int32_t function)
{
    using script::Op;
    // The builtins that put a scene up: the bars, black, a chapter screen, the ending.
    static const std::array<std::uint32_t, 5> sceneBuiltins{builtinId("WideScreen"), builtinId("Blind"), builtinId("ImmediateBlind"),
                                                           builtinId("StartupScreen"), builtinId("EndGame")};
    // (A value an argument's own instruction pushes, nothing else between it and the call.)
    const auto singlePush = [](std::uint32_t op) {
        return op == static_cast<std::uint32_t>(Op::PushInt) || op == static_cast<std::uint32_t>(Op::PushFloat) ||
               op == static_cast<std::uint32_t>(Op::PushString) || op == static_cast<std::uint32_t>(Op::LoadVarInt) ||
               op == static_cast<std::uint32_t>(Op::LoadVarFloat) || op == static_cast<std::uint32_t>(Op::LoadVarString);
    };
    if (function < 0) {
        return false;
    }
    std::vector<std::uint32_t> todo{static_cast<std::uint32_t>(function)};
    std::set<std::uint32_t> seen;
    while (!todo.empty()) {
        const std::uint32_t f = todo.back();
        todo.pop_back();
        if (f >= file.functions.size() || !seen.insert(f).second) {
            continue;
        }
        const std::vector<std::uint32_t>& code = file.functions[f].code;
        std::vector<std::size_t> starts; // (each instruction's first word, before the one at hand)
        for (std::size_t i = 0; i < code.size();) {
            const std::uint32_t op = code[i];
            if (!script::opName(op)) {
                break; // (the interpreter returns there)
            }
            const std::size_t next = i + 1 + script::operandCount(op);
            if (next > code.size()) {
                break;
            }
            if (op == static_cast<std::uint32_t>(Op::CallScript)) {
                todo.push_back(code[i + 1]);
            } else if (op == static_cast<std::uint32_t>(Op::CallBuiltin)) {
                const std::uint32_t id = code[i + 1];
                if (std::ranges::find(sceneBuiltins, id) != sceneBuiltins.end()) {
                    return true;
                }
                if (id < script::kBuiltinCount) {
                    // A function handed to the builtin (a timer's, a callback's, SetDialog's): its
                    // index pushed as the argument's own instruction.
                    const auto& args = script::builtinSignature(id).args;
                    for (std::size_t a = 0; a < args.size(); ++a) {
                        const std::size_t back = args.size() - a; // (instructions before the call)
                        if (args[a] != script::Type::Function || back > starts.size()) {
                            continue;
                        }
                        bool plain = true;
                        for (std::size_t k = starts.size() - back; k < starts.size(); ++k) {
                            plain = plain && singlePush(code[starts[k]]);
                        }
                        const std::size_t at = starts[starts.size() - back];
                        if (plain && code[at] == static_cast<std::uint32_t>(Op::PushInt)) {
                            todo.push_back(code[at + 1]);
                        }
                    }
                }
            }
            starts.push_back(i);
            i = next;
        }
    }
    return false;
}

bool CoopDirector::mayStartDialog(server::Fight& fight, server::ServerObject& player, server::ServerObject& npc)
{
    (void)fight;
    if (!host_) {
        return true;
    }
    // A click the game's dialogStart would do nothing with (a dead player, an NPC with nothing to
    // say): left to it, no notice.
    const bool wouldTalk = (player.classes & things::ObjectClass::Player) && (npc.classes & things::ObjectClass::Monster) &&
                           !(player.flags & (server::ObjFlag::Dead | server::ObjFlag::Destroyed)) && (npc.xStatus & 0x10) && npc.monster &&
                           npc.monster->dialogStart != -1 && npc.monster->dialogEnd != -1;
    if (!wouldTalk) {
        return true;
    }
    // One talker for an NPC: a talk's stage lives in the map's globals and its answer on the NPC,
    // so two at once on one NPC would run its stage (its reward, its journal) twice.
    for (server::ServerObject* u : partyUnits()) {
        if (u != &player && u->player && u->player->dialogWith == npc.extent) {
            talkTaken(player, *u);
            return false;
        }
    }
    if (isHostUnit(&player)) {
        return true;
    }
    // Anyone talks, at any time (two at once as in the game's quest), but a talk that can play a
    // scene is the host's, as the scenes are. (Its functions serve each of the talk's stages, so
    // one that plays a scene at any stage is the host's at every stage.)
    if (server::ScriptHost* s = world().script) {
        const script::ScriptFile& file = s->vm().file();
        if (talkPlaysScene(file, npc.monster->dialogStart) || talkPlaysScene(file, npc.monster->dialogEnd)) {
            hostOnly(player, 0xFFFFFFFFu);
            return false;
        }
    }
    return true;
}

} // namespace nox::coop
