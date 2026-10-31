// The co-op campaign (NoX Core's own mode): friends join a NoX Core client's hosted campaign game
// and play the Warrior, Wizard or Conjurer story chain together. docs/vanilla_deviations.md lists
// what differs from the solo game.
//
// The game logic stays the solo game's: the world runs the solo flags (0x800 without 0x2000), the
// campaign's maps and scripts unchanged, the host's player slot 0x1F as GetHost. The director adds
// the guests on top, through the core's generic hooks (server/mode_hooks.hpp, server/hosted_mode.hpp,
// GameHost::Options' soloGuests, onJoin / onLeave and noxcppMessages):
//   - Same class only: the campaign's class; a joiner of another is refused by the lobby
//     (the Access settings' classes refused: the client's own "class not allowed").
//   - Only the host sets off story scenes: GetHost stays the host, so the scripts'
//     IsCaller(GetHost()) gates are the host's; a guest who touches one is told so. Anyone may talk
//     to an NPC at any time, but a talk that can play a scene (talkPlaysScene) is the host's: a
//     guest's click on one is refused with a notice. Every guest is told this as it joins. As the
//     host's scene begins, the others far from it are brought to it.
//   - The party moves as one where the scripts move "the player": Frozen and MoveObject on the
//     host hold or move every player (a scene freezes the party; a scripted teleport takes it);
//     the journal is shared (JournalEntry / Edit / Delete on the host are every player's);
//     HasItem on the host is true when any player holds the thing; AwardSpell on the host is
//     every player's (they're of its class).
//   - XP and gold: kill XP to the killer and gold to whoever takes it, as the game; the scripts'
//     rewards to the host (GiveXp, ChangeGold's gains) are every player's, a charge the host's.
//   - Guests are team 1 ("COOP") with the host, placed by it as they come.
// Each event a player sees goes through the presentation layer: a NoX Core client
// (GameHost::identified) gets MSG_NOXCPP_COOP (coop_proto.hpp), a stock one the stock equivalent.
#pragma once

#include "noxcore/coop/coop_proto.hpp"
#include "noxcore/modes/mode.hpp"
#include "noxcore/script/script_file.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/mode_hooks.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::coop {

/// The lines the rules tell players (English; a stock client gets them as the server's lines: in
/// the middle of its screen for a few seconds, and in its console).
inline constexpr std::string_view kRulesLine = "Co-op campaign: only the host can start story scenes.";
inline constexpr std::string_view kHostOnlyLine = "Only the host can set this off.";
inline constexpr std::string_view kTalkRefusedLine = "Only the host can start this talk: it can play a story scene.";
/// A click on an NPC another player is talking to ("<name>" and this).
inline constexpr std::string_view kTalkTakenLine = " is talking to them.";

/// Whether script function `function` (an NPC's dialog start or end: -1 none) can play a scene:
/// it, or a function it calls or hands to a builtin (a timer, a callback, SetDialog), calls a
/// builtin that puts one up (WideScreen, Blind, ImmediateBlind, StartupScreen, EndGame).
bool talkPlaysScene(const script::ScriptFile& file, std::int32_t function);

/// The game flags a stock guest is told (a quest's network game: quest 0x1000, network 0x2000,
/// 0x10000, the game running 0x20000) in place of the world's solo mode.
inline constexpr std::uint32_t kStockGuestFlags = 0x1000u | 0x2000u | 0x10000u | 0x20000u;

/// A class's name as the campaign's ("Warrior", "Wizard", "Conjurer").
std::string_view className(std::uint8_t playerClass);
/// The campaign's first map for a class (War01A, Wiz01A, Con01A).
std::string_view firstMapOf(std::uint8_t playerClass);
/// The name the game is listed by ("Coop Wizard": the Access page's 15 letters).
std::string listedName(std::uint8_t playerClass);
/// A player's key from game to game: its account and name ("account/name"; no account: the
/// name), as the Undead Tide keys its runners.
std::string runnerKey(const server::HostedPlayer& p);

class CoopDirector;

/// The host's own client as a co-op campaign's server sees it (ModeHooks::hostClient): each call
/// passed on to the host's own client (none: as with no client: a fade's `done` at once), and
/// each one a guest should see told to the guests by the presentation layer (the bars, fades,
/// black, the map intro, music, a line's voice, the ending). What the server reads of the client
/// is the host's.
class CineRelay final : public server::HostClient {
public:
    explicit CineRelay(CoopDirector& director) : director_(director) {}
    server::HostClient* own = nullptr;

