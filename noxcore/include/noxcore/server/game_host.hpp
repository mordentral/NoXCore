// A hosted game on the network: the server (world, script, clock) behind the server transport,
// with the players who join it. No sockets: feed it datagrams and the time, send what it hands
// back (a dedicated server does, on a UDP port).
//
// Joining, as GAME.EXE's server does it:
//   CLIENT_ACCEPT   Plr_CreateFromJoinData: a NewPlayer (PlayerFemale) unit at a
//                   PlayerStart; its PlayerInfo is the message's first 97 bytes. Then the join
//                   sequence (SrvNet_SendJoinSequence): FULL_TIMESTAMP, JOIN_DATA,
//                   GAME_SETTINGS, GAME_SETTINGS_2, its NEW_PLAYER, then USE_MAP; everyone else
//                   gets its NEW_PLAYER.
//   CLIENT_READY    (SrvNet_OnClientReady) it's in: MapInitialize / MapEntry run once a
//                   player is.
//   TEXT_MESSAGE    relayed as it came (SrvNet_OnPacket 0xA8) to every player, the sender too.
//   CLIENT_CLOSE    the player goes; everyone gets PLAYER_QUIT.
// Each frame, every ready player gets its update (updatePlayer).
#pragma once

#include "noxcore/lobby/listing.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/net/server_transport.hpp"
#include "noxcore/net/update_stream.hpp"
#include "noxcore/server/map_cycle.hpp"
#include "noxcore/server/map_vote.hpp"
#include "noxcore/server/rules.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/update_writer.hpp"
#include "noxcore/server/xfer.hpp"
#include "noxcore/things/game_data.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server {

/// A set of extents, kept as a bit each (one past ExtentTable::kLimit in a std::set): a set's
/// look-ups, walked upward as std::set's are.
class ExtentSet {
public:
    class iterator {
    public:
        using value_type = std::uint32_t;
        using difference_type = std::ptrdiff_t;
        iterator() = default;
        std::uint32_t operator*() const noexcept { return far_ ? *it_ : e_; }
        iterator& operator++()
        {
            if (far_) {
                ++it_;
            } else {
                seek(e_ + 1);
            }
            return *this;
        }
        iterator operator++(int)
        {
            iterator was = *this;
            ++*this;
            return was;
        }
        friend bool operator==(const iterator& a, const iterator& b) noexcept
        {
            return a.far_ == b.far_ && (a.far_ ? a.it_ == b.it_ : a.e_ == b.e_);
        }

    private:
        friend class ExtentSet;
        iterator(const ExtentSet* s, std::uint32_t from) : s_(s) { seek(from); }
        iterator(const ExtentSet* s, std::set<std::uint32_t>::const_iterator it) : s_(s), far_(true), it_(it) {}
        /// The first bit set from `from` on; past them, the set's first.
        void seek(std::uint32_t from)
        {
            const auto& w = s_->words_;
            for (std::size_t i = from / 64; i < w.size(); ++i) {
                std::uint64_t bits = w[i];
                if (i == from / 64) {
                    bits &= ~0ull << (from % 64);
                }
                if (bits != 0) {
                    e_ = static_cast<std::uint32_t>(i * 64 + static_cast<std::size_t>(std::countr_zero(bits)));
                    return;
                }
            }
            far_ = true;
            it_ = s_->far_.begin();
        }

        const ExtentSet* s_ = nullptr;
        bool far_ = false;
        std::uint32_t e_ = 0;
        std::set<std::uint32_t>::const_iterator it_;
    };
    using const_iterator = iterator;

    iterator begin() const { return iterator(this, 0u); }
    iterator end() const { return iterator(this, far_.end()); }
    bool contains(std::uint32_t e) const noexcept
    {
        if (e >= kNear) {
            return far_.contains(e);
        }
        const std::size_t i = e / 64;
        return i < words_.size() && (words_[i] >> (e % 64) & 1u) != 0;
    }
    void insert(std::uint32_t e)
    {
        if (e >= kNear) {
            far_.insert(e);
            return;
        }
        const std::size_t i = e / 64;
        if (i >= words_.size()) {
            words_.resize(i + 1, 0);
        }
        words_[i] |= 1ull << (e % 64);
    }
    std::size_t erase(std::uint32_t e)
    {
        if (e >= kNear) {
            return far_.erase(e);
        }
        const std::size_t i = e / 64;
        if (i >= words_.size() || !(words_[i] >> (e % 64) & 1u)) {
            return 0;
        }
        words_[i] &= ~(1ull << (e % 64));
        return 1;
    }
    /// Erases the one at `it`; the next after it.
    iterator erase(iterator it)
    {
        const std::uint32_t e = *it;
        ++it;
        erase(e);
        return it;
    }
    void clear() noexcept
    {
        words_.clear();
        far_.clear();
    }
    bool empty() const noexcept { return begin() == end(); }

private:
    static constexpr std::uint32_t kNear = 0x10000;
    std::vector<std::uint64_t> words_;
    std::set<std::uint32_t> far_;
};

// One player in the hosted game, keyed by its transport peer: its join data, what its client
// knows, and the important messages held for it while it loads.
struct HostedPlayer {
    std::uint8_t peer = 0;         ///< the transport's stream id
    std::uint32_t extent = 0;      ///< its unit; the net code clients know it by
    /// Its join's number in this host, never given again (1 on): a cache's key for the player
    /// (a peer, its record's address and a unit's number are all used again). Not on the wire.
    std::uint32_t joinSerial = 0;
    net::msg::ClientAccept accept;
    std::array<std::uint8_t, 97> info{}; ///< the PlayerInfo block
    std::string name;
    bool ready = false;            ///< sent CLIENT_READY
    /// Sent INCOMING_CLIENT (its bit in g_playersInGameMask, kept through map changes): important
    /// messages to everyone wait for its CLIENT_READY (frees a node only once each
    /// in-game player has it).
    bool joined = false;
    std::uint32_t heardAt = 0;     ///< (Player's: the frame of its last packet, SrvNet_OnPacket)
    std::uint8_t slot = 0;        ///< its bit in objects' syncMask (0-31)
    float viewHalfWidth = 320;     ///< half its screen (CLIENT_ACCEPT's size / 2: Player view)
    float viewHalfHeight = 240;
    net::AliasTable aliases;       ///< its client's names for objects (MSG_NEW_ALIAS)
    ExtentSet knows; ///< objects it's been sent and not told are out of sight
    /// An important message held for it (Net_QueueImportantEx's node): `onlyJoined` (its
    /// argument) is dropped when it goes if the player isn't in the game then (g_playersInGameMask:
    /// netImportant's check).
    struct Held {
        std::vector<std::uint8_t> bytes;
        bool onlyJoined = false;
    };
    /// Its important messages from before it was ready (netImportant sends a player
    /// none of the queue till its status 0x10), oldest first; kept through a map change.
    std::vector<Held> pending;
};

