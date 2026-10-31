// Delving Mode's director: an endless run of generated quest levels.
//
// The run is the classic quest itself (quest.cpp). The host starts on a chat map with a quest chosen
// (GameHost::Options::quest) and the quest's maps narrowed to the one generated map
// (Options::questMapsOnly), so the quest's every pick is that map; each time the quest goes to
// it (its start, or everyone out by its quest exit), Options::prepareMap lets the director
// write the next level there first. The quest's stage is the depth: its difficulty, lives,
// exits, briefings and scores are the stock game's, so original clients play it as any quest.
//
// The map keeps one name for the whole run (DEL_1): a client holding last level's copy sees the
// CRC differ and downloads the new one over it (gameChangeMap). Generated maps
// are disposable; sweep() deletes them at startup and when the session ends.
//
// The server opens on a generated lobby (DEL_LOB, a chat map): the rules on signs, which speak
// their line to a player coming near (MSG_TEXT_MESSAGE from the sign: the stock client draws
// it over it, as a map script's Chat; a sign's own text is a string table id, so it can't carry
// new words), and the portal, a quest exit (inert outside a quest) set into its north-west wall.
// Someone stepping onto it begins the quest with everyone in the lobby and locks the server: a
// closed game, its allowed list the party (one who drops may come back, nobody new). Everyone
// gone mid-run, the lobby is loaded again (the quest ended as any chat map after a quest ends
// it), the server unlocked, and the next run starts at level 1.
//
// A run is acts of six levels, a branching map of rooms: five rooms in
// the act's biome (a fight first, campfires and shops last, fights, elites, mysteries and a
// shop between) and its guardian. The rooms branch (DelveAct): each of the first four levels
// ends in two gates, each to its own next room, a sign beside each saying which; the gate most
// of the party takes is the way on. The guardian's arena forks the same way: when it dies two
// gates open, each to its own next act; the floor before them shows where.
#pragma once

#include "noxcore/mapgen/arena.hpp"
#include "noxcore/server/delve_map.hpp"
#include "noxcore/server/delve_records.hpp"
#include "noxcore/server/object_key.hpp"
#include "noxcore/server/which_map.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <random>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

namespace nox::things {
struct ThingDb;
}
namespace nox::bot {
class ItemKinds;
}
namespace nox::modes::script {
class ScriptedMode;
}

namespace nox::server {

class GameHost;
class World;
/// The Delve's script and its rules (delve.cpp's).
struct DelveRules;
struct ModeHooks;
class Watchers;
class KeyCarry;
struct WatchEvent;
struct HostedPlayer;
struct ServerObject;

/// The generated map's name: one per server, overwritten each level.
inline constexpr std::string_view kDelveMap = "DEL_1";

/// A generated map's name (DEL_..., any case): disposable, never offered by a map list, a vote,
/// the map cycle or a quest of its own accord.
bool isGeneratedMapName(std::string_view name);
/// The lobby's.
inline constexpr std::string_view kDelveLobby = "DEL_LOB";

/// A room of an act. (A blessing room: an altar offering each player one of three boons for the
/// rest of the run, delvemap::Blessing.)
enum class DelveRoom : std::uint8_t { Fight, Elite, Mystery, Shop, Campfire, Guardian, Blessing };
/// What a mystery room turns out to be, once entered: mostly something small (a stash of
/// potions, a purse of gold, a healing shrine, a mana obelisk, a skirmish with a little loot),
/// now and then something rare (a campfire's spell, or a lone champion with a rare drop).
enum class DelveMystery : std::uint8_t { Stash, Purse, Shrine, Obelisk, Skirmish, Fire, Champion };

/// A fight's bonus objective (from depth 2), a reward for it: slay every monster (Purge), slay
/// the quarry, an elite named as the level starts (Hunt), break the seals, three orb pools to
/// touch (Seals), or reach a gate in time (Swift: gold, as the party's leaving).
enum class DelveObjective : std::uint8_t { None, Purge, Hunt, Seals, Swift };

/// A run's difficulty (chosen in the lobby: "?difficulty <name>"): the same levels, their
/// monsters tougher and more of them, elites sooner, more and of more traits.
enum class DelveDifficulty : std::uint8_t { Adventurer, Veteran, Nightmare, Oblivion };
inline constexpr std::size_t kDelveDifficulties = 4;
/// "Adventurer" ...
std::string_view delveDifficultyName(DelveDifficulty d);

/// As players are told it: "fight", "elite", "?", "shop", "campfire", "guardian".
std::string_view delveRoomName(DelveRoom room);

// (The tables: the creatures by tier, the guardians, the difficulties' numbers and words, the
// lobby's signs, the rooms' and climates' words, the names given, the champion's kit, are the
// Delve's script's: script/server/data.luau, built into the program and read once at set-up,
// src/delve_data.hpp, read at the director's set-up. The functions above answer from that copy.)

/// An act: six levels in one biome, its guardian last, played along one of its ways. Its rooms
/// are rows: row r (its level r + 1) holds r + 1 rooms side by side,
/// its guardian's row one; room i of a row goes on to rooms i (the left gate) and i + 1 (the
/// right) of the next, each of the fifth row's to the guardian.
struct DelveAct {
    static constexpr int kLength = 6;
    int number = 1;
    std::string biome;
    std::array<std::vector<DelveRoom>, kLength> rows;
    /// The run's way through it: the room taken in each row so far (its index in the row).
    std::vector<std::uint8_t> path;
    bool operator==(const DelveAct&) const = default;

