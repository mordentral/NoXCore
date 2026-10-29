// The game server, headless: a map's world, its script, and the frame loop. It has no network
// of its own: GameHost (game_host.hpp) brings the players and their packets in through its hooks.
//
// A frame (the exe's server tick) in the order GAME.EXE runs it: the units update
// (everything moves its frame), the script timers, the deletions of the frame, then (once a
// player is in, or when asked to without one) MapInitialize and MapEntry. Substeps after a
// frame's first carry the important objects on (clock.hpp).
#pragma once

#include "noxcore/script/script_file.hpp"
#include "noxcore/server/clock.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <utility>

namespace nox::server {

class Server {
public:
    struct Options {
        int multiplier = 1;        ///< substeps a frame: 1-4 (30-120 Hz)
        /// Run MapInitialize and MapEntry without a player (the game waits for one): for a
        /// headless run of a map's script.
        bool startWithoutPlayers = false;
        std::uint32_t seed = 1;    ///< the scripts' Random
        /// modifier.bin, for the weapons and armour (classes, values); null: anything goes.
        const things::ModifierDb* modifiers = nullptr;
        /// gamedata.bin, for the classes', items' and spells' numbers (a real run always loads
        /// the install's; null reads every number as 0).
        const things::GameData* gameData = nullptr;
        /// The game's flags (GameFlag): the mode the logic plays by. A network host's carry
        /// Network and its mode (arena, CTF...); a solo game Solo, a quest Network | Quest.
        std::uint32_t gameFlags = GameFlag::Network | GameFlag::Arena;
        /// nox.csf, for the lines the server sends (MSG_TEXT_MESSAGE); null: the labels go.
        const strings::StringTable* strings = nullptr;
        /// The host's gameplay switches (CheckGameplayFlags: 1 friendly fire, 2
        /// teams fixed, 4 team play), and each mode's lesson and time limits (minutes; modes.hpp's
        /// order: arena, elimination, CTF, KotR, flagball, quest; 0 none).
        std::uint32_t gameplayFlags = 0;
        std::array<std::uint16_t, 6> lessonLimits{};
        std::array<std::uint8_t, 6> timeLimits{};
        /// The host's server options (World::serverOptions: 2 weapons respawn).
        std::uint32_t serverOptions = 0;
        /// monster.bin's monsters (loaded for this kind of game; null: monsters without them).
        const things::MonsterDb* monsterDb = nullptr;
        /// The host's automatic respawn option (World::autoRespawn, kept from map to map).
        bool autoRespawn = false;
        /// A game hosted on WOL (World::wolGame): the host's Advanced Server switches count.
        bool wolGame = false;
        /// soundset.bin's creature sound sets (null: creatures make none of their own sounds).
        const things::SoundSets* soundSets = nullptr;
        /// The frame's sounds at most (World::soundLimit): a host that sends and frees them each
        /// frame passes the exe's 0x80; 0 none.
        std::size_t frameSounds = 0;
        /// A quest's screens (World::questScreens): the start's rules, each stage's, the results.
        bool questScreens = true;
        /// A quest's saves sent its clients (World::questSaves).
        bool questSaves = true;
        /// Every player of this class (World::forceClass).
        std::optional<std::uint8_t> forceClass;
        /// Multiclass for noxsdl players (World::multiclass).
        bool multiclass = false;
        /// Monsters looking all about them (World::monstersAlert).
        bool monstersAlert = false;
        /// A dead player keeping its things outside a quest (World::keepThingsOnDeath).
        bool keepThingsOnDeath = false;
        /// A mode's book outside a quest: only what it grants (World::modeOwnsBook).
        bool modeOwnsBook = false;
        /// The map editor's play-test sandbox: players can't die (World::sandboxPlayersCantDie).
        bool playersCantDie = false;
        /// A map editor session's sandbox: nothing scores (World::sandboxNoScoring).
        bool noScoring = false;
        /// A mode that keeps its stock look-alike's limits itself (World::modeOwnsLimits).
        bool modeOwnsLimits = false;
        /// A mode's pickup rules (World::pickupAnyClass, World::pickupDuplicates).
        bool pickupAnyClass = false;
        bool pickupDuplicates = false;
        /// The game's own float rounding for the server's figuring: towards zero, as its x87 chops
        /// (util::RoundingScope at each entry: the map loads and the frames); off, to nearest
        /// (VD-37): the same code, the old last bits.
        bool exactMaths = true;
    };

