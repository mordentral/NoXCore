// Undead Tide's director: an auto-weapon survival run, hosted as a
// quest of one generated arena.
//
// The run is the classic quest, as Delving Mode's is (delve.hpp): the host opens on a generated
// lobby (UTD_LOB, a chat map) whose portal begins the quest with everyone there, the quest's maps
// narrowed to the arena (UTD_1), written anew for each run (prepare). The quest gives the co-op
// team, the lives and the game over; the stock rules play on untouched. The director only drives
// them:
//   - the tide: creatures of the arena's biome made just out of each player's sight on a clock,
//     more and tougher by the minute, the harder ones in waves (spawn), sent at the players as a
//     map script's Walk sends a creature (the horde, modes/common's horde.hpp);
//   - the auto-weapons: a loadout per player kept here alone (Runner), fired by the server with the
//     player as their caster (a blow, a spell, an arrow). Nothing is granted: the player's book,
//     abilities and weapons are emptied as the run begins (prepareUnit), so a stock client has
//     nothing to cast, swing or throw by hand;
//   - soul gems (BlueOrb / GreenOrb / RedOrb: lights, not items) dropped by the slain, drawn to a
//     player near them, XP and levels; each level a pick of three, offered by text and answered by
//     a number in chat (a stock client), the first taken after a while;
//   - no extra life at the start, a rare ankh dropped (AnkhCollide's life), rare armour dropped
//     (the quest's reward tables);
//   - the end: everyone out of lives (lost), or the timer's dawn reached (won); the results told,
//     then back to the lobby.
#pragma once

#include "noxcore/mapgen/arena.hpp"
#include "noxcore/server/which_map.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace nox::things {
struct ThingDb;
}

namespace nox::modes::script {
class ScriptedMode;
}