    /// Row `row`'s rooms: row + 1 of them, the guardian's one.
    static int width(int row) { return row >= kLength - 1 ? 1 : row + 1; }
    /// The room at `lane` of `row` (lanes past the row's last clamped).
    DelveRoom room(int row, int lane) const;
    /// Its rooms all of one kind (a test's, or a look at one), the guardian's row its own.
    void fill(DelveRoom kind);

    /// An act drawn: row 1 a fight; rows 2-4 fights, mysteries, elites, a shop or so and a
    /// blessing or two (no elite after an elite, two side by side never the same: either gate a
    /// different room); row 5 campfires and shops (a mystery now and then); row 6 the guardian.
    /// Any way through it meets one shop at the most (a design rule), one blessing at the most
    /// (every act one at the least), and one campfire (row 5's: a mystery's own fire makes two at
    /// the most, levelFor). Its biome any but the shop's and those in `notBiomes`.
    static DelveAct roll(int number, std::uint32_t seed, std::span<const std::string> notBiomes = {});
    /// A guardian's fork: two next acts, other biomes than `notBiome` and each other's, their
    /// rooms not the same.
    static std::array<DelveAct, 2> fork(int number, std::uint32_t seed, const std::string& notBiome);
    /// The whole act as a sign says it: "a fight, then ways through 2 elites, 3 mysteries and a
    /// shop, then its guardian".
    std::string describe() const;
};

/// One level of a run: its map's parameters and what it holds.
struct DelveLevel {
    mapgen::ArenaParams params;
    DelveRoom room = DelveRoom::Fight;
    std::optional<DelveMystery> mystery;
    /// An elite's champion or the guardian (params.centre's first), its health this many times.
    std::string boss;
    int bossHealthTimes = 1;
    /// A rare drop the boss carries (a lone champion's): a reward this many stages up (0: none).
    int bossDrop = 0;
    DelveObjective objective = DelveObjective::None;
    /// Swift's time to reach a gate, in seconds.
    int objectiveSeconds = 0;
    /// A guardian played by a bot (from act 2, half the time): its class (0 warrior, 1 wizard,
    /// 2 conjurer). `boss` is then the monster that stands in for it if the bot can't come.
    std::optional<std::uint8_t> botBoss;
};

/// The seals' thing (Seals): broken, it's turned off (VISIBLE_ENABLE: drawn as the empty pool).
inline constexpr std::string_view kDelveSeal = "OrbPoolWithOrb";

class DelveDirector {
    /// (delve_links.hpp: its records' links, carried through a map change.)
    template <class>
    friend struct Links;

public:
    struct Options {
        std::filesystem::path mapsDir;
        std::string mapName{kDelveMap};
        std::string lobbyName{kDelveLobby};
        /// The run's seed; each level's is drawn from it and the depth.
        std::uint32_t seed = 1;
        /// Every act in this biome (tests, or a host trying one out); "": drawn.
        std::string biome;
        /// Every level this room (tests); none: the act's.
        std::optional<DelveRoom> room;
        /// Every level this bonus objective (tests); none: the level's own.
        std::optional<DelveObjective> objective;
        /// Every guardian a bot boss of this class (tests); none: the level's own.
        std::optional<std::uint8_t> botBoss;
        /// The difficulty a server starts at (the lobby's "?difficulty" changes it).
        DelveDifficulty difficulty = DelveDifficulty::Adventurer;
        /// Where each run's record goes (the process's store, shared by its servers); none: no
        /// records kept.
        DelveRecords* records = nullptr;
        /// (Tests) A script file in place of the built-in one, by its path ("server/level.luau");
        /// nothing: the built-in's. The director's own script only (a lent one is the built-in).
        std::function<std::optional<std::string>(std::string_view path)> scriptOverride;
    };