    Server(const things::ThingDb& things, Options options);

    /// Loads a map (its world and script) and runs GLOBAL's initialisers, as the exe's map load
    /// (server_loadMapFile -> scriptRunFirst).
    void loadMap(const std::filesystem::path& path);
    void loadMap(const map::MapFile& map, std::string_view fileName = {});
    /// The next map in a running game (the map's load with mapSwitchLevel): the players
    /// and what they hold stay, with their teams, the host's settings and the mode chosen; the new
    /// map's mode set up with them in it; each at a player start, its score cleared; the timer at
    /// its limit. fileName ("<map>.map"): the quest map list's.
    /// waypoint (an exit's "map:waypoint"): every player put there after (the map load's switchToWP).
    void switchMap(const map::MapFile& map, std::string_view fileName = {}, std::string_view waypoint = {});

    /// Runs one substep (a frame's first runs the frame).
    void step();
    /// Runs whole frames.
    void runFrames(std::uint32_t frames);

    /// True when the last step ran a frame (a classic client's update goes out then).
    bool steppedFrame() const noexcept { return steppedFrame_; }
    /// The substep the last step ran (0: a frame) and the frame's count of them.
    int lastSubstep() const noexcept { return lastSubstep_; }
    int lastMultiplier() const noexcept { return lastMultiplier_; }
    /// True when the last step's frame updated the world (the tick's block under Game_IsPaused, not
    /// in a match's end: the units, the votes, the deletions); false paused or over.
    bool updatedWorld() const noexcept { return updatedWorld_; }
    /// True when the last step's frame took the tick's update branch (updateServer, the packets,
    /// the transfers, SrvNet_UpdateAllClients), paused or not; false in a match's end and a map's load tick.
    bool ranUpdate() const noexcept { return ranUpdate_; }
    /// A player is in the game (MapInitialize and MapEntry wait for one).
    void setPlayersPresent(bool present) noexcept { playersPresent_ = present; }
    /// A finished match's wait is over: the host loads the next map (the same one, or the map
    /// cycle's: the tick's end). True once, then false.
    bool takeNextMapDue() noexcept { return std::exchange(nextMapDue_, false); }
    /// (Its wait over, not yet taken by the host.)
    bool nextMapDue() const noexcept { return nextMapDue_; }
    /// noxcpp's map vote: a match over waits till at least `frame` (else its 10 s), its wait begun
    /// or about to be. 0 lets it go back to the stock 10 s (the vote switched off in it).
    void holdGameOver(std::uint32_t frame) noexcept
    {
        gameOverHold_ = frame;
        if (gameOverEnd_) {
            gameOverEnd_ = std::max(gameOverStockEnd_, frame);
        }
    }
    /// The tick's packets (netlist / SrvNet_OnPacket): run inside the frame where the
    /// exe reads them (after updateServer, before the units; in a match over before
    /// its wait's check; not in a map's load tick); never in a substep (what comes between frames
    /// waits for the next frame's read, as the exe's). The host's.
    void setPacketHook(std::function<void()> hook) { packetHook_ = std::move(hook); }
    /// updateServer's network part (the host's: the latency report every 500 ms), where the exe
    /// has it: in the update branch after the time limit, the camper alarm and sudden death,
    /// before the chat bases and the quest; not with the match over (game flag 8, the time limit's
    /// end in this frame too).
    void setUpdateServerHook(std::function<void()> hook) { updateServerHook_ = std::move(hook); }
    /// mapLoad only asks (a global's flag): the next frame is the load's own tick
    /// (the map's load under game flag 0x8000000: loadHook, the host's), then the tick's end; no
    /// update, no packets.
    void requestMapLoad() noexcept { mapLoadRequested_ = true; }
    bool mapLoadRequested() const noexcept { return mapLoadRequested_; }
    void setMapLoadHook(std::function<void()> hook) { mapLoadHook_ = std::move(hook); }
    /// The tick's clients' update (SrvNet_UpdateAllClients, the host's: its transfers,
    /// votes and the updates; it asks ranUpdate itself): run at each frame's tick end, after the
    /// deletions' sweep and before the new things' pass, so what the frame made goes
    /// out the next tick, as the exe's.
    void setClientsHook(std::function<void()> hook) { clientsHook_ = std::move(hook); }
    /// The map editor's sandbox (Options::playersCantDie) changed in play: the world's now, the
    /// maps' after.
    void setPlayersCantDie(bool on) noexcept
    {
        options_.playersCantDie = on;
        world_.sandboxPlayersCantDie = on;
    }
    /// The session sandbox's "no scoring" (Options::noScoring) changed: now and the maps after.
    /// Whether the server's figuring rounds as the game's x87 (Options::exactMaths).
    bool exactMaths() const noexcept { return options_.exactMaths; }
    void setNoScoring(bool on) noexcept
    {
        options_.noScoring = on;
        world_.sandboxNoScoring = on;
    }