namespace nox::server {

class GameHost;
class World;
class TideRecords;
class ReadyTimers;
class Horde;
class Watchers;
class Gatherables;
struct HostedPlayer;
struct ServerObject;


/// The arena's name (one per server, written anew each run) and the lobby's.
inline constexpr std::string_view kTideMap = "UTD_1";
inline constexpr std::string_view kTideLobby = "UTD_LOB";
/// An Undead Tide map's name (UTD_..., any case): disposable, never offered by a map list, a
/// vote, the map cycle or a quest of its own accord.
bool isTideMapName(std::string_view name);

/// The auto-weapons (one a class to start).
enum class TideWeapon : std::uint8_t {
    Swing,        ///< a warrior's: a blow at the nearest enemy in reach (Fight::attack, its damage the loadout's)
    MagicMissile, ///< a wizard's: Spells::cast(Magic Missile) at the loadout's level as its power
    Arrows,       ///< a conjurer's: arrows the server looses for it, more as it levels
    /// Any class's pick (2026-10-11): Force of Nature's DeathBalls circling the player
    /// for their life (4 s), more of them and faster as it levels; the stock ball's harm.
    ForceOfNature,
    // Stock spells the server casts for it at the loadout's level as their power
    // (Spells::cast), at the nearest foe or about it; each its classes' pick (tideWeaponClasses).
    Fireball,   ///< (0x1B) at the nearest foe, a stronger ball as it levels
    DeathRay,   ///< (0x10) on the nearest foe
    PixieSwarm, ///< (0x3A) pixies that hunt the tide, more as it levels
    Meteor,     ///< (0x34) falls on the nearest foe
    Fist,       ///< (0x1D) Fist of Vengeance on the nearest foe
    Earthquake, ///< (0x17) about the player
    ToxicCloud, ///< (0x80) a poisonous cloud on the nearest foe
    ForceField, ///< (0x33, Shield) on the player, renewed while foes are near
    // The second set (2026-10-12): about a dozen more for each class. Stock spells cast as
    // the first set's (the script's table, data.luau: their aim, reach, waits), and a few of the
    // director's.
    Burn,         ///< (0x05) a flame where the nearest foe stands
    RingOfFire,   ///< (0x0A, Cleansing Flame) a ring of flames about the player
    Confuse,      ///< (0x0C) on foes not confused yet
    Fear,         ///< (0x1A) foes flee
    Freeze,       ///< (0x20) foes frozen still
    Slow,         ///< (0x48)
    Stun,         ///< (0x4A) held (a heavy one slowed)
    Poison,       ///< (0x3C)
    Shockwave,    ///< the director's: the tide about the player thrown back, hurt
    Gather,       ///< the director's: the tide about the player drawn in
    Shock,        ///< (0x47) on the player: who strikes it is shocked
    Vampirism,    ///< (0x82) on the player: its blows heal it
    MeteorShower, ///< (0x35) on the nearest foe
    Inversion,    ///< (0x26) foes' missiles turned back
    Blind,        ///< (0x03)
    TurnUndead,   ///< (0x15) about the player
    Doom,         ///< (0x0F, Death) the strongest foe near (not the guardian) dies in 15 s
    Firewalk,     ///< (0x1C) flames in the player's wake; no class's (its flames burn the party)
    Cleave,       ///< the director's: a wide blow that throws the struck back
    Crossbow,     ///< the director's: heavy bolts, one at each of the nearest foes
    WarCry,       ///< the director's: the foes about the player stunned
    Harpoon,      ///< the director's: the nearest foe hauled in and hurt
    Wolves,       ///< summoned wolves fight beside the player (summonAt), more each level
    Bear,         ///< a summoned bear (a BlackBear; a Bear from level 3)
    Wisps,        ///< summoned will-o'-wisps
};
inline constexpr std::size_t kTideWeapons = 37;
// (The weapons', passives', difficulties', pickups' and creatures' tables, their names and words
// among them, are the Tide's script's: script/server/data.luau, built into the program and read
// once at set-up, src/tide_data.hpp. The functions below answer from that copy.)
/// The script's tables read now, at a set-up (the director's and the client's constructors), so no
/// frame is the first to ask. Throws std::runtime_error when the built-in script isn't the data it
/// should be.
void loadTideData();
/// A weapon's icon: a spell's (its id), else an item's (its type) or a warrior ability's (1-5).
struct TideIcon {
    std::uint32_t spell = 0;
    std::string_view item;
    int ability = 0;
};
TideIcon tideWeaponIcon(TideWeapon w);
/// The classes a weapon is a pick of (bit 1 warrior, 2 wizard, 4 conjurer).
std::uint8_t tideWeaponClasses(TideWeapon w);
/// The passives (the player's own numbers, or the run's: nothing the client could use).
enum class TidePassive : std::uint8_t {
    Haste,     ///< the auto-weapons' waits shorter
    Vitality,  ///< more health
    Magnet,    ///< soul gems drawn from farther
    Recovery,  ///< health back over time
    Multishot, ///< another arrow, ball or cast (the weapons' count)
    Wisdom,    ///< more XP from soul gems
};
inline constexpr std::size_t kTidePassives = 6;

/// How hard a run is (Delving Mode's four, the same numbers: more and tougher creatures, elites
/// sooner and with more traits). Chosen in the lobby: "?difficulty <name>" in chat, or noxsdl's
/// question at the gate.
enum class TideDifficulty : std::uint8_t { Adventurer, Veteran, Nightmare, Oblivion };
inline constexpr std::size_t kTideDifficulties = 4;
std::string_view tideDifficultyName(TideDifficulty d);
std::string_view tideDifficultyAbout(TideDifficulty d);

std::string_view tideWeaponName(TideWeapon w);
std::string_view tidePassiveName(TidePassive p);
/// What it does, in a line (noxsdl's level-up cards).
std::string_view tideWeaponText(TideWeapon w);
std::string_view tidePassiveText(TidePassive p);

/// A level-up's choice.
struct TidePick {
    enum class Kind : std::uint8_t { Weapon, Passive, Heal } kind = Kind::Heal;
    std::uint8_t which = 0;  ///< the TideWeapon or TidePassive
    std::uint8_t level = 0;  ///< the level it takes it to
    bool operator==(const TidePick&) const = default;
};

/// MSG_NOXCPP_MODE_F5 (0xF5, noxcpp's own, both ways between our server and a client that identified
/// itself, a noxsdl): {u8 0xF5, u8 sub, u16 the rest's length, the rest}. A stock client never
/// gets one (it has the chat lines instead).
namespace tidemsg {
inline constexpr std::uint8_t kOp = 0xF5;
/// Whether its player is in a run's arena: {u8 1 / 0}. (noxsdl's game-over window then speaks of
/// the run, not of a quest's penalty.)
inline constexpr std::uint8_t kState = 1;
/// The player's run as it stands, a few times a second (Hud).
inline constexpr std::uint8_t kHud = 2;
/// A level-up's pick (Offer; none in it: the offer's closed).
inline constexpr std::uint8_t kOffer = 3;
/// The run's end (Results).
inline constexpr std::uint8_t kResults = 4;
/// (client to server) The pick taken: {u8 index, u16 the offer's level}.
inline constexpr std::uint8_t kPick = 5;
/// At the lobby's gate, the question "begin at which difficulty?": {u8 the difficulty now}, or
/// {0xFF}: put away (the player walked off, or the run began).
inline constexpr std::uint8_t kAsk = 6;
/// (client to server) The answer: {u8 TideDifficulty}; set, and the run begun when it's at the gate.
inline constexpr std::uint8_t kChoose = 7;
inline constexpr std::uint8_t kAskClosed = 0xFF;
std::vector<std::uint8_t> askMessage(std::uint8_t current);
std::vector<std::uint8_t> chooseMessage(std::uint8_t difficulty);

/// {u16 level, u16 xp, u16 need, u16 seconds, u16 dawn, u16 boss's time, u8 boss, u16 kills,
///  u8 weapons, each {u8 kind, u8 level, u16 cooldown, u16 ready in (frames)}, u8 passives, each u8,
///  u8 stage} (the stage last: one without it is stage 1)
struct Hud {
    std::uint16_t level = 1;
    std::uint16_t xp = 0;
    std::uint16_t need = 1;      ///< the XP to the next level
    std::uint16_t seconds = 0;   ///< into the run
    std::uint16_t dawn = 0;      ///< the run's won at
    std::uint16_t bossAt = 0;    ///< the night's guardian comes at
    std::uint8_t boss = 0;       ///< 0 not come, 1 standing, 2 slain
    std::uint16_t kills = 0;
    std::uint8_t stage = 1;      ///< the land the party's in (sent last: an older noxsdl stops before it)
    struct Weapon {
        std::uint8_t kind = 0;   ///< TideWeapon
        std::uint8_t level = 1;
        std::uint16_t cooldown = 0; ///< its wait between shots (frames)
        std::uint16_t readyIn = 0;  ///< frames till it fires next
        bool operator==(const Weapon&) const = default;
    };
    std::vector<Weapon> weapons;
    std::vector<std::uint8_t> passives; ///< by TidePassive
    bool operator==(const Hud&) const = default;
};
/// {u8 level, u8 seconds to answer, u8 picks, each {u8 kind, u8 which, u8 level}}
struct Offer {
    std::uint8_t level = 0;   ///< the level it's for (0: closed)
    std::uint8_t seconds = 0; ///< till the first is taken for the player
    std::vector<TidePick> picks;
    bool operator==(const Offer&) const = default;
};
/// {u8 won, u8 boss (as Hud's), u16 seconds, u16 elites, u8 len + the guardian's name,
///  u8 players, each {u8 len + name, u16 level, u16 kills, u8 weapons, each {u8 kind, u8 level}},
///  u8 stage} (the stage last: one without it is stage 1)
struct Results {
    bool won = false;
    std::uint8_t boss = 0;
    std::uint16_t seconds = 0;   ///< the whole run's, every stage's
    std::uint8_t stage = 1;      ///< the stage it ended in
    std::uint16_t elites = 0;
    std::string bossName;
    struct Player {
        std::string name;
        std::uint16_t level = 1;
        std::uint16_t kills = 0;
        std::vector<std::pair<std::uint8_t, std::uint8_t>> weapons; ///< (TideWeapon, level)
        bool operator==(const Player&) const = default;
    };
    std::vector<Player> players;
    bool operator==(const Results&) const = default;
};

/// Results' players at most, and a name's bytes at most: the message stays well within one
/// reliable packet (0x400).
inline constexpr std::size_t kResultsMost = 12;
inline constexpr std::size_t kNameMost = 32;

std::vector<std::uint8_t> stateMessage(bool inRun);
std::vector<std::uint8_t> hudMessage(const Hud& h);
std::vector<std::uint8_t> offerMessage(const Offer& o);
std::vector<std::uint8_t> resultsMessage(const Results& r);
std::vector<std::uint8_t> pickMessage(std::uint8_t index, std::uint16_t level);
/// The sub and the rest, or none when it's cut short.
std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m);
/// A sub's rest read back; none when it's cut short or out of range.
std::optional<Hud> parseHud(std::span<const std::uint8_t> rest);
std::optional<Offer> parseOffer(std::span<const std::uint8_t> rest);
std::optional<Results> parseResults(std::span<const std::uint8_t> rest);
} // namespace tidemsg