    void blind() override;
    void unblind() override;
    void fadeToBlack(int frames, bool overMenu, std::function<void()> done) override;
    void setMapDrawn(bool drawn) override;
    bool wideScreenMoving() const override { return own && own->wideScreenMoving(); }
    void wideScreen(bool on) override;
    void playMapIntro(bool immediate) override;
    void endGame(int kind) override;
    void musicPush() override;
    void musicPop() override;
    void music(int track, int volume) override;
    void playVoice(std::string_view file, int volume) override;
    void stopAllFades() override;
    bool wideScreenOn() const override { return own && own->wideScreenOn(); }
    bool briefingShown() const override { return own && own->briefingShown(); }
    bool bookFlying() const override { return own && own->bookFlying(); }
    int inventoryRoom(std::uint16_t typeId, int count) const override { return own ? own->inventoryRoom(typeId, count) : kNoGrid; }
    InventoryWindow inventoryWindow() const override { return own ? own->inventoryWindow() : InventoryWindow{}; }
    void clearInventoryWeapons() override
    {
        if (own) {
            own->clearInventoryWeapons();
        }
    }
    bool wallDrawn(int x, int y) const override { return own && own->wallDrawn(x, y); }
    bool minimapMarked(std::uint16_t netCode) const override { return own && own->minimapMarked(netCode); }
    void saveStarting() override
    {
        if (own) {
            own->saveStarting();
        }
    }
    std::optional<bool> saveClientSections(const std::filesystem::path& plr, const ClientSave& info) override
    {
        return own ? own->saveClientSections(plr, info) : std::nullopt;
    }
    bool loadClientSections(const std::filesystem::path& plr, ClientLoad& out) override { return !own || own->loadClientSections(plr, out); }
    /// (A saved game's load: the director picks the party up from it as the map comes.)
    void gameLoaded() override;

private:
    CoopDirector& director_;
};

/// (Its registry entry, coop_mode.cpp, makes it as the mode's server side: a ServerMode, the rest
/// of which it leaves as the defaults.)
class CoopDirector final : public server::ModeHooks, public modes::ServerMode {
    /// (coop_links.hpp: its records' links, carried as each map loads: mapLoaded.)
    template <class>
    friend struct server::Links;

public:
    struct Options {
        /// The campaign's class (0 warrior, 1 wizard, 2 conjurer): every player this class.
        std::uint8_t campaignClass = 1;
        /// The most players, the host among them (the campaign's rooms are small: 4).
        int maxPlayers = 4;
        /// The seed of the director's own draws (where a guest is put): never the game's.
        std::uint32_t seed = 1;
        /// A new player behind the party is given the experience to the party's lowest level.
        bool catchUp = true;
        /// The monsters' health and the damage players take scaled by the party's size (off: the
        /// solo game's numbers); each player past the first adds these shares (Normal's).
        bool scaleDifficulty = true;
        float healthPerPlayer = 0.6f;
        float damagePerPlayer = 0.15f;
        /// The host's difficulty (0 Easy, 1 Normal, 2 Hard, 3 Nightmare): the scale's start (a
        /// party of one) and its shares a player. Normal is the scale above, the solo game's own
        /// numbers when alone.
        int difficulty = 1;
    };
    /// A difficulty's scale: the monsters' health and the damage players take with one player,
    /// and what each player past the first adds.
    struct Difficulty {
        float health, damage, healthPerPlayer, damagePerPlayer;
    };
    /// Easy, Normal (its shares Options'), Hard, Nightmare.
    static constexpr Difficulty kDifficulties[4] = {
        {0.75f, 0.75f, 0.4f, 0.10f},
        {1.0f, 1.0f, 0.6f, 0.15f},
        {1.25f, 1.15f, 0.8f, 0.20f},
        {1.5f, 1.3f, 1.0f, 0.25f},
    };

    explicit CoopDirector(Options options);

    const Options& options() const noexcept { return options_; }

    // ---- HostedMode (ServerMode's)
    /// The host's options a co-op campaign's: a solo game with room for the guests, the lobby's
    /// class refused but the campaign's, the hooks. The map stays the host's (a new campaign's
    /// first map, or a saved one's).
    std::filesystem::path configure(server::GameHost::Options& o) override;
    void attach(server::GameHost& host) override;
    void tick(server::GameHost& host, std::uint32_t nowMs) override;
    void detach(server::GameHost& host) override;

