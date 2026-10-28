// BotSquad (bot_squad.hpp): network bots on one server. Each tick it pumps every bot's two
// sockets, lets its brain think, keeps the count of bots to what the server has room for, hands
// out team roles, and turns what one listening bot hears (chat, kills, notices) into the log,
// the chat, banter and the people list a UI shows.
#include "noxcore/bot/bot_squad.hpp"

#include "noxcore/bot/bot_host.hpp"
#include "noxcore/bot/caster_brain.hpp"
#include "noxcore/bot/warrior_brain.hpp"
#include "noxcore/content/content_pack.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>

namespace nox::bot {

namespace {

using Stage = client::HeadlessClient::Stage;

/// A 22-digit serial of its own for each bot: a server refuses a duplicate (join error 12).
std::string serialFor(std::mt19937& rng)
{
    std::string s;
    nox::util::uniform_int_distribution<int> digit(0, 9);
    for (int i = 0; i < 22; ++i) {
        s += static_cast<char>('0' + digit(rng));
    }
    return s;
}

/// A bot's name, as the people lists tell them apart: "[BOT] " first.
bool botNamed(std::string_view name)
{
    return name.starts_with("[BOT] ");
}

/// A person, as the kill banter and records count them: a name, and not a bot's.
bool person(const client::PlayerEntry& p)
{
    return !p.name.empty() && !botNamed(p.name);
}

} // namespace

// One bot: its client and brain (NetBot), its own lobby and game sockets, and its bookkeeping.
struct BotSquad::Seat {
    std::unique_ptr<NetBot> bot;
    net::UdpSocket lobby;
    net::UdpSocket game;
    std::string name;
    std::string serial; ///< the CD key it joins with
    std::uint8_t playerClass = 0;
    std::uint32_t startAt = 0;
    bool started = false;
    bool leaving = false;
    std::uint32_t leftAt = 0;
    std::uint32_t inGameAt = 0; ///< when it got into the game (0: not yet)
    bool wasDead = false;
    int deaths = 0;
    bool failNoted = false; ///< a join that failed has held the next back (joinRetryAt_)
    const Persona* persona = nullptr; ///< who it is (null: a plain "[BOT] " name, the roster used up)
    std::uint32_t banteredAt = 0;     ///< its last taunt or whine (0 none yet)
    std::uint32_t nagAt = 0;          ///< a chat map: its next nudge to the host (0: not yet set)
    // Our content packs' added object types: the join waits for the host's answer to the query
    // naming them (resent every kTypesResendMs, kTypesWaitMs at most).
    bool typesAsked = false;
    bool typesAnswered = false;
    std::uint32_t typesFirstAt = 0;
    std::uint32_t typesAskedAt = 0;