// The hosted game. The owner feeds it datagrams (receive) and the time (tick) and sends what
// takeOutgoing hands back; the rest is the host's windows and console reading or changing it.
class GameHost {
public:
    struct Options {
        std::string serverName = "noxcpp";
        int maxPlayers = 16;
        std::string password;
        int multiplier = 1;
        std::uint32_t gameFlags = 0x100; ///< arena (lobby/listing.hpp's mode bits)
        /// setQuest(1): a quest game hosted (World::questChosen: startSoloQuest begins it).
        bool quest = false;
        /// (noxcpp: a NoX Core mode's) When not empty, the quest's maps are these alone,
        /// whatever the maps folder holds; when empty, a mode's generated maps are never among them.
        std::vector<std::string> questMapsOnly;
        /// (a NoX Core mode's) Called with the map about to load and the world before it, so a
        /// generated map can be written there first (the mode's set-up gives it).
        std::function<void(const std::filesystem::path&, const World&)> prepareMap;
        /// (a NoX Core mode's) A player's chat line (its text, trimmed; a stock client's or a noxsdl
        /// one's), before it's relayed: true takes it (not relayed; the mode's own answers and
        /// commands, as "?difficulty").
        std::function<bool(GameHost&, std::uint8_t peer, std::u16string_view text)> onChat;
        /// noxcpp's own messages from a noxsdl client (one that identified itself; a stock
        /// client's are dropped), by their opcode: each NoX Core mode's set-up adds its own
        /// (MSG_NOXCPP_MODE_F4 0xF4, MSG_NOXCPP_MODE_F5 0xF5 and MSG_NOXCPP_COOP 0xF7: the built-in
        /// modes', each its registry entry's opcode; MSG_NOXCPP_EDIT 0xF6:
        /// EditSessionDirector::onMessage; MSG_NOXCPP_MODEUI 0xF8: a script mode's UI). One with
        /// none: dropped.
        using NoxcppHandler = std::function<void(GameHost&, std::uint8_t peer, std::span<const std::uint8_t> m)>;
        std::map<std::uint8_t, NoxcppHandler> noxcppMessages;
        /// noxcpp (a NoX Core mode's): a player made, its unit in the world before its first frame
        /// (a solo game's host after its saved game's restore and its .plr), and a player about to
        /// go, its unit still there with what it holds.
        std::function<void(GameHost&, std::uint8_t peer, ServerObject& unit)> onJoin;
        std::function<void(GameHost&, std::uint8_t peer, ServerObject& unit)> onLeave;
        /// noxcpp (a NoX Core mode's): a player's CLIENT_READY (its map loaded, at its join and at
        /// each map's), after what the game sends it then.
        std::function<void(GameHost&, std::uint8_t peer)> onClientReady;
        /// noxcpp (a NoX Core mode's): an object a mode gave a draw scale (ServerObject::ModSize's
        /// drawScale, not 1) went to a player that didn't know it: its record written in this
        /// player's update, the first since it came into its view (or since its client forgot
        /// it). Asked of no other object; it may only send (nox.setDrawScale's message).
        std::function<void(GameHost&, std::uint8_t peer, ServerObject& o)> onScaledSeen;
        /// noxcpp (a NoX Core mode's): a reliable message the frame tells one player (and the walls
        /// broken told at its join), put another way for it; none: as it is (what that client
        /// draws differs from the world's numbering, e.g. a solo game's guest on the map's own file).
        std::function<std::optional<std::vector<std::uint8_t>>(GameHost&, std::uint8_t peer, std::span<const std::uint8_t> m)> forPeer;
        /// noxcpp (a NoX Core mode's): a player's own character from its file (SAVE_SERVER) put on
        /// its unit, or not (`loaded`).
        std::function<void(GameHost&, std::uint8_t peer, bool loaded)> onCharacter;
        /// noxcpp (a NoX Core mode's): the game flags a player's client is told it's in (the
        /// world's `flags` given): MSG_GAME_SETTINGS', and its character read and written as that
        /// game's. None: the world's for everyone.
        std::function<std::uint32_t(GameHost&, std::uint8_t peer, const HostedPlayer& p, std::uint32_t flags)> clientGameFlags;
        /// noxcpp (a NoX Core mode's): the mode the game is listed by (the server info's and the
        /// game list's mode bits, 0x17F0); none: the world's.
        std::optional<std::uint32_t> listedMode;
        /// A solo game (the campaign: game flag 0x800 without the network's 0x2000, the main menu's
        /// windowMainMenuProc; gameFlags unused): one player, the host's own, in slot 0x1F (solo.hpp).
        bool solo = false;
        /// (noxcpp: a NoX Core mode's) A solo game with room for this many guests besides its
        /// host: the host still made and awaited first in slot 0x1F (while it's awaited, no other
        /// player is taken; with setHostPeer's address, only that one is the host), each guest in
        /// the lowest free slot below it. 0: the solo game alone, as the exe's.
        int soloGuests = 0;
        /// noxcpp: feature bits a NoX Core mode adds to the extended reply (lobby/extended.hpp's
        /// ServerFeature, as CoopCampaign), for noxsdl's game list.
        std::uint32_t features = 0;
        /// noxcpp (a NoX Core-only mode's: mode.json's "stockClients": false): a stock client (one
        /// that didn't identify itself) is let in only to read `notice` (as the message of the day
        /// and a text line), held out of play as an observer (PlayerControl::heldOut), and put out
        /// `holdMs` after its CLIENT_READY (120 s after its join if it never readies, or when it
        /// leaves first). The extended reply says NoxCoreOnly. None: everyone plays.
        struct StockRefusal {
            std::string notice;
            std::uint32_t holdMs = 15000;
            std::uint32_t readyWithinMs = 120000; ///< (put out this long after its join, never ready)
        };
        std::optional<StockRefusal> stockRefusal;
        /// noxcpp: the lowest NoX Core client level let in (lobby::kClientLevel; 0: any, as before):
        /// a NoX Core game below it is refused at its join (ServerTransport::setMinClientLevel).
        std::uint16_t minClientLevel = 0;
        /// noxcpp (a mode's "clientKeys"): this host's client key realm (0: none). The extended reply
        /// names it, and a NoX Core client that sends its secret for it is given a key (clientKey).
        std::uint64_t clientKeyRealm = 0;
        /// A solo game's saves: the game's folder (Save\ under it, solo_save.hpp; empty: none
        /// written, every save succeeds) and the host client's own Player.plr sections and its
        /// closing 0 id (noxsdl's; none: only the 0 id).
        std::filesystem::path saveDir;
        std::function<bool(const std::filesystem::path&)> clientSections;
        /// A solo game the character screen's Load starts (windowSelCharProc's
        /// 0x1F6): the map is the save's copy (this path: Server::startFromSave) and the script
        /// ids go on from `scriptCounter` (the save's counter). Empty: a new game.
        std::string startFromSave;
        std::optional<std::int32_t> scriptCounter;
        /// A solo game's host player, new or saved, is made before the first load in the exe
        /// (the main loop: the solo start, then servInitialMapLoad): ours takes the
        /// counter's first id (its first load's outfit the next ones) and the
        /// world runs no frame till its CLIENT_ACCEPT; then its team 1 and
        /// the saved game's restore with it in (Server::firstLoadHostIn: a save's SaveGameLocation...), its
        /// PlayerInit's outfit, and
        /// the solo host's join loads `hostPlayerFile` (clientCurSave: a new character's WORKING
        /// Player.plr, a continued one's slot's) onto it, its server sections as SAVE_SERVER
        /// (CliNet_SendSaveServerXfer, the host's at once: the received file's case 3; a new
        /// character's has none, cliPlrInfoLoadFromFile's resets alone) and its client sections by the host's
        /// own client (Plr_LoadClientPlayerData). No map intro, no "Game Loaded.". Empty: none
        /// (not the exe's, whose client always has one): nothing loaded.
        std::filesystem::path hostPlayerFile;
        /// gamedata.bin, for the game's numbers (class stats, spells...): required, the
        /// constructor throws without it.
        const things::GameData* gameData = nullptr;
        /// modifier.bin, for the weapons and armour (null: anyone may use anything).
        const things::ModifierDb* modifiers = nullptr;
        /// monster.bin, a network game's numbers (null: monsters without them).
        const things::MonsterDb* monsterDb = nullptr;
        /// soundset.bin, the creatures' sounds (null: none).
        const things::SoundSets* soundSets = nullptr;
        /// nox.csf, for the server's lines (null: their labels).
        const strings::StringTable* strings = nullptr;
        /// Gameplay switches (4 team play...) and each mode's limits (Server::Options').
        std::uint32_t gameplayFlags = 0;
        /// noxcpp (a mode's: mode.json's "gameplay"): the gameplay flags of 1 team damage, 2
        /// auto-assign and 4 team play that stay as `gameplayFlags` has them for the game. The
        /// host's team options refuse a change to them (setUseTeams, setAutoAssign, setTeamDamage),
        /// and with 4 pinned also Shuffle and a CTF or Flag Ball choice's teams. 0: none, as before.
        std::uint32_t gameplayPinned = 0;
        std::array<std::uint16_t, 6> lessonLimits{};
        std::array<std::uint8_t, 6> timeLimits{};
        /// The server options (Server::Options'; MSG_GAME_SETTINGS +13): 2 weapons respawn.
        std::uint32_t serverOptions = 0;
        bool autoRespawn = false; ///< (Server::Options')
        /// mapcycle.txt (MapCycle; empty: none, the map plays again), and the maps folder its
        /// names are found in (empty: the hosted map's folder's).
        std::filesystem::path mapCycle;
        std::filesystem::path mapsDir;
        /// The game's folder, where execrul finds its .rul files (OTQuest.rul); empty: the maps
        /// folder's parent, as it is in every game but the map editor's quest play-test (whose
        /// maps folder is its own).
        std::filesystem::path gameDir;
        /// Votes a kick needs (the console's; 1-32, 0 none: no kicking).
        int kickVotes = 4;
        /// nox.cfg's ResetQuestMinVotes and KickQuestPlayerMinVotes (server_resetQuestMinVotes / server_kickQuestPlayerMinVotes, 6
        /// and 5): a quest's restart vote and its kick vote may be cast while they aren't 0 (their
        /// numbers aren't used: an original bug).
        int resetQuestVotes = 6;
        int kickQuestPlayerVotes = 5;
        /// The sysop password (Game_GetSysopPassword; the console's "set sysop"): a client that
        /// sends it (MSG_SYSOP_PW) may run host commands remotely. Empty: nobody may.
        std::u16string sysopPassword;
        /// noxcpp's test commands ("nx ...": spawn, give, god, heal...) on the host's console and
        /// a sysop's, their lines echoed back to the sysop (not GAME.EXE's: for testing).
        bool testCommands = false;
        /// The message of the day (motd.txt, read whole at the start: motd) and
        /// nox.cfg's SendMessageOfTheDay: sent each player as it comes in (MSG_XFER "MOTD", action
        /// 1, its text with its NUL: playerSendMOTD) in a network game not a quest.
        std::optional<std::string> motd;
        bool sendMotd = false;
        /// Hosted on WOL (the host logged in to XWIS): World::wolGame.
        bool wolGame = false;
        /// The map cycle on (nox.cfg's MapCycle, "set cycle").
        bool mapCycleOn = true;
        /// noxcpp's map vote (map_vote.hpp): at a match's end, and called by a player.
        bool mapVote = true;
        /// noxcpp: a quest's screens shown (World::questScreens); off for a mode whose levels
        /// follow on without the rules, stage and results screens.
        bool questScreens = true;
        /// noxcpp: a quest's saves sent its clients (World::questSaves: each level's end, before a
        /// game over's kick); off for a mode whose players come in from a chat lobby with
        /// their network characters, which a quest save would overwrite.
        bool questSaves = true;
        /// noxcpp: every player of this class whatever its character's (World::forceClass): in a
        /// quest without quest saves only (a forced class saved to a stock client's .plr would
        /// leave its spell bars under the wrong class). E.g. 2, conjurers.
        std::optional<std::uint8_t> forceClass;
        /// noxcpp: a mode's multiclass for the players on noxsdl (those that identified,
        /// the extended query): any class's spells, abilities and guides learned and used, any
        /// class's weapons, a conjurer's mana (PlayerControl::multiclass). Stock clients' players
        /// stay their classes. Told to the client with MSG_NOXCPP_MODE_F4 kMulticlass.
        bool multiclass = false;
        /// noxcpp: a mode's monsters look all about them, not only ahead (World::monstersAlert).
        bool monstersAlert = false;
        /// noxcpp: a mode's dead players keep their things outside a quest, as a quest's do
        /// (World::keepThingsOnDeath).
        bool keepThingsOnDeath = false;
        /// noxcpp: outside a quest a network game's book is the mode's: a player starts with no
        /// spells, abilities or field guide creatures and has only what the mode grants, checked as
        /// a quest checks them (World::modeOwnsBook). A script mode tells its NoX Core clients
        /// (MSG_NOXCPP_MODEUI kBook), whose books then list only what was granted.
        bool modeOwnsBook = false;
        /// noxcpp: the map editor's play-test sandbox, players can't die (World::sandboxPlayersCantDie).
        bool playersCantDie = false;
        /// noxcpp: a map editor session's sandbox, nothing scores (World::sandboxNoScoring).
        bool sandboxNoScoring = false;
        /// noxcpp: a mode that plays as a stock one and keeps its limits itself: lessonLimits and
        /// timeLimits told to clients, not acted on by the game's checks (World::modeOwnsLimits).
        bool modeOwnsLimits = false;
        /// noxcpp (a mode's: mode.json's "mapRespawns": false): no map thing respawns
        /// (Server::Options::noMapRespawns).
        bool noMapRespawns = false;
        /// noxcpp (a mode's: mode.json's "pickup"): players pick up what their class can't use
        /// (never wearing it), and seconds under server option 2 (World::pickupAnyClass,
        /// World::pickupDuplicates).
        bool pickupAnyClass = false;
        bool pickupDuplicates = false;
        /// The game's own float rounding (towards zero, as its x87 chops) for the server's figuring;
        /// off: to nearest (VD-37). Server::Options::exactMaths.
        bool exactMaths = true;
        /// A client of the host's own in the process (noxsdl's): the game flags' 2 (a dedicated
        /// server's are 1 alone), as the settings' slot 0 keeps them (its refresh's + 0x34).
        bool hostClient = false;
    };