    enum class State : std::uint8_t {
        Lobby,    ///< in the lobby, open to anyone
        Starting, ///< the portal taken: the quest's countdown, locked
        Running,  ///< in a level, locked to the party
    };

    DelveDirector(const things::ThingDb& things, Options options);
    ~DelveDirector();
    DelveDirector(const DelveDirector&) = delete;
    DelveDirector& operator=(const DelveDirector&) = delete;

    /// The host gone (the mode's detach): the script's own let go of it.
    void detach(GameHost& host);
    /// A map change's carry of its records' links (GameHost::Options::carryLinks: delve_links.hpp's
    /// rules), right after the switch.
    void carryLinks(const KeyCarry& carry);

    /// The UDP port the server listens on (its loop's, once the socket's open): a bot boss joins
    /// there from this process. None (0): a guardian is always a monster.
    void setServerPort(std::uint16_t port) noexcept { serverPort_ = port; }
    /// The bot boss's name while one is about (tests), else "".
    std::string botBossName() const;
    /// The runs' difficulty; set in the lobby only (false in a run: it holds till the lobby).
    DelveDifficulty difficulty() const noexcept { return difficulty_; }
    bool setDifficulty(DelveDifficulty d);

    /// Deletes every generated map under `mapsDir` (folders named DEL_*). Returns how many.
    /// (One server alone on its maps folder; a pool's supervisor at its start.)
    static int sweep(const std::filesystem::path& mapsDir);
    /// Deletes this director's own maps alone (its level and lobby): a server of a pool, whose
    /// others' maps share the folder.
    int sweepOwn() const;

    /// A fight at `depth` (1 is the first): a longer level and more, tougher monsters as it
    /// deepens, drawn from the stock creatures, in a biome at random.
    static mapgen::ArenaParams levelFor(int depth, std::uint32_t seed);
    /// The level at `depth` in `act`: row (depth - 1) % 6's room on the act's path (its last
    /// lane), two gates to the next row's rooms (rows 1-4), a guardian's gates to `fork`.
    static DelveLevel levelFor(const DelveAct& act, int depth, std::uint32_t seed, const std::array<DelveAct, 2>* fork = nullptr);
    /// `level`'s bonus objective made `objective` (Seals' seals placed, Swift's time set).
    static void setObjective(DelveLevel& level, DelveObjective objective);
    /// A guardian's level made a bot boss's of `playerClass` (a duel: no monsters, its spot free).
    static void setBotBoss(DelveLevel& level, std::uint8_t playerClass);
    /// The lobby: a chat map, the rules' signs (one per line of rules()), the portal.
    static mapgen::ArenaParams lobbyParams();
    /// What the lobby's signs say, west to east (each one speech line).
    static std::span<const std::string_view> rules();
    /// Writes the lobby (maps\<lobbyName>); returns its .map, the map to host first.
    std::filesystem::path writeLobby();

    /// The server's listed name at `depth`: "DELVE 3", "DELVE lobby" at 0 (open to join).
    static std::string listedName(int depth);

    /// Each frame, from the loop that runs the host (after its tick): the lobby's signs and
    /// portal, the lock, the reset when everyone's gone, the server renamed for the map loaded
    /// (listedName). True when the lock changed (locked()).
    bool tick(GameHost& host);
    State state() const noexcept { return state_; }
    bool locked() const noexcept { return state_ != State::Lobby; }