class TideDirector {
public:
    struct Options {
        std::filesystem::path mapsDir;
        std::string mapName{kTideMap};
        std::string lobbyName{kTideLobby};
        /// The first run's seed (each run after a fresh one).
        std::uint32_t seed = 1;
        /// Every run in this biome; "": drawn.
        std::string biome;
        /// Dawn: the run won this many seconds in (20 minutes, the plan's).
        std::uint32_t winSeconds = 1200;
        /// The night's guardian comes this many seconds in (0: at dawn; with dawnWins, two minutes
        /// before dawn, or at three quarters of a short run); slain, the run's won there and then.
        std::uint32_t bossSeconds = 0;
        /// Dawn itself wins the run, and the guardian's fall (the first build's rule; tests' short
        /// runs). Off (2026-10-12): the guardian comes at dawn, and its fall carries the
        /// party on to a new biome, a stage harder, the clock anew; the run ends only when the tide
        /// has taken everyone.
        bool dawnWins = false;
        /// Seconds from the guardian's fall to the next stage's load (the gems and pickups taken up).
        std::uint32_t stageDelaySeconds = 8;
        /// The quiet before the tide (seconds from the run's start).
        std::uint32_t graceSeconds = 5;
        /// Live creatures at most about one player (its view and a margin), and in all.
        int perView = 60;
        int maxCreatures = 240;
        /// For benchmarks: keep this many creatures about each player from the start, the clock's
        /// own numbers aside (0: the clock's).
        int holdPerPlayer = 0;
        /// The arena's corner distance in wall cells (even).
        int arenaRadius = 50;
        /// A revive shrine rises this long after someone falls for good (or after the last one
        /// was used), while anyone still stands.
        std::uint32_t reviveSeconds = 60;
        /// The records kept (each run's filed as it ends, the lobby's board tells them); none:
        /// none kept, the board says so.
        TideRecords* records = nullptr;
        /// The difficulty a lobby opens at (and goes back to after each run).
        TideDifficulty difficulty = TideDifficulty::Adventurer;
        /// Where a failure of the Tide's script is told (none: the standard error). A rule that
        /// fails (a fault in the built-in script, or its budget spent) is told there as a script
        /// failure and its frame's work dropped; the host goes on.
        std::function<void(std::string_view)> log;
        /// (Tests: each script call's budget in safepoints; 0: the built-in scripts' own, no
        /// count, a minute's time.)
        std::uint64_t scriptSafepoints = 0;
        /// (Tests: each arena's parameters handed over before it's built, a failure thrown there
        /// as the generator's own would be.)
        std::function<void(mapgen::ArenaParams&)> arenaHook;
    };