    GameHost(const things::ThingDb& things, const std::filesystem::path& mapPath, Options options);

    void receive(const net::Endpoint& from, std::span<const std::uint8_t> datagram, std::uint32_t nowMs);
    /// Runs the transport and as many substeps as the time calls for.
    void tick(std::uint32_t nowMs);
    std::vector<net::ServerTransport::Datagram> takeOutgoing() { return transport_.takeOutgoing(); }
    /// The same into the caller's list (its room kept for the next: no allocation a frame).
    void takeOutgoing(std::vector<net::ServerTransport::Datagram>& into) { transport_.takeOutgoing(into); }
    /// Tests and test harnesses: a decoded datagram's messages handled as if `peer`'s stream had
    /// carried them (what the transport hands on for a connected client).
    void payloadForTest(std::uint8_t peer, std::span<const std::uint8_t> bytes) { onPayload(peer, bytes); }

    Server& server() noexcept { return server_; }
    /// The host's own client (host_client.hpp: noxsdl's in-process host), for GAME.EXE's direct
    /// calls into it (fades, widescreen, the map intro, music...); null: none.
    void setHostClient(HostClient* client) noexcept { server_.setHostClient(client); }
    const std::map<std::uint8_t, HostedPlayer>& players() const noexcept { return players_; }
    /// The unit of the player on `peer` (null: no such player, or no unit of its extent in the world).
    ServerObject* unitOf(std::uint8_t peer);
    /// The game flags a player's client is told it's in (Options::clientGameFlags; none: the world's).
    std::uint32_t clientFlags(std::uint8_t peer);
    /// (The same for a player being made: `p` not in players() yet.)
    std::uint32_t clientFlags(std::uint8_t peer, const HostedPlayer& p);
    /// noxcpp (a mode's): the quest's save step for one player (not the host's own): its save,
    /// written as its client's game, sent (MSG_GAUNTLET 0x1C {1}, SAVEDATA). Whether it had a unit.
    bool sendSaveTo(std::uint8_t peer);
    /// A player's latency (ms) as the server reports it (MSG_REPORT_ALL_LATENCY): its stream's
    /// (ServerTransport::latency); the host's own (player 0x1F's) is the latency compensation's:
    /// 0 none, 1 the least of the others' over 0, 2 their average, 3 the amount set. 0 for no player.
    int latencyOf(std::uint8_t peer) const;
    const Options& options() const noexcept { return options_; }
    /// Where the game's maps are (Options::mapsDir; none: the map's folder's parent).
    std::filesystem::path mapsDir() const;
    /// The game as XWIS lists it (the WOL record GAME.EXE posts: lobby::makeWolRecord): the name,
    /// the map, the mode, the players and their limit, the quest's stage, the Access settings
    /// (closed, password, the classes refused, the ping limits). The port is the caller's.
    lobby::GameListing listing();

