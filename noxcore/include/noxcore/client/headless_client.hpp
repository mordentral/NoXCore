// A headless 1.2b client: joins a vanilla server, stays connected, moves and chats.
//
// Join, as the vanilla client does it (GAME.EXE):
//   1. lobby TRY_JOIN -> JOIN_OK, then the transport handshake and CLIENT_ACCEPT (transport.hpp)
//   2. MSG_INCOMING_CLIENT 0xAD, reliable (after loading client data)
//   3. the server sends USE_MAP; the client checks its copy of the map against the CRC stored in
//      the map header (mapValidateMB), loads it and sends MSG_CLIENT_READY 0xC0,
//      reliable (gameChangeMap). Map download isn't implemented yet.
//   4. deathmatch games start new players as observers (Plr_CreateFromJoinData); the Action
//      control (code 6) respawns them (Plr_UpdateObserver).
// While in game it answers IMPORTANT (0xAB with the frame of this datagram's timestamp),
// reports aliases for the update stream (0xA5), accepts XFER transfers such as the MOTD
// (accept / ack each chunk / done, all reliable), and sends its controls every frame: always
// Orientation, plus MoveForward while moving, as Cli_ProcessControlEvents does.
// No sockets here: the caller moves datagrams.
#pragma once

#include "noxcore/server/xfer.hpp"
#include "noxcore/client/navigation.hpp"
#include "noxcore/client/server_silence.hpp"
#include "noxcore/client/world.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/net/player_input.hpp"
#include "noxcore/net/transport.hpp"
#include "noxcore/net/update_stream.hpp"
#include "noxcore/things/modifier.hpp"

#include <memory>
#include <array>
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

namespace nox::things {
struct ThingDb;
}

namespace nox::map {
struct MapFile;
}

namespace nox::modes {
class ClientMode;
}

namespace nox::client {

struct PlayerProfile {
    std::string name = "NoXPlayer";    ///< UTF-8, at most 24 characters are sent
    std::uint8_t playerClass = 0;      ///< 0 warrior, 1 wizard, 2 conjurer
    bool female = false;
    std::string serial = "0000000000000000000000"; ///< CD key, 22 ASCII characters
    std::string account;               ///< WOL account name, may be empty
    std::string password;              ///< sent if the server asks for one
    std::uint32_t screenWidth = 640;
    std::uint32_t screenHeight = 480;
    std::array<std::uint8_t, 15> colors{};  ///< 5 x RGB: hair, skin, mustache, goatee, beard
    std::array<std::uint8_t, 5> clothing{}; ///< colour indexes
    /// Joining a quest game (game flag 0x1000): CLIENT_ACCEPT's flags bit 7, which the server
    /// requires to match whether its quest is running (Plr_CreateFromJoinData).
    bool quest = false;
};

struct ClientEvent {
    enum class Kind : std::uint8_t {
        Log,          ///< progress and diagnostics
        InGame,       ///< CLIENT_READY sent; the server now streams the world
        Chat,         ///< netCode = speaker, text = message (name in `name` when known)
        PlayerJoined, ///< netCode, name
        PlayerLeft,   ///< netCode, name
        Motd,         ///< text
        Notice,       ///< a game notice (MSG_INFORM): kills, flag events, string ids
        Disconnected, ///< text = reason
        Kill,         ///< the kill feed (MSG_INFORM 14): netCode = victim, attacker (0: they died)
    };
    Kind kind = Kind::Log;
    std::string text;
    std::uint16_t netCode = 0;
    std::string name;
    bool team = false; ///< Chat: team chat (the game prefixes Guirank.c:team, not resolved here)
    std::uint16_t attacker = 0; ///< Kill: who did it (0: nobody, they died)
    std::uint16_t assist = 0;   ///< Kill: who helped (0: nobody)
    std::uint16_t weapon = 0;   ///< Kill: a thing type (killKind 1) or a damage type (killKind 2)
    std::uint8_t killKind = 0;
    std::uint8_t informSub = 0xFF; ///< Notice: the MSG_INFORM sub-code it came from (0xFF: none)
    std::uint8_t textFlags = 0;    ///< Chat: MSG_TEXT_MESSAGE's flags (8 a string id, 0x10 a notice...)
};

class HeadlessClient {
public:
    enum class Stage : std::uint8_t {
        Idle,
        Joining,      ///< lobby and transport handshake
        WaitingMap,   ///< sent INCOMING_CLIENT, waiting for USE_MAP
        Downloading,  ///< fetching the map from the server (REQUEST_MAP ... RECEIVED_MAP)
        InGame,
        Quitting,     ///< OUTGOING_CLIENT sent, waiting for the server to take it
        Closed,       ///< left the game
        Failed,
    };
    /// Why the game ended for us (endReason): what the client shows once it's back at the menus.
    enum class EndReason : std::uint8_t {
        None,           ///< still in, or never got in (joinRefused etc.: the stage says)
        Left,           ///< we left (quit())
        ServerQuit,     ///< the host shut the game down (MSG_SERVER_QUIT, acked)
        ServerClosed,   ///< the server closed our stream (SERVER_CLOSE)
        Kicked,         ///< MSG_KICK_NOTIFICATION
        TimedOut,       ///< MSG_TIMEOUT_NOTIFICATION: the server dropped us as silent
        LostConnection, ///< the lag window's Exit: the server stopped answering
    };