    // ---- ModeHooks
    std::optional<int> builtin(server::ScriptHost& script, std::uint32_t id, script::Vm& vm) override;
    /// A kill's experience, given to the one who earned it, given to the rest of the party too:
    /// each their own award (the game's, by their own experience), so nobody falls behind.
    void killExperienceGiven(server::Fight& fight, server::ServerObject& victim, server::ServerObject& earner) override;
    bool mayStartDialog(server::Fight& fight, server::ServerObject& player, server::ServerObject& npc) override;
    void mapLoaded(server::Server& server) override;
    /// A map change's carry of its records' links (GameHost::Options::carryLinks: coop_links.hpp's
    /// rules), right after the switch.
    void carryLinks(const server::KeyCarry& carry);
    /// No world-wide pause for one player's level-up, award flight or the host's menus while a
    /// guest is in (the campaign's rule); a chapter screen's, a map load's and a fade's stand.
    bool mayPause(server::PauseReason why) override;
    /// A chapter screen's pause held till every NoX Core guest is done reading (or the host skips,
    /// or a minute and a half goes by).
    bool mayUnpause(server::PauseReason why) override;
    server::HostClient* hostClient(server::HostClient* own) override;
    /// The party gathers at an exit: the first player on it starts a 30 s countdown; the exit
    /// goes when every living player stands on it, when the countdown's over, or at the host's
    /// "?go". Alone (or the only one alive): as the game.
    bool exitTouched(server::Fight& fight, server::ServerObject& exit, server::ServerObject& player) override;
    /// Only a party wipe loses: a player who dies while another lives keeps the pending save, runs
    /// no PlayerDeath functions and is back by the party's checkpoint 10 s on; the last one's
    /// death is the game's (its PlayerDeath functions: DeathScreen, the host's load menu).
    bool playerDeath(server::Fight& fight, server::ServerObject& player, server::DeathStep step) override;
    /// A reliable message to one player (GameHost::Options::forPeer): a guest's MSG_DESTROY_WALL by
    /// the id its own map file gives the wall.
    std::optional<std::vector<std::uint8_t>> forPeer(std::uint8_t peer, std::span<const std::uint8_t> m);
    /// One key ring: a party member's key for a door another of the party touches.
    server::ServerObject* sharedKey(server::Fight& fight, server::ServerObject& door, server::ServerObject& toucher,
                                    std::string_view keyType) override;
    /// A book or guide used up as it's picked up: the same one put back where it lay while another
    /// of the party hasn't had it yet.
    void pickedUp(server::World& w, server::ServerObject& taker, server::ServerObject& item, float x, float y) override;
    /// A spell book put back is for those below the level it was put back at.
    bool mayPickUp(server::World& w, server::ServerObject& taker, server::ServerObject& item) override;
    /// A player's CLIENT_READY (GameHost::Options::onClientReady): the party's team told between
    /// it and the other NoX Core players (tellTeam); a guest's view of a map it loaded from its own
    /// file put right (the things the client draws from it that the world no longer has hidden:
    /// MSG_DESTROY_OBJECT with the map number's high bit).
    void clientReady(server::GameHost& host, std::uint8_t peer);
    /// Whether the party lost (every one dead at once), till the next map's load.
    bool wiped() const noexcept { return wipe_; }
    /// The players down, waiting for their respawn (by peer).
    std::size_t down() const noexcept { return downs_.size(); }
    /// The party's checkpoint: where the host came into this map.
    std::optional<std::pair<float, float>> checkpoint() const noexcept { return checkpoint_; }
    /// An exit's gathering under way (the exit's extent).
    std::optional<std::uint32_t> gathering() const noexcept { return gather_ ? std::optional<std::uint32_t>(gather_->exit) : std::nullopt; }
    /// The map numbers (with the high bit) a guest's own file draws that the world no longer has
    /// (as of the last guest's CLIENT_READY).
    const std::vector<std::uint16_t>& hiddenStatics() const noexcept { return hidden_; }