    // The host's Server Settings window (noxsdl's server_options, servopts.wnd: GAME.EXE's
    // ServOpts.c on the host's settings data).
    /// A network map the window lists: its modes (as game flags) and recommended players.
    struct MapChoice {
        std::string name;
        std::uint32_t modes = 0;
        int minPlayers = 2;
        int maxPlayers = 16;
    };
    /// What the window edits: the game's name, the mode chosen, the map and that mode's limits.
    struct ServerSettings {
        std::string name;
        std::uint32_t mode = 0;
        std::string map;
        int lessonLimit = 0;
        int timeLimit = 0;
        int teams = 0; ///< (shown: getTeamCounter)
        /// The spells, weapons and armour allowed (Advanced...): Go writes them to the map's
        /// user.rul for the mode, and they're the game's from its load on.
        std::optional<RuleSet> rules;
        /// The type the window chose had flags (Capture the Flag, Flag Ball: 0x60) though `mode`
        /// is the chat map's ("Form a new game"): makes the teams by the settings'
        /// type all the same.
        bool flagTeams = false;
    };
    ServerSettings serverSettings();
    /// The game's name as the window's field is left (ServOpts.c's 0x4003 for 0x2775:
    /// gameSetServername at once, Go or not).
    void setServerName(const std::string& name);
    /// noxcpp's own messages (MSG_NOXCPP_*): `bytes` to `peer`, reliably, when its client
    /// identified itself (a noxsdl: lobby/extended.hpp); a stock client never gets one. Whether
    /// it went.
    bool sendToIdentified(std::uint8_t peer, std::vector<std::uint8_t> bytes);
    /// (noxcpp's: questSaves off) The SAVEDATA a quitting client is sent instead of its unit's:
    /// short of the client's 700 bytes, so it writes nothing and its .plr stays.
    static constexpr std::size_t kRefusedSaveBytes = 8;
    /// Whether `peer` identified itself (a noxsdl).
    bool identified(std::uint8_t peer) const { return transport_.identified(peer); }
    /// What an identified client said it takes (lobby::ClientFeature); 0 for any other.
    std::uint32_t clientFeatures(std::uint8_t peer) const { return transport_.clientFeatures(peer); }
    /// The client level an identified client named (lobby::kClientLevel: a feature keyed "level N
    /// or above" goes only to one at N or above); 0 for any other.
    std::uint16_t clientLevel(std::uint8_t peer) const { return transport_.clientLevel(peer); }
    /// (Options::clientKeyRealm) The key `peer`'s client proved for this host's realm: 32 hex
    /// digits, the same for that PC every time; empty for any other.
    const std::string& clientKey(std::uint8_t peer) const { return transport_.clientKey(peer); }
    /// (noxcpp's) The reliable packets `peer` has out and not yet acknowledged (the sequence
    /// numbers from its last acknowledgement to the next we'd send).
    std::size_t reliableOutstanding(std::uint8_t peer) const { return transport_.reliableOutstanding(peer); }
    /// (Tests) A peer's stream: our next reliable sequence number, its last acknowledgement.
    std::optional<net::ServerTransport::ReliableSeqs> reliableSeqs(std::uint8_t peer) const { return transport_.reliableSeqs(peer); }
    /// (noxcpp's) The most reliable packets a client may have out before NoX Core's own paced
    /// senders hold back (the fast map send, a mode's UI, assets, values and views, its weather, an
    /// edit session's live view and mod files). The transport's acknowledgement compares sequence
    /// numbers safely only within 32 of each other (transport.cpp's acknowledged; OB-001, OB-410):
    /// past that a packet can be taken as acknowledged and dropped unsent, or kept a lap. These
    /// senders together stay under this, leaving the rest of the 32 to the game's own messages,
    /// which are never held (the game's own may still pass 32 in a long wait for an
    /// acknowledgement). (The session's messages, edit_session.hpp, wait the same way.)
    static constexpr std::size_t kReliableSpan = 24;
    /// How many more reliable packets one of those senders may put out to `peer` now (0: wait).
    /// Each asks before each packet it adds. (A small one, latest wins: a mode's values, views and
    /// weather.)
    std::size_t reliableRoom(std::uint8_t peer) const
    {
        const std::size_t out = reliableOutstanding(peer);
        return out < kReliableSpan ? kReliableSpan - out : 0;
    }
    /// The span's last packets the bulk senders (the fast map send, a mode's assets and UI, an edit
    /// session's messages, live view and mod files) leave to the small ones, so a value or the
    /// weather never waits on a big transfer.
    static constexpr std::size_t kReliableBulkReserve = 4;
    /// reliableRoom for a bulk sender: less kReliableBulkReserve.
    std::size_t reliableBulkRoom(std::uint8_t peer) const
    {
        const std::size_t room = reliableRoom(peer);
        return room > kReliableBulkReserve ? room - kReliableBulkReserve : 0;
    }
    /// The fast map send's block (map_send.cpp, to a client of lobby::kFastMapSendLevel or above):
    /// the most a reliable packet carries (net::kMaxReliablePayload) less MSG_MAP_SEND_PACKET's
    /// 6-byte head (a 1026-byte datagram: under the usual 1500-byte MTU).
    static constexpr std::uint16_t kFastMapBlock = static_cast<std::uint16_t>(net::kMaxReliablePayload - 6);
    /// The fast send's most blocks in a frame to one client (30 Hz: about 240 KB/s).
    static constexpr int kFastMapBlocksPerFrame = 8;
    /// The fast sends' most blocks in a frame to all clients together, shared round robin (about
    /// 2 MB/s: a burst a socket's send buffer takes). The stock stream's blocks don't count.
    static constexpr int kFastMapBlocksPerFrameAll = 64;
    /// The client's unacknowledged reliable packets the fast send stops at, at most: it keeps the
    /// map's own packets in flight under the 32 numbers the transport's acknowledgement compares
    /// safely (transport.cpp's acknowledged); other reliable traffic to the client counts in the
    /// window but isn't held by it. (A bulk sender: reliableBulkRoom stops it there.)
    static constexpr std::size_t kFastMapWindow = kReliableSpan - kReliableBulkReserve;
    /// The window a send starts with, and the least it falls to. A client takes reliable packets in
    /// order only, so one lost drops all sent after it, and they come again one round trip at a
    /// time (an acknowledgement's resend) or after 2-3 s (the resend timer): a frame's blocks not
    /// acknowledged within kFastMapLateMs and twice the round trip (the quickest a frame's first
    /// packet was acknowledged; nothing late before one is, till kFastMapFirstLateMs) halve the
    /// window, and each frame's acknowledged in time opens it (by as many packets till the first
    /// late, by one a round trip after), so a lossy link keeps few in flight, as the stock stream
    /// does.
    static constexpr std::size_t kFastMapStartWindow = 8;
    static constexpr std::size_t kFastMapMinWindow = 2;
    static constexpr std::uint32_t kFastMapLateMs = 100;
    static constexpr std::uint32_t kFastMapFirstLateMs = 1000;
    /// A client's requests after each map load that get the fast send (a retry after a bad CRC
    /// too); its later ones in the same map get the stock stream, so a client that asks again and
    /// again can't have the host send at the fast rate for good.
    static constexpr std::uint8_t kFastMapRequestsPerMap = 2;
    /// (noxcpp's) The reliable packets `peer` hasn't acknowledged yet (its queue holds
    /// net::kMaxQueuedReliable; past that a reliable send is lost): what a "latest wins" sender
    /// looks at before adding more.
    std::size_t queuedReliable(std::uint8_t peer) const { return transport_.queuedReliable(peer); }
    /// noxcpp's own messages sent unreliably (the map editor's presence: latest wins), to an
    /// identified client only. Whether it went.
    bool sendToIdentifiedUnreliable(std::uint8_t peer, std::vector<std::uint8_t> bytes);
    /// Whether `peer` is the host's own client (setHostPeer's).
    bool isHostPeer(std::uint8_t peer) const;
    // The map editor's session (noxcpp's, edit sessions only: server/edit_session.hpp).
    /// The map file now on disk (maps\<map>\<map>.map / .nxz, rewritten by an objects-only
    /// publish) becomes the one a joining client is told and sent (its CRC, the download's .nxz),
    /// with no map load: the world already holds what changed. Sends under way keep their copy (as
    /// a map load's mapSendLoadMap does).
    void refreshMapFile();
    /// A map load asked for now, as the console's "load" (mapLoad), with the game's mode set first
    /// when given (the next map's, as the Server Settings' Go keeps it).
    void loadMapNamed(const std::string& name, std::optional<std::uint32_t> mode = std::nullopt);
    /// The sandbox's "players can't die" (World::sandboxPlayersCantDie), now and for the maps after.
    void setPlayersCantDie(bool on);
    /// The session sandbox's "no scoring" (World::sandboxNoScoring), now and for the maps after.
    void setSandboxNoScoring(bool on);
    /// What the game list's extended reply says the server does (lobby/extended.hpp): high rates,
    /// and an edit session (a pencil in noxsdl's browser).
    std::uint32_t extendedFeatures() const;
    /// The maps folder's network maps (as scanMaps finds them), strcmp order.
    std::vector<MapChoice> mapChoices();
    /// A mode's limits as the game began (its Options: nox.cfg's, --host-...): the window's Defaults.
    std::pair<int, int> defaultLimits(std::uint32_t mode) const;
    /// A mode's limits as they stand (servGamedataGet).
    std::pair<int, int> limitsFor(std::uint32_t mode);
    /// The Server tab's General page and its Advanced Server window (general.wnd,
    /// windowServerOptionsGeneralProc; advserv.wnd).
    struct HostSwitches {
        bool forceRespawn = false; ///< (World::autoRespawn; a quest's never)
        bool sendMotd = false;     ///< server_sendMotd
        bool mapCycle = true;      ///< the map cycle on
        bool itemRespawn = false;  ///< server option 2
        /// Server option 0x2000; always on in elimination and CTF (game flags 0x420).
        bool camperAlarm = false;
        bool bones = false;        ///< (World::leaveBones)
        bool gestures = true;      ///< (World::gestures)
        /// Latency compensation: 0 none, 1 the least ping, 2 the average, 3 `latencyAmount`
        /// (ms).
        int latency = 0;
        int latencyAmount = 0;
        /// A latency button clicked (0x83A-0x83D: the announce flag set): the mode announced at the
        /// server's next update even when it's the same (write only).
        bool announceLatency = false;
        /// A WOL game: only there is the General page's Advanced... shown
        /// (the game hides 0x284F in a LAN game). Read only.
        bool wol = false;
        /// noxcpp's: the important objects' updates a second to noxsdl clients (30, 60, 90 or
        /// 120: the clock's substeps, MSG_NOXCPP_SUBSTEP between frames); classic clients and the
        /// game's logic stay at 30.
        int updateRate = 30;
        /// noxcpp's map vote (Options::mapVote): at a match's end, and called (noxsdl's Vote Map, a stock player's "?MapVote").
        bool mapVote = true;
    };
    HostSwitches hostSwitches();
    /// The game's rules as they stand (the settings' slot 0: read from the map's user.rul at its
    /// load, the map load's).
    const RuleSet& currentRules() const noexcept { return rules_; }
    /// A map's rules for a mode as the window shows them on choosing it (flags 7):
    /// its user.rul, else its own .rul; `file` a rule set of its folder instead; in a WOL game
    /// internet.rul after.
    RuleSet rulesFor(const std::string& map, std::uint32_t mode, const std::optional<std::string>& file = std::nullopt);
    /// In a WOL game, internet.rul's [COMMON] rules (the Spells page's flags 4, 0x17F0:
    /// spells it leaves out can't be allowed); otherwise none.
    std::optional<RuleSet> internetRules();
    /// The Rules page's rule sets (rulelist.wnd): those in the map's folder,
    /// one written (Save As / Save: `name`.rul) or deleted.
    std::vector<std::string> ruleSetsOf(const std::string& map);
    bool saveRuleSet(const std::string& map, const std::string& name, const RuleSet& rules, std::uint32_t mode);
    void deleteRuleSet(const std::string& map, const std::string& name);
    /// The teams as the window shows them: the Game page's options (gameplay flags 4 team play, 2
    /// auto-assign, 1 team damage), the teams (id, name, colour) and the players (name, unit's net
    /// code, team id or 0).
    struct TeamsView {
        bool teams = false;
        bool autoAssign = false;
        bool teamDamage = false;
        bool chat = false; ///< a chat game (0x80)
        /// (noxcpp) Those of the three flags a mode pinned (Options::gameplayPinned): shown, not
        /// changeable.
        std::uint32_t pinned = 0;
        struct Team {
            std::uint8_t id = 0;
            std::string name;
            std::uint8_t color = 0;
        };
        std::vector<Team> list;
        struct Player {
            std::string name;
            std::uint32_t netCode = 0;
            std::uint8_t team = 0;
        };
        std::vector<Player> players;
    };
    TeamsView teamsView();
    /// The Server tab's Access page (access.wnd, windowAccessProc; the host's access
    /// settings): closed (only the allowed list joins), private (the password asked
    /// for, 9 letters), the player limit (1-32, Game_SetMaxPlayers; noxcpp: a solo game with
    /// guests' at most its host and Options::soloGuests, the lobby's limit with it), the ping limits (none:
    /// nullopt), limit the resolution (the game list's video flag 0x80), the classes refused
    /// (1 warrior, 2 wizard, 4 conjurer), the sysop password; the allowed names. The lobby's checks
    /// follow (ServerTransport::Access).
    struct AccessSettings {
        bool closed = false;
        bool privateGame = false;
        std::string password;
        int maxPlayers = 16;
        std::optional<int> minPing;
        std::optional<int> maxPing;
        bool limitResolution = false;
        std::uint8_t restrictedClasses = 0;
        std::string sysopPassword;
        std::vector<std::string> allowed;
    };
    AccessSettings accessSettings();
    void setAccessSettings(const AccessSettings& a);
    /// The banned names (the ban list) and whether each is for a while (a vote's kick).
    std::vector<std::pair<std::string, bool>> bannedNames();
    /// The Access page's Add (0x2780): a name onto the allowed list (shown) or the banned list (for
    /// good); Remove (0x2781): that list's entry; Kick / Ban (0x27CF / 0x27D0) the players named (not
    /// the host's own: `issuer`'s net code), as the console's kick and ban.
    void addAccessName(const std::string& name, bool allowedList);
    void removeAccessName(std::size_t index, bool allowedList);
    void kickPlayers(const std::vector<std::string>& names, std::uint32_t issuer, bool ban);
    /// (noxcpp: a NoX Core mode's) The player on `peer` out as kickPlayers puts one out (reason 4),
    /// by its peer (names needn't be unique); never the host's own.
    void kickPeer(std::uint8_t peer);
    /// (noxcpp) Whether the player on `peer` is a stock client held out to be put out
    /// (Options::stockRefusal).
    bool refused(std::uint8_t peer) const { return refused_.contains(peer); }
    /// Whether `extent` is such a player's unit (none held: false at once).
    bool refusedUnit(std::uint32_t extent) const
    {
        for (const auto& [peer, at] : refused_) {
            if (const auto it = players_.find(peer); it != players_.end() && it->second.extent == extent) {
                return true;
            }
        }
        return false;
    }
    /// Whether any player is held out so.
    bool anyRefused() const noexcept { return !refused_.empty(); }
    /// The Game page's team options (ServOpts.c 0x285A-0x285D): Use Teams (on: createFlagTeams, then
    /// the players shared out with auto-assign, else the team flags on; off: the flags off and
    /// every team gone), Auto-Assign (on: the flags off, off: the flags on), Team
    /// Damage (gameplay flag 1), Shuffle (the players shared out anew). (noxcpp: each refused, nothing
    /// done, for a flag a mode pinned: Options::gameplayPinned.)
    void setUseTeams(bool on);
    void setAutoAssign(bool on);
    void setTeamDamage(bool on);
    void shuffleTeams();
    /// players.wnd's Assign (0x2907): those named (not observing nor dead-flagged) into the team,
    /// added or moved, told to everyone and put at its base.
    void assignToTeam(const std::vector<std::string>& names, std::uint8_t team);
    /// Join (0x290B): the host's own player (its net code) into the team; out of a
    /// chat game it goes to a player start.
    void joinTeam(std::uint32_t netCode, std::uint8_t team);
    /// Rename (0x290D's dialog, 0xFA1): Team_Rename, unless another team has the name.
    void renameTeam(std::uint8_t team, const std::string& name);
    /// What changed applied as the window's controls do: the options told the clients
    /// (server_gameSettingsUpdated), a new latency compensation announced.
    void setHostSwitches(const HostSwitches& s);
    /// Go: the name and the mode's limits set, the mode kept for the next map
    /// (World::chosenMode); another map chosen, it loads (mapLoad).
    void applyServerSettings(const ServerSettings& s);