    enum class State : std::uint8_t {
        Lobby,    ///< in the lobby, open to anyone
        Starting, ///< the portal taken: the quest begun, locked
        Running,  ///< in the arena
    };

    /// A player's run, by its account and player name (runnerKey: one who drops and comes back
    /// keeps it; two players of one name on different accounts have their own).
    struct Runner {
        std::string name;       ///< its player name (as shown)
        std::uint8_t cls = 0;   ///< its class (0 warrior, 1 wizard, 2 conjurer)
        bool multiclass = false; ///< every class's spells and abilities (its record says MultiClass)
        bool noxCore = false;    ///< on the NoX Core client (it identified itself; for its record)
        std::uint32_t unit = 0; ///< its unit as last seen
        struct Weapon {
            TideWeapon kind = TideWeapon::Swing;
            std::uint8_t level = 1;
            std::uint32_t readyAt = 0; ///< the frame it fires next
        };
        std::vector<Weapon> weapons;
        std::array<std::uint8_t, kTidePassives> passives{};
        int level = 1;
        int xp = 0;       ///< toward the next level
        int xpCarry = 0;  ///< hundredths of XP carried (Wisdom's share)
        int kills = 0;
        int banked = 0;   ///< levels not picked for yet (the offer's own one counted)
        std::vector<TidePick> offer;
        std::uint32_t offeredAt = 0;
        bool out = false; ///< out of lives: watching till the run's end
        std::uint32_t lives = 0; ///< its quest lives as last seen (one coming back gets them again)
        std::uint16_t baseHealth = 0; ///< its most health before Vitality (as its class gave it)
        std::uint32_t hiddenUntil = 0; ///< the Invisibility pickup's end: its weapons don't show it till then
    };

    TideDirector(const things::ThingDb& things, Options options);
    ~TideDirector();
    TideDirector(const TideDirector&) = delete;
    TideDirector& operator=(const TideDirector&) = delete;

    /// The host gone (the mode's detach): the script's own let go of it.
    void detach(GameHost& host);

    /// A player's runner key: "account/name" (its WOL account, as it joined), or its name alone
    /// when it gave no account (a LAN game).
    static std::string runnerKey(const HostedPlayer& p);

    /// Deletes every Undead Tide map under `mapsDir` (folders named UTD_*). Returns how many.
    static int sweep(const std::filesystem::path& mapsDir);
    /// Deletes this director's own maps alone.
    int sweepOwn() const;

    /// The arena of a run: open ground in the biome (drawn by the seed when ""), its scenery for
    /// cover, no monsters, items or exit (the tide brings the creatures).
    static mapgen::ArenaParams arenaParams(std::uint32_t seed, std::string_view biome, int radius = 50);
    /// The lobby: a chat map, the rules' signs, the portal.
    static mapgen::ArenaParams lobbyParams();
    /// Writes the lobby; returns its .map (the map to host first).
    std::filesystem::path writeLobby();
    /// The server's listed name: "Undead Tide" in the lobby, "Tide 3m" in a run (whole minutes).
    static std::string listedName(std::optional<std::uint32_t> seconds);

