// Watchers: a mode's frame loops as generic engine plumbing (the
// state machine's move into script). A mode registers what it watches (zones about a thing or
// a point, a player's first sight); each director tick it has a group evaluated where its own loop
// stood, and the group calls its handlers only when something happens: a player comes within a
// zone or leaves it, a zone fires for whoever's in it, a player is first seen. The handlers are
// the mode's (C++ now, its script's rules as they move). Nothing here knows any mode. Besides the
// groups: lists of things watched for their end (a barrel broken, a monster dead), and finders
// (a thing of a type near a point, sought till found).
//
// A group's players are the host's (in its order) that are ready, have a unit and its player,
// and pass the group's own test. Its pass is player-major, as a mode's own loop is: for each
// player, its arrival, then each zone in the order registered; the zones that fire for anyone
// after every player.
#pragma once

#include "noxcore/server/name.hpp"
#include "noxcore/server/object_key.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nox::server {

class GameHost;
class World;
struct HostedPlayer;
struct ServerObject;

/// What a watcher saw: a player's arrival, its coming within a zone or leaving it, a zone's firing,
/// a player within finding its spells changed.
struct WatchEvent {
    enum class Kind : std::uint8_t { Arrive, Enter, Leave, Fire, Changed, Stats, Beat, Once, First, Exit, Anyone, After, Begin, Held, Due } kind = Kind::Fire;
    std::uint32_t zone = 0; ///< the zone's own number (its registration's); arrivals: 0
    std::uint8_t peer = 0;  ///< (Any: the last player within)
    const HostedPlayer* player = nullptr;
    ServerObject* unit = nullptr;
    /// (A list's or a find's: the thing, and where it last stood. The event's: a key, not a stored link.)
    ObjectKey id = 0;
    float x = 0, y = 0;
};

class Watchers {
    template <class>
    friend struct Links; ///< (block_links.hpp)

public:
    /// What's watched for a player, besides being in the group: bits all of which must hold.
    enum Filter : std::uint32_t {
        NotObserving = 1u << 0, ///< statusFlags bit 0 clear
        NotBusy = 1u << 1,      ///< statusFlags bits 0 and 1 clear
        Alive = 1u << 2,        ///< its unit's health not 0
        NotWhole = 1u << 3,     ///< its unit's health or its mana short of the most
        /// (EachEnter: not a test) also fire Changed as a player within finds its spells' levels
        /// changed.
        SpellsChanged = 1u << 4,
        GameOver = 1u << 5,     ///< its player at its quest's game over (questGameOverAt not 0)
        /// (EachEnter: not a test) who's within kept by its unit's number, not its name.
        ByUnit = 1u << 6,
        /// (EachEnter: not a test) who's within kept by its peer, not its name (forgotten as the
        /// peer leaves the host).
        ByPeer = 1u << 7,
    };
    /// How a zone fires:
    /// - EachEnter: for each player, as it comes within (Enter) and as it leaves (Leave). A
    ///   player not in the group a tick keeps its place, in or out.
    /// - AnyCooldown: for the first player within (Fire), once `cooldown` frames have passed
    ///   since it last fired (it never has: at once).
    /// - Any: once a pass, after every player, when any player was within (Fire).
    enum class Mode : std::uint8_t { EachEnter, AnyCooldown, Any };

    using Event = WatchEvent;
    using Handler = std::function<void(const Event&)>;
    /// A group's own test for its players (none: every ready one with a unit and its player).
    using Admit = std::function<bool(const HostedPlayer&, const ServerObject& unit)>;

    /// A group: its players' test, and its arrivals' handler (each player once, as it's first
    /// seen in a pass; forgotten as it leaves the host; none: no arrivals watched).
    int group(Admit admit = {}, Handler arrive = {}, int roster = -1);
    /// A zone in group `g`: within `radius` of thing `thing` (its place each pass; gone: never
    /// within) or of the point (x, y) when `thing` is 0. Returns its number.
    std::uint32_t zone(int g, std::uint32_t thing, float x, float y, float radius, std::uint32_t filter, Mode mode, std::uint32_t cooldown,
                       Handler handler);
    /// Zone `zone` about thing `thing` now (a map's load: its things numbered afresh), what it
    /// knew of its players kept. 0: about nothing (never within).
    void retarget(std::uint32_t zone, std::uint32_t thing);
    /// An AnyCooldown zone's wait forgotten: it fires at once again.
    void rearm(std::uint32_t zone);
    /// Group `g`'s zones and arrivals gone (its number kept, to be given zones again).
    void reset(int g);
    /// Every group and zone gone.
    void clear();
    bool empty() const noexcept { return groups_.empty(); }