    /// `things` gives the thing classes the update stream needs (may be null: the stream is then
    /// skipped). `mapsDir` is the game's maps folder. Maps it lacks are downloaded from the server
    /// into `mapCache` (<cache>/<name>/<name>.map, looked up there too); an empty `mapCache`
    /// turns downloading off.
    HeadlessClient(PlayerProfile profile, const things::ThingDb* things, std::filesystem::path mapsDir,
                   std::filesystem::path mapCache = {});
    ~HeadlessClient();
    /// (Its modes' client sides hold it by reference: never copied or moved.)
    HeadlessClient(const HeadlessClient&) = delete;
    HeadlessClient& operator=(const HeadlessClient&) = delete;

    /// A NoX Core mode's client side (noxcore/modes: each registered mode's, made with the client;
    /// each idle unless its server plays that mode), by the mode's id; null: none.
    modes::ClientMode* modeSide(std::string_view id) const;
    /// A mode's client side that isn't made with every client (Mode::everyClient off), made now
    /// (once: again, the one made); null: no such mode, or it has none.
    modes::ClientMode* addModeSide(std::string_view id);
    /// A client side that isn't a registered mode's (a script mode's UI: modes/script), kept under
    /// `id` from now on (one there already: kept, `side` let go). The one kept.
    modes::ClientMode* adoptModeSide(std::string id, std::unique_ptr<modes::ClientMode> side);

    // ---- I/O
    void start(std::uint32_t nowMs);
    void receiveLobby(std::span<const std::uint8_t> datagram);
    void receive(std::span<const std::uint8_t> datagram, std::uint32_t nowMs);
    /// How many datagrams have come in (the netstat's packet strip, g_guiFpsPacketCount).
    std::uint64_t datagramsReceived() const { return datagramsReceived_; }
    /// Call often (every few ms); runs a game frame every 33 ms.
    void tick(std::uint32_t nowMs);
    std::vector<net::ClientConnection::Datagram> takeOutgoing();
    /// Chat, notices, the kill feed... since the last call, oldest first. A client that never takes
    /// them keeps about the newest kMaxEvents (the oldest half dropped when it's full).
    std::vector<ClientEvent> takeEvents();
    static constexpr std::size_t kMaxEvents = 1024;

    /// Called with every game message received (after splitting) and queued for sending.
    using Tracer = std::function<void(bool outgoing, std::span<const std::uint8_t> message)>;
    void setTracer(Tracer tracer) { tracer_ = std::move(tracer); }