    // ---- the presentation layer's cinematics (CineRelay's calls)
    void cine(const coopmsg::Cine& c);
    /// A stock game message to every guest (the music's MSG 0xE5 - 0xE7, a fade's 0xE4).
    void toGuests(const std::vector<std::uint8_t>& bytes, bool stockOnly);
    /// The bars are up (a scene under way): late joiners come in with them, frozen.
    bool sceneOn() const noexcept { return bars_; }
    /// The NoX Core guests still reading a chapter screen (by peer).
    std::size_t stillReading() const noexcept { return reading_.size(); }

    // ---- saves and returning players
    // The campaign's saves are the solo game's (Save\WORKING, AUTOSAVE, SAVE%04d under the co-op
    // root) with a Party folder in each: every guest's character (<file key>.plr, the server's
    // sections as a quest player's) and party.txt (who, where, the creatures each owns). A guest
    // the campaign knows (by runnerKey) gets its campaign character back as it joins, and after a
    // saved game's load; a new one behind the party is caught up; each save sends a NoX Core
    // guest its campaign character to keep.
    /// The host's client's saved game's load (CineRelay::gameLoaded): the party taken from it as
    /// its map comes in.
    void gameLoaded();
    /// A player's own character loaded from its file (GameHost::Options::onCharacter).
    void characterIn(server::GameHost& host, std::uint8_t peer, bool loaded);
    /// The runners the campaign has kept (party.txt's), by key.
    struct Record {
        std::string key;
        std::string name;
        std::uint8_t playerClass = 0;
        std::string map;
        float x = 0;
        float y = 0;
        std::vector<std::int32_t> owned; ///< its creatures' script ids
    };
    const std::map<std::string, Record>& records() const noexcept { return records_; }
    /// WORKING's Party folder (none: no save folder).
    std::filesystem::path partyFolder() const;
    /// Whether a thing a script hands "the player" is every player's (Pickup(GetHost(), it)): not a
    /// book (the party's award of it, rewardToParty), and not one the party keeps single (a key:
    /// the party's ring shares it; War01A's sponsorship letter, a later talk's check; the Oblivion
    /// staves, unique).
    static bool isSharedReward(const server::ServerObject& item);

    // ---- GameHost's callbacks (set by configure)
    void joined(server::GameHost& host, std::uint8_t peer, server::ServerObject& unit);
    void leaving(server::GameHost& host, std::uint8_t peer, server::ServerObject& unit);
    void onMessage(server::GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m);
    /// A player's chat line: the host's "?skip" ends a chapter screen's wait (taken).
    bool onChat(server::GameHost& host, std::uint8_t peer, std::u16string_view text);

    /// A player in the party, in the order they came.
    struct Member {
        std::string key;  ///< runnerKey
        std::string name;
        std::uint8_t peer = 0;
        server::ObjectRef extent;
        bool host = false;
        bool identified = false;
        bool welcomed = false; ///< told the campaign and the rules (once its map was loaded)
        bool restored = false; ///< its campaign character given back (a returning player)
        bool characterIn = false; ///< its own character loaded from its file
        bool caughtUp = false;    ///< its catch-up looked at
        std::uint64_t welcomedAt = 0; ///< (the world's ticks)
        /// Its character of another class than the campaign's: told so at these ticks, let go 3 s
        /// on (0: not).
        std::uint64_t otherClassAt = 0;
    };
    const std::vector<Member>& party() const noexcept { return party_; }

private:
    /// The game's builtins the rules answer, by id (looked up by name once).
    struct Ids {
        std::uint32_t frozen = 0;
        std::uint32_t moveObject = 0;
        std::uint32_t journalEntry = 0;
        std::uint32_t journalDelete = 0;
        std::uint32_t journalEdit = 0;
        std::uint32_t hasItem = 0;
        std::uint32_t awardSpell = 0;
        std::uint32_t isCaller = 0;
        std::uint32_t startupScreen = 0;
        std::uint32_t deathScreen = 0;
        std::uint32_t tellStory = 0;
        std::uint32_t giveXp = 0;
        std::uint32_t changeGold = 0;
        std::uint32_t getGold = 0;
        std::uint32_t pickup = 0;
    };

    server::World& world();
    /// A script value as an object (server_scriptValToObjectPtr's: -1 the caller, -2 the
    /// trigger, else a script id).
    server::ServerObject* scriptObject(script::Vm& vm, std::int32_t value);
    /// The party's live units, the host's first.
    std::vector<server::ServerObject*> partyUnits();
    bool isHostUnit(const server::ServerObject* o);
    Member* memberOfUnit(const server::ServerObject& unit);