    /// A pass's players: each ready one with a unit and its player that `admit` takes (none:
    /// every one), in the host's order. Gathered once a tick, each group's pass reads it.
    struct Member {
        std::uint8_t peer = 0;
        const HostedPlayer* player = nullptr;
        ServerObject* unit = nullptr;
    };
    using Roster = std::vector<Member>;
    static void gather(GameHost& host, Roster& out, const Admit& admit = {});
    /// A roster shared by groups (their own test none: its), gathered once a tick (gatherRosters,
    /// before the groups' passes); a group made on it (group's `roster`).
    int roster(Admit admit = {});
    void gatherRosters(GameHost& host);
    /// Group `g`'s pass, at `frame`: over its roster (gathered this tick), or its own players.
    void evaluate(int g, GameHost& host, std::uint32_t frame);

    /// What ends a watched thing (a list's): any of these bits.
    enum End : std::uint32_t {
        Missing = 1u << 0,    ///< no thing of its number
        Gone = 1u << 1,       ///< gone (ServerObject::gone)
        Dead = 1u << 2,       ///< ObjFlag::Dead
        NoHealth = 1u << 3,   ///< health 0
        NotMonster = 1u << 4, ///< not a monster
        OtherType = 1u << 5,  ///< another type than it was watched as
        Deleted = 1u << 6,    ///< pendingDelete
        Destroyed = 1u << 7,  ///< ObjFlag::Destroyed
    };
    /// A list of things watched for their end (`ends`): each fires once (its id, where it last
    /// stood) and is dropped, in the order watched. Returns its number.
    int list(std::uint32_t ends, Handler handler);
    /// Thing `thing` (of type `type`, standing at x, y) watched by list `l`; every thing of list
    /// `l` dropped; how many it watches.
    void watch(int l, std::uint32_t thing, Name type = {}, float x = 0, float y = 0);
    void unwatchAll(int l);
    std::size_t watching(int l) const;
    /// List `l`'s pass: each thing's end looked for, else its place kept.
    void evaluateList(int l, World& world);

    /// A finder: a thing of a type near a point, sought each pass (the first in the world's order
    /// within the radius) till found; then it fires once (the thing) and stops.
    int finder(Handler handler);
    void seek(int f, Name type, float x, float y, float radius);
    void evaluateFind(int f, World& world);

    /// What's kept on a player of group `g` (by its name) at each keeping pass, in this order:
    /// - stats: fire Stats when its unit's most health, most mana or strength isn't what they were
    ///   after the last pass (the first pass too);
    /// - enchants: while in play (not observing, health not 0, not dead), each enchantment of the
    ///   list, missing or timed, put on for good with its power;
    /// - beat: while in play, fire Beat on frames that are a multiple of it (0: none);
    /// - once: while in play, fire Once the first pass of each serial (a level's).
    /// - strip: each enchantment of the list on for good (not timed) taken off, in play or not.
    /// A beat fires when (frame - beatFrom) % beat == beatAt: once in that frame, however many
    /// passes see it.
    struct Keep {
        bool stats = false;
        std::vector<std::pair<int, int>> enchants; ///< (enchantment, power)
        std::vector<int> strip;
        std::uint32_t beat = 0;
        std::uint32_t beatFrom = 0;
        std::uint32_t beatAt = 0;
        bool once = false;
    };
    /// Group `g`'s keeping handler (Stats, Beat, Once).
    void keeping(int g, Handler handler);
    /// What's kept on player `name` (what it was seen at, and its last Once, kept).
    void keep(int g, const std::string& name, Keep keep);
    /// What's kept on any player of group `g` with none of its own (each its own state).
    void keepEveryone(int g, Keep keep);
    void unkeepAll(int g);
    /// Player `name`'s Stats fired at the next pass, changed or not.
    void restat(int g, const std::string& name);
    /// A group's keeping pass: its admitted players' keeps, at `frame` of level `serial`.
    void evaluateKeep(int g, GameHost& host, std::uint32_t frame, std::uint32_t serial);
    /// Group `g`'s firsts: each player by name, once (till forgotten), as it's first admitted
    /// passing `filter` (First).
    void firsts(int g, std::uint32_t filter, Handler handler);
    void forget(int g, const std::string& name);
    void forgetAll(int g);
    void evaluateFirsts(int g, GameHost& host);
    /// Group `g`'s exits: each player's unit once (till reset), as its exit gate is one of `gates`
    /// (Exit; zone: its index in them).
    void exits(int g, std::vector<ObjectRef> gates, Handler handler);
    void evaluateExits(int g, GameHost& host);