    SimClock& clock() noexcept { return clock_; }
    const SoundTable& soundTable() const noexcept { return soundTable_; }
    World& world() noexcept { return world_; }
    ScriptHost* script() noexcept { return script_.get(); }
    const script::ScriptFile& scriptFile() const noexcept { return scriptFile_; }
    const things::ThingDb& things() const noexcept { return things_; }
    const things::ModifierDb* modifiers() const noexcept { return options_.modifiers; }
    /// The map file the world was loaded from (a save writes what the World doesn't keep from it).
    const map::MapFile& mapFile() const noexcept { return mapFile_; }
    /// The last map script read (the exe's <work dir>\nc.obj, written as a map's ScriptObject is
    /// read): a save of a map without one writes it (an original bug).
    const std::vector<std::uint8_t>& lastScriptCode() const noexcept { return lastScriptCode_; }

private:
    /// The map editor's hot reload (script_reload.hpp, NoX Core's only: never in a game) swaps
    /// the script between frames.
    friend class ScriptReloadAccess;

    void frame();
    /// The world's update (the units, missiles, motion, enchantments, respawns, spells, abilities,
    /// script timers, deletions): not while paused.
    void updateWorld(std::uint32_t now);
    /// The tick's end: the timed pause, the next frame's count, the map's start, the level-up's end.
    void tickEnd();
    void loadMapKeeping(const map::MapFile& map, World* old, std::string_view fileName, std::string_view waypoint = {});
    void substep(int k, int m);
    /// The host's own client as the world sees it: a mode's in its place (ModeHooks::hostClient),
    /// else the one set.
    HostClient* seenHostClient();