    // Gone for good: never started and told to leave, closed or failed, or left 25 s ago with no answer.
    bool done(std::uint32_t nowMs) const
    {
        if (!started) {
            return leaving; // never joined
        }
        const Stage stage = bot->client().stage();
        return stage == Stage::Closed || stage == Stage::Failed || (leaving && nowMs - leftAt > 25'000);
    }
};

BotSquad::BotSquad(SquadOptions options, const things::ThingDb& things, const ItemKinds& kinds)
    : options_(std::move(options))
    , things_(things)
    , kinds_(kinds)
    , rng_(options_.seed != 0 ? options_.seed : std::random_device{}())
{
}

BotSquad::~BotSquad() = default;

void BotSquad::log(std::string line)
{
    log_.push_back(std::move(line));
    // The log is capped: past 1000 lines the oldest 500 go.
    if (log_.size() > 1000) {
        log_.erase(log_.begin(), log_.begin() + 500);
    }
}

std::vector<std::string> BotSquad::takeLog()
{
    return std::exchange(log_, {});
}

std::size_t BotSquad::maxBots() const
{
    const std::optional<int> limit = options_.playerLimit ? options_.playerLimit : gameLimit_;
    // (Unknown: the game's most, 32; balance() lets one in to learn the real one.)
    return static_cast<std::size_t>(std::max(0, limit.value_or(32) - SquadOptions::kSeatsKept));
}

std::size_t BotSquad::active() const
{
    return static_cast<std::size_t>(std::ranges::count_if(seats_, [&](const auto& s) { return !s->done(lastTick_); }));
}

std::size_t BotSquad::add(std::uint8_t playerClass, std::size_t count, std::uint32_t nowMs)
{
    const std::size_t room = maxBots() > wanted_.size() ? maxBots() - wanted_.size() : 0;
    if (count > room) {
        log("only " + std::to_string(room) + " more bot" + (room == 1 ? "" : "s") + " fit (at most " +
            std::to_string(maxBots()) + " here: the player limit less " + std::to_string(SquadOptions::kSeatsKept) +
            " kept for people)");
        count = room;
    }
    wanted_.insert(wanted_.end(), count, playerClass);
    balancedAt_ = 0; // (the first of them now, if there's room)
    balance(nowMs);
    return count;
}

void BotSquad::addSeat(std::uint8_t playerClass, std::uint32_t atMs)
{
    // The NetBot is made below, once the profile is filled in.
    auto seat = std::make_unique<Seat>(Seat{nullptr, net::UdpSocket::open(), net::UdpSocket::open()});
    client::PlayerProfile profile;
    // Who: a bot coming back is the one that went (one of its class gone, not in again, of the
    // difficulties wanted now);
    // else a persona of the class in the difficulty range, not in already (ours or anyone's
    // name in the game). (Chosen apart from the profile: its name starts as the test clients'
    // default, "NoXPlayer".)
    // Names in use: our bots still here and everyone the listening bot sees in the game.
    std::set<std::string> inUse;
    for (const auto& s : seats_) {
        if (!s->done(lastTick_)) {
            inUse.insert(s->name);
        }
    }
    if (listener_) {
        for (const auto& [netCode, p] : listener_->bot->client().world().players()) {
            inUse.insert(p.name);
        }
    }
    const Persona* persona = nullptr;
    std::string name;
    // A gone seat of this class whose persona is in the wanted range comes back as itself.
    for (const auto& s : seats_) {
        if (s->done(lastTick_) && s->playerClass == playerClass && !inUse.contains(s->name) &&
            (!s->persona || (s->persona->tier >= options_.minTier && s->persona->tier <= options_.maxTier))) {
            name = s->name;
            persona = s->persona;
            break;
        }
    }
    if (name.empty()) {
        persona = pickPersona(playerClass, options_.minTier, options_.maxTier, inUse, rng_);
        name = persona ? playerNameOf(*persona) : botName(rng_, names_);
    }
    profile.name = name;
    profile.playerClass = playerClass;
    profile.serial = serialFor(rng_);
    profile.password = options_.password;
    names_.insert(profile.name);
    seat->name = profile.name;
    seat->serial = profile.serial;
    seat->playerClass = playerClass;
    seat->startAt = atMs;
    seat->persona = persona;
    // The persona's tier picks the skill; with the roster used up, the lowest tier wanted.
    // (a class the game doesn't have: a warrior's)
    const Skill skill = skillFor(persona ? persona->tier : options_.minTier);
    auto brain = makeBrain(playerClass, skill, rng_(), persona ? persona->style : Style::Balanced);
    if (!brain) {
        brain = std::make_unique<WarriorBrain>(skill, rng_());
    }
    seat->bot = std::make_unique<NetBot>(profile, things_, kinds_, options_.mapsDir, options_.mapCache, std::move(brain));
    // The gone seat of that name, if any, makes way: one row per bot in the status. Of the other
    // gone ones (each a whole client, its world and two sockets) the newest kGoneKept stay, for
    // the status and to come back as themselves; the rest, the oldest, go: a squad up for days
    // changed classes and names often enough to keep hundreds.
    // (The tick's listener may be a gone seat, closed since the tick chose it: not held past it,
    // an add() before the next tick reads it.)
    const std::size_t othersGone = static_cast<std::size_t>(std::ranges::count_if(seats_, [&](const auto& s) {
        return s->done(lastTick_) && s->name != seat->name;
    }));
    std::size_t oldestToDrop = othersGone > kGoneKept ? othersGone - kGoneKept : 0;
    std::erase_if(seats_, [&](const auto& s) {
        if (!s->done(lastTick_)) {
            return false;
        }
        bool drop = s->name == seat->name;
        if (!drop && oldestToDrop > 0) {
            --oldestToDrop;
            drop = true;
        }
        if (drop && s.get() == listener_) {
            listener_ = nullptr;
        }
        return drop;
    });
    seats_.push_back(std::move(seat));
}

bool BotSquad::mayJoin(std::uint32_t nowMs)
{
    if (!options_.skipNoxCoreOnly || coreProbe_ == CoreProbe::Clear) {
        return true;
    }
    if (coreProbe_ == CoreProbe::CoreOnly) {
        return false;
    }
    // The extended query from a socket of the probe's own (the bots' sockets stay unknown to the
    // host, as stock clients'): a noxcpp host answers at once, a stock one never.
    const auto ask = [&] {
        coreProbeSocket_->sendTo(options_.server, lobby::encodeExtendedQuery(nowMs));
        coreProbeAskedAt_ = nowMs;
    };
    try {
        if (coreProbe_ == CoreProbe::NotAsked) {
            coreProbeSocket_.emplace(net::UdpSocket::open());
            coreProbe_ = CoreProbe::Asking;
            coreProbeAt_ = nowMs;
            ask();
        }
        std::uint8_t buffer[2048];
        while (const auto got = coreProbeSocket_->receive(buffer)) {
            const auto reply = got->from == options_.server ? lobby::decodeExtendedReply(std::span(buffer, got->size)) : std::nullopt;
            if (reply) {
                coreProbe_ = (reply->server.features & lobby::ServerFeature::NoxCoreOnly) ? CoreProbe::CoreOnly : CoreProbe::Clear;
            }
        }
        if (coreProbe_ == CoreProbe::Asking && nowMs - coreProbeAskedAt_ >= 500) {
            ask();
        }
    } catch (const std::exception& ex) {
        log(std::string("network error asking the server what it is: ") + ex.what());
        coreProbe_ = CoreProbe::Clear;
    }
    if (coreProbe_ == CoreProbe::Asking && nowMs - coreProbeAt_ >= SquadOptions::kCoreProbeMs) {
        coreProbe_ = CoreProbe::Clear; // (no answer: a stock host)
    }
    if (coreProbe_ != CoreProbe::Asking) {
        coreProbeSocket_.reset();
        if (coreProbe_ == CoreProbe::CoreOnly) {
            log("a NoX Core-only game: no bots join it (they'd be held out and put out as stock players)");
        }
    }
    return coreProbe_ == CoreProbe::Clear;
}

std::optional<int> BotSquad::others(std::uint32_t nowMs)
{
    // As a bot in the game a while sees them: everyone not ours, spectators too (they take a
    // place).
    std::set<std::string> ours;
    for (const auto& s : seats_) {
        if (!s->done(nowMs)) {
            ours.insert(s->name);
        }
    }
    for (const auto& s : seats_) {
        if (s->inGameAt != 0 && nowMs - s->inGameAt > 3000 && s->bot->client().stage() == Stage::InGame) {
            return static_cast<int>(std::ranges::count_if(s->bot->client().world().players(), [&](const auto& p) {
                return !ours.contains(p.second.name);
            }));
        }
    }
    // None of ours in: the game list's count, asked every 15 s.
    if (std::ranges::any_of(seats_, [&](const auto& s) { return !s->done(nowMs); })) {
        return std::nullopt; // (one on its way in: it'll see)
    }
    if (!probe_) {
        lobby::ServerBrowser::Options o;
        o.xwis = false;
        o.lan = false;
        o.servers = {options_.server.toString()};
        probe_ = std::make_unique<lobby::ServerBrowser>(o);
    }
    if (probedAt_ == 0 || nowMs - probedAt_ > 15'000) {
        probe_->refresh(nowMs);
        probedAt_ = nowMs;
    }
    probe_->poll(nowMs);
    for (const auto& e : probe_->servers()) {
        if (e.answered && e.game.address == options_.server.address && e.game.port == options_.server.port) {
            if (e.game.maxPlayers > 0 && !options_.playerLimit) {
                gameLimit_ = e.game.maxPlayers;
            }
            return e.game.players;
        }
    }
    return std::nullopt;
}

bool BotSquad::pingHost(std::uint32_t nowMs)
{
    // A ping and the extended query beside it (a game list's: lobby::ServerBrowser), once: a host
    // that answers the ping but not the query is a stock one.
    constexpr std::uint32_t kPingWaitMs = 3000;  // (no answer at all by then: joined as a stock one)
    constexpr std::uint32_t kQueryGraceMs = 50; // (the query's answer comes just after the ping's)
    if (!hostProbe_) {
        lobby::ServerBrowser::Options o;
        o.xwis = false;
        o.lan = false;
        o.servers = {options_.server.toString()};
        hostProbe_ = std::make_unique<lobby::ServerBrowser>(o);
        hostProbe_->refresh(nowMs);
        hostPingAt_ = nowMs;
    }
    hostProbe_->poll(nowMs);
    for (const auto& e : hostProbe_->servers()) {
        if (e.game.address != options_.server.address || e.game.port != options_.server.port) {
            continue;
        }
        if (e.extended) {
            hostAnswers_ = true;
        } else if (e.answered) {
            hostPingAnsweredAt_ = hostPingAnsweredAt_ != 0 ? hostPingAnsweredAt_ : nowMs;
            if (nowMs - hostPingAnsweredAt_ >= kQueryGraceMs) {
                hostSilent_ = true;
                log("the server answers as a stock one: the bots join at once");
            }
        }
    }
    if (!hostAnswers_ && !hostSilent_ && nowMs - hostPingAt_ >= kPingWaitMs) {
        hostSilent_ = true;
        log("the server didn't answer a ping: the bots join at once");
    }
    if (hostAnswers_ || hostSilent_) {
        hostProbe_.reset();
        return true;
    }
    return false;
}

std::size_t BotSquad::allowedBots(std::optional<int> limit, std::optional<int> others, std::size_t mine, std::size_t wanted)
{
    std::size_t allowed = 0;
    if (limit && others) {
        allowed = static_cast<std::size_t>(std::max(0, *limit - SquadOptions::kSeatsKept - *others));
    } else {
        allowed = mine == 0 ? 1 : mine; // (one in to see; nothing changes until it does)
    }
    return std::min(allowed, wanted);
}

void BotSquad::balance(std::uint32_t nowMs)
{
    if (quitting_ || leaveAt_ != 0) {
        return;
    }
    // The bots in or on their way in (not leaving), oldest first.
    std::vector<Seat*> mine; // in, or on the way (oldest first)
    for (const auto& s : seats_) {
        if (!s->leaving && !s->done(nowMs)) {
            mine.push_back(s.get());
        }
    }
    const auto people = others(nowMs);
    const std::size_t allowed =
        allowedBots(options_.playerLimit ? options_.playerLimit : gameLimit_, people, mine.size(), wanted_.size());
    // At most one join or leave each kBalanceMs (balancedAt_ 0: at once).
    if (balancedAt_ != 0 && nowMs - balancedAt_ < kBalanceMs) {
        return;
    }
    if (mine.size() > allowed) {
        // People need the place (or fewer are wanted): the newest goes.
        Seat& s = *mine.back();
        log(people && mine.size() <= wanted_.size()
                ? "[" + s.name + "] leaving to keep " + std::to_string(SquadOptions::kSeatsKept) + " places free (" +
                      std::to_string(*people) + " others in)"
                : "[" + s.name + "] leaving");
        leaveSeat(s, nowMs);
        balancedAt_ = nowMs;
        return;
    }
    // (Signed: joinRetryAt_ may lie ahead of nowMs.)
    if (mine.size() < allowed && static_cast<std::int32_t>(nowMs - joinRetryAt_) >= 0) {
        // Room again: a bot of a class that's short.
        // How many of each class are short: wanted, less those in.
        std::map<std::uint8_t, int> short_;
        for (const auto c : wanted_) {
            ++short_[c];
        }
        for (const auto* s : mine) {
            --short_[s->playerClass];
        }
        for (const auto c : wanted_) {
            if (short_[c] > 0) {
                addSeat(c, nowMs);
                log("[" + seats_.back()->name + "] " + (mine.empty() && balancedAt_ == 0 ? "joining" : "room for it: joining"));
                break;
            }
        }
        balancedAt_ = nowMs;
    }
}

void BotSquad::leaveSeat(Seat& s, std::uint32_t nowMs)
{
    if (!s.leaving) {
        s.leaving = true;
        s.leftAt = nowMs;
        if (s.started) {
            s.bot->client().quit();
        }
    }
}

void BotSquad::remove(std::size_t count, std::uint32_t nowMs)
{
    std::size_t removed = 0;
    for (auto it = seats_.rbegin(); it != seats_.rend() && removed < count; ++it) {
        if (!(*it)->leaving && !(*it)->done(nowMs)) {
            // Wanted no more: its class off the wanted list (the newest of it).
            if (const auto w = std::ranges::find(wanted_.rbegin(), wanted_.rend(), (*it)->playerClass); w != wanted_.rend()) {
                wanted_.erase(std::next(w).base());
            }
            leaveSeat(**it, nowMs);
            ++removed;
        }
    }
    // Fewer in than asked to remove (some away making room): the rest off the wanted list.
    for (; removed < count && !wanted_.empty(); ++removed) {
        wanted_.pop_back();
    }
}

void BotSquad::setWanted(const std::vector<std::uint8_t>& classes, std::uint32_t nowMs)
{
    std::map<std::uint8_t, int> want;
    for (const auto c : classes) {
        ++want[c];
    }
    // The voice stays: it takes a place of its class if there is one, else one of the class
    // most wanted (the count is what's kept).
    Seat* voice = voiceSeat();
    if (voice && !classes.empty()) {
        auto it = want.find(voice->playerClass);
        if (it == want.end() || it->second == 0) {
            it = std::ranges::max_element(want, {}, &std::pair<const std::uint8_t, int>::second);
        }
        --it->second;
    }
    // The rest, oldest first: kept while their class still wants them, else away.
    for (auto& s : seats_) {
        if (s.get() == voice || s->leaving || s->done(nowMs)) {
            continue;
        }
        if (auto it = want.find(s->playerClass); it != want.end() && it->second > 0) {
            --it->second;
        } else {
            leaveSeat(*s, nowMs);
        }
    }
    wanted_ = classes;
    balancedAt_ = 0; // (the next change at once)
}

void BotSquad::setTiers(Tier minTier, Tier maxTier, std::uint32_t nowMs)
{
    options_.minTier = std::min(minTier, maxTier);
    options_.maxTier = std::max(minTier, maxTier);
    const Seat* voice = voiceSeat();
    for (auto& s : seats_) {
        if (s.get() != voice && !s->leaving && !s->done(nowMs) && s->persona &&
            (s->persona->tier < options_.minTier || s->persona->tier > options_.maxTier)) {
            leaveSeat(*s, nowMs); // (balance() sends one of the new range in its place)
        }
    }
    balancedAt_ = 0;
}

BotSquad::Seat* BotSquad::voiceSeat() const
{
    for (const auto& s : seats_) {
        if (!s->leaving && !s->done(lastTick_)) {
            return s.get();
        }
    }
    return nullptr;
}

std::string BotSquad::voice() const
{
    const Seat* v = voiceSeat();
    return v ? v->name : std::string{};
}

void BotSquad::announce(const std::string& why, std::uint32_t nowMs)
{
    log("leaving: " + why);
    // The voice alone says so; the rest just go.
    leaveAt_ = say("The bots are leaving: " + why + ".", nowMs) ? nowMs + 1500 // (time for the line to get out)
                                                                 : nowMs;      // (not in to say it)
}

void BotSquad::leaveAll(const std::string& reason, std::uint32_t nowMs)
{
    wanted_.clear();
    if (leaveAt_ == 0 && !quitting_) {
        announce(reason, nowMs);
    }
}

bool BotSquad::say(const std::string& text, std::uint32_t nowMs)
{
    const Seat* v = voiceSeat();
    if (!v) {
        log("not said (no bot in): " + text);
        return false;
    }
    return sayAs(v->name, text, nowMs);
}

BotSquad::Seat* BotSquad::seatNamed(std::string_view name) const
{
    const auto it = std::ranges::find_if(seats_, [&](const auto& s) { return s->name == name && !s->leaving; });
    return it != seats_.end() ? it->get() : nullptr;
}

bool BotSquad::sayAs(const std::string& bot, const std::string& text, std::uint32_t nowMs)
{
    Seat* const s = seatNamed(bot);
    if (!s || !s->started || s->bot->client().stage() != Stage::InGame) {
        log("not said (" + bot + " isn't in the game): " + text);
        return false;
    }
    s->bot->client().say(text);
    said_.push_back({bot, text, nowMs});
    chat_.push_back({nowMs, bot, text, false});
    return true;
}

bool BotSquad::finished() const
{
    return std::ranges::all_of(seats_, [&](const auto& s) { return s->done(lastTick_); });
}

void BotSquad::tick(std::uint32_t nowMs)
{
    lastTick_ = nowMs;
    // leaveAll's line has had its moment: everyone leaves now.
    if (leaveAt_ != 0 && static_cast<std::int32_t>(nowMs - leaveAt_) >= 0 && !quitting_) {  // signed difference: right across the ms clock's wrap
        quitting_ = true;
        for (auto& s : seats_) {
            leaveSeat(*s, nowMs);
        }
    }
    // One datagram's worth for receiving.
    std::uint8_t buffer[2048];
    // One bot listens to the chat (they all hear the same): the voice, else the first in the game.
    const Seat* listener = nullptr;
    for (const auto& s : seats_) {
        if (s->started && s->bot->client().stage() == Stage::InGame) {
            listener = s.get();
            if (const int limit = s->bot->client().world().game().playerLimit; limit > 0) {
                gameLimit_ = limit;
            }
            break;
        }
    }
    const Seat* const speaker = voiceSeat();
    if (speaker && speaker->started && speaker->bot->client().stage() == Stage::InGame) {
        listener = speaker;
    }
    listener_ = listener;
    // Our own lines are remembered for 5 s, to drop the server's echo of them.
    std::erase_if(said_, [&](const Said& s) { return nowMs - s.atMs > 5000; });
    // A bot that never got in (the server full, refusing, not answering): the next try a while
    // later. (Noted the tick it fails, before balance() puts a fresh bot in its place.)
    const auto noteFailed = [&](Seat& s) {
        if (s.failNoted || s.leaving || s.inGameAt != 0 || s.bot->client().stage() != Stage::Failed) {
            return;
        }
        s.failNoted = true;
        const std::uint32_t wait = joinRetryAfter(++joinFails_);
        joinRetryAt_ = nowMs + wait;
        log("[" + s.name + "] couldn't join: trying again in " +
            (wait >= 60'000 ? std::to_string(wait / 60'000) + " min" : std::to_string(wait / 1000) + " s"));
    };
    for (auto& sp : seats_) {
        Seat& s = *sp;
        auto& c = s.bot->client();
        // A seat not started yet starts at its staggered time (startAt).
        if (!s.started) {
            if (s.leaving || static_cast<std::int32_t>(nowMs - s.startAt) < 0 || !mayJoin(nowMs)) {
                continue; // (not yet; or a NoX Core-only game: never, SquadOptions::skipNoxCoreOnly)
            }
            // Our content packs' added object types, if any: a host with added types lets in only a
            // game whose query named the same ones (from the socket it connects from, as noxsdl's).
            // Only then does a bot identify itself (naming our client level in it too): our level
            // alone never does, so bots keep a stock client's join, updates and class wherever no
            // added types ask for more (and a host with a lowest level takes them as stock ones).
            // A host known not to answer (a stock one: the options', the ping's, or a seat's wait
            // unanswered) is joined at once, never slower than before (a ping's round trip first).
            const std::uint64_t types = content::newThingTypes().hash;
            const bool asks = types != 0;
            if (asks && !options_.knownStock && !hostSilent_ && !hostAnswers_ && !pingHost(nowMs)) {
                continue; // (the ping's answer awaited)
            }
            const bool mayAnswer = !options_.knownStock && !hostSilent_;
            if (asks && !mayAnswer) {
                // (the query beside the join all the same, twice: a stock host drops it)
                const auto query = lobby::encodeExtendedQuery(nowMs, 0, types, lobby::kClientLevel);
                s.game.sendTo(options_.server, query);
                s.game.sendTo(options_.server, query);
            }
            if (asks && mayAnswer && !s.typesAnswered) {
                constexpr std::uint32_t kTypesWaitMs = 3000;
                constexpr std::uint32_t kTypesResendMs = 250;
                if (!s.typesAsked) {
                    log("[" + s.name + "] joining");
                    s.typesAsked = true;
                    s.typesFirstAt = nowMs;
                    s.typesAskedAt = nowMs - kTypesResendMs;
                }
                if (nowMs - s.typesFirstAt < kTypesWaitMs) {
                    if (nowMs - s.typesAskedAt >= kTypesResendMs) {
                        s.game.sendTo(options_.server, lobby::encodeExtendedQuery(nowMs, 0, types, lobby::kClientLevel));
                        s.typesAskedAt = nowMs;
                    }
                    try {
                        while (const auto got = s.game.receive(buffer)) {
                            s.typesAnswered = s.typesAnswered || (got->from == options_.server && lobby::decodeExtendedReply(std::span(buffer, got->size)));
                        }
                    } catch (const std::exception& ex) {
                        log("[" + s.name + "] network error: " + ex.what());
                    }
                    if (!s.typesAnswered) {
                        continue; // (the join waits for it)
                    }
                } else {
                    hostSilent_ = true; // (unanswered: a stock host, the next bots join at once)
                }
            } else {
                log("[" + s.name + "] joining");
            }
            c.start(nowMs);
            s.started = true;
        }
        if (s.done(nowMs)) {
            noteFailed(s);
            continue; // left, failed, or the server never took the leave (25 s)
        }
        // Read what came from the server on each socket, let the bot think, send what it queued.
        try {
            while (const auto got = s.lobby.receive(buffer)) {
                if (got->from == options_.server) {
                    c.receiveLobby(std::span(buffer, got->size));
                }
            }
            while (const auto got = s.game.receive(buffer)) {
                if (got->from == options_.server && !(s.typesAsked && lobby::decodeExtendedReply(std::span(buffer, got->size)))) { // (the query's answer: not the game's)
                    c.receive(std::span(buffer, got->size), nowMs);
                }
            }
            s.bot->tick(nowMs);
            for (const auto& d : c.takeOutgoing()) {
                (d.lobby ? s.lobby : s.game).sendTo(options_.server, d.bytes);
            }
        } catch (const std::exception& ex) {
            log("[" + s.name + "] network error: " + ex.what());
        }
        // What happened to this bot: in game, chat, kills, disconnects, notices.
        // (Chat and kills from the listening bot only: they all hear the same.)
        for (const auto& e : c.takeEvents()) {
            using K = client::ClientEvent::Kind;
            switch (e.kind) {
            case K::InGame:
                log("[" + s.name + "] in game on " + e.text);
                s.inGameAt = nowMs;
                joinFails_ = 0;
                everIn_ = true;
                s.bot->setStandDown(standingDown_); // (a late joiner does as the rest)
                break;
            case K::Chat: {
                if (&s != listener) {
                    break;
                }
                // Our own line back from the server: it's in already (say()).
                const auto echo = std::ranges::find_if(said_, [&](const Said& d) { return d.bot == e.name && d.text == e.text; });
                if (echo != said_.end()) {
                    said_.erase(echo);
                    break;
                }
                const auto* who = c.world().player(e.netCode);
                chat_.push_back({nowMs, e.name, e.text, e.team, who ? who->account : ""});
                // Kept to the last 500-1000 lines.
                if (chat_.size() > 1000) {
                    chat_.erase(chat_.begin(), chat_.begin() + 500);
                }
                break;
            }
            case K::Kill:
                if (&s != listener) {
                    break;
                }
                banterOn(e.attacker, e.netCode, nowMs);
                noteKill(e.attacker, e.netCode, nowMs);
                if (e.attacker != 0 && e.attacker == e.netCode) {
                    // Killed themselves: with what (a thing, or a kind of damage).
                    // killKind 1: `weapon` is a thing type (index + 1); otherwise a kind of damage.
                    const bool thing = e.killKind == 1 && e.weapon > 0 && e.weapon <= things_.things.size();
                    log("  (by " + (thing ? things_.things[e.weapon - 1u].name : "damage type " + std::to_string(e.weapon)) +
                        ", kind " + std::to_string(e.killKind) + ")");
                }
                break;
            case K::Disconnected:
                log("[" + s.name + "] disconnected: " + e.text);
                if (s.inGameAt == 0) {
                    lastJoinFailure_ = e.text; // (never got in: why)
                }
                break;
            case K::Notice:
                // Every bot hears the game's notices (and the server repeats some each frame):
                // each once, not again within kNoticeMs.
                std::erase_if(notices_, [&](const auto& n) { return nowMs - n.second > kNoticeMs; });
                if (notices_.try_emplace(e.text, nowMs).second) {
                    log(e.text);
                }
                break;
            default:
                break;
            }
        }
        // Count deaths: an edge to dead (or the DIE / DEAD animation, 1 or 2).
        if (c.stage() == Stage::InGame) {
            const auto* me = c.world().player(c.netCode());
            const bool dead = me && me->dead;
            const auto* d = c.world().drawable(c.netCode());
            const bool fallen = dead || (d && (d->animState == 1 || d->animState == 2));
            if (fallen && !s.wasDead) {
                ++s.deaths;
            }
            s.wasDead = fallen;
        }
        noteFailed(s);
    }
    assignRoles();
    // Who's having a hard time against us: the easy bots go easy on them.
    const auto struggling = strugglers(nowMs);
    for (auto& s : seats_) {
        s->bot->setStruggling(struggling);
    }
    nagHost(nowMs);
    // People playing (names without "[BOT] ", not spectating, not away), as the first bot that's been in
    // a while sees them: none, and the bots watch (held 5 s, so joining's flurry doesn't flip
    // it). Room for people is kept by balance().
    people_.reset();
    humans_.clear();
    for (const auto& s : seats_) {
        // Who they are, for the UI: as the first bot in the game sees them.
        if (s->bot->client().stage() != Stage::InGame) {
            continue;
        }
        const auto& w = s->bot->client().world();
        for (const auto& [netCode, p] : w.players()) {
            if (botNamed(p.name)) {
                continue;
            }
            SquadPerson h;
            h.name = p.name;
            h.playerClass = p.playerClass;
            if (const auto t = w.teams().find(p.team); p.team != 0 && t != w.teams().end()) {
                h.team = t->second.name;
            }
            h.observer = (p.status & 1) != 0;  // status bit 0: observing
            h.kills = p.lessons; // (the score the rank board lists: lessons taught)
            h.pingMs = p.latency;
            h.account = p.account;
            humans_.push_back(std::move(h));
        }
        break;
    }
    // Who's standing still, as every bot in the game sees them (each sees only what's near it):
    // a person in some bot's view at the same place since `since`; out of every bot's view, not
    // counted as idle (a person away from the bots may well be playing). Looked at once a second.
    if (nowMs - stillAt_ >= 1000) {
        stillAt_ = nowMs;
        for (const auto& s : seats_) {
            if (s->bot->client().stage() != Stage::InGame) {
                continue;
            }
            const auto& w = s->bot->client().world();
            for (const auto& [netCode, p] : w.players()) {
                const auto* d = botNamed(p.name) ? nullptr : w.drawable(netCode);
                if (!d) {
                    continue;
                }
                auto [it, fresh] = stillness_.try_emplace(p.name);
                Stillness& st = it->second;
                if (fresh || std::abs(d->x - st.x) > 2 || std::abs(d->y - st.y) > 2) {
                    st.x = d->x;
                    st.y = d->y;
                    st.since = nowMs;
                }
                st.seenAt = nowMs;
            }
        }
        std::erase_if(stillness_, [&](const auto& e) {
            return std::ranges::none_of(humans_, [&](const SquadPerson& h) { return h.name == e.first; });
        });
    }
    for (SquadPerson& h : humans_) {
        if (const auto it = stillness_.find(h.name); it != stillness_.end() && nowMs - it->second.seenAt < 2500) {
            h.idleMs = nowMs - it->second.since;
            h.inView = true;
        }
    }
    for (const auto& s : seats_) {
        if (s->inGameAt != 0 && nowMs - s->inGameAt > 5000 && s->bot->client().stage() == Stage::InGame) {
            people_ = static_cast<std::size_t>(std::ranges::count_if(s->bot->client().world().players(), [&](const auto& p) {
                return !botNamed(p.second.name) && !(p.second.status & 1) && p.second.name != away_;
            }));
            break;
        }
    }
    if (people_ && !quitting_ && leaveAt_ == 0) {
        const bool alone = options_.observeAlone && *people_ == 0;
        if (alone != standingDown_) {
            if (standDownChangeAt_ == 0) {
                standDownChangeAt_ = nowMs;
            } else if (nowMs - standDownChangeAt_ > 5000) {
                standingDown_ = alone;
                standDownChangeAt_ = 0;
                log(alone ? "nobody else playing: the bots watch" : "someone's playing: the bots play");
                for (auto& s : seats_) {
                    s->bot->setStandDown(alone);
                }
            }
        } else {
            standDownChangeAt_ = 0;
        }
    }
    balance(nowMs);
    sayPending(nowMs);
}

// A kill between a bot of ours and a person: maybe a taunt or a whine, queued for later.
void BotSquad::banterOn(std::uint16_t attacker, std::uint16_t victim, std::uint32_t nowMs)
{
    if (!listener_ || attacker == 0 || attacker == victim || options_.banterChance <= 0 || quitting_ || leaveAt_ != 0) {
        return;
    }
    const auto& players = listener_->bot->client().world().players();
    const auto a = players.find(attacker);
    const auto v = players.find(victim);
    if (a == players.end() || v == players.end()) {
        return;
    }
    // Between one of ours and a person (bots among themselves say nothing).
    Seat* bot = nullptr;
    Banter::Kind kind{};
    std::string at;
    if (Seat* s = seatNamed(a->second.name); s && person(v->second)) {
        bot = s;
        kind = Banter::Kind::Taunt;
        at = v->second.name;
    } else if (Seat* s2 = seatNamed(v->second.name); s2 && person(a->second)) {
        bot = s2;
        kind = Banter::Kind::Whine;
        at = a->second.name;
    }
    if (!bot || (bot->banteredAt != 0 && nowMs - bot->banteredAt < kBanterBotGapMs) ||
        (bantered_ != 0 && nowMs - bantered_ < kBanterGapMs) ||
        nox::util::uniform_real_distribution<float>(0, 1)(rng_) >= options_.banterChance) {
        return;
    }
    bot->banteredAt = nowMs;
    bantered_ = nowMs;
    pendingLines_.push_back({bot->name, banter_.line(kind, at, rng_), nowMs + kBanterDelayMs});
}

// Keeps each person's deaths to and kills of our bots, for struggles().
void BotSquad::noteKill(std::uint16_t attacker, std::uint16_t victim, std::uint32_t nowMs)
{
    if (!listener_ || attacker == 0 || attacker == victim) {
        return;
    }
    const auto& players = listener_->bot->client().world().players();
    const auto a = players.find(attacker);
    const auto v = players.find(victim);
    if (a == players.end() || v == players.end()) {
        return;
    }
    // (Ours: any seat of that name, one leaving too.)
    const auto ours = [&](const std::string& name) {
        return std::ranges::any_of(seats_, [&](const auto& s) { return s->name == name; });
    };
    if (ours(a->second.name) && person(v->second)) {
        record_[victim].deaths.push_back(nowMs);
    } else if (person(a->second) && ours(v->second.name)) {
        record_[attacker].kills.push_back(nowMs);
    }
}

// Drops records older than kStruggleWindowMs (and of people gone), returning who struggles now.
std::vector<std::uint16_t> BotSquad::strugglers(std::uint32_t nowMs)
{
    std::vector<std::uint16_t> out;
    const auto* players = listener_ ? &listener_->bot->client().world().players() : nullptr;
    for (auto it = record_.begin(); it != record_.end();) {
        auto& r = it->second;
        std::erase_if(r.deaths, [&](std::uint32_t at) { return nowMs - at > kStruggleWindowMs; });
        std::erase_if(r.kills, [&](std::uint32_t at) { return nowMs - at > kStruggleWindowMs; });
        if ((r.deaths.empty() && r.kills.empty()) || (players && !players->contains(it->first))) {
            it = record_.erase(it); // (nothing lately, or gone)
            continue;
        }
        if (struggles(r.deaths.size(), r.kills.size())) {
            out.push_back(it->first);
        }
        ++it;
    }
    return out;
}

bool BotSquad::struggles(std::size_t deaths, std::size_t kills)
{
    return deaths >= 3 && deaths >= 2 * (kills + 1);
}

bool BotSquad::isCaller(std::string_view account, std::string_view caller)
{
    while (!account.empty() && account.back() == ' ') {
        account.remove_suffix(1);
    }
    return !caller.empty() && util::lowered(account) == util::lowered(caller); // (account names without case)
}

void BotSquad::nagHost(std::uint32_t nowMs)
{
    // On a chat map (no game yet, the host to pick one), with people in to hear it: each of ours
    // in play nudges the host, every kNagMinMs to kNagMaxMs on a clock of its own.
    const bool chatMap = listener_ && (listener_->bot->client().world().game().gameFlags & PlayerBrain::kChatMode);
    const bool nagging = options_.nagHost && chatMap && !quitting_ && leaveAt_ == 0 && people_ && *people_ > 0;
    nox::util::uniform_int_distribution<std::uint32_t> later(kNagMinMs, kNagMaxMs);
    for (auto& s : seats_) {
        const auto& c = s->bot->client();
        const auto* me = c.world().player(c.netCode());
        if (!nagging || !s->started || s->leaving || c.stage() != Stage::InGame || !me || (me->status & 1)) {
            s->nagAt = 0; // (not now: watching, or no chat map, or nobody to hear)
            continue;
        }
        if (s->nagAt == 0) {
            s->nagAt = nowMs + later(rng_);
        } else if (static_cast<std::int32_t>(nowMs - s->nagAt) >= 0) {
            sayAs(s->name, banter_.line(Banter::Kind::Waiting, "", rng_), nowMs);
            s->nagAt = nowMs + later(rng_);
        }
    }
}

// Says the taunts and whines whose time has come (none once leaving).
void BotSquad::sayPending(std::uint32_t nowMs)
{
    for (auto it = pendingLines_.begin(); it != pendingLines_.end();) {
        if (static_cast<std::int32_t>(nowMs - it->atMs) >= 0) {
            if (!quitting_ && leaveAt_ == 0) {
                sayAs(it->bot, it->text, nowMs);
            }
            it = pendingLines_.erase(it);
        } else {
            ++it;
        }
    }
}

void BotSquad::assignRoles()
{
    // Our bots in the game by team, in the order they joined; and which teams' flags an enemy
    // is carrying (a player of another team with the flag: WeaponEquip bit 1).
    std::map<std::uint32_t, std::vector<Seat*>> teams;
    std::set<std::uint32_t> robbed;
    std::map<std::uint32_t, std::uint16_t> carriers; ///< by team: one of it with the enemy's flag (a bot or a person)
    std::uint32_t gameFlags = 0;
    for (auto& sp : seats_) {
        Seat& s = *sp;
        if (!s.started || s.leaving || s.bot->client().stage() != Stage::InGame) {
            continue;
        }
        const auto& world = s.bot->client().world();
        gameFlags = world.game().gameFlags;
        const auto* me = world.player(s.bot->client().netCode());
        const std::uint32_t team = me ? me->team : 0;
        if (team == 0) {
            s.bot->setRole(BotView::Role::Free);
        } else {
            teams[team].push_back(&s);
            for (const auto& [netCode, p] : world.players()) {
                if (!(p.weaponEquip & 1)) { // weaponEquip bit 0: carrying a flag.
                    continue;
                }
                if (p.team == team) {
                    carriers[team] = netCode;
                } else if (p.team != 0) {
                    robbed.insert(team);
                }
            }
        }
    }
    constexpr std::uint32_t kCtf = 0x20;  // GAME_SETTINGS' capture the flag flag
    for (auto& [team, members] : teams) {
        if (gameFlags & kCtf) {
            // Capture the flag: one keeps our flag (with two or more of us), the rest go for theirs.
            // An enemy has ours: about half of the rest (at least one) hunt them down.
            const std::size_t guards = members.size() >= 2 ? 1 : 0;
            const std::size_t hunters = robbed.contains(team) ? std::max<std::size_t>(1, (members.size() - guards + 1) / 2) : 0;
            std::vector<std::pair<float, Seat*>> attackers; // (by how far from our carrier)
            const auto carrier = carriers.find(team);
            for (std::size_t i = 0; i < members.size(); ++i) {
                const auto role = i < guards           ? BotView::Role::Guard
                                  : i < guards + hunters ? BotView::Role::Hunt
                                                         : BotView::Role::Attack;
                members[i]->bot->setRole(role);
                members[i]->bot->setEscorts({});
                const auto& c = members[i]->bot->client();
                if (role == BotView::Role::Attack && carrier != carriers.end() && c.netCode() != carrier->second) {
                    const auto* them = c.world().drawable(carrier->second);
                    const auto* me = c.world().drawable(c.netCode());
                    attackers.emplace_back(them && me ? std::hypot(float(them->x - me->x), float(them->y - me->y)) : 1e9f,
                                           members[i]);
                }
            }
            // One of ours has their flag: the nearest attacker (two, with three or more of them)
            // runs with the carrier, bot or person, to see it home.
            std::ranges::stable_sort(attackers, {}, &std::pair<float, Seat*>::first);
            const std::size_t escorts = std::min<std::size_t>(attackers.size(), attackers.size() >= 3 ? 2 : 1);
            std::vector<std::uint16_t> escortCodes;
            for (std::size_t i = 0; i < escorts; ++i) {
                attackers[i].second->bot->setRole(BotView::Role::Escort, carrier->second);
                escortCodes.push_back(attackers[i].second->bot->client().netCode());
            }
            // (A carrier of ours knows who's with it: it gives them a moment to catch up.)
            if (carrier != carriers.end()) {
                for (Seat* m : members) {
                    if (m->bot->client().netCode() == carrier->second) {
                        m->bot->setEscorts(escortCodes);
                    }
                }
            }
        } else {
            // Other team games: small groups of up to three, each behind its first.
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (i % 3 == 0) {
                    members[i]->bot->setRole(BotView::Role::Free);
                } else {
                    members[i]->bot->setRole(BotView::Role::Follow, members[i - i % 3]->bot->client().netCode());
                }
            }
        }
    }
}