    // ---- actions
    void say(std::string_view utf8, bool teamOnly = false);
    /// Runs toward `direction` (0-255, Nox's facing units) until stopped with nullopt.
    void move(std::optional<std::uint8_t> direction);
    /// Leaves observer mode (sends the Action control once).
    void spawn();
    /// Runs to (x, y) along a path around the map's walls, and stops within a few units of it.
    void walkTo(int x, int y);
    /// Item and object actions, sent like the vanilla client (unreliable, netCode as known).
    void pickup(std::uint16_t netCode);  ///< MSG_TRY_GET
    void drop(std::uint16_t netCode);    ///< MSG_TRY_DROP at our feet
    void dropAt(std::uint16_t netCode, int x, int y); ///< MSG_TRY_DROP at a world point
    void use(std::uint16_t netCode);     ///< MSG_TRY_USE (potions, scrolls)
    void equip(std::uint16_t netCode);   ///< MSG_TRY_EQUIP
    void unequip(std::uint16_t netCode); ///< MSG_TRY_DEQUIP
    void collide(std::uint16_t netCode); ///< MSG_TRY_COLLIDE (doors, switches, shops)
    /// MSG_INFO_BOOK_DATA: asks what an info book holds (`kind` 1 spell book, 2 lore scroll, 4
    /// ability book), for its tooltip (Gui_Tooltip_GetDrawableText); the answer lands in
    /// World::self().bookContents.
    void askBook(std::uint16_t netCode, std::uint8_t kind);
    /// MSG_TRY_CREATURE_COMMAND (Gui_Summon_CommandItemProc): orders creature `netCode`
    /// (0 all of ours): 0 banish, 1 observe, 3 guard, 4 escort, 5 hunt.
    void commandCreature(std::uint16_t netCode, std::uint8_t command);
    /// Shops and trades (MSG_TRADE, the client's sub-ops as GUIShop.c / GUITrade.c send them):
    /// open the shop of shopkeeper `netCode` (0x15, CliNet_SendOpenShop), leave it
    /// (0x12), buy one item (0x16) or `count` of a type (0x17), sell one (0x18) or `count` of a
    /// type (0x19), repair or recharge one (0x1A), ask what one sells for (0x1C) or what its
    /// repair costs (0x1E); a trade's accept (0x11), cancel (0x0E) and taking an item back (0x10).
    void openShop(std::uint16_t netCode);
    void shopExit();
    void shopBuy(std::uint16_t netCode);
    void shopBuyStack(std::uint16_t typeId, std::uint8_t count);
    void shopSell(std::uint16_t netCode);
    void shopSellStack(std::uint16_t typeId, std::uint8_t count);
    void shopRepair(std::uint16_t netCode);
    void shopAskSellPrice(std::uint16_t netCode);
    void shopAskRepairCost(std::uint16_t netCode);
    void tradeAccept();
    void tradeCancel();
    void tradeWithdraw(std::uint16_t netCode);
    /// MSG_DIALOG: talk to NPC `netCode` (0x01, CliNet_SendDialogStart), or answer the
    /// open dialog (0x02): 0 Done / Next, 1 Yes, 2 No (Gui_NpcDialog_WndProc).
    void dialogStart(std::uint16_t netCode);
    void dialogAnswer(std::uint8_t answer);
    /// Asks to join a team (TEAM 0x0A, CliNet_SendTeamJoinRequest). CTF needs one to
    /// touch flags.
    void joinTeam(std::uint32_t teamId);
    /// Asks to become an observer (MSG_SERVER_CMD 0, the quit menu's Observer Mode).
    void observe();
    /// Chases an object and attacks it (Action every frame, facing it) when within reach;
    /// nullopt stops.
    void hunt(std::optional<std::uint16_t> netCode);
    /// Faces `direction` (0-255) without moving: the vanilla client turns to the cursor every
    /// frame (Cli_ProcessControlEvents). Moving faces the way it goes.
    void face(std::uint8_t direction);
    /// Holds the Action control while `held`: sent every frame, as holding the attack button
    /// does (swing the weapon, or use what's under the cursor).
    void setAttack(bool held);
    /// Jumps (the Jump control, once).
    void jump();
    /// Sends a control without data once, with the next frame's input.
    void sendControl(net::ControlCode code);
    /// noxcpp servers only (LiveSession turns it on after their extended reply): the facing,
    /// moving and cursor are also sent as they change between the 30-a-second input frames (at
    /// most every 8 ms), so the server's next frame takes the newest. One-off controls (attacks,
    /// jumps, spells) keep to the input frames, and walking to a point steers on them as before.
    void setFreshInput(bool on) { freshInput_ = on; }

    /// noxcpp's map vote (server/map_vote.hpp), as a noxcpp server last told us
    /// (MSG_NOXCPP_MAPVOTE 0xF3; a stock server never sends it).
    struct MapVoteState {
        bool open = false;
        std::uint8_t kind = 0;                ///< 0 a match's end, 1 called by a player
        std::vector<std::string> candidates;
        std::vector<int> tally;
        int voters = 0;
        std::optional<int> mine;              ///< our vote
        std::uint32_t endsAtMs = 0;           ///< when it closes, on our clock
        std::optional<int> result;            ///< the closed one's choice (none: the cycle's, or the call failed)
        std::uint32_t version = 0;            ///< bumped at each message
        bool available = false;               ///< a vote can be called here (the server's kAvailable)
    };
    const MapVoteState& mapVote() const noexcept { return mapVote_; }
    /// Multiclass (a NoX Core mode's server says so): every class's spells, abilities,
    /// guides and weapons, a conjurer's mana. Never on a stock server.
    bool multiclass() const noexcept { return multiclass_; }
    void setMulticlass(bool on) { multiclass_ = on; }
    void setMulticlassForTest(bool on) { multiclass_ = on; }
    /// The book is the server's mode's (a script mode's bookIsTheMod, told by MSG_NOXCPP_MODEUI's
    /// kBook, sent only to a client of level 2 or above that named ModeUi): a network game's book
    /// lists only what was granted, not every spell, ability and creature (bookListsEverything).
    /// Never on a stock server.
    bool modeOwnsBook() const noexcept { return modeOwnsBook_; }
    void setModeOwnsBook(bool on) { modeOwnsBook_ = on; }
    /// noxcpp's map editor session (MSG_NOXCPP_EDIT 0xF6, mapedit/session_proto.hpp): each message
    /// from the server handed to `handler` whole (a stock server never sends one); ours sent,
    /// reliable or not (presence: latest wins).
    using EditHandler = std::function<void(std::span<const std::uint8_t> message)>;
    void setEditHandler(EditHandler handler) { setNoxcppHandler(static_cast<std::uint8_t>(net::Op::NoxcppEdit), std::move(handler)); }
    void sendEdit(std::vector<std::uint8_t> message, bool reliable = true) { sendNoxcpp(std::move(message), reliable); }
    /// noxcpp's own messages a NoX Core mode handles itself (its client side's: noxcore/modes
    /// ClientMode; MSG_NOXCPP_EDIT 0xF6: the map editor's session), by opcode: each handed to its
    /// handler whole (a stock server never sends one). Ours sent, reliable or not.
    using NoxcppHandler = std::function<void(std::span<const std::uint8_t> message)>;
    void setNoxcppHandler(std::uint8_t op, NoxcppHandler handler) { noxcppHandlers_[op] = std::move(handler); }
    void sendNoxcpp(std::vector<std::uint8_t> message, bool reliable = true)
    {
        if (reliable) {
            queueReliable(std::move(message));
        } else {
            queueUnreliable(std::move(message));
        }
    }
    /// Our vote for candidate `choice` (0-based), and a vote called: noxcpp servers only.
    void voteMap(int choice);
    void callMapVote();
    /// An emote (MSG_SOCIAL, Cli_ProcessControlEvents): the server plays its animation
    /// and sound. The game's keys allow a taunt or point a second, a laugh two.
    enum class Social : std::uint8_t { Laugh = 1, Taunt = 2, Point = 4 };
    void social(Social kind);
    /// A vote to kick the player of that name (MSG_VOTE sub 0, its name UTF-16 in 25), or its
    /// withdrawal (sub 2).
    void voteKick(std::u16string_view name, bool withdraw = false);