    /// GameHost::Options::prepareMap: when `next` is the delve's map, the next level written
    /// there, its depth the quest's stage + 1 (the stage the load takes it to; 1 when no quest
    /// is running). False when `next` is another map.
    bool prepare(const std::filesystem::path& next, const World& world);

    /// The level last written (0: none yet), and the deepest this session.
    int depth() const noexcept { return depth_; }
    int deepest() const noexcept { return deepest_; }
    const Options& options() const noexcept { return options_; }
    /// The run's seed (a fresh one each run after the first).
    std::uint32_t runSeed() const noexcept { return runSeed_; }
    /// The run's act and the level last written; the fork its guardian opens (a guardian's level).
    const DelveAct& act() const noexcept { return act_; }
    const DelveLevel& level() const noexcept { return level_; }
    const std::optional<std::array<DelveAct, 2>>& fork() const noexcept { return fork_; }
    /// Which of the fork's acts the last fork took (0 left, 1 right; -1 none yet).
    int lastChoice() const noexcept { return lastChoice_; }
    /// The level's gates once open (their extents, left then right).
    const std::vector<ObjectRef>& gates() const noexcept { return gates_; }

    /// GameHost::Options::onChat: a stock player's answer at a campfire (the number of a spell
    /// offered it) strengthens that spell, at an altar (1-3) takes that blessing; "?blessings"
    /// tells it its own. True when it was one of those (taken, not relayed).
    bool onChat(GameHost& host, std::uint8_t peer, std::u16string_view text);
    /// GameHost::Options::onDelveMessage: a noxsdl player's pick at a campfire (kUpgrade) or an
    /// altar (kBless).
    void onMessage(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m);
    /// A campfire's offer to a player (its extent): the spells it may strengthen, in the order
    /// numbered (empty: none open).
    std::vector<delvemap::Upgradable> offer(std::uint32_t player) const;
    /// An altar's offer to a player (its extent): three blessings (empty: none open).
    std::vector<delvemap::BlessingOffer> blessingOffer(std::uint32_t player) const;
    /// A player's blessings this run (by its name), how many of each.
    std::array<std::uint8_t, delvemap::kBlessings> blessings(const std::string& name) const;

    /// Levels built ahead on worker threads, by (depth, the gate taken into it): the possible next
    /// levels of the one being played, and the first of the next run while in the lobby. (Tests.)
    std::size_t aheadCount() const noexcept { return ahead_.size(); }
    /// How many levels prepare() found built ahead, and how many it had to build itself.
    int builtAheadUsed() const noexcept { return aheadUsed_; }
    /// The level's elites still about (tests).
    std::size_t eliteCount() const noexcept { return elites_.size(); }
    /// The level's bonus objective as it stands (kind 0: none).
    delvemap::Objective objective() const;
    /// The quarry (Hunt's) and the seals still whole (Seals'), by extent (tests).
    std::uint32_t quarry() const noexcept { return objective_.quarry; }
    const std::vector<ObjectRef>& seals() const noexcept { return objective_.targets; }
    int builtOnTheSpot() const noexcept { return builtOnTheSpot_; }
    /// The levels still being built ahead, let go: the director's end no longer waits for their
    /// threads; the call returned does. Called with `wait` false it only lets go of those finished
    /// and answers whether any still build; with true it waits for them all. The thing database
    /// must outlive the builds (they read it). For a host torn down on a thread that mustn't stop
    /// (noxsdl's leaving a game).
    std::function<bool(bool wait)> releaseBuilds();

private:
    /// The run as a level makes it: its act (the way through it), the fork its guardian opens,
    /// the last fork's choice, the act left behind (a new act's), a new run.
    struct Plan {
        int depth = 1;
        DelveAct act;
        std::optional<std::array<DelveAct, 2>> fork;
        int lastChoice = -1;
        std::optional<std::pair<DelveAct, int>> finished;
        bool fresh = false;
        std::optional<DelveObjective> objective; ///< Options::objective
        std::optional<std::uint8_t> botBoss;     ///< Options::botBoss
        DelveDifficulty difficulty = DelveDifficulty::Adventurer;
    };
    /// The run at `depth`, the party having taken gate `choice` out of the level before (a fork's
    /// act, or the next row's room; -1: none, -2: the same level again). Reads the run's state,
    /// changes nothing.
    Plan plan(int depth, int choice) const;
    /// listedName(depth), kept for the depth it was last made for (tick names the server each time).
    const std::string& listed(int depth);
    std::optional<std::pair<int, std::string>> listed_;
    /// The gate prepare() takes the party to have come through into `depth` (plan()'s choice).
    int choiceInto(int depth, const World& world) const;
    /// A level built: what it holds, where its things went, its files' bytes. Pure: safe on a
    /// worker thread (the thing database is only read).
    struct Built {
        DelveLevel level;
        mapgen::ArenaSpots spots;
        mapgen::EncodedMap files;
    };
    static Built build(const things::ThingDb& things, std::uint32_t seed, const Plan& plan);
    /// The possible next levels of the one just made (or a run's first, from the lobby), started
    /// on worker threads; those no longer wanted let finish and dropped (dropAhead).
    void buildAhead(int depth);
    void dropAhead();