    const things::ThingDb& things_;
    Options options_;
    SimClock clock_;
    SoundTable soundTable_; ///< thing.bin's sound numbers (World::soundTable points here)
    World world_;
    map::MapFile mapFile_;
    std::vector<std::uint8_t> lastScriptCode_;
    script::ScriptFile scriptFile_;
    std::unique_ptr<ScriptHost> script_;
    bool mapInitPending_ = false;
    bool mapEntryPending_ = false;
    bool steppedFrame_ = false;
    int lastSubstep_ = 0;
    int lastMultiplier_ = 1;
    /// The important objects the substeps carried off their frame's place since the last frame
    /// (extents; one may appear more than once): put back at the next frame's start.
    std::vector<std::uint32_t> substepsMoved_;
    /// A substep's carried objects and where its last one had each (substep's own list, kept to
    /// save its allocations: empty between substeps).
    struct Carried {
        ServerObject* o;
        float x, y;
    };
    std::vector<Carried> substepCarried_;
    /// updateWorld's lists (the players, the motion's contacts, what decays, what's deleted), kept
    /// to save their allocations: each emptied as it's used.
    struct FrameLists {
        std::vector<ServerObject*> players;
        std::vector<Contact> contacts;
        std::vector<ServerObject*> decayed;
        std::vector<std::uint32_t> deleted;
    };
    FrameLists frameLists_;
    /// DeleteMark's count as the deletions pass last looked (updateWorld). (One never equal to a
    /// count the first time: the first pass always walks.)
    std::optional<std::uint32_t> deletionsLookedAt_;
    bool playersPresent_ = false;
    bool countNextFrame_ = true; ///< (the last tick's end left the game unpaused)
    bool updatedWorld_ = false;  ///< (the last frame ran updateWorld)
    bool ranUpdate_ = false;     ///< (the last frame took the update branch)
    std::minstd_rand rng_; ///< the physics' (two bodies on one spot)
    /// A match over: the frame its 10 s wait ends (there in ms).
    std::optional<std::uint32_t> gameOverEnd_;
    std::uint32_t gameOverHold_ = 0; ///< holdGameOver's (noxcpp's map vote)
    std::uint32_t gameOverStockEnd_ = 0; ///< the wait's stock end (its 10 s), under any hold
    /// The camper alarm's last look (0 at start-up, kept
    /// from map to map).
    std::uint32_t camperCheckedAt_ = 0;
    bool nextMapDue_ = false;
    bool mapLoadRequested_ = false;
    std::function<void()> packetHook_;
    std::function<void()> updateServerHook_;
    std::function<void()> mapLoadHook_;
    std::function<void()> clientsHook_;
    PlayerAnimations playerAnimations_;
    QuestMaps questMaps_; ///< (World::questMaps points here)
    std::function<void(Fight&, ServerObject&)> questSendSave_;    ///< (World's, given each map)
    std::function<void(Fight&, ServerObject&)> questSaveAndKick_;
    std::function<bool(const std::string&, ServerObject*)> saveGame_; ///< (the host's; none, saved)
    HostClient* hostClient_ = nullptr;
    ModeHooks* modeHooks_ = nullptr; ///< (World::modeHooks, given each map)
    std::filesystem::path saveFolder_; ///< (Game_GetWorkDir: where Save\ is; none, no saved copies)
    bool firstLoadFromSave_ = false;   ///< (startFromSave: the next load without an old map)
    std::string firstSoloPath_;
    bool firstLoadAwaitsHost_ = false; ///< (the first load had no host: firstLoadHostIn)
    bool firstLoadSoloMap_ = false;    ///< (and read a solo map: found no host for team 1)

public:
    /// A solo game's save (the game saved as `name`, the exit that asked or none);
    /// false when it fails. None set: every save succeeds (and nothing's written).
    void setSaveGame(std::function<bool(const std::string&, ServerObject*)> save) { saveGame_ = std::move(save); }
    /// The save set (a mode wraps it).
    const std::function<bool(const std::string&, ServerObject*)>& saveGameHook() const noexcept { return saveGame_; }
    /// The game's folder a solo game's saves are under (Save\WORKING\<map>\<map>.map: a visited
    /// map's saved copy loads instead of the map); none: maps always load afresh.
    void setSaveFolder(std::filesystem::path folder) { saveFolder_ = std::move(folder); }
    /// The next map load (the game's first) is a saved game's, as the character screen's Load
    /// starts one (windowSelCharProc's 0x1F6: gameSetSwitchSolo(1),
    /// gameSetNoMPFlag(1), the solo path, mapLoadOrSaveMB(1)): read as a
    /// visited map's saved copy (its ScriptData, no GLOBAL or MapInitialize) and no AUTOSAVE asked.
    void startFromSave(std::string soloPath)
    {
        firstLoadFromSave_ = true;
        firstSoloPath_ = std::move(soloPath);
    }
    /// The host's player (slot 0x1F) made after the game's first load had none: soloMapLoaded as
    /// the exe's first map load runs it, its host made before the load (the host's creation,
    /// then servInitialMapLoad): a solo map's read put it in team 1
    /// (soloTeam), then the host's creatures told it and
    /// switch-to-solo off; a saved game's (startFromSave) first the host on the SaveGameLocation
    /// with its script id, the monsters' saved references and the owners resolved, the timers'
    /// objects found (script_activatorResolveObjs). False (nothing done) when the last load wasn't a first one
    /// without the host, or there's still no host. No frame may run between the load and this.
    bool firstLoadHostIn();
    /// The host's own client (host_client.hpp), kept from map to map; null: none.
    void setHostClient(HostClient* client)
    {
        hostClient_ = client;
        world_.hostClient = seenHostClient();
    }
    /// noxcpp: a NoX Core mode's hooks inside the frame (mode_hooks.hpp; World::modeHooks), kept
    /// from map to map; null: none (every call site plays as the game's).
    void setModeHooks(ModeHooks* hooks)
    {
        modeHooks_ = hooks;
        world_.modeHooks = hooks;
        world_.hostClient = seenHostClient();
    }
    ModeHooks* modeHooks() const noexcept { return modeHooks_; }
    /// The host's own client as set (null: none), whatever a mode puts in its place.
    HostClient* ownHostClient() const noexcept { return hostClient_; }
    /// The host's client's book flight in a solo game (solo.hpp): its start
    /// (bookFillAll: levelUpMoment(0, kind), kind 2 a spell, 3 an ability, 4 the
    /// other slot search) and its landing (levelUpEnd, Game_SetPaused(0)). Out of
    /// a solo game nothing.
    void bookFlightBegins(int kind);
    void bookFlightLands();
    /// The host's side of a quest's saved players (World::questSendSave / questSaveAndKick), kept
    /// from map to map.
    void setQuestSaveHooks(std::function<void(Fight&, ServerObject&)> send, std::function<void(Fight&, ServerObject&)> saveAndKick)
    {
        questSendSave_ = std::move(send);
        questSaveAndKick_ = std::move(saveAndKick);
        world_.questSendSave = questSendSave_;
        world_.questSaveAndKick = questSaveAndKick_;
    }
    /// The player animations' lengths (from thing.bin; tests may set them).
    PlayerAnimations& playerAnimations() noexcept { return playerAnimations_; }
    /// The quest's maps (the host fills them from the game's maps: QuestMaps::build).
    QuestMaps& questMaps() noexcept { return questMaps_; }
    /// A new player's outfit (unitInitPlayer -> playerMakeDefItems).
    void giveDefaultItems(ServerObject& unit);
    /// A player picking an item up (Obj_TryPickup as MSG_TRY_GET calls it).
    bool pickUp(ServerObject& unit, ServerObject& item);
    /// The items context for this frame (dropping, equipping).
    Items items() { return Items{world_, things_, options_.modifiers, rng_, clock_.frame()}; }
    /// The fighting context for this frame.
    Fight fight() { return Fight{world_, rng_, clock_.frame(), &things_, options_.modifiers}; }
    /// The spells context for this frame (MSG_TRY_SPELL, the speaking, the spells).
    Spells spells() { return Spells{world_, things_, options_.modifiers, rng_, clock_.frame()}; }
    /// MSG_TRY_ABILITY: a warrior's ability (abilities.hpp).
    void tryAbility(ServerObject& unit, int ability);
    /// A new player's unit as this game makes it (its class's numbers, its level's in a solo
    /// game; its spells).
    void makePlayer(ServerObject& unit, std::uint8_t classId, std::uint8_t level = 10)
    {
        server::makePlayer(unit, classId, options_.gameData, options_.gameFlags, level);
    }
};

} // namespace nox::server