    /// The builtin run for every player in place of the host (its first argument): each
    /// player's result in turn, folded by `fold` (none: the host's kept).
    int fanOut(server::ScriptHost& script, std::uint32_t id, script::Vm& vm, std::vector<std::uint32_t> args, bool anyTrue);
    /// MoveObject on the host: the host where the script says, each other player at a clear
    /// point about it.
    int moveParty(server::ScriptHost& script, std::uint32_t id, script::Vm& vm, std::vector<std::uint32_t> args);

    // ---- the presentation layer (one game logic, each client told its own way)
    /// A notice to one player: a NoX Core client's as MSG_NOXCPP_COOP kNotice, a stock client's
    /// as a line (MSG_TEXT_MESSAGE from no one).
    void notice(const Member& m, coopmsg::Notice kind, std::string_view text);
    /// A guest who touched what only the host sets off told so, at most once in a while for the
    /// same thing.
    void hostOnly(server::ServerObject& guest, std::uint32_t what);
    /// The roster told to every NoX Core player when it changed.
    void tellRoster(server::GameHost& host);
    void tellHello(server::GameHost& host, const Member& m);
    /// A guest put in team 1 with the host.
    void intoTeam(server::ServerObject& unit);
    /// At `ready`'s CLIENT_READY: each other member's team to it, and its own to each other NoX Core
    /// member already in (MSG_TEAM_MSG sub 1, NoX Core clients only). A solo game tells no team
    /// change (addTeamMember sends only in a network game), so a client knew only the members its
    /// own join listed: the host's client none of the guests (their names grey over them, where a
    /// guest's client showed the host's in the party's colour).
    void tellTeam(const Member& ready);

    /// A chapter screen sent to the guests: StartupScreen's / DeathScreen's chapter and kind.
    void chapterToGuests(std::uint8_t chapter, bool startup);
    /// The chapter's wait over: the pause the host's screen ended let go.
    void releaseChapter();
    void tellReadyState();
    /// A player's talk (`talker`'s: the host's or a guest's) mirrored to the others as it opens and
    /// closes.
    void mirrorDialog(const server::ServerObject& talker, const std::string& textId);
    void watchDialog();
    /// The host's scene begun (its bars up): every other living player farther than
    /// kSceneGatherNear from the host put at a clear point about it, to see it there.
    void gatherForScene();
    /// A player's click on an NPC `talker` is talking to: told so, at most once in 3 s.
    void talkTaken(server::ServerObject& player, const server::ServerObject& talker);
    /// The guest whose talk is running (the VM's caller a guest, its trigger an NPC with a talk), or
    /// none: its gold and its things are its own, not the host's.
    server::ServerObject* guestTalker(script::Vm& vm);
    static constexpr float kSceneGatherNear = 120.0f;
    /// One player of the party told `bytes` (a game message, MSG_NOXCPP_COOP's to NoX Core
    /// clients only), through the world's messages: in order with the game's, held for one still
    /// loading its map.
    void tell(const Member& m, std::vector<std::uint8_t> bytes);
    /// A level-up moment that couldn't pause the world ended a second on (its state would hold
    /// the player).
    void endUnpausedLevelUp();
    /// A guest coming in during a scene: the bars, and held as the party is.
    void joinScene(const Member& m);
    /// A line to every player (its notice; a stock one's as the server's line).
    void noticeAll(coopmsg::Notice kind, std::string_view text);
    /// Each tick: the players due back respawned by the checkpoint; a gathering nobody stands on
    /// any more given up.
    void respawnDue();
    void gatherTick();
    /// The things a client draws from the map's own file (by map number): pristine_, at the map's load.
    void readPristine(server::Server& server);
    /// Those of them the world no longer has (a visited map's saved copy, or gone before a guest
    /// came): hidden_.
    void findHiddenStatics();
    /// A player back on its feet by the checkpoint (the game's respawn, keeping what it holds).
    void respawnPlayer(server::ServerObject& unit);
    std::string nameOf(const server::ServerObject& unit) const;