    void lock(GameHost& host);
    void unlock(GameHost& host);
    /// A level's frame in a run: the level told, its champion made strong, the gates opened when
    /// the guardian dies, the gates' signs, who took which gate, the fire's and shrine's heals.
    void runLevel(GameHost& host);
    void beginLevel(GameHost& host, World& world, std::uint32_t frame); ///< (runLevel's first frame of a level)
    /// runLevel's parts, in its order; `party` the players in the level (a bot boss aside).
    using Party = std::vector<std::pair<const HostedPlayer*, ServerObject*>>;
    void tellLevel(const Party& party);
    void findBoss(World& world);
    void runGates(World& world, std::uint32_t frame);
    void clearBars();
    void runAir(GameHost& host, std::uint32_t frame);
    void runShrine();
    /// The gate the party took: the most of those through one, a tie the first taken.
    int choose(const World& world) const;
    /// The run as the script's words read it (told.luau's run table), pushed.
    void pushRun(lua_State* L) const;

    const things::ThingDb& things_;
    Options options_;
    /// The run's seed: every level, act and fork drawn from it. The first run's the server's
    /// (Options::seed); each run after a fresh one (tick: back in the lobby), so no two runs alike.
    std::uint32_t runSeed_ = 0;
    std::uint32_t runs_ = 0;
    /// The run's party as it began (names, classes) and the deepest it went: its record, filed
    /// as it ends (fileRecord). Whether a new deepest delve was told this run.
    std::vector<std::pair<std::string, std::uint8_t>> runParty_;
    int runDepth_ = 0;
    bool toldBest_ = false;
    /// The noxsdl players' records table (MSG_NOXCPP_MODE_F4 kRecords): the store's version each has.
    std::map<std::uint8_t, std::uint64_t> recordsSent_;
    void fileRecord();
    /// A fallen party's run over: each one's things and gold gone, its spells a quest's start's
    /// (its class's first), and the server back to the lobby.
    void wipeParty(GameHost& host);
    int depth_ = 0;
    int deepest_ = 0;
    State state_ = State::Lobby;
    /// The Access settings before the lock (put back at the unlock).
    bool savedClosed_ = false;
    std::vector<std::string> savedAllowed_;
    /// The frame's loops as watchers (watchers.hpp): the lobby's group (its greeting, signs, board,
    /// portal), made at its first tick, its zones on the lobby's things as it's loaded
    /// (watchLobby). Who's by the board and who's been greeted are the watchers' (the board's
    /// zone, the group's arrivals); what's done as each fires, the script's hooks (lobby.luau:
    /// the greeting, the signs' lines, the records told and their wait, the run begun).
    std::unique_ptr<Watchers> watch_;
    int lobbyGroup_ = -1;
    std::vector<std::uint32_t> signZones_;
    std::vector<ObjectRef> lobbySigns_; ///< each sign zone's sign (0: none)
    std::uint32_t boardZone_ = 0;
    std::uint32_t portalZone_ = 0;
    bool lobbyTargeted_ = false;
    GameHost* lobbyHost_ = nullptr;  ///< (the pass's, for its handlers)
    std::uint32_t lobbyFrame_ = 0;
    void watchLobby(GameHost& host);
    /// The script's owner functions (owner.records, owner.begin, owner.startSoloQuest).
    void addOwnerFunctions(modes::script::ScriptedMode& s);
    /// The server's mode hooks while the director runs it: a map's load told (the lobby's things
    /// found again: lobbyTargeted_), nothing else asked or told of the game's own.
    struct Hooks;
    std::unique_ptr<Hooks> hooks_;
    GameHost* hooksHost_ = nullptr;
    /// The doors' insides (tick, prepare, onChat, onMessage: a built-in rule's failure stops at
    /// the doors).
    bool tickRun(GameHost& host);
    bool prepareRun(const std::filesystem::path& next, const World& world);
    bool onChatRun(GameHost& host, std::uint8_t peer, std::u16string_view text);
    void onMessageRun(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m);
    /// A level's groups (made once; their zones made again each level): its gates' signs (the
    /// party's), the shrine (the party's). The level's host, for their handlers.
    int partyRoster_ = -1;
    int everyoneRoster_ = -1;
    int gateSignGroup_ = -1;
    int shrineGroup_ = -1;
    bool shrineWatched_ = false;
    /// The campfire's and the altar's (every ready player), the seals' (each seal's zone: who of the
    /// party in play was last within, this pass).
    int campfireGroup_ = -1;
    int altarGroup_ = -1;
    int sealGroup_ = -1;
    bool campfireWatched_ = false;
    bool altarWatched_ = false;
    std::map<std::uint32_t, const ServerObject*> sealBy_;
    /// The lists: barrels till broken (those broken this pass), elites till gone or dead, the
    /// guardian till it falls (the one watched); the boss's finder.
    int barrelList_ = -1;
    int eliteList_ = -1;
    int guardianList_ = -1;
    int bossFinder_ = -1;
    std::vector<WatchEvent> broken_;
    bool guardianFell_ = false;
    ObjectRef guardianWatched_;
    bool bossSought_ = false;
    void bossFound(ServerObject& o);
    /// The blessed (kept by name: blessingKept), the air (every one of the party: its heat's bites
    /// this pass), the spell bars cleared once a run (clearBar), the gates taken (the gates watched
    /// so far).
    int blessGroup_ = -1;
    int airGroup_ = -1;
    int barsGroup_ = -1;
    int exitGroup_ = -1;
    bool airKept_ = false;
    std::size_t exitGates_ = ~std::size_t{0};
    std::vector<WatchEvent> heat_;
    void keepBlessings(const std::string& name);
    void blessingKept(const WatchEvent& e);
    void clearBar(const WatchEvent& e);
    void atCampfire(const WatchEvent& e);
    void atAltar(const WatchEvent& e);
    /// The level's conditions over the party (levelGroup_, rearmed as a level begins): the
    /// objective started with the first in (anyone), the level told two seconds in (after), the
    /// whole party fallen (all at game over: told, then a moment on wiped: wiped_, the pass over).
    /// The timers: the elites' regenerating beat, Purge's count, Swift's deadline (due).
    int levelGroup_ = -1;
    int eliteTimer_ = -1;
    int purgeTimer_ = -1;
    int swiftTimer_ = -1;
    bool wiped_ = false;
    const Party* levelParty_ = nullptr; ///< (the pass's, for its handlers)
    void partyFallen(const WatchEvent& e);
    void regenerateElites();
    /// The objective's pass: the party in play (ready, not observing, alive), whether it's done.
    std::vector<ServerObject*> inPlay_;
    bool objectiveDone_ = false;
    bool objectiveAlive(World& world, std::uint32_t extent) const;
    void objectiveProgress(int done);
    void countPurge();
    GameHost* levelHost_ = nullptr;
    void watchLevel();

