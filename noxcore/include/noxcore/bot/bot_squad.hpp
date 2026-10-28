// A squad of network bots on one server: each a NetBot with its own two sockets (the lobby's and
// the game's, as the vanilla client), joined a little apart, ticked together. It keeps the count
// of bots asked for (add / remove) as far as the server has room while always leaving
// kSeatsKept places for people: as people come in the newest bots leave, as they go bots rejoin
// (the classes missing), one change at a time. With nobody else playing they watch. Now and then
// a bot taunts a person it killed or whines at one who killed it. It keeps a log and a status
// for a UI. The program running the bots drives one.
#pragma once

#include "noxcore/bot/banter.hpp"
#include "noxcore/bot/bot.hpp"
#include "noxcore/bot/net_bot.hpp"
#include "noxcore/bot/personas.hpp"
#include "noxcore/lobby/browser.hpp"
#include "noxcore/net/udp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nox::bot {

struct SquadOptions {
    net::Endpoint server;
    std::string password;
    /// The difficulties bots join at: each is a persona of its class from this range, at random,
    /// playing as well as its tier.
    Tier minTier = Tier::Medium;
    Tier maxTier = Tier::Medium;
    /// The part of kills between a bot and a person that get a taunt or a whine (0 none).
    float banterChance = 0.3f;
    std::uint32_t staggerMs = 1500;   ///< between one bot's join and the next
    // (No "all leave past this many people" limit any more: the squad makes room a bot at a time
    // instead, kSeatsKept, and rejoins when there's room again.)
    bool observeAlone = true;         ///< nobody else playing: they watch
    /// On a chat map they nudge the host to start (kNagMinMs): off for a host's own bots (noxsdl).
    bool nagHost = true;
    std::filesystem::path mapsDir;    ///< the game's maps
    std::filesystem::path mapCache;   ///< where maps the server sends are kept
    std::uint32_t seed = 0;           ///< names, serials, brains (0: random)
    /// The server's player limit, if known beforehand (the game list's): the squad never has
    /// more than it less kSeatsKept. Unknown, it takes the limit from the game settings once a
    /// bot is in, and lets only one join until then.
    std::optional<int> playerLimit;
    /// A game list saw the server answer its ping but not the extended query (a GAME.EXE host):
    /// the bots join at once, never waiting for that query's answer. Unknown (false), the squad
    /// pings it first (a round trip): a stock host is joined at once, one that answers waited for.
    bool knownStock = false;
    static constexpr int kSeatsKept = 2; ///< places always left free for people
    /// (The bot manager's) Before the first join, the server asked whether it's a noxcpp one
    /// that turns stock clients away (the extended reply's ServerFeature::NoxCoreOnly), from a
    /// socket of its own: if so, no bot joins (they'd be held out and put out as stock players).
    /// A host that doesn't answer (a stock one) within kCoreProbeMs is joined as before.
    bool skipNoxCoreOnly = false;
    static constexpr std::uint32_t kCoreProbeMs = 1500;
};

/// A line of chat heard in the game.
struct ChatLine {
    std::uint32_t atMs = 0; ///< when it came (the squad's clock)
    std::string name;       ///< the speaker ("" for the server)
    std::string text;
    bool team = false;
    std::string account;    ///< the speaker's account (the WOL login), "" if unknown
};

/// One bot as a UI shows it.
struct SquadBotStatus {
    std::string name;
    /// The CD key it joins with: random, its own, so a server it's hosted beside can tell it from
    /// a person for certain (a name anyone can take).
    std::string serial;
    std::uint8_t playerClass = 0;
    std::string state;   ///< "joining", "in game", "watching", "dead", "left", "failed"...
    std::uint16_t health = 0;
    std::uint16_t maxHealth = 0;
    std::string target;  ///< who it's after, by name
    int kills = 0;
    int deaths = 0;
    // For logs (a trace of each bot):
    float x = 0;
    float y = 0;
    std::uint32_t team = 0;
    std::string role;    ///< its part in a team game ("guard", "attack"...), "" for none
    std::string doing;   ///< what its brain is at (PlayerBrain::doing)
    bool carryingFlag = false;
    std::vector<BotView::FlagBase> flags; ///< the flags it sees
    // A caster's:
    std::string page;      ///< its bar page ("attack", "control", "defence"), "" for a warrior
    std::string lastSpell; ///< the last spell cast, its thing.bin name without "SPELL_"
    std::string casts;     ///< every spell cast so far and how often ("FIREBALL 3, SLOW 1")
};