// A row for each seat for a UI: state, health, target, team play and a caster's spells.
std::vector<SquadBotStatus> BotSquad::status() const
{
    std::vector<SquadBotStatus> out;
    for (const auto& sp : seats_) {
        const Seat& s = *sp;
        const auto& c = s.bot->client();
        SquadBotStatus st;
        st.name = s.name;
        st.serial = s.serial;
        st.playerClass = s.playerClass;
        st.deaths = s.deaths;
        const auto* me = c.world().player(c.netCode());
        const bool listed = me != nullptr;
        // State: a seat not yet started is waiting or has left; else by the client's join stage.
        if (!s.started) {
            st.state = s.leaving ? "left" : "waiting to join";
        } else {
            switch (c.stage()) {
            case Stage::Idle:
            case Stage::Joining: st.state = "joining"; break;
            case Stage::WaitingMap: st.state = "waiting for the map"; break;
            case Stage::Downloading: st.state = "downloading the map"; break;
            case Stage::InGame:
                st.state = s.wasDead ? "dead" : "playing";
                if (listed) {
                    if (me->status & 1) {  // status bit 0: observing
                        st.state = "watching";
                    }
                    st.kills = me->lessons;  // (the score the rank board lists: lessons taught)
                }
                break;
            case Stage::Quitting: st.state = "leaving"; break;
            case Stage::Closed: st.state = "left"; break;
            case Stage::Failed: st.state = "failed"; break;
            }
        }
        st.health = c.world().self().health;
        st.maxHealth = c.world().self().maxHealth;
        const auto* brain = dynamic_cast<const PlayerBrain*>(&std::as_const(*s.bot).brain());
        if (brain && brain->target()) {
            // A player by name, a creature by its kind.
            const auto* t = c.world().player(brain->target());
            const auto* d = c.world().drawable(brain->target());
            st.target = t                                                        ? t->name
                        : d && d->typeId > 0 && d->typeId <= things_.things.size() ? things_.things[d->typeId - 1u].name
                                                                                   : "?";
        }
        // Position: the bot's own drawable when it has one, else the client's own position().
        if (const auto* d = c.world().drawable(c.netCode())) {
            st.x = static_cast<float>(d->x);
            st.y = static_cast<float>(d->y);
        } else if (const auto p = c.position()) {
            st.x = static_cast<float>(p->first);
            st.y = static_cast<float>(p->second);
        }
        if (listed) {
            st.team = me->team;
            st.carryingFlag = (me->weaponEquip & 1) != 0;  // weaponEquip bit 0: carrying a flag
        }
        st.role = roleName(s.bot->role());
        st.doing = brain ? brain->doing() : "";
        st.flags = s.bot->flagsSeen();
        // Casters also report their current spell page, last spell and per-spell cast counts.
        if (const auto* caster = dynamic_cast<const CasterBrain*>(brain)) {
            static constexpr std::array<const char*, 3> kPages{"attack", "control", "defence"};
            st.page = kPages[static_cast<std::size_t>(caster->page())];
            const auto nameOf = [&](std::uint32_t id) {
                const auto* def = things_.findSpell(id);
                std::string name = def ? def->id : "#" + std::to_string(id);
                return name.starts_with("SPELL_") ? name.substr(6) : name;
            };
            if (caster->lastSpell() != 0) {
                st.lastSpell = nameOf(caster->lastSpell());
            }
            for (const auto& [id, n] : caster->castCounts()) {
                st.casts += (st.casts.empty() ? "" : ", ") + nameOf(id) + " " + std::to_string(n);
            }
        }
        out.push_back(std::move(st));
    }
    return out;
}

} // namespace nox::bot