    // The settings' records (g_gameSettingsData: Game_GetSettingsData(slot), 58 bytes):
    //   +0x00 the map's name (9)        +0x2C the weapons allowed (u32)
    //   +0x09 the game's name (15)      +0x30 the armour allowed (u32)
    //   +0x18 the spells allowed (5 u32: bit n spell n)
    //   +0x34 the game flags (u16; the Server Settings window's: its type)
    //   +0x36 the lesson limit (u16)    +0x38 the time limit (u8)    +0x39 "Form a new game"
    // Slot 0 is the game's (refreshed from the game); slot 1 the Server Settings
    // window's, copied from slot 0 at each map's load but a chat map's (server_loadMapFile)
    // and at the window's opening, and what a joining player is told (MSG_UPDATE_GUI_GAME_SETTINGS
    // 0xB1, SrvNet_SendUpdateGuiGameSettings) while the window is shut; while it's open,
    // the window's own state is told, at the join and, as it changes, to every
    // player (the main loop's). Never to the host's own player (GAME.EXE's 0x1F, whose
    // client shares the records in-process: here setHostPeer's).
    using SettingsRecord = std::array<std::uint8_t, 58>;
    const SettingsRecord& settingsRecord(int slot) const { return settingsSlots_[slot & 1]; }
    /// The window as its read puts it into a record: the name field (0x2775, its first 15
    /// letters at + 9), the type button's (+ 0x34), the Advanced pages' spells,
    /// weapons and armour (+ 0x18 / + 0x2C / + 0x30), the
    /// limit fields' numbers when they aren't empty (wcstol: + 0x36 / + 0x38; empty, those bytes are
    /// left as they were), "Form a new game" (0x278A: + 0x39), and the map list's chosen line's
    /// first word (strtok on a tab: + 0; none chosen, + 0 is 0).
    struct SettingsWindow {
        std::string name;
        std::uint16_t type = 0x100;
        RuleSet rules;
        std::optional<int> lessonLimit;
        std::optional<int> timeLimit;
        bool newGame = false;
        std::optional<std::string> mapLine;
    };
    /// The window opened (guiServerOptsLoad, the host's): slot 0 refreshed
    /// and copied to slot 1, the window marked changed.
    void settingsWindowOpened(const SettingsWindow& w);
    /// The window's state as it stands (while it's open; the join and the main loop read it).
    void settingsWindowState(const SettingsWindow& w) { window_ = w; }
    /// The window gone (Gui_ServerOptions_Hide).
    void settingsWindowClosed() { window_.reset(); }
    /// The window's writes into slot 1 (its current slot while it's open), as
    /// guiServerOptionsProcPre makes them, each but `name` marking the window
    /// changed:
    ///   name         0x4003 for 0x2775 (the field left): strcpy at + 9 (a 15-letter name's NUL
    ///                over + 0x18's first byte: an original bug);
    ///   lessonLimit  0x4003 for 0x2796: + 0x36 (below 0, 0);
    ///   timeLimit    0x4003 for 0x2797: + 0x38 (above 255, 255);
    ///   type         0x4010 from 0x2788: + 0x34 (its 0xE80F bits kept), the type's limits (+ 0x36,
    ///                + 0x38: servGamedataGet);
    ///   map          0x4010 from 0x2782: the chosen line's first word at + 0 (strcpy), and `rules`
    ///                (its flags 7 into + 0x18, + 0x2C, + 0x30);
    ///   rules        alone: Advanced's Done (0x27A4) the pages' into + 0x18, + 0x2C, + 0x30 (not
    ///                marked);
    ///   changed      the other places (0x278A, Defaults, the rule set, spell and
    ///                weapon / armour pages);
    ///   fill         first of the window into it (the rule sets' Save As / Save,
    ///                0x27BC / 0x27BD; not marked).
    struct SettingsEdit {
        std::optional<SettingsWindow> fill;
        std::optional<std::string> name;
        std::optional<int> lessonLimit;
        std::optional<int> timeLimit;
        std::optional<std::uint16_t> type;
        std::optional<std::string> map;
        std::optional<RuleSet> rules;
        bool changed = false;
    };
    void editSettings(const SettingsEdit& e);
    /// Go (0x27A1): a read of the window into slot 1, slot 1 copied to slot 0 (before its
    /// notices: applyServerSettings follows on Yes).
    void settingsWindowGo(const SettingsWindow& w);
    /// The host's own client's address (noxsdl's, joined to us over the loopback): GAME.EXE's player
    /// 0x1F, not sent MSG_UPDATE_GUI_GAME_SETTINGS; while it's loading a map (the host's game flags
    /// 0x900000) the main loop doesn't tell the window's changes. None: no such client.
    void setHostPeer(std::optional<net::Endpoint> at) { hostPeerAt_ = at; }
    /// rateUpdate: the net rate (1 at the start; nox.cfg's, the General
    /// page's connection type and slider, all yet to come), a change told to every player but the
    /// host's own (MSG_RATE_CHANGE 0xEC, SrvNet_SendRateChange(0x9F)) while the server is up (game
    /// flag 0x20000: all the time here). A WOL game's players are sent their datagrams only every
    /// `rate` frames (SrvNet_UpdateAllClients).
    void setNetRate(int rate);
    int netRate() const noexcept { return netRate_; }
    /// Chat lines relayed, for logging: (speaker's name, the message's bytes). At most kChatKept
    /// wait for it (a host nobody takes them from, noxsdl's, lets the oldest half go).
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> takeChat() { return std::exchange(chat_, {}); }
    static constexpr std::size_t kChatKept = 1000;