/// A person in the game (not one of the bots), as a UI shows them.
struct SquadPerson {
    std::string name;
    std::uint8_t playerClass = 0;
    std::string team;      ///< its name, "" with none
    bool observer = false; ///< spectating
    int kills = 0;
    std::uint16_t pingMs = 0;
    std::string account;   ///< their XWIS login, "" if the game gives none (OpenNox's)
    /// How long a bot has seen them standing in one place (0 while they move, or while no bot
    /// has had them in view lately: out of sight isn't idle).
    std::uint32_t idleMs = 0;
    bool inView = false; ///< a bot of ours has them in view now
};

class BotSquad {
public:
    /// `things` and `kinds` must outlive the squad.
    BotSquad(SquadOptions options, const things::ThingDb& things, const ItemKinds& kinds);
    ~BotSquad();
    BotSquad(const BotSquad&) = delete;
    BotSquad& operator=(const BotSquad&) = delete;

    /// `count` more bots of `playerClass` (0 warrior, 1 wizard, 2 conjurer) wanted, within
    /// maxBots(), each with its class's brain. They join one after another as the server has
    /// room for them (people always keep kSeatsKept places). Returns how many it took on.
    std::size_t add(std::uint8_t playerClass, std::size_t count, std::uint32_t nowMs);
    /// The most bots this server may have: its player limit less SquadOptions::kSeatsKept
    /// (unknown limit: as many as asked, one joining first to learn it).
    std::size_t maxBots() const;
    /// Bots not gone (joining, playing, leaving).
    std::size_t active() const;
    /// Joins failed in a row (since a bot last got in), why the last one did (the client's
    /// words: "no answer from the server's lobby" when the game can't be reached, "join refused
    /// (error N)" when it's full or closed...), and whether any bot of ours has been in at all.
    int joinFails() const noexcept { return joinFails_; }
    const std::string& lastJoinFailure() const noexcept { return lastJoinFailure_; }
    bool everIn() const noexcept { return everIn_; }
    /// How many bots are wanted (what add and remove asked for): the count kept as room allows.
    std::size_t wanted() const noexcept { return wanted_.size(); }
    /// The newest `count` wanted no more: they leave (cleanly, without announcing).
    void remove(std::size_t count, std::uint32_t nowMs);
    /// Exactly these wanted (a class each, as add's): the count and the mix. The bot that speaks
    /// for them (voice()) stays whatever its class; of the rest, those of a class there are now
    /// too many of leave, newest first; those missing join as there's room.
    void setWanted(const std::vector<std::uint8_t>& classes, std::uint32_t nowMs);
    /// The classes wanted now, oldest first.
    const std::vector<std::uint8_t>& wantedClasses() const noexcept { return wanted_; }
    /// The difficulties bots join at from now on; those in of another (but the voice) leave, to
    /// come back as one of these.
    void setTiers(Tier minTier, Tier maxTier, std::uint32_t nowMs);
    Tier minTier() const noexcept { return options_.minTier; }
    Tier maxTier() const noexcept { return options_.maxTier; }
    /// How many of the `wanted` bots may be in: the player `limit` less kSeatsKept less the
    /// `others` (everyone not ours, spectators too). Either unknown: one in to see (or the `mine`
    /// already in or on the way, unchanged until they do).
    static std::size_t allowedBots(std::optional<int> limit, std::optional<int> others, std::size_t mine,
                                   std::size_t wanted);
    /// Between one bot leaving or joining to keep the count and the next.
    static constexpr std::uint32_t kBalanceMs = 3000;
    /// The gone seats kept (the newest) as a new one's added; older ones let go.
    static constexpr std::size_t kGoneKept = 8;
    /// A join that failed (the server full, refusing, not answering): the next try waits this
    /// long, and kJoinBackOffMs once one has failed again before a bot got in (its port closed,
    /// say: no use knocking every half minute).
    static constexpr std::uint32_t kJoinRetryMs = 30'000;
    static constexpr std::uint32_t kJoinBackOffMs = 5 * 60'000;
    /// The wait after `fails` joins in a row failed.
    static constexpr std::uint32_t joinRetryAfter(int fails) { return fails <= 1 ? kJoinRetryMs : kJoinBackOffMs; }
    /// Everyone leaves: the voice says "The bots are leaving: <reason>." (the only one that does)
    /// and they go a moment later.
    void leaveAll(const std::string& reason, std::uint32_t nowMs);
    /// The voice says it (and it's in the chat as theirs). False if the voice isn't in the game.
    bool say(const std::string& text, std::uint32_t nowMs);
    /// The bot named `bot` says it (as say(), for an answer from that bot). False if it isn't in
    /// the game.
    bool sayAs(const std::string& bot, const std::string& text, std::uint32_t nowMs);
    /// The bot that speaks for them all: the first still here (the first to join, until it's
    /// gone). Its name, "" with none.
    std::string voice() const;
    /// Call often (every few ms): sockets read, brains think, clients run, rules applied.
    void tick(std::uint32_t nowMs);