    // ---- saves (coop_saves.cpp)
    /// The core's save wrapped: the party written into WORKING first (the save's copy takes it
    /// along), then the game's save, then each NoX Core guest sent its character.
    bool save(const std::string& name, server::ServerObject* exit);
    void readRecords();
    void writeRecords();
    /// A guest's character and record kept in WORKING's Party folder (its server sections as a
    /// quest's SAVEDATA carries them, for the guest: none, failed).
    std::vector<std::uint8_t> keepGuest(const Member& m, server::ServerObject& unit);
    /// A known runner's campaign character put on its unit (and, `place`, where it was); whether.
    bool restoreGuest(Member& m, server::ServerObject& unit, bool place);
    /// After a saved game's map: each guest's character from the save, the dead back up.
    void restoreParty();
    /// The creatures (script ids) owned by nobody given to `to`.
    void giveCreatures(const std::vector<std::int32_t>& owned, server::ServerObject& to);
    /// A new player behind the party given the experience to the party's lowest level.
    void catchUp(Member& m, server::ServerObject& unit);
    void sendCharacter(const Member& m, std::span<const std::uint8_t> plr);
    // ---- balance and keys (coop_balance.cpp)
    /// The monsters' health and World::modeDamage by the party's size: every monster at once when
    /// the party changed (`force`, or a new size), else those not seen yet every 5 s.
    void rescale(bool force);
    /// Every monster's health at `health` times its own (1: the game's own, as a save writes them).
    void applyScale(float health);
    /// A leaving player's keys to the host.
    void keysToHost(server::ServerObject& unit);
    /// A guest's keys gone at a chapter's start (StartupScreen's, the host's done by the game).
    void keysGone(server::ServerObject& unit);
    struct Base {
        std::int32_t scriptId = 0;
        std::uint16_t type = 0;
        std::uint16_t own = 0; ///< its own max health
    };
    server::RefMap<Base> baseHealth_; ///< each monster's own max health (by extent)
    server::RefMap<std::uint8_t> copies_; ///< spell books put back (extent -> the level they're for)
    /// A book, ability book or field guide a script hands "the player" (Pickup on the host, or on
    /// a guest in its own talk), as it was before it was used up.
    struct RewardBook {
        std::string type;
        std::string useFn;
        std::uint32_t bookEntry = 0;
        std::string guideCreature;
        std::uint32_t subclasses = 0;
    };
    /// Whether `item` is one (used up as it's picked up in a solo game).
    static bool isRewardBook(const server::ServerObject& item);
    /// The reward the script gave `taker` given to each other player of the party who hasn't got
    /// it (below the taker's level of a spell now; an ability or creature not known), as its own
    /// use of the same book (the game's award, its messages, its book's flight).
    void rewardToParty(server::ServerObject& taker, const RewardBook& book);
    /// Any other thing a script hands "the player" (gold, gear, potions, food...), as it was
    /// before it was taken.
    struct RewardItem {
        std::string type;
        std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
        std::uint8_t charges = 0;
        std::uint8_t maxCharges = 0;
        std::uint32_t goldAmount = 0;
    };
    /// `taker` took it from the script: each other player of the party gets its own of the same
    /// (its pickup: the game's messages, its gold's amount, a potion drunk as the taker's would be).
    /// One a player can't carry (the game's "carrying too much") isn't left on the floor.
    void itemToParty(server::ServerObject& taker, const RewardItem& item);
    /// A new player joining mid-campaign (catchUp): every spell, creature and ability any other
    /// player of the party knows, at the best level any has it (told as a .plr's load tells them).
    void partyBookTo(server::ServerObject& unit);
    bool scriptedReward_ = false; ///< (a script's Pickup of a book under way: no copy put back on the floor)
    /// The party's health told to each other (MSG_TEAM_MSG 0x0C, the quest's team health): what
    /// each player was last told of each other one ((its peer, the other's extent) -> share + 1).
    /// (Each also the health and most its share was from, and the pass that last saw it.)
    struct HealthTold {
        int share = 0;
        std::uint16_t health = 0;
        std::uint16_t maxHealth = 0;
        std::uint32_t pass = 0;
    };
    std::map<std::pair<std::uint8_t, server::ObjectKey>, HealthTold> healthTold_; ///< (not links: emptied as each map loads)
    std::uint32_t healthPass_ = 0;
    void tellPartyHealth();
    /// The host's talk in a scene shown to the others as the game's own dialog (MSG_DIALOG 3, its
    /// close 4), view-only: their answers find no talk of theirs and do nothing.
    void showSceneDialog(const server::ServerObject& host, std::size_t mark);
    void watchSceneDialog();
    server::RefSet sceneViewers_;          ///< the players shown the host's scene talk (extents)
    server::ObjectRef sceneTalker_;        ///< the host's unit while it's shown
    float scaledHealth_ = 1.0f;
    std::uint64_t lastRescale_ = 0;
    std::function<bool(const std::string&, server::ServerObject*)> coreSave_;
    std::map<std::string, Record> records_;
    bool loadPending_ = false;