    // ---- the secondary weapon (guiinv.c), as the vanilla client keeps it
    /// The weapon-swap key (client_invAlterWeapon): takes the equipped weapon off; when
    /// the server says it's off, the secondary goes on and the old one becomes the secondary
    /// (else the old one just becomes it). With nothing in hand, puts the secondary on. A quiver
    /// as the secondary swaps quivers instead.
    /// Whether it did anything (the game's NextWeapon sound plays then).
    bool swapWeapons();
    /// A swap under way (a weapon coming off to have the secondary put on, or the secondary
    /// being put on): the weapon panel then shows no fist.
    bool weaponSwapPending() const noexcept { return swappingOut_ != 0 || pendingSecondary_ != 0; }
    /// Makes an inventory item the secondary weapon, or none with 0, and tells the server
    /// (clientSetAltWeapon, MSG_REPORT_SECONDARY_WEAPON).
    void setSecondaryWeapon(std::uint16_t netCode);
    /// The equipped weapon dragged into the inventory: off, becoming the secondary,
    /// or swapped with the secondary if there is one.
    void putAwayWeapon(std::uint16_t netCode);
    /// The secondary weapon's net code, 0 if none.
    std::uint16_t secondaryWeapon() const noexcept { return world_.self().secondaryWeapon; }
    /// g_guiInvLastQuiverNetCode: the last quiver worn (Gui_Inv_OnItemEquipped) or told
    /// (MSG_REPORT_LAST_QUIVER, Gui_Inv_SetLastQuiver).
    std::uint16_t lastQuiver() const noexcept { return lastQuiver_; }
    /// A solo .plr's read (plr_rwInventory, the host's own client): the secondary
    /// weapon and its previous one forgotten (Gui_Inv_SetSecondaryWeapon(0, 0), nothing sent) and
    /// the last quiver (Gui_Inv_SetLastQuiver(0)).
    void clearInventoryWeapons() noexcept
    {
        world_.setSecondaryWeapon(0);
        previousSecondary_ = 0;
        lastQuiver_ = 0;
    }
    /// An item's weapon class bit (ammoCheck: 2 quiver, 4 bow, 8 crossbow...), 0 if
    /// it's no weapon or wand.
    std::uint32_t weaponBit(std::uint16_t typeId) const;
    /// modifier.bin, for which class may use a weapon (a picked-up one becomes the secondary
    /// only if ours may). Without it, any may.
    void setModifiers(const things::ModifierDb* modifiers) { modifiers_ = modifiers; }
    /// The cursor's world position. It's sent (MSG_MOUSE) after a frame's input whenever it
    /// changes (CliNet_QueuePlayerInput); aimed spells go there.
    void setCursor(int x, int y);
    /// The drawable the cursor is on (spriteGetMB: the frontmost one whose shape
    /// holds it, clientOnCursorHover), or none. While there's one, MSG_MOUSE
    /// carries its y for the cursor's (CliNet_QueuePlayerInput): what's aimed at is where
    /// it stands, not where it's drawn (a flying bat, a player's body).
    void setCursorSprite(std::optional<std::uint16_t> netCode) { cursorSprite_ = netCode; }
    /// Casts a spell slot (MSG_TRY_SPELL, CliNet_SendSpellCast): up to 5 spell ids,
    /// cast in order, and the slot's target flag.
    void castSpells(std::span<const std::uint32_t> spells, bool targetFlag = false);
    /// Uses a warrior ability (MSG_TRY_ABILITY, CliNet_SendAbilityUse): 1 berserker
    /// charge, 2 war cry, 3 harpoon, 4 tread lightly, 5 eye of the wolf.
    void useAbility(std::uint8_t ability);
    /// Leaves like the vanilla client: OUTGOING_CLIENT, wait for the server's ack, then
    /// CLIENT_CLOSE (keep calling tick() until stage() is Closed).
    void quit();
    /// How long quit() waits for the server to take OUTGOING_CLIENT before it closes anyway.
    static constexpr std::uint32_t kQuitAckWaitMs = 20'000;
    /// While quit() waits for that ack (Stage::Quitting): how long it has waited so far, in ms.
    std::optional<std::uint32_t> quitWaitedMs() const noexcept;
    /// A quest's leave by a client that isn't the host (client_quit's): the first
    /// time our save is asked for (CliNet_SendRequestSavePlayer: MSG_REQUEST_SAVE_PLAYER 0xC1
    /// with our net code) and the client marked closing: it leaves once the
    /// SAVEDATA comes and is kept, instead of telling the host it's ready;
    /// asked again while closing, it leaves at once.
    void questQuit();
    /// Waiting for our save to leave.
    bool closingForSave() const noexcept { return closingForSave_; }
    /// Why the game ended (None while it goes on).
    EndReason endReason() const noexcept { return endReason_; }
    // The server gone quiet (server_silence.hpp), in a game: the disconnect icon after 2 s, the
    // lag window (discon.wnd) after 22 s.
    /// Watch for it (on by default; off for the host's own client, which the exe never runs it for).
    void setWatchSilence(bool on);
    bool silenceIconShown() const noexcept { return silence_.iconShown(); }
    bool lagWindowShown() const noexcept { return silence_.windowShown(); }
    /// Milliseconds since the server was last heard from, in a game (0 otherwise).
    std::uint32_t quietFor() const noexcept;
    /// The lag window's Wait: closed, back in 20 s if the server stays quiet.
    void lagWait();
    /// The lag window's Exit: we leave (quit(): OUTGOING_CLIENT, its ack waited for up to 20 s, as
    /// the exe's client does after Game_FinishQuit), the end's reason LostConnection.
    void lagExit();
    /// A quest's game over answered (GGOver.wnd's OK, Gui_GameOver_WndProc): MSG_GAUNTLET
    /// 3, back in as an observer.
    void questContinue();
    /// What keeps a SAVEDATA transfer (the received-file handler's action 2: the character's file
    /// and the client's own sections), called before the quest's answer or leave.
    using SaveDataHandler = std::function<void(std::span<const std::uint8_t> data)>;
    void setSaveDataHandler(SaveDataHandler handler) { saveData_ = std::move(handler); }
    /// Our character's file (clientCurSave, as on disk): joining a quest game (profile.quest), its
    /// server sections go to the host as SAVE_SERVER once we're in (the join's after-load
    /// CliNet_SendSaveServerXfer, after INCOMING_CLIENT), so a quit can be answered with our save.
    void setSaveServerFile(std::vector<std::uint8_t> file) { saveServerFile_ = std::move(file); }
    /// That file's server sections to the host now (SAVE_SERVER, as a quest client's at its join):
    /// a quest client's, and a NoX Core mode's guest's when the host greets it (the exe's
    /// client uploads only as a quest client or under its own game flag 0x800). False: no file.
    bool sendCharacter();