    /// Each frame, after the host's tick: the lobby's signs and portal, the lock, the run.
    /// True when the lock changed.
    bool tick(GameHost& host);
    /// GameHost::Options::prepareMap: the arena written when `next` is it. False for another map.
    bool prepare(const std::filesystem::path& next, const World& world);
    /// The portal's doing (and tests' and the bench's): the run begun with everyone in, locked.
    void beginRun(GameHost& host);

    /// GameHost::Options::onChat: a stock player's pick (1-3) at a level-up. True when taken.
    bool onChat(GameHost& host, std::uint8_t peer, std::u16string_view text);
    /// GameHost::Options::onTideMessage: a noxsdl player's pick (kPick), for the offer open now.
    void onMessage(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m);
    /// A pick by its index in the player's offer (0-2): taken, told. False when none open.
    bool pick(GameHost& host, const std::string& name, std::size_t index);
    /// A player's auto-weapon at `level` (added, or set; 0 takes it away): tests, and the host's
    /// test commands. False when it has no runner (not in the arena yet).
    bool giveWeapon(const std::string& name, TideWeapon kind, int level);
    /// A player's passive at `level` (0-its top). False when it has no runner.
    bool givePassive(const std::string& name, TidePassive kind, int level);
    /// XP as soul gems give it (levels and their offers as from them). False when it has no runner
    /// or no unit.
    bool giveXp(GameHost& host, const std::string& name, int xp);
    /// A piece of armour charmed (tests and the host's test commands): warding or repelling.
    void charmItem(std::uint32_t item, bool warding) { charms_[item] = {warding ? Ward::Warding : Ward::Repulsion, 0}; }