    /// All of them have left (or never got in), after leaveAll or remove.
    bool finished() const;
    bool leaving() const noexcept { return leaveAt_ != 0 || quitting_; }
    std::size_t size() const noexcept { return seats_.size(); }
    std::vector<SquadBotStatus> status() const;
    /// People playing (not bots, not spectating) as the bots see them; nullopt before one has
    /// been in a while.
    std::optional<std::size_t> people() const noexcept { return people_; }
    /// (SquadOptions::skipNoxCoreOnly) The server said it's NoX Core only: no bot joins it.
    bool noxCoreOnly() const noexcept { return coreProbe_ == CoreProbe::CoreOnly; }
    /// Someone (by name; "" none) counted as not playing, though in (a game's owner sitting it out).
    /// With nobody else playing, the bots watch.
    void setAway(std::string name) { away_ = std::move(name); }
    /// The people in the game (spectators too), as the first bot in it sees them; empty while
    /// none is in.
    const std::vector<SquadPerson>& humans() const noexcept { return humans_; }
    /// What happened since the last call: joins, notices, leaving.
    std::vector<std::string> takeLog();
    /// The game's chat since the last call, heard by one bot (each bot hears the same).
    std::vector<ChatLine> takeChat() { return std::exchange(chat_, {}); }

    /// A bot's taunt or whine waits this long after the kill (a moment, as a person types)...
    static constexpr std::uint32_t kBanterDelayMs = 1500;
    /// ...and none comes from the same bot within kBanterBotGapMs, nor from any within
    /// kBanterGapMs: a few lines a minute at most, whatever the kills.
    static constexpr std::uint32_t kBanterBotGapMs = 45'000;
    static constexpr std::uint32_t kBanterGapMs = 10'000;

    /// Someone who in the last kStruggleWindowMs has died to our bots this often and killed them
    /// that often is having a hard time: at least 3 deaths, and twice their kills plus one. The
    /// easy bots go easy on them (Skill::gentle).
    static bool struggles(std::size_t deaths, std::size_t kills);
    static constexpr std::uint32_t kStruggleWindowMs = 180'000;

    /// Whether a player's `account` (as the game sends it: trailing spaces) is `caller`, any
    /// case: the one who gives orders (a game's owner, say). Never an empty `caller`.
    static bool isCaller(std::string_view account, std::string_view caller);
    /// On a chat map, with people in, each of ours nudges the host ("hurry up host") every
    /// kNagMinMs to kNagMaxMs, on a clock of its own.
    static constexpr std::uint32_t kNagMinMs = 3'000;
    static constexpr std::uint32_t kNagMaxMs = 10'000;

private:
    struct Seat;
    Seat* voiceSeat() const;
    /// Our seat of that name not leaving, if any.
    Seat* seatNamed(std::string_view name) const;
    /// Team games: who guards, who attacks, who keeps with whom (NetBot::setRole).
    void assignRoles();
    void leaveSeat(Seat& s, std::uint32_t nowMs);
    void announce(const std::string& why, std::uint32_t nowMs);
    void log(std::string line);
    /// A new seat for a bot of `playerClass`, joining at `atMs` (a name from one gone, if any).
    void addSeat(std::uint8_t playerClass, std::uint32_t atMs);
    /// Keeps the wanted count as far as room allows: one bot out when people need the place,
    /// one in (a missing class) when there's room again, kBalanceMs apart.
    void balance(std::uint32_t nowMs);
    /// People (anyone not ours) in the game: as a bot in it sees them, else from the game
    /// list (probe_), else unknown.
    std::optional<int> others(std::uint32_t nowMs);
    /// (SquadOptions::skipNoxCoreOnly) Whether bots may join: asked once, answered (or not)
    /// within kCoreProbeMs.
    bool mayJoin(std::uint32_t nowMs);