    // ---- state
    Stage stage() const noexcept { return stage_; }
    /// The lobby's refusal (SERVER_ERROR's code: net::JoinError), or -1.
    int joinError() const noexcept { return conn_.joinError(); }
    std::uint16_t netCode() const noexcept { return netCode_; }
    std::uint32_t frame() const noexcept { return frame_; }
    const std::string& mapName() const noexcept { return mapName_; }
    /// While the map downloads, how much of it has come (0-100, Cli_MapDownload_OnBlock's
    /// progress); nullopt otherwise.
    std::optional<int> downloadPercent() const noexcept
    {
        if (stage_ != Stage::Downloading) {
            return std::nullopt;
        }
        return download_.size == 0 ? 0 : static_cast<int>(download_.data.size() * 100 / download_.size);
    }
    /// Camera position from the last update stream (the followed object, normally us).
    std::optional<std::pair<int, int>> position() const noexcept { return position_; }
    /// Update streams applied so far: it changes whenever the world gets a new snapshot (one per
    /// server frame the server sends us), for renderers that interpolate between them.
    std::uint32_t snapshotCount() const noexcept { return snapshots_; }
    /// The frames' own update streams applied so far (snapshotCount without a noxcpp server's
    /// substep updates): one per server frame sent us, paused frames too.
    std::uint32_t frameUpdateCount() const noexcept { return frameUpdates_; }
    /// Update streams that came after a newer frame's (out of order): what they introduced kept,
    /// nothing moved back, no snapshot.
    std::uint32_t lateUpdateCount() const noexcept { return lateUpdates_; }
    /// The server time of the newest snapshot, in frames: frame() and, after a noxcpp server's
    /// substep update (MSG_NOXCPP_SUBSTEP), its fraction k / m.
    double frameTime() const noexcept { return frame_ + substep_; }
    /// A noxcpp server's substeps a frame as its last substep update said (1: none seen).
    int substepsPerFrame() const noexcept { return substepsPerFrame_; }
    const std::map<std::uint16_t, std::string>& players() const noexcept { return players_; }
    /// Everything the client knows about the game (milestone 4).
    const World& world() const noexcept { return world_; }
    /// The transport (tests: its key and sequence numbers, to build a server's datagram by hand).
    const net::ClientConnection& connection() const noexcept { return conn_; }
    /// Tests: a message handled as if the server had sent it (noxsdl's --fake-shop and the like).
    void injectForTest(std::span<const std::uint8_t> m) { world_.apply(m, frame_); }
    /// Tests: a decoded datagram's messages, as if received at `nowMs`.
    void payloadForTest(std::span<const std::uint8_t> payload, std::uint32_t nowMs)
    {
        nowMs_ = nowMs;
        handlePayload(payload);
    }
    /// World::setUserColorBase (modifier.bin's UserColor1 id, for the default outfit's colours).
    void setUserColorBase(std::uint8_t id) { world_.setUserColorBase(id); }
    /// World::setOutfitModifiers.
    void setOutfitModifiers(std::uint8_t armorQuality1, std::uint8_t material1, std::uint8_t replenishment1)
    {
        world_.setOutfitModifiers(armorQuality1, material1, replenishment1);
    }
    /// The world's FX messages since the last call (World::takeEffects).
    std::vector<FxEvent> takeEffects() { return world_.takeEffects(); }
    /// The world's sounds and music changes since the last call (World::takeAudio).
    std::vector<AudioCue> takeAudio() { return world_.takeAudio(); }
    /// The spellbook's awards and revokes since the last call (World::takeBookEvents).
    std::vector<BookEvent> takeBookEvents() { return world_.takeBookEvents(); }
    /// The map's wall grid (cells with a wall in them), built when the map loads.
    const NavGrid& nav() const noexcept { return nav_; }
    /// Who we joined as (name, class, screen size...).
    const PlayerProfile& profile() const noexcept { return profile_; }
    /// thing.bin, if we were given it.
    const things::ThingDb* things() const noexcept { return things_; }
    /// An object the map places (ObjectData's top-level records: not ones inside another's
    /// inventory): what a player who knows the map knows is there.
    struct MapObject {
        std::uint16_t typeId = 0; ///< thing.bin THNG index + 1
        float x = 0;
        float y = 0;
        std::uint32_t extent = 0;            ///< the map's object number
        std::uint32_t flags = 0;             ///< object flags as the map has them (0x1000000: enabled)
        std::uint8_t team = 0;               ///< the map's team for it (a CTF flag's), 0 none
        std::vector<std::uint8_t> xferExtra; ///< its type's own fields (ObjectXfer::extra)
    };
    const std::vector<MapObject>& mapObjects() const noexcept { return mapObjects_; }
    /// A map's placed objects (top-level ObjectData records whose type thing.bin knows).
    static std::vector<MapObject> mapObjectsOf(const map::MapFile& file, const things::ThingDb& things);
    /// The rides among them for paths: teleporters to their destinations, elevators to their
    /// shafts and back (enabled ones).
    static std::vector<NavGrid::Link> ridesOf(const std::vector<MapObject>& objects, const things::ThingDb& things);
    /// Bumped each time a map is loaded (mapObjects() and nav() are then new).
    std::uint32_t mapLoads() const noexcept { return mapLoads_; }
    /// The file the last map loaded came from (empty before one has).
    const std::filesystem::path& mapPath() const noexcept { return mapPath_; }
    /// The host's own client of a solo game (the exe's server and client one process):
    /// gameChangeMap reads the map from the solo path
    /// (mapFilenameGetSolo, a visited map's saved copy) while the server's "not
    /// multiplayer" flag is up (gameIsNotMultiplayer), checked against USE_MAP's CRC
    /// (mapValidateMB); and every map read ends by lowering the flag
    /// (mapCliReadAll: gameSetNoMPFlag(0)).
    struct SoloMap {
        std::function<std::optional<std::filesystem::path>()> path; ///< the solo path, while the flag is up
        std::function<void()> read;                                 ///< the flag lowered
    };
    void setSoloMap(SoloMap soloMap) { soloMap_ = std::move(soloMap); }
    /// What a game frame's input said (movement prediction replays them): moving (MoveForward),
    /// the facing sent (Orientation), the Action control, and the cursor the server has (MSG_MOUSE,
    /// world units).
    struct SentInput {
        bool move = false;
        std::uint8_t facing = 0;
        bool attack = false;
        std::optional<std::pair<int, int>> cursor;
    };
    /// Game frames' input sent so far, and the last one's.
    std::uint32_t inputFrames() const noexcept { return inputFrames_; }
    const SentInput& lastInput() const noexcept { return lastInput_; }
    /// World::dropPredicted: a renderer drops the client-predicted drawables out of sight.
    void dropPredicted(const std::function<bool(const Drawable&)>& gone) { world_.dropPredicted(gone); }
    /// World::setFadeObjects: a renderer's Fade Objects option.
    void setFadeObjects(bool on) noexcept { world_.setFadeObjects(on); }

private:
    struct XferIn {
        std::uint8_t sendId = 0;
        std::string type;
        std::uint32_t size = 0;
        std::vector<std::uint8_t> data;
        std::uint16_t nextChunk = 1;
    };