    /// The difficulty (the next run's in the lobby, the run's own in one). Set in the lobby alone:
    /// false (unchanged) once the run has begun.
    TideDifficulty difficulty() const noexcept { return difficulty_; }
    bool setDifficulty(TideDifficulty d);
    State state() const noexcept { return state_; }
    bool locked() const noexcept { return state_ != State::Lobby; }
    const Options& options() const noexcept { return options_; }
    std::uint32_t runSeed() const noexcept { return runSeed_; }
    /// The run's biome (the arena last written).
    const std::string& biome() const noexcept { return biome_; }
    /// Seconds since the stage began (0 outside a run).
    std::uint32_t seconds(std::uint32_t frame) const;
    /// Seconds since the run began, every stage's (0 outside one).
    std::uint32_t runSeconds(std::uint32_t frame) const { return stagesSeconds_ + seconds(frame); }
    /// The stage the run's in (1 its first biome; each guardian slain, one more).
    int stage() const noexcept { return stage_; }
    /// The next stage's load asked for (its guardian slain, the wait over), till it's in.
    bool advancing() const noexcept { return advanceAt_ != 0; }
    const std::map<std::string, Runner>& runners() const noexcept { return runners_; }
    /// The live creatures of the tide (extents).
    std::size_t creatures() const noexcept;
    /// The dawn boss as it stands (0: not come, or slain), the chests on the ground, the armour
    /// charmed, the elites slain (tests).
    std::uint32_t boss() const noexcept { return boss_; }
    bool bossCame() const noexcept { return bossCame_; }
    std::size_t chests() const noexcept { return chests_.size(); }
    std::size_t charms() const noexcept { return charms_.size(); }
    /// The revive shrine standing (0: none), and the fallen it has brought back (tests).
    std::uint32_t shrine() const noexcept { return shrine_; }
    int revived() const noexcept { return revived_; }
    /// The lobby's records board (0: not found yet).
    std::uint32_t board() const noexcept { return board_; }
    /// The arena's scenery made walk-through as the run began (tests).
    int sceneryWalked() const noexcept { return sceneryWalked_; }
    int elitesSlain() const noexcept { return elitesSlain_; }
    /// When the dawn boss comes, in seconds into the run.
    std::uint32_t bossSeconds() const;
    /// The XP from `level` to the next.
    /// (The script's rule; a director's own calls ask its own script, this one a script of the
    /// calling thread's, for callers with no director.)
    static int xpFor(int level);
    /// Whether the run's over (won or lost) and when (frames; 0: not).
    bool won() const noexcept { return won_; }
    /// The script's failures told (each a frame's work, or a chat line's, dropped).
    int scriptFailures() const noexcept { return scriptFailures_; }
    std::uint32_t endedAt() const noexcept { return endedAt_; }

private:
    /// (tick, onChat and onMessage themselves: a script's failure thrown out of them is caught
    /// by the public ones.)
    bool tickFrame(GameHost& host);
    bool chatLine(GameHost& host, std::uint8_t peer, std::u16string_view text);
    /// (prepare itself: the arena written; a failure caught by prepare.)
    bool prepareArena(const std::filesystem::path& next, const World& world);
    void message(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m);
    void runLobby(GameHost& host);
    void runArena(GameHost& host);
    void beginArena(World& world, std::uint32_t frame); ///< (runArena's first frame of a new arena)
    /// A player's unit made ready for the run (once a unit): its book, abilities, guides emptied
    /// (not revoked on its client: its bars are its own .plr's), no extra life, its loadout its
    /// class's start.
    void prepareUnit(World& world, ServerObject& unit, Runner& r);
    /// Its weapons and wands gone (a respawn's outfit gives a warrior a sword again).
    void disarm(GameHost& host, ServerObject& unit);
    /// The players in play this frame (the tide's health scales with it).
    int partySize_ = 1;
    /// The dawn boss slain this frame (the horde's fall): the run's won at the frame's end.
    bool bossSlain_ = false;
    void runGems(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive);
    void gainXp(GameHost& host, ServerObject& unit, Runner& r, int xp);
    void offerPicks(GameHost& host, ServerObject& unit, Runner& r);
    void applyPassives(World& world, ServerObject& unit, Runner& r);
    void endRun(GameHost& host, bool won);
    /// The player behind a peer, its runner (null: none).
    Runner* runnerOf(GameHost& host, std::uint8_t peer);
    /// A runner by its key, or else by its player name (the first of that name): tests, the
    /// host's test commands.
    Runner* findRunner(const std::string& keyOrName);
    /// The revive shrine: risen near a player in play a while after someone falls for good; a
    /// player in play touching it brings every fallen one who's watching back beside it.
    void runShrine(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive);
    std::uint32_t shrine_ = 0;
    std::uint32_t shrineDueAt_ = 0; ///< when the next rises (0: nobody fallen)
    int revived_ = 0;
    /// The records: this run's filed as it ends; the lobby's board tells the table to whoever
    /// walks up to it (each at most every 20 s).
    void fileRecord(GameHost& host);
    bool runBoard(GameHost& host, const ServerObject& unit); ///< (true: told)
    std::uint32_t board_ = 0;
    int sceneryWalked_ = 0;
    TideDifficulty difficulty_ = TideDifficulty::Adventurer;
    /// The noxsdl players asked at the gate (by peer: its unit then, when), till they walk off or
    /// answer. One that stays there unanswered kAskWaitFrames begins the run (an older noxsdl
    /// knows no question).
    struct Asked {
        std::uint32_t unit = 0;
        std::uint32_t at = 0;
    };
    std::map<std::uint8_t, Asked> askedAtGate_;
    /// The gate's question put away for everyone asked (the run begun).
    void closeAsks(GameHost& host);
    /// The difficulty changed: the open questions told it.
    void refreshAsks(GameHost& host);
    /// The lobby's zones (modes/common's watchers): the signs, the board, the gate, made afresh as
    /// the lobby loads; what's done as each fires the script's (lobby.luau) or the gate's own.
    std::unique_ptr<Watchers> watch_;
    int lobbyGroup_ = -1;
    GameHost* lobbyHost_ = nullptr;
    std::set<std::uint32_t> boardWaiting_; ///< (by unit: within the board, its wait not over)
    std::set<std::uint8_t> atGate_;       ///< (by peer: within the gate)
    bool beginNow_ = false;
    void watchLobby();
    std::uint32_t cooldown(const Runner& r, const Runner::Weapon& w) const;