    SquadOptions options_;
    enum class CoreProbe : std::uint8_t { NotAsked, Asking, Clear, CoreOnly };
    CoreProbe coreProbe_ = CoreProbe::NotAsked;
    std::optional<net::UdpSocket> coreProbeSocket_;
    std::uint32_t coreProbeAt_ = 0;     ///< when it was first asked
    std::uint32_t coreProbeAskedAt_ = 0; ///< (resent every 500 ms)
    const things::ThingDb& things_;
    const ItemKinds& kinds_;
    std::mt19937 rng_;
    std::vector<std::unique_ptr<Seat>> seats_;
    std::set<std::string> names_;
    std::vector<std::string> log_;
    std::vector<ChatLine> chat_;
    /// What our bots said lately: the server's echo of it isn't shown twice.
    struct Said {
        std::string bot;
        std::string text;
        std::uint32_t atMs = 0;
    };
    std::vector<Said> said_;
    std::optional<int> gameLimit_; ///< the player limit from the game's settings, once a bot is in
    /// The game's notices logged lately, and when: every bot hears them, logged once.
    std::map<std::string, std::uint32_t> notices_;
    static constexpr std::uint32_t kNoticeMs = 3000;
    std::optional<std::size_t> people_;
    std::vector<SquadPerson> humans_;
    /// Where each person (by name) was last seen by any bot, since when they've stood there, and
    /// when they were last seen at all (SquadPerson::idleMs).
    struct Stillness {
        int x = 0;
        int y = 0;
        std::uint32_t since = 0;
        std::uint32_t seenAt = 0;
    };
    std::map<std::string, Stillness> stillness_;
    std::uint32_t stillAt_ = 0; ///< stillness_ last looked at (once a second is plenty)
    std::string away_;          ///< setAway's
    bool standingDown_ = false;
    std::uint32_t standDownChangeAt_ = 0;
    std::vector<std::uint8_t> wanted_; ///< the classes asked for, oldest first
    std::uint32_t balancedAt_ = 0;     ///< the last join or leave to keep the count
    std::uint32_t joinRetryAt_ = 0;    ///< no join before this (after a failed one)
    int joinFails_ = 0;                ///< joins failed in a row (since a bot last got in)
    std::string lastJoinFailure_;      ///< why the last join that never got in failed
    bool everIn_ = false;              ///< a bot of ours has been in the game
    /// The game list's entry for our server, asked now and then while no bot of ours is in
    /// (to see whether people have made room again).
    std::unique_ptr<lobby::ServerBrowser> probe_;
    std::uint32_t probedAt_ = 0;
    /// A bot's wait for the extended query's answer ran out, or the ping found a stock host: the
    /// bots join at once.
    bool hostSilent_ = false;
    /// The ping found a host that answers the extended query: the bots wait for its answer.
    bool hostAnswers_ = false;
    /// Before the first bot's join with added types (never for our client level alone), a ping of an
    /// unknown host (not knownStock): true once it's known (hostAnswers_ or hostSilent_), a
    /// round trip later.
    bool pingHost(std::uint32_t nowMs);
    std::unique_ptr<lobby::ServerBrowser> hostProbe_;
    std::uint32_t hostPingAt_ = 0;
    std::uint32_t hostPingAnsweredAt_ = 0;
    // ---- banter
    Banter banter_;
    struct PendingLine {
        std::string bot;
        std::string text;
        std::uint32_t atMs = 0;
    };
    std::vector<PendingLine> pendingLines_; ///< taunts and whines, said a moment after the kill
    std::uint32_t bantered_ = 0;            ///< the last one's time (0 none yet)
    /// A kill between one of ours and a person, now and then: a taunt from ours over them, or
    /// a whine at them.
    void banterOn(std::uint16_t attacker, std::uint16_t victim, std::uint32_t nowMs);
    /// The lines due.
    void sayPending(std::uint32_t nowMs);
    // ---- how people are doing against us
    struct Record {
        std::vector<std::uint32_t> deaths; ///< when they died to one of ours
        std::vector<std::uint32_t> kills;  ///< when they killed one of ours
    };
    std::map<std::uint16_t, Record> record_; ///< by person's netCode
    void noteKill(std::uint16_t attacker, std::uint16_t victim, std::uint32_t nowMs);
    /// Those who struggle (struggles()) now, by netCode; records old or of people gone dropped.
    std::vector<std::uint16_t> strugglers(std::uint32_t nowMs);
    void nagHost(std::uint32_t nowMs);
    std::uint32_t leaveAt_ = 0; ///< when they all go, once the reason's been said
    bool quitting_ = false;
    std::uint32_t lastTick_ = 0;
    const Seat* listener_ = nullptr; ///< this tick's: the bot whose view of the game we use
};

} // namespace nox::bot