    void event(ClientEvent::Kind kind, std::string text, std::uint16_t netCode = 0, std::string name = {});
    void pushEvent(ClientEvent e);
    void handlePayload(std::span<const std::uint8_t> payload);
    void handleMessage(std::span<const std::uint8_t> m, std::uint32_t& datagramFrame);
    void onUseMap(std::span<const std::uint8_t> m);
    void onMapBlock(std::span<const std::uint8_t> m);
    void finishDownload();
    void mapReady(const map::MapFile& file);
    void onXfer(std::span<const std::uint8_t> m);
    void onText(std::span<const std::uint8_t> m);
    void onInform(std::span<const std::uint8_t> m);
    void onMapVote(std::span<const std::uint8_t> m);
    void gameFrame(std::uint32_t nowMs);
    /// MSG_PLAYER_INPUT with these controls, and MSG_MOUSE (u16 x, u16 y).
    void queueControls(std::span<const net::ControlEntry> controls);
    void queueMouse(int x, int y);
    void steer(std::uint32_t nowMs);
    void queueUnreliable(std::vector<std::uint8_t> bytes);
    void queueReliable(std::vector<std::uint8_t> bytes);
    std::optional<std::uint32_t> classOf(std::uint16_t typeId) const;
    /// The secondary-weapon bookkeeping on the server's equip reports (Gui_Inv_OnItemEquipped, Gui_Inv_OnItemDequipped,
    /// Gui_Inv_SetSecondaryWeapon).
    void onEquipReport(std::span<const std::uint8_t> m);
    /// An equipped inventory item whose weapon bit matches `want` (quiver or not).
    std::optional<std::uint16_t> equippedWeapon(bool quiver) const;