    const things::ThingDb& things_;
    Options options_;
    std::uint32_t runSeed_ = 0;
    std::uint32_t runs_ = 0;
    State state_ = State::Lobby;
    bool savedClosed_ = false;
    std::vector<std::string> savedAllowed_;
    std::string biome_;
    std::uint32_t written_ = 0; ///< arenas written (prepare)
    std::uint32_t seen_ = 0;    ///< the arena the run's state is for
    std::uint32_t startFrame_ = 0;
    std::uint32_t nextSpawn_ = 0; ///< the tide's next spawn (the script's spawn says when)
    std::uint32_t lastToldMinute_ = 0;
    /// The listed name in a run, as last built (listedName), and the minute it's for.
    std::string listed_;
    std::uint32_t listedMinute_ = 0;
    /// The stages: the run's own (1 the first biome), the seconds of those before this one, the
    /// frame the next one's load is due (0: none; 1: asked for), and whether the arena coming in
    /// carries the party on (its runners kept) rather than beginning a run.
    int stage_ = 1;
    std::uint32_t stagesSeconds_ = 0;
    std::uint32_t advanceAt_ = 0;
    bool carry_ = false;
    /// The runners out as their stage ended: back in play in the next (2026-10-12).
    std::vector<std::string> bringBack_;
    /// The guardian slain: the party carried on to the next stage a while on.
    void advance(GameHost& host);
    bool won_ = false;
    std::uint32_t endedAt_ = 0;
    std::uint32_t endSeconds_ = 0; ///< the run's time as it ended (the HUD's clock stops there)
    std::vector<std::uint32_t> signs_; ///< the lobby's, west to east
    std::uint32_t portal_ = 0;
    std::map<std::string, Runner> runners_;
    std::vector<std::uint32_t> prepared_; ///< units made ready (prepareUnit), this arena
    /// The soul gems on the ground (their worth the items' values) and the pickups lying out (their
    /// kinds): modes/common's gatherables, drawn in and taken natively, what's taken the script's.
    std::unique_ptr<Gatherables> gemField_;
    std::unique_ptr<Gatherables> pickupField_;
    /// The players taking up gems and pickups this frame (the takers' list), and the host.
    const std::vector<std::pair<ServerObject*, Runner*>>* gathering_ = nullptr;
    GameHost* gatherHost_ = nullptr;
    /// A gem's reach by Magnet level (the script's, asked once a level; < 0 not yet), and the
    /// passives' health by base health, Vitality and Recovery (the script's, asked once each).
    std::vector<float> gemReach_;
    std::map<std::uint64_t, std::pair<std::uint16_t, int>> passiveHealth_;
    /// The LevelUp glows made for a level up, put away a while on (extent, made at).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> glows_;
    /// The elites' chests (CryptChest: a touch opens it, ChestCollide), each a free upgrade to the
    /// one who opens it (the nearest player as it opens); the opened ones put away later (extent,
    /// when).
    struct Chest {
        std::uint32_t extent = 0;
        float x = 0, y = 0;
    };
    std::vector<Chest> chests_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> openedChests_;
    void runChests(GameHost& host);
    /// Rare armour's effects, kept here (the item itself a stock one): while it's worn, Warding
    /// casts Shield on its wearer when it has none, Repulsion pushes the tide off it now and then.
    enum class Ward : std::uint8_t { Warding, Repulsion };
    struct Charm {
        Ward ward = Ward::Warding;
        std::uint32_t nextAt = 0;
    };
    std::map<std::uint32_t, Charm> charms_; ///< by the item's extent
    void runCharms(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive);
    /// The dawn boss: its extent once come (0: not yet, or slain), its name.
    std::uint32_t boss_ = 0;
    bool bossCame_ = false;
    std::string bossName_;
    int elitesSlain_ = 0;
    /// Pickups on the ground (2026-10-12): stock items (potions, food, amulets, gems)
    /// made NO_COLLIDE so no hand takes them, used at once by the player in play who touches one
    /// (runPickups; what each does is the script's, pickups.luau). Gone a minute on.