    /// A line for the host's console (server_parseCmdText as the host runs it: the
    /// commands of flag 1, the host's; with Options::testCommands also "nx ..."), `issuer` the
    /// host's own player when it has one (its net code; 0: a dedicated server's console). What
    /// it printed (Gui_Console_Printf), UTF-8.
    std::vector<std::string> command(std::string_view line, std::uint32_t issuer = 0);
    /// What the server's console printed on its own since last taken (a sysop's remote
    /// commands, sysop logins, script functions run by MSG_SERVER_CMD), UTF-8.
    std::vector<std::string> takeConsole() { return std::exchange(console_, {}); }

    // The host leaving its game (Game_BeginQuit / Game_FinishQuit, then the server's end session).
    /// Game_BeginQuit's host part, once (its quit frame kept): with players other than the host's
    /// own, MSG_SERVER_QUIT (0xC7, 1 byte) queued to them all (Net_QueueImportant(0x9F, ..,
    /// onlyJoined 1): those not yet in skipped, those loading held for their ready), and their
    /// count kept as the acks awaited; with none, the quit is done at once.
    void beginQuit();
    /// Whether beginQuit ran.
    bool quitting() const noexcept { return quitStarted_; }
    /// Whether every ack came (Game_OnQuitAckReceived counted the awaited ones down to 0:
    /// Game_FinishQuit), or none was awaited.
    bool quitDone() const noexcept { return quitStarted_ && quitPendingAcks_ <= 0; }
    /// The server's end session (servEndSession's Net_CloseStream of the host's stream):
    /// every client's stream closed, each flushed, sent SERVER_CLOSE (0x0B) and flushed again. The
    /// host's own client's too (none in the exe: its client is in-process). Not in a solo game.
    void endSession();

private:
    /// MSG_SERVER_CMD (SrvNet_OnServerCmd): 0 go observing, 1 run a script function
    /// (a sysop's), 2 a console command (a sysop's), 3 print to everyone, 4 watch a player (an
    /// observer), 5 broadcast.
    void onServerCmd(std::uint8_t peer, std::uint8_t cmd, const std::u16string& text);
    /// MSG_SYSOP_PW (SrvNet_OnPacket 0xBC): the password (any case), MSG_SYSOP_RESULT 0xBD.
    void onSysopPassword(std::uint8_t peer, const std::u16string& password);
    /// The console's commands (host_console.cpp), `issuer` the player running it (0: none).
    void runConsole(std::string_view line, std::uint32_t issuer);
#ifndef NOX_NO_TEST_COMMANDS
    void testCommand(const std::vector<std::string>& words, std::uint32_t issuer);
#endif
    void print(std::string_view label, std::initializer_list<std::string_view> values = {});
    /// The string table's line `id` (Str_GetString), its %s / %S / %d filled in order, UTF-8.
    std::string stringLine(const std::string& id, std::initializer_list<std::string_view> values);
    void printText(std::string text);
    HostedPlayer* playerNamed(std::string_view name);
    /// Plr_DisconnectByIndex: its stream closed, it leaves.
    void disconnect(std::uint8_t peer, std::uint8_t reason);
    /// MSG_GAME_SETTINGS and _2 to everyone (SrvNet_BroadcastGameSettings, the main
    /// loop's when server_gameSettingsUpdated is set).
    void broadcastSettings();
    /// MSG_GAME_SETTINGS (and, `withSecond`, its second) to every player in the game: one message to
    /// all, or with Options::clientGameFlags each player its own flags.
    void sendSettings(bool withSecond);
    net::msg::GameSettings2 gameSettings2() const;
    std::set<std::uint8_t> sysops_;       ///< (g_srvSysopList) by peer
    std::vector<std::string> console_;
    std::uint32_t echoTo_ = 0;            ///< a sysop's unit, for test-command echoes
    bool settingsChanged_ = false;        ///< (server_gameSettingsUpdated)
    bool mapCycleOn_ = true;              ///< ("set cycle")
    int latency_ = 0;                     ///< (the latency compensation mode)
    int latencyAmount_ = 0;
    /// SrvNet_BroadcastNextPlayerLatency (updateServer's, every 500 ms: Server's update-server hook):
    /// the next player round the list (by slot, from the last reported) that is the host's own or
    /// has a latency over 0, its MSG_REPORT_ALL_LATENCY {net code, latency} to every player
    /// (Net_QueueMsg: the frame's datagram).
    void reportNextLatency();
    std::optional<std::uint32_t> latencyReportedAt_; ///< (the last report's time, ms)
    std::optional<std::uint8_t> latencyCursor_;      ///< (the last reported player's slot)
    AccessSettings access_;               ///< (the host's access settings, the allowed list)
    RuleSet rules_;                       ///< (the settings' slot 0: spells, weapons, armour)
    /// The map's rules read and put into the world (spells off, weapons and armour types off):
    /// at each map's load.
    void loadRules();
    void applyRules();
    /// The lobby's checks and the game list's view of the game anew (ServerTransport::Access,
    /// server_makeServerInfoPacket's info).
    void syncAccess();
    void refreshServerInfo();
    bool aiDebug_ = false;                ///< ("show ai": engine flag 8)