    // The run.
    DelveAct act_;
    std::optional<std::array<DelveAct, 2>> fork_;
    int lastChoice_ = -1;
    DelveLevel level_;
    mapgen::ArenaSpots spots_;
    /// Each level written counts one; the level's frame state below is for levelSeen_.
    std::uint32_t levelSerial_ = 0;
    std::uint32_t levelSeen_ = 0;
    std::uint32_t levelFrame_ = 0;
    bool told_ = false;
    ObjectRef boss_; ///< the champion's or guardian's extent, once found
    bool bossDown_ = false;
    bool gatesOpen_ = false; ///< the level's gates made (a guardian's once it dies)
    std::vector<ObjectRef> gates_;
    std::vector<ObjectRef> gateSigns_;
    int firstGate_ = -1;
    RefSet healed_;                       ///< players the shrine healed, or the fire strengthened
    /// The campfire's open offers, by player: the spells, numbered in this order.
    RefMap<std::vector<delvemap::Upgradable>> offers_;
    /// A spell strengthened: sparks rain about where it happened for a couple of seconds, a
    /// LevelUp glow (`glow`, made then) over the player.
    struct Celebration {
        float x = 0, y = 0;
        std::uint32_t from = 0;
        ObjectRef glow;
    };
    std::vector<Celebration> celebrations_;
    /// The level's breakable barrels and crates, what each holds (rolled as the level starts) and
    /// where it last stood: broken, it gives it there (runBarrels).
    struct Barrel {
        ObjectRef extent;
        std::string type; ///< (its extent another's once it's gone and freed)
        float x = 0, y = 0;
        enum class Holds : std::uint8_t { Nothing, Loot, Monster, Gem } holds = Holds::Nothing;
    };
    RefMap<Barrel> barrels_; ///< (by extent; watched in order: barrelList_)
    void runBarrels(GameHost& host);
    /// An elite: a creature made stronger by traits (the stock enchantments, so every client
    /// sees them, and two of the director's own). Swift (Hasted), Vampiric (Vampirism),
    /// Reflective (ReflectiveShield), Tough (half again its health), Regenerating (a little
    /// health back every second: runElites).
    enum class Trait : std::uint8_t { Swift, Vampiric, Reflective, Tough, Regenerating };
    struct Elite {
        ObjectRef extent;
        std::vector<Trait> traits;
    };
    std::vector<Elite> elites_;
    std::vector<std::string> eliteLines_; ///< what's told of them with the level's line
    /// `unit` made an elite of these traits (the script's draw: draws.luau), told to everyone ("a
    /// swift, tough Wolf!"). None: not one.
    void makeElite(ServerObject& unit, std::span<const int> traits);
    void runElites(GameHost& host);
    /// The campfire: offers made to those near it (a noxsdl player's kCampfire, a stock player's
    /// numbered lines) and taken back from those who walked off; the celebrations' sparks.
    void runCampfire(GameHost& host);
    /// `spell` (one offered) a level up, its group with it; told and celebrated.
    bool upgrade(GameHost& host, std::uint8_t peer, ServerObject& unit, std::uint8_t spell);
    /// The blessings: what each player holds this run (by its name: one who drops and comes back
    /// keeps them). (What of it was applied to its unit, raised again as a respawn's or a new
    /// map's stats put back the class's numbers: the script's, level.blessingKept.)
    struct Blessed {
        std::array<std::uint8_t, delvemap::kBlessings> held{};
    };
    std::map<std::string, Blessed> blessed_;
    /// The altar's open offers, by player.
    RefMap<std::vector<delvemap::BlessingOffer>> blessOffers_;
    /// The altar's offers (a noxsdl player's kBlessing, a stock player's lines), and each player's
    /// blessings kept on it (runBlessings).
    void runAltar(GameHost& host);
    void runBlessings(GameHost& host);
    bool bless(GameHost& host, std::uint8_t peer, ServerObject& unit, delvemap::Blessing b);
    /// The run's blessings gone: each player's raised numbers put back, its enchantments off.
    void unbless(GameHost& host);
    void celebrate(World& world, const ServerObject& unit, std::uint32_t frame);
    /// The level's bonus objective (DelveLevel::objective), set up as it starts: what's to do
    /// (Purge's foes, Seals' seals still whole, Hunt's quarry), how far it's gone, its end;
    /// told to noxsdl players as it changes (kObjective: the version each has).
    struct ObjectiveRun {
        std::vector<ObjectRef> targets;
        ObjectRef quarry;
        int done = 0;
        int of = 0;
        std::uint8_t state = 0; ///< 0 open, 1 done, 2 failed
        std::uint32_t deadline = 0; ///< Swift's, in frames
        std::uint32_t version = 0;
        bool started = false;
        bool halfTold = false;
        std::string quarryName; ///< "Wolf"
    };
    ObjectiveRun objective_;
    std::map<std::uint8_t, std::uint32_t> objectiveSent_;
    /// A guardian played by a bot (DelveLevel::botBoss): a bot::NetBot of this process joining
    /// the server over loopback (its own sockets), let into the locked game, out of observing
    /// and on a team of its own (the party's foe; never quest-active: no lives, no part in the
    /// quest's exits, tallies or difficulty), set in the arena with gear, spells and health; gone
    /// once it falls (the gates open as a guardian's) or the level's left. Not come in time: the
    /// monster guardian instead. The bot isn't one of the party anywhere (isBotBoss).
    struct BotBoss;
    std::unique_ptr<BotBoss> botBoss_;
    std::unique_ptr<bot::ItemKinds> botKinds_;
    std::uint16_t serverPort_ = 0;
    std::uint32_t botBossSerial_ = 0; ///< the level its bot was made for
    void runBotBoss(GameHost& host);
    /// Its sockets and client, each director tick (finishing a quit once let go).
    void pumpBotBoss();
    void endBotBoss(GameHost& host);
    bool isBotBoss(const std::string& name) const;
    DelveDifficulty difficulty_ = DelveDifficulty::Adventurer;
    /// Creatures whose health the director raised (the difficulty's, Tough, a champion's or
    /// guardian's), by extent, and to what: the quest's rescale (questRescale, as the party's size
    /// changes) puts an unhurt one back to its stock health, so it's raised again.
    RefMap<std::uint16_t> keptHealth_;
    void keepHealth(const ServerObject& o);
    /// The Delve's script (script/server/main.luau, built in): the run's rules are its functions,
    /// each looked up once, the director calling them. Attached to the host at the first tick (its
    /// float maths the host's rounding). (A level or act drawn off the director, on any thread,
    /// borrows one: DelveRules::Lease.)
    std::unique_ptr<DelveRules> rules_;
    GameHost* scriptHost_ = nullptr;
    std::uint32_t lastFrame_ = ~0u; ///< (tick runs as often as the host's loop: its work once a frame)
    detail::WhichMap whichMap_;     ///< (the lobby's or a level's up: tick asks every time it runs)
    std::uint32_t frameNow_ = 0; ///< the level's frame last run
    int swiftGold_ = 0;          ///< the last level's Swift done: each one's gold, paid as this one opens
    /// The objective's start (with the first of the party in: `someone`, whose foes Purge's
    /// are), its progress, its reward.
    void startObjective(World& world, const ServerObject& someone);
    void runObjective(GameHost& host);
    void sendObjective(GameHost& host);
    std::string objectiveText() const;
    /// Revokes owed (MSG_GAUNTLET 0x11 / 0x12 / 0x13, by player name: a wipe's go on in the
    /// lobby): the script's (nox.revoke), sent 30 a frame (a client keeps 64 book events between its
    /// frames, client::World::kMaxBookEvents; a run's start revokes ~180) by level.sendRevokes,
    /// called only while some are owed (revokesOwed_: set as a hook queues them, cleared as none
    /// are left).
    bool revokesOwed_ = false;
    std::uint32_t revokedAt_ = ~0u; ///< the frame they last went
    /// The acts done this run and the gate taken out of each (the map's past).
    std::vector<std::pair<DelveAct, int>> history_;
    /// The run's map (MSG_NOXCPP_MODE_F4) each noxsdl player has had: the level it was sent for.
    std::map<std::uint8_t, std::uint32_t> mapSent_;
    /// Levels being built ahead (buildAhead), and those no longer wanted, left to finish (a
    /// std::async future's end waits for its thread).
    std::map<std::pair<int, int>, std::future<Built>> ahead_;
    std::vector<std::future<Built>> dropped_;
    int aheadUsed_ = 0;
    int builtOnTheSpot_ = 0;
    void sendMaps(GameHost& host);
};

} // namespace nox::server