    int pickupsTaken_ = 0;
public:
    /// Pickups lying in the arena and those taken this run (tests).
    std::size_t pickups() const noexcept;
    int pickupsTaken() const noexcept { return pickupsTaken_; }
    /// A pickup laid at (x, y): its kind by its table index (tests, the host's test commands).
    bool dropPickup(GameHost& host, int kind, float x, float y);
    /// The pickups' kinds (tests).
    static std::size_t pickupKinds();
    /// Whether a class in the run may wear an armour (modifier.bin's CLASSUSE): only such drop.
    bool armourForParty(const GameHost& host, std::string_view type) const;
private:
    void runPickups(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive);
    /// The classes in the run (bits: 1 warrior, 2 wizard, 4 conjurer): armour dropped is theirs.
    std::uint8_t partyClasses() const;
    /// The slain's bodies, cleared a while on (extent, died at).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bodies_;
    /// What each noxsdl player (by peer: its unit then, and whether in a run) was last told of the run
    /// (MSG_NOXCPP_MODE_F5 kState; a peer's number taken by a newcomer, told afresh).
    std::map<std::uint8_t, std::pair<std::uint32_t, bool>> told_;
    void tellState(GameHost& host);
    /// Each noxsdl player's run (kHud), every few frames; its offer (kOffer) as it opens and closes;
    /// the results (kResults). A stock player has the chat lines.
    void tellHuds(GameHost& host);
    std::uint32_t hudFrame_ = 0xFFFFFFFFu; ///< (the frame the HUDs last went: once, the host's loop ticking more often)
    void tellOffer(GameHost& host, const std::string& name, const Runner& r);
    tidemsg::Results results() const;
    /// The peer playing as `name`, when it's a noxsdl's.
    std::optional<std::uint8_t> identifiedPeer(GameHost& host, const std::string& name) const;
    std::uint32_t lastFrame_ = 0xFFFFFFFFu; ///< (tick runs as often as the host's loop: once a frame)
    detail::WhichMap whichMap_;              ///< (the lobby's or the arena's up: tick asks every time it runs)
    std::uint32_t rngState_ = 1;
    /// The Tide's script (script/server/main.luau, built in): the run's rules are its functions,
    /// the director calls them (rule). Attached to the host at the first tick (its float maths the
    /// host's rounding).
    std::unique_ptr<modes::script::ScriptedMode> script_;
    GameHost* scriptHost_ = nullptr;
    /// The script's rules (main.luau's functions of these names, in order: kRuleNames), each looked
    /// up once (ScriptedMode::Rule, in rules_).
    enum class Rule : std::uint8_t { XpFor, Cooldown, SetOptions, BeginStage, Spawn, SpawnBoss, GainXp, OfferPicks, Pick, StageDamage, Advance, ChestOpened, Record, Filed, RiseShrine, BringBack, OnChat, SetUp, Shoot, Shape, Forget, Fell, ComeBack, Sign, Board, LobbyLoaded, GuardianAt, Prepare, StageBack, Coming, SetsDifficulty, Rises, Minute, Taken, PickedForYou, Ended, KeptSafe, GemTaken, PickupTaken, GemReach, PassiveHealth, Charm, BodyDue, RunOver };
    struct ScriptRules;
    std::unique_ptr<ScriptRules> rules_;
    /// The script's rule called with `args`, its first `out.size()` results into `out`. Throws
    /// std::runtime_error when it fails (a fault in the built-in script: the tests run it).
    void rule(Rule which, std::initializer_list<double> args, std::span<double> out) const;
    /// A rule's failure caught where the host called in (tick, onChat, onMessage): told as a
    /// script failure (Options::log), counted.
    void scriptFailed(std::string_view where, const std::exception& e);
    int scriptFailures_ = 0;
    /// The script's rule called with what `push` pushes (its count returned), `read` reading its
    /// `nresults` results. Throws std::runtime_error when it fails.
    void ruleWith(Rule which, int nresults, const std::function<int(lua_State*)>& push, const std::function<void(lua_State*)>& read = {}) const;
    /// When the night's guardian comes, seconds into a stage (the script's guardianAt, asked once).
    std::uint32_t bossAt_ = 0;
    /// A rule's failure thrown (its error the script's last).
    [[noreturn]] void failed(Rule which) const;
    int xpNeed(int level) const;
    /// What the script may ask of the director (the `owner` table: its runners, gems, pickups, the
    /// horde's and the weapons' blocks, the records), those on a runner on the runner in hand, the
    /// player whose rule is running (ruleUnit_).
    void addOwnerFunctions(modes::script::ScriptedMode& script);
    /// The spawn rules (spawns.luau's spawn, or spawnBoss with `guardian`).
    void scriptSpawn(GameHost& host, const std::vector<ServerObject*>& alive, bool guardian);
    /// The weapons' blocks (modes/common's): each player's weapons counted down, its rings turned and
    /// its summons and missiles kept natively, the script's shoot asked as one is ready (the script
    /// sets their ways up, setUp). The players in play whose weapons are being fired (the frame's,
    /// the shots' holders) and the frame.
    std::unique_ptr<ReadyTimers> timers_;
    const std::vector<std::pair<ServerObject*, Runner*>>* firing_ = nullptr;
    std::uint32_t firingFrame_ = 0;
    void fireWeapons(GameHost& host, const std::vector<std::pair<ServerObject*, Runner*>>& alive);
    /// The tide's creatures, steered and reaped natively (modes/common's), the slain and the guardian
    /// left far behind the script's (fall.luau's fell and comeBack).
    std::unique_ptr<Horde> horde_;
    GameHost* ruleHost_ = nullptr;
    ServerObject* ruleUnit_ = nullptr;
    Runner* ruleRunner_ = nullptr;
    std::uint32_t ruleFrame_ = 0;
    /// The runner in hand for a rule's call (what the owner functions act on: the four above): set
    /// for the call, the one before put back after (a rule's call inside another's: a pickup's XP).
    class HandScope {
    public:
        HandScope(TideDirector& d, GameHost* host, ServerObject* unit, Runner* runner, std::uint32_t frame);
        ~HandScope();
        HandScope(const HandScope&) = delete;
        HandScope& operator=(const HandScope&) = delete;

    private:
        TideDirector& d_;
        GameHost* host_;
        ServerObject* unit_;
        Runner* runner_;
        std::uint32_t frame_;
    };
    std::uint32_t roll();
};

} // namespace nox::server