    void onPayload(std::uint8_t peer, std::span<const std::uint8_t> bytes);
    void onClientReady(std::uint8_t peer);                                  ///< CLIENT_READY (SrvNet_OnClientReady)
    void onIncomingClient(std::uint8_t peer);                               ///< INCOMING_CLIENT (SrvNet_OnIncomingClient)
    void onTextMessage(std::uint8_t peer, std::span<const std::uint8_t> m); ///< TEXT_MESSAGE (SrvNet_OnPacket 0xA8)
    void join(std::uint8_t peer, std::span<const std::uint8_t> accept);
    /// A solo game's host player made (Options::hostPlayerFile): the rest of the exe's start with
    /// it in (the saved game's restore, the solo host's join's SAVE_SERVER and Plr_LoadClientPlayerData).
    void soloHostIn(std::uint32_t extent);
    bool awaitingHost_ = false;                 ///< (a solo game: no frame till the host's in)
    std::optional<std::int32_t> hostScriptId_;  ///< (its unit's: taken ahead of the first load)
    void leave(std::uint8_t peer);
    /// A frame's host work at its tick end (Server::setClientsHook: after the deletions' sweep,
    /// before the new things' pass): the votes, the transfers and the clients' update (afterFrame).
    void frameClients();
    void afterFrame();
    /// The settings told when the game's mode changed since they last were (afterFrame's first).
    void tellModeChange();
    /// noxcpp's: MSG_NOXCPP_SUBSTEP to the identified clients after a substep between frames.
    void sendSubstep();
    /// A finished match's next map (the server frame's match-over step), or an exit's (exitTo): the
    /// map cycle's next for the
    /// mode, or this one again; the server switches (Server::switchMap) and every player is sent
    /// GAME_SETTINGS, MSG_USE_MAP and its own object again (SrvNet_SendUseMap), what it knew
    /// forgotten, not updated till its CLIENT_READY.
    void nextMap(std::optional<std::string> exitTo = std::nullopt);
    /// The load nextMap asked for, in the server's next frame (its load tick: Server's map-load hook).
    void loadNextMap();
    std::optional<std::string> pendingExit_;
    // noxcpp's map vote (map_vote.hpp; not in the stock game).
    MapVote vote_;
    std::minstd_rand voteRng_; ///< (its own: the game's random numbers stay the game's)
    std::optional<std::string> voteNext_;  ///< the map a vote chose, for loadNextMap
    bool votedThisMatch_ = false;          ///< (a match's end opens one vote)
    bool voteAvailableSent_ = false;       ///< voteAvailable() as the noxsdl players were last told
    std::uint32_t callAgainAt_ = 0;        ///< the frame the next vote can be called at (5 minutes after the last)
    /// Each step: a match's end opens a vote; a vote closes at its time, a called one as soon as
    /// it has its majority.
    void voteFrame();
    /// The vote opened (none with fewer than two maps); a called one by caller (a peer).
    bool openVote(MapVote::Kind kind, std::uint8_t caller);
    void closeVote(std::optional<int> winner);
    /// A player's vote (a peer's), its tally to the noxsdl players.
    void castVote(std::uint8_t peer, int choice);
    /// A player asks for a vote mid-game (noxsdl's Vote Map: MSG_NOXCPP_MAPVOTE kCall; a stock player's chat "?MapVote"): opened
    /// unless one is open, the match is over, it's a quest or chat game, or the last one called
    /// was less than 5 minutes ago.
    void callVote(std::uint8_t peer);
    /// Whether a vote can be called here: the vote on, a network game, not a quest (a chat map's
    /// among the other chat maps).
    bool voteAvailable();
    /// Whether a match's end vote holds the match over: connects taken and the clients updated
    /// meanwhile (the stock end of a match has neither).
    bool endVoteHolds();
    /// MSG_NOXCPP_MAPVOTE kAvailable to a noxsdl player: at its join, and on a change (voteFrame).
    void sendVoteAvailable(std::uint8_t peer);
    /// Whether the vote's window goes to it now: a noxsdl player in the game.
    bool inVoteWindow(std::uint8_t peer) const;
    /// The vote's state to a noxsdl player (open with its time left, then its tally).
    void sendVoteTo(std::uint8_t peer);
    void sendTallies();
    /// The server's line in chat to everyone (MSG_TEXT_MESSAGE from no one): held for one still
    /// loading, none for one not in the game yet.
    void announce(const std::u16string& text);
    /// The server's maps for the game's mode (mapChoices'), and the map cycle's next (peeked).
    std::vector<std::string> mapsForMode();
    std::optional<std::string> cycleNextPeek();
    /// The transport's events since the last (Server's packet hook).
    void takePackets();
    net::msg::GameSettings gameSettings(std::uint32_t frame) const;
    /// MSG_VOTE (0xEE) from a player (SrvNet_OnVoteCast / SrvNet_OnVoteWithdraw): subs 0 / 1 vote
    /// to kick the player named (UTF-16, 25), 2 / 3 withdraw it (both the kind-0 vote, as the
    /// exe); in a quest 0 / 2 are the quest's kick (kind 3: both in the quest, needing 6), and 4 /
    /// 5 cast / withdraw the quest's restart (kind 2, no name).
    void onVote(std::uint8_t peer, std::span<const std::uint8_t> m);
    /// voteUptate (each frame): a vote whose target's gone is dropped; one that
    /// passes (its votes reach the number needed, or two or more from all but one of
    /// the players, of the target's team in team play) kicks it (Plr_DisconnectByIndex, reason 4)
    /// and bans its name 15 minutes. MSG_VOTE sub 6 {state} to everyone when the
    /// first vote opens and the last closes. A quest's restart once everyone in the
    /// quest has voted: each respawned, stage 0, the next quest map; a quest's kick:
    /// 6, or all in the quest but one (two or more), its save and out, banned.
    void updateVotes();
    /// a vote over (a restart's voters told, MSG_VOTE 7).
    void endVote(std::size_t index);
    struct Vote {
        int kind = 0;
        std::uint32_t target = 0;   ///< its unit
        std::uint8_t votes = 0;
        std::uint32_t voters = 0;   ///< by slot
        std::uint8_t needed = 0;
        bool team = false;          ///< (team play: its team's)
    };
    std::vector<Vote> votes_;     ///< newest first
    std::map<std::u16string, std::uint32_t> bans_; ///< name, until (ms)
    std::uint32_t nowMs_ = 0;
    /// (noxcpp: Options::stockRefusal) The stock players held out, by peer: when each joined, and
    /// when it's put out (ms; none till its CLIENT_READY, which starts its hold). Looked at only
    /// while not empty.
    struct HeldOut {
        std::uint32_t since = 0;
        std::optional<std::uint32_t> until;
    };
    std::map<std::uint8_t, HeldOut> refused_;
    /// A stock player turned away at its join: held out of play, told why.
    void refuse(std::uint8_t peer, ServerObject& unit);
    /// Those held out whose time is up, put out.
    void putOutRefused();
    /// The held-out player on `peer` watching, unseen and out of reach still (after its ready, a
    /// map's switch).
    void keepHeldOut(std::uint8_t peer);
    /// The frame's update for one player (SrvNet_UpdatePlayer): its timestamp, then
    /// MSG_UPDATE_STREAM with it as the header and the objects in view it's owed (changed, or
    /// new to it, and not behind a wall), then the out-of-sight notices.
    void updatePlayer(HostedPlayer& p);
    /// The frame's sounds a player in the game hears (netUpdateRemotePlr), to it (afterFrame's).
    void sendHeardSounds(std::uint8_t peer, const HostedPlayer& p, ServerObject& unit);
    /// A player's own reports, each when it changed since last sent (afterFrame's, after its update).
    void sendReports(std::uint8_t peer, ServerObject& unit);
    /// The frame's look-ups for the players' updates (noxcpp's, for speed; afterFrame makes it
    /// once the world has moved, before the updates): the players and the sentry globes. (What's
    /// in a view comes from the world's map grid, as the exe's.) In a quest, each player's keys
    /// (bit 0 a SilverKey held, bit 1 a GoldKey: the gauntlet key reports'), by `players`.
    struct FrameIndex {
        std::vector<ServerObject*> players;
        std::vector<ServerObject*> sentries;
        std::vector<std::uint8_t> keys;
        void build(World& world);
    };
    FrameIndex frameIndex_;
    /// updatePlayer's (and sendHeardSounds') buffers, kept from player to player and frame to frame
    /// to save their allocations (each emptied as a player's update starts).
    struct UpdateScratch {
        std::vector<ServerObject*> candidates;                ///< what the view's rectangle found
        std::vector<UpdateRecord> records;                    ///< the stream's records
        std::vector<std::pair<ServerObject*, bool>> recordOf; ///< whose each is, known before or not
        std::vector<std::uint8_t> notices;
        std::vector<std::uint8_t> exact;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> sounds; ///< (sendHeardSounds')
        std::vector<bool> written; ///< the records the stream took
        std::vector<bool> used;    ///< (the stream writer's own)
    };
    UpdateScratch updateScratch_;
    std::vector<net::ControlEntry> controlEntries_; ///< (a PLAYER_INPUT's, read into it: kept to save its allocations)
    std::vector<net::ServerTransport::Event> transportEvents_; ///< (takePackets' own, kept likewise)
    /// Net_QueueImportant(Seq) to one player: netImportant sends a player its
    /// important messages only once it's in (status 0x10, its CLIENT_READY), so till then they
    /// wait in its `pending`, in order; then reliable at once. `onlyJoined` (the call's argument):
    /// not sent a player that isn't in the game (g_playersInGameMask: its INCOMING_CLIENT) as it
    /// goes.
    void sendImportant(std::uint8_t peer, std::vector<std::uint8_t> bytes, bool onlyJoined = false);
    /// Net_QueueImportant(Seq) to everyone (0xFF), or everyone but a player (0x80 | its index:
    /// `except`): the node waits for each player in the game as it's queued (
    /// g_playersInGameMask), so a player in but not ready gets it at its CLIENT_READY; one not in
    /// yet (no INCOMING_CLIENT) doesn't.
    void sendImportantToAll(std::span<const std::uint8_t> bytes, bool onlyJoined = false, std::optional<std::uint8_t> except = std::nullopt);
    /// A player's CLIENT_READY: what waited for it goes first (the important queue's oldest first).
    void releasePending(HostedPlayer& p);
    /// GAME.EXE's player 0x1F (the host's own: setHostPeer's, or a solo game's).
    bool isHostPlayer(const HostedPlayer& p) const;
    // The settings' records and the Server Settings window (settingsRecord).
    SettingsRecord settingsSlots_[2]{};    ///< (g_gameSettingsData, zeroed)
    std::optional<SettingsWindow> window_; ///< (g_guiServerOptionsWindow: open)
    bool windowChanged_ = false;           ///< (the window changed since last told)
    SettingsRecord guiSent_{};             ///< (what was last told)
    std::uint32_t refreshSettingsAt_ = 30u * 60u; ///< (the main loop's each minute)
    int netRate_ = 1;                      ///< (the net rate: setNetRate)
    std::optional<net::Endpoint> hostPeerAt_;
    bool quitStarted_ = false; ///< (g_quitStartFrame set)
    int quitPendingAcks_ = 0;  ///< (g_quitPendingAcks: every MSG_SERVER_QUIT_ACK counts it down)
    /// Slot 0 refreshed: the game flags (the low 16 when + 0x34's differ from them past the
    /// low 4 bits), the mode's limits, the game's name (strncpy 15 when it differs), the map's
    /// (strcpy when it differs), the spells, the weapons and armour, as the game has them
    /// (`allOn`: a map's load having turned them all on first).
    void refreshSettingsSlot0(bool allOn = false);
    /// server_loadMapFile's part for the settings after a map's load: slot 0 refreshed with
    /// everything on (refreshSettingsSlot0(true)) and copied to slot 1 but for a chat map;
    /// neither under game flag 0x200000.
    void mapLoadedSettings();
    /// MSG_USE_MAP for the map loaded (its file name and CRC) at `frame` (SrvNet_SendUseMap's);
    /// (noxcpp) `guest`: a solo game's guest's, the map's own file's CRC (pristineCrc_).
    std::vector<std::uint8_t> useMapBytes(std::uint32_t frame, bool guest = false) const;
    /// (noxcpp) A solo game's guest (Options::soloGuests): not the host's slot 0x1F.
    bool soloGuest(const HostedPlayer& p) const;
    /// (noxcpp) The map's own file's CRC, the saved copy's when a visited map loads from one aside.
    std::uint32_t pristineCrc_ = 0;
    /// Game_CopySettingsData(src, dst).
    void copySettings(int from, int to) { settingsSlots_[to & 1] = settingsSlots_[from & 1]; }
    /// The window's state read into `a`.
    static void fillFromWindow(SettingsRecord& a, const SettingsWindow& w);
    /// SrvNet_SendUpdateGuiGameSettings: {0xB1, slot, the record} (Net_QueueImportantSeq).
    void sendGuiSettings(std::uint8_t peer, std::uint8_t slot, const SettingsRecord& r);
    /// The main loop's part for the settings (mainloop, the host's): settings changed
    /// (server_gameSettingsUpdated: a map's load too), them to everyone
    /// (SrvNet_BroadcastGameSettings), else in a network game the window's changes;
    /// each minute slot 0 refreshed.
    void settingsLoop();