    /// Group `g`'s conditions over its players this pass, each once till rearmed:
    /// - anyone: as it first has a player (the event's unit: its first);
    /// - after: as it has a player and `afterFrames` < frame - `afterFrom`;
    /// - all: as every player passes `allFilter` (at least one): Begin as that's first seen (its
    ///   frame kept: 0 is none, so seen again), Held once `allFrames` < frame - that frame; not
    ///   so before Held: forgotten.
    struct Conditions {
        Handler anyone;
        std::uint32_t afterFrames = 0;
        std::uint32_t afterFrom = 0;
        Handler after;
        std::uint32_t allFilter = 0;
        std::uint32_t allFrames = 0;
        Handler all; ///< (Begin, Held)
    };
    void conditions(int g, Conditions c);
    /// Its conditions as new (each to fire again; after's from now `afterFrom`).
    void rearmConditions(int g, std::uint32_t afterFrom);
    void evaluateConditions(int g, GameHost& host, std::uint32_t frame);

    /// A timer: Beat each frame where (frame - from) % beat == at (every; once in that frame, however
    /// many passes see it), or Due once when frame passes its frame (due; till set again). Its pass
    /// where the mode's own check stood.
    int timer(Handler handler);
    void every(int t, std::uint32_t beat, std::uint32_t from = 0, std::uint32_t at = 0);
    void due(int t, std::uint32_t frame);
    void stop(int t);
    void evaluateTimer(int t, std::uint32_t frame);

private:
    struct Zone {
        std::uint32_t number = 0;
        ObjectRef thing;
        bool point = false; ///< about (x, y), not a thing
        float x = 0, y = 0, radius = 0;
        std::uint32_t filter = 0;
        Mode mode = Mode::EachEnter;
        std::uint32_t cooldown = 0;
        std::uint32_t firedAt = 0; ///< (AnyCooldown's; 0: never)
        // (A pass's own: where it is, and Any's last player within.)
        bool there = false;
        float px = 0, py = 0;
        bool anyHit = false;
        WatchEvent anyEvent;
        std::set<std::string> inside; ///< (EachEnter's: by player name)
        std::set<std::uint32_t> insideIds; ///< (EachEnter's with ByUnit or ByPeer: by unit or peer; a peer or a key by the filter, not a link)
        std::map<std::string, std::array<std::uint8_t, 0x89>> spells; ///< (SpellsChanged's: as each was last seen)
        Handler handler;
    };
    struct Group {
        Admit admit;
        Handler arrive;
        int roster = -1;
        std::set<std::string> seen;
        std::vector<std::uint32_t> joins; ///< (the host's players' join numbers as `seen` was last looked over)
        std::vector<Zone> zones;
        // (Keeping, firsts, exits.)
        struct Kept {
            Keep keep;
            bool seen = false;
            std::uint16_t maxHealth = 0, maxMana = 0;
            std::uint32_t strength = 0;
            std::uint32_t onceAt = 0;
            std::uint32_t beatFired = ~0u; ///< (the frame its Beat last fired)
        };
        Handler keeping;
        std::map<std::string, Kept> kept;
        bool everyone = false;
        Keep keepAll;
        struct Cached {
            std::uint32_t join = 0; ///< (HostedPlayer::joinSerial: 0 none)
            ObjectRef unit;
            Kept* kept = nullptr;
        };
        std::array<Cached, 256> keptByPeer{};
        std::uint32_t firstsFilter = 0;
        Handler firsts;
        std::set<std::string> first;
        std::vector<ObjectRef> gates;
        Handler exits;
        RefSet through;
        Conditions conditions;
        bool anyoneDone = false, afterDone = false, allDone = false;
        std::uint32_t allSince = 0;
    };
    struct Timer {
        Handler handler;
        std::uint32_t beat = 0, from = 0, at = 0;
        std::uint32_t beatFired = ~0u; ///< (the frame its Beat last fired)
        bool dueArmed = false;
        std::uint32_t dueAt = 0;
    };
    std::vector<Timer> timers_;
    // (A type as the server's interned name: an object's typeName is the same entry, so a type's
    // compared by one pointer.)
    struct Watched {
        ObjectRef thing;
        Name type;
        float x = 0, y = 0;
    };
    struct List {
        std::uint32_t ends = 0;
        Handler handler;
        std::vector<Watched> things;
    };
    struct Finder {
        Handler handler;
        bool seeking = false;
        Name type;
        float x = 0, y = 0, radius = 0;
    };
    Zone* find(std::uint32_t zone);

    std::vector<List> lists_;
    std::vector<Finder> finders_;
    std::vector<Group> groups_;
    std::uint32_t nextZone_ = 1;
    struct Shared {
        Admit admit;
        Roster members;
    };
    std::vector<Shared> rosters_;
    Roster scratch_;
    /// Group `g`'s players this pass.
    const Roster& membersOf(int g, GameHost& host);
};

} // namespace nox::server