    bool classCanUse(std::uint16_t typeId) const;
    const things::ModifierDb* modifiers_ = nullptr;
    std::uint64_t datagramsReceived_ = 0;
    std::uint16_t swappingOut_ = 0;  ///< g_guiInvEquippedWeaponDrawable: the weapon being taken off
    std::uint16_t pendingSecondary_ = 0; ///< it, to become the secondary once the other is on
    std::uint16_t lastQuiver_ = 0;       ///< g_guiInvLastQuiverNetCode: put back on with a bow
    std::uint16_t previousSecondary_ = 0; ///< restored if the server refuses a new one

    PlayerProfile profile_;
    const things::ThingDb* things_;
    std::filesystem::path mapsDir_;
    std::filesystem::path mapCache_;
    std::filesystem::path mapPath_;
    SoloMap soloMap_;
    struct Download {
        std::string name;               ///< map name without extension
        std::uint32_t expectedCrc = 0;  ///< from USE_MAP
        std::uint32_t size = 0;         ///< of the .nxz, from MAP_SEND_START
        std::uint16_t nextBlock = 1;    ///< Cli_MapDownload_Start starts at 1
        bool started = false;           ///< MAP_SEND_START came (blocks before it are dropped)
        std::vector<std::uint8_t> data;
        std::map<std::uint16_t, std::vector<std::uint8_t>> pending; ///< blocks that came early
        std::size_t pendingBytes = 0;   ///< theirs together (with data, never past size)
    } download_;
    /// The most blocks held early at once (a server sends them in order over the reliable stream:
    /// none ever come early from ours or the exe's).
    static constexpr std::size_t kMaxPendingBlocks = 1024;
    net::ClientConnection conn_;
    Stage stage_ = Stage::Idle;
    net::AliasTable aliases_;
    World world_;
    std::uint32_t frame_ = 0;
    std::uint16_t netCode_ = 0;
    std::string mapName_;
    std::optional<std::pair<int, int>> position_;
    std::uint32_t snapshots_ = 0;
    std::uint32_t frameUpdates_ = 0; ///< (frameUpdateCount)
    std::uint32_t lateUpdates_ = 0;  ///< (lateUpdateCount)
    double substep_ = 0;        ///< (noxcpp's substep updates: the newest's k / m)
    int substepsPerFrame_ = 1;
    std::map<std::uint8_t, std::uint32_t> dropped_; ///< datagrams cut short, by the message that ended them
    std::map<std::uint16_t, std::string> players_;
    std::map<std::uint8_t, XferIn> xfers_;
    /// MSG_SEQ_IMPORTANT, as the game takes it (CliNet_QueueSeqImportant):
    /// handled in id order from seqNext_; an id in the 10000 before it is done already; one
    /// waited on for 30 s is skipped to. Both ends start again from 0 with each map.
    struct SeqPending {
        std::vector<std::uint8_t> bytes;
        std::uint32_t atMs = 0;
    };
    std::uint16_t seqNext_ = 0;
    std::map<std::uint16_t, SeqPending> seqPending_;
    void drainSeqImportant(std::uint32_t& datagramFrame);
    std::optional<std::uint8_t> moveDirection_;
    std::uint8_t facing_ = 64; ///< Orientation sent every frame; the last move direction
    std::optional<std::pair<int, int>> walkTarget_;
    std::optional<std::uint16_t> huntTarget_;
    std::uint32_t huntReplanAt_ = 0;
    std::uint8_t quitSeq_ = 0;
    std::uint32_t quitDeadline_ = 0;
    EndReason endReason_ = EndReason::None;
    /// Out of the game on the server's word (its close, a kick, a timeout): failed, `why` the
    /// reason, the lag watch off, a Disconnected event saying `text`.
    void dropped(EndReason why, const char* text);
    ServerSilence silence_;
    bool watchSilence_ = true;
    NavGrid nav_;
    /// The map's breakable walls (DestructableWalls, by MSG_DESTROY_WALL's index) and secret
    /// walls (SecretWalls: their cell, and whether they start open), for syncLiveWalls.
    std::vector<std::pair<int, int>> breakableCells_;
    std::vector<std::pair<std::pair<int, int>, bool>> secretCells_;
    std::uint32_t navWallsVersion_ = 0; ///< the world's wallsVersion nav_ last took
    std::vector<MapObject> mapObjects_;
    std::uint32_t mapLoads_ = 0;
    /// The world's walls now (the Wall spell's, broken ones, secret ones open) into nav_.
    void syncLiveWalls();
    std::vector<std::pair<int, int>> path_;
    std::uint32_t progressAt_ = 0;
    std::pair<int, int> progressFrom_{};
    bool spawnPending_ = false;
    bool attackHeld_ = false;
    bool jumpPending_ = false;
    std::vector<net::ControlCode> pendingControls_; ///< sendControl, with the next frame's input
    std::optional<std::pair<int, int>> cursor_;
    std::optional<std::pair<int, int>> sentCursor_;
    std::optional<std::uint16_t> cursorSprite_; ///< (setCursorSprite)
    /// The cursor as MSG_MOUSE sends it: its y the cursor sprite's while there's one.
    std::optional<std::pair<int, int>> cursorToSend() const;
    std::uint32_t nextFrameAt_ = 0;
    std::uint32_t inputFrames_ = 0;
    SentInput lastInput_;
    // setFreshInput's: the continuous controls as last sent, and when a fresh send last went.
    void sendFreshInput();
    bool freshInput_ = false;
    MapVoteState mapVote_;
    bool multiclass_ = false;
    bool modeOwnsBook_ = false;
    std::uint8_t sentFacing_ = 64;
    bool sentMoving_ = false;
    std::uint32_t freshAt_ = 0;
    std::uint32_t lastQueuedAt_ = 0;
    bool queuedThisFrame_ = false;
    std::uint32_t nowMs_ = 0;
    std::vector<ClientEvent> events_;
    Tracer tracer_;
    SaveDataHandler saveData_;
    std::map<std::uint8_t, NoxcppHandler> noxcppHandlers_; ///< (setNoxcppHandler)
    /// The registered modes' client sides, by id (after the handlers: they go first).
    std::vector<std::pair<std::string, std::unique_ptr<modes::ClientMode>>> modeSides_;
    std::vector<std::uint8_t> saveServerFile_;
    /// Our own sends (SAVE_SERVER), the server's xfer.hpp; made at the first.
    std::unique_ptr<server::Xfers> sends_;
    bool closingForSave_ = false; ///< the client marked closing (questQuit)
};

} // namespace nox::client