    const things::ThingDb& things_;
    Options options_;
    std::filesystem::path mapPath_;
    /// What the server info (refreshServerInfo) was last made from: made again only when it changes.
    struct InfoMadeFrom {
        int maxPlayers = 0;
        std::uint32_t gameFlags = 0;
        std::uint8_t flags = 0;
        bool limitResolution = false;
        std::optional<int> minPing;
        std::optional<int> maxPing;
        bool operator==(const InfoMadeFrom&) const = default;
    };
    std::optional<InfoMadeFrom> infoMadeFrom_;
    std::filesystem::path::string_type infoMap_;
    std::string infoName_;
    std::string mapFile_; ///< "Estate.map"
    std::uint32_t mapCrc_ = 0;
    /// The game flags (& 0x7FFF0) as the last frame left them: a change sends the settings.
    static constexpr std::uint32_t kNoSettingsFlags = 0xFFFFFFFFu;
    std::uint32_t settingsFlags_ = kNoSettingsFlags;
    Server server_;
    net::ServerTransport transport_;
    /// The file transfers (MSG_XFER: xfer.hpp), through the transport. A chunk (XFER 2, Net_Send's
    /// flag 0) goes out at once in its own datagram, as the exe's chunk send does (Net_FlushStream(.., 1)).
    Xfers xfers_{[this](std::uint8_t peer, std::vector<std::uint8_t> bytes, bool reliable) {
        if (reliable) {
            transport_.sendReliable(peer, bytes);
        } else {
            transport_.sendUnreliable(peer, bytes);
            transport_.flushUnreliable(peer);
        }
    }};
    /// A file a client sent in whole (the received file's handling, server side).
    void onFile(const Xfers::Received& file);

    // Map downloads (Server\Network\mapsend.c): the map's own maps\<map>\<map>.nxz sent as it is
    // to a client that asked for it (MSG_REQUEST_MAP), 512 bytes a block, a block a frame while
    // the client has at most 2 of them unacknowledged.
    // noxcpp's fast send, to a NoX Core client of lobby::kFastMapSendLevel or above, for its first
    // kFastMapRequestsPerMap requests after each map load (the same messages): blocks of
    // kFastMapBlock bytes, up to kFastMapBlocksPerFrame of them every frame (kFastMapBlocksPerFrameAll
    // to all of them together)
    // while the client's unacknowledged reliable packets (all of them, not only the map's) number
    // fewer than its window (kFastMapStartWindow-kFastMapWindow, halved when late); a fast sender
    // whose player leaves is let go.
    struct MapSender {                   ///< (g_mapSenders[i])
        std::uint16_t state = 0;         ///< 1 sending
        bool privateCopy = false;        ///< its own copy of an older map's file
        std::vector<std::uint8_t> copy;  ///< that copy's bytes
        std::uint32_t size = 0;
        std::uint32_t seq = 1;           ///< the next block's number
        std::uint16_t blockSize = 0x200;
        std::uint32_t sent = 0;          ///< the bytes sent
        std::uint8_t window = 2;         ///< map packets left unacknowledged before it waits
        std::uint32_t waits = 0;         ///< times it waited on the client
        std::uint64_t startMs = 0;       ///< when the send began
        bool fast = false;               ///< (noxcpp's fast send)
        // (The fast send's own:)
        std::size_t fastWindow = kFastMapStartWindow; ///< its window now (kFastMapMinWindow-kFastMapWindow)
        bool lateSeen = false;           ///< a frame's blocks went late (the window grows by one since)
        std::uint32_t ackMs = 0;         ///< the quickest a frame's first packet was acknowledged (0: none yet)
        std::uint32_t grownAt = 0;       ///< when the window last opened (after a late frame: once a round trip)
        struct Flight {
            std::uint32_t at = 0;        ///< when a frame's blocks went
            std::uint8_t seqBegin = 0;   ///< the stream's next sequence number before them
            std::uint8_t seqEnd = 0;     ///< and after them
            std::uint8_t packets = 0;    ///< how many
            bool timed = false;          ///< its first packet's acknowledgement timed (ackMs)
        };
        std::vector<Flight> flights;     ///< frames' blocks not yet acknowledged, oldest first
    };
    std::map<std::uint8_t, MapSender> mapSenders_; ///< by peer
    std::vector<std::uint8_t> mapSendData_;        ///< (g_mapSendData / g_mapSendSize)
    std::string mapSendName_;                      ///< (g_mapSendName)
    int mapSendsInProgress_ = 0;                   ///< (g_mapSendsInProgress)
    std::uint32_t mapSendsStarted_ = 0;            ///< (g_mapSendsStarted: the unacknowledged count's "stream" for sender 30)
    void onRequestMap(std::uint8_t peer);  ///< SrvNet_OnRequestMap
    void onCancelMap(std::uint8_t peer);   ///< SrvNet_OnCancelMap
    void onReceivedMap(std::uint8_t peer); ///< SrvNet_OnReceivedMap
    /// A slot forgotten (SrvNet_OnReceivedMap's, Plr_OnDisconnect's): every object owed to player slot
    /// `slot` afresh, not known to it but the always-sent kinds (0x20400000), what was owed it of
    /// them dropped and those kinds' standing state owed.
    void forgetObjects(std::uint8_t slot);
    /// (each frame, before the transfers) The stock senders on even frames only (`even`); the fast
    /// ones every frame.
    void mapSendFrame(bool even);
    /// A player gone (leave): its fast sender let go and counted down (a stock one goes on: OB-163).
    void mapSendPeerGone(std::uint8_t peer);
    /// A sender reset (the exe's sender reset; a fast one sending counted off mapSendsFast_).
    void mapSendReset(MapSender& s);
    int mapSendsFast_ = 0; ///< fast senders sending (none: odd frames skip the senders)
    std::uint8_t mapSendFastLast_ = 0; ///< the peer the fast sends' last frame served last (round robin)
    std::map<std::uint8_t, std::uint8_t> fastMapRequests_; ///< by peer: fast sends since the map's load
    /// A fast sender's frame: up to `budget` blocks (the frame's share left, taken from it).
    void mapSendFast(std::uint8_t peer, MapSender& s, int& budget);
    void mapSendNextBlock(std::uint8_t peer, MapSender& s); ///< SrvNet_MapSendNextBlock
    void mapSendAbortAll(std::uint8_t reason); ///< (SrvNet_MapSendAbort each)
    bool mapSendLoadMap();                 ///< SrvNet_MapSendLoadMap
    /// A mapsend.c line (Str_GetString(label), its %s / %S / %d filled in order) to the console.
    void mapSendPrint(std::string_view label, std::initializer_list<std::string_view> values = {});
    /// A quest's saved players (quest.hpp's questMapStart, the player update's game over): the
    /// file as sent (Plr_SaveServerPlayerData's sections, decrypted, as far as
    /// computeServerPlayerDataBufferSize counts, its low 16 bits as
    /// SrvNet_SendSaveDataToClient keeps them; nullopt when the save fails),
    /// SrvNet_SendSaveDataToClient (false when one is pending, or nothing to send),
    /// the map start's step for a player.
    /// (`peer`: written as the game that player's client is told it's in: clientFlags.)
    std::optional<std::vector<std::uint8_t>> savedPlayer(Fight& fight, ServerObject& unit, std::optional<std::uint8_t> peer = std::nullopt);
    bool sendSaveData(std::uint8_t peer, const HostedPlayer& p, std::vector<std::uint8_t> file);
    void questSendSave(Fight& fight, ServerObject& unit);
    void questSaveAndKick(Fight& fight, ServerObject& unit);
    /// The drops asked for (Plr_DisconnectByIndex, reason 4), made once the walk that
    /// asked is over: the frame's update, the votes' update, the console's kick.
    void flushDrops();
    /// noxcpp's forced class (World::forceClass): where none's forced, each player its
    /// character's own class back; and each one whose class changed, its MSG_NEW_PLAYER to
    /// everyone again (its own client takes its class from it: its meters, book and bars).
    void syncClasses();
    std::uint32_t savePending_ = 0;   ///< a save on its way to the slot's client
    std::uint32_t kickAfterSave_ = 0; ///< out once its client has its save
    std::vector<std::uint8_t> dropAfterFrame_; ///< (Plr_DisconnectByIndex from the frame's update)
    std::map<std::uint8_t, HostedPlayer> players_;
    std::uint32_t joinSerials_ = 0; ///< (the last HostedPlayer::joinSerial given)
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> chat_;
    std::uint32_t startMs_ = 0;
    bool started_ = false;
    std::uint64_t substepsRun_ = 0;
    std::uint64_t clockRate_ = 0; ///< the substeps a second the count's at (tick)
    MapCycle cycle_;
    std::map<std::string, std::uint32_t> mapModes_; ///< (a map's types, read once)
    std::map<std::string, MapChoice> mapChoices_;   ///< (mapChoices': a folder's map, read once)
    /// A map's types as game flags (its MapInfo, checkMapFile; 0 when it isn't
    /// there), from the maps folder, read once.
    std::uint32_t mapModesOf(const std::string& name);
    /// scanAllMaps then mapSelectFirst: the maps folder's maps
    /// (folders, not the solo maps wiz / war / con + digit, names cut to 8, in strcmp order), the
    /// quest's of them into Server::questMaps.
    void scanMaps();
};

} // namespace nox::server