    Options options_;
    Ids ids_;
    server::GameHost* host_ = nullptr;
    CineRelay relay_{*this};
    bool bars_ = false;                    ///< the host's WideScreen(1) last
    bool sceneGathered_ = false;           ///< the party gathered for the scene under way (this map's)
    std::set<std::uint8_t> reading_;       ///< NoX Core guests (peers) still on the chapter screen
    std::uint8_t readingOf_ = 0;           ///< how many were sent it
    bool hostDoneReading_ = false;         ///< the host's screen asked to unpause meanwhile
    bool releasing_ = false;               ///< (our own unpause)
    std::uint32_t refused_ = 0;            ///< the pauses refused (a bit per PauseReason): their next unpause too
    std::uint32_t chapterAt_ = 0;          ///< the frame the chapter's wait began
    bool mirrorOpen_ = false;
    server::ObjectRef mirrorNpc_;
    server::ObjectRef mirrorTalker_;       ///< the mirrored talk's player (extent)
    // (P3: exits, deaths, revisited maps)
    struct Gather {
        server::ObjectRef exit;           ///< the first exit touched
        std::string to;                   ///< where it leads (its key: side-by-side exits are one way)
        std::uint64_t since = 0;          ///< the world's ticks as it began
        std::uint64_t lastTouch = 0;
        server::RefMap<std::uint64_t> standing; ///< each player's last touch (by extent)
        bool warned = false;              ///< the "10 s left" told
    };
    std::optional<Gather> gather_;
    bool goNow_ = false;                  ///< the host's "?go"
    std::optional<std::uint64_t> scriptMovedAt_; ///< the world's ticks of a script's last MoveObject of the party
    std::map<std::uint8_t, std::uint64_t> downs_; ///< a dead player's peer -> the tick it's back
    bool wipe_ = false;
    server::ObjectRef wipeBy_;            ///< the last one down (its PlayerDeath functions the party's)
    bool wipeRan_ = false;                ///< the wipe's PlayerDeath functions run
    std::optional<std::pair<float, float>> checkpoint_;
    std::vector<std::uint32_t> pristine_;
    std::vector<std::uint16_t> hidden_;
    std::map<std::uint16_t, std::uint16_t> wallIds_; ///< our wall id -> the map file's (where they differ)
    std::vector<std::uint16_t> goneWalls_;           ///< the file's breakable walls our copy lacks (its ids)
    std::map<std::uint8_t, std::multiset<std::uint16_t>> fileNumbered_; ///< our own 0x3A by the file's id, on their way
    std::vector<Member> party_;
    std::minstd_rand rng_;
    /// When each guest was last told of a host-only thing ((its extent, the thing's) -> frame).
    std::map<std::pair<server::ObjectKey, server::ObjectKey>, std::uint32_t> toldAt_; ///< (not links: a guest's leave drops its own)
    coopmsg::Roster lastRoster_;
    bool rosterDirty_ = true;
    /// The party's creatures (kCreatures, the friend dot over them): each party member's
    /// summons and charmed creatures, looked for again only when the party changes, a map loads,
    /// a creature report goes out (World::creatureReports) or one of them is no longer the party's;
    /// the whole list to each NoX Core player whose last differs, while it has reliable room.
    struct PartyCreature {
        server::ObjectRef extent;
        server::ObjectRef owner;
    };
    std::vector<PartyCreature> partyCreatures_;
    std::vector<std::uint16_t> partyCreatureCodes_; ///< (their net codes, sorted: the message's)
    std::map<std::uint8_t, std::vector<std::uint16_t>> creaturesTold_; ///< by peer: the list it was last sent
    bool creaturesDirty_ = true;
    std::uint32_t creatureReportsSeen_ = 0;
    void tellCreatures(server::GameHost& host);
};

} // namespace nox::coop
