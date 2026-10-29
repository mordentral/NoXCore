// A game hosted by noxsdl itself: noxcore's server (GameHost) on a UDP port in this process,
// that noxsdl then joins at 127.0.0.1 as any client would (and others on the network can join
// too). Its console runs the host's commands.
//
// The threads and what each owns
// ------------------------------
// The server thread (run) owns the GameHost, the edit session director, a mode's server side and
// the
// socket, and all they hold. Each millisecond it takes the server lock for one pump: the jobs posted to it, the
// datagrams waiting, GameHost::tick (the substeps due by its own clock, so a 60-120 Hz update rate
// goes out evenly), the director's tick, then the datagrams out. It lets the lock go between pumps
// and never waits for the main thread's frame (but for a solo save's park, below).
//
// The main thread (SDL's) owns the client: the LiveSession and its HeadlessClient, Cinematics, the
// HUD and the windows, the inventory, the audio, the sprite cache (whose decoder threads are its
// own business). It holds the server lock only inside withGame, never across its frame. It
// reaches the game we host in three ways:
//   withGame(fn)  fn(GameHost&) at once, the server lock held while fn runs: what needs an answer,
//                 or must be in place before the main thread goes on (the Server Settings window,
//                 a solo game's pause, save request and load, the in-game menu's check). Never the
//                 main thread's own slow work inside it (a map load, an image, a movie): the server
//                 waits meanwhile. Re-entrant on the main thread; called on the server thread (a
//                 client call run while the main thread is parked) it just runs fn. Called from a
//                 queued client call while the server waits in its tick for the main thread to
//                 park (EndGame's unpause, ahead of a save's park), it runs fn without the lock:
//                 the server is stopped in that wait, as GAME.EXE's tick is inside the call.
//   post(job)     job on the server thread at its next pump, before the tick: a fade's `done`, the
//                 Server Settings' apply, the console's host commands (command(): what they print
//                 comes back at a later beginFrame). In the order posted.
//   the views     gameFlags() and soloPath(): what the main thread reads every frame without the
//                 lock, published by each pump (and each withGame) before the datagrams go out.
// game() itself is only for the server thread or inside withGame.
//
// The host's own client (HostClient: GAME.EXE's server calling its client directly)
// -------------------------------------------------------------------------------
// GameHost reaches our client (Cinematics' HostClient) only through LocalHost's bridge:
//   - A call that changes the client (a fade, the widescreen, the map intro, the music, a voice,
//     the inventory's weapons cleared, the console cleared) is queued for the main thread, in
//     order with the datagrams the server sends our client: each datagram to it leaves a marker in
//     the queue (host_queue.hpp), and the main thread runs the calls ahead of a marker just before
//     the client takes that datagram (datagramArriving, from LiveSession), and those ahead of the
//     first unmatched marker once it has taken what came (datagramsTaken) and at its frame's start
//     (beginFrame). Before a batch the client's own pending
//     screen messages are handled (beforeCalls: Cinematics' queue), so a call's effect comes after
//     everything the server sent before it and before everything it sent after it, as in
//     GAME.EXE, whose server and client share one thread.
//   - What the server reads of the client from frame to frame (the widescreen on or moving, a
//     briefing up, the map drawn, an award flying, the inventory grid's room) comes from a view the
//     main thread publishes (publishClient: after it runs queued calls, before its client sends,
//     at its frame's end), with the bridge's own guesses on top for calls still queued (a
//     WideScreen read back in the tick that asked for it sees it).
//   - A solo save or load needs the client there and then (saveStarting, the client's .plr
//     sections, the inventory window, the minimap's walls and markers): the server thread parks
//     the main thread (a park in the queue, served like a call: once the client has taken every
//     datagram sent before it, so the client is as GAME.EXE's would be in that tick; a datagram
//     lost on the way doesn't hold it past the socket's drain or the next frame, nor a wait for
//     the server lock at all) and calls the client directly while it's parked, letting it go
//     after saveClientSections or gameLoaded, or at the pump's end. Only a solo game saves or
//     loads so, with nobody else waiting on its server.
//   - On the main thread (GameHost used inside withGame) the bridge runs whatever is queued, then
//     calls the client directly.
// No deadlock: the server thread waits on the main thread only for a park, and the main thread
// waits on the server thread only for the lock, serving parks meanwhile (or, inside a queued
// call with a park waiting, not waiting at all: withGame above).
//
// Shutdown: the destructor (main thread) stops and joins the server thread; what's still queued
// either way is dropped. setHostClient(nullptr) (Cinematics going) drops the calls queued for it.
// The thread starts pumping at the first beginFrame, so whatever the main thread sets up right
// after the constructor (its LiveSession, the host peer, the host client) is in place first.
#pragma once

#include "host_queue.hpp"
#include "inventory_room.hpp"

#include "noxcore/modes/mode.hpp"
#include "noxcore/net/udp_socket.hpp"
#include "noxcore/server/edit_session.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/hosted_mode.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

struct HostSettings {
    std::string map = "Estate";        ///< a map's name (the game's maps folder) or a .map path
    std::uint16_t port = 18590;
    std::string name = "noxsdl";
    std::uint32_t mode = 0x100;        ///< arena; 0x20 CTF, 0x40 flagball, 0x10 KotR, 0x400 elimination
    int maxPlayers = 16;
    std::uint16_t lessons = 0;         ///< the lesson limit (0: none)
    std::uint8_t minutes = 0;          ///< the time limit (0: none)
    bool teams = false;
    /// nox.cfg's server settings, as the exe hosts with them: each mode's lesson and time limits
    /// (LessonLimit / TimeLimit; here in modes.hpp's order: arena, elimination, CTF, KotR,
    /// flagball; when set they replace `lessons` / `minutes`), the message of the day sent (SendMessageOfTheDay:
    /// motd.txt), the map cycle (MapCycle), items respawning (ItemRespawn: server option 2), the
    /// camper alarm (CamperAlarm: 0x2000).
    std::optional<std::array<std::uint16_t, 5>> lessonLimits;
    std::optional<std::array<std::uint8_t, 5>> timeLimits;
    bool sendMotd = false;
    bool mapCycle = true;
    bool itemRespawn = false;
    bool camperAlarm = false;
    /// noxcpp's update rate to noxsdl clients (30, 60, 90, 120: GameHost::Options::multiplier x 30).
    int updateRate = 60;
    bool mapVote = true; ///< noxcpp's map vote (GameHost::Options::mapVote)
    /// Hosted while logged in to XWIS (GameHost::Options::wolGame).
    bool wolGame = false;
    bool testCommands = true;          ///< noxcpp's "nx" test commands on our console
    std::string sysopPassword;
    /// A solo game (the campaign: --solo): monster.bin's SOLO numbers, our player the host's
    /// (slot 0x1F), no network game (GameHost::Options::solo).
    bool solo = false;
    /// A quest game hosted (setQuest(1): the main menu's Solo Quest): GameHost::Options::quest.
    bool quest = false;
    /// A NoX Core mode hosted by its id (noxcore/modes' registry: Host Custom's):
    /// its server side made from the registry, the options its own before the game's made; its own
    /// map first (`map` unused when it names one), its generated maps deleted when the host
    /// closes. Empty: none. Its settings, by key.
    std::string modeId;
    nox::modes::OptionValues modeOptions;
    /// With modeId: `map` hosted even when the mode's configure names a map of its own (the map
    /// editor's play-test of a mod on the editor's map).
    bool mapOverMode = false;
    /// The folder a solo game's Save\ is under (Game_GetWorkDir's in the exe; empty: the game's).
    std::filesystem::path saveRoot;
    /// The character screen's Load (windowSelCharProc's 0x1F6): the saved game
    /// (already copied to WORKING; map its map's copy) the game starts from, its Player.plr
    /// loaded onto our player as it's made (GameHost::Options::startFromSave). Empty: a new game.
    std::string continueSlot;
    /// A solo game's character file (clientCurSave: a new character's Save\WORKING\Player.plr,
    /// a continued one's slot's), loaded onto our player as it's made
    /// (GameHost::Options::hostPlayerFile). Empty: a continued slot's own, else none.
    std::filesystem::path characterFile;
    /// The map editor's play-test sandbox: players can't die (GameHost::Options::playersCantDie).
    bool playersCantDie = false;
    /// The server's floats figured as the original's x87 (GameHost::Options::exactMaths; off: VD-37).
    bool exactMaths = true;
    /// The maps folder the game's later loads find their maps in (GameHost::Options::mapsDir: a
    /// quest's, an exit's, the map list's); empty: the game's maps folder. (The map editor's
    /// play-test: its own folder.)
    std::filesystem::path mapsDir;
    /// A quest's maps, these alone (GameHost::Options::questMapsOnly: the map editor's quest
    /// play-test, its map); empty: the maps folder's quest maps.
    std::vector<std::string> questMapsOnly;
    /// The port bound on the loopback alone (--host-loopback: a local test's host nothing outside
    /// this computer reaches); else on every interface, as the game's.
    bool loopbackOnly = false;
    /// The game's password (GameHost::Options::password: the stock prompt for stock clients);
    /// empty: none.
    std::string password;
    /// A collaborative map editor session hosted (Host Custom > Map Editor, File > Host a Session:
    /// edit_session.hpp): its map (`mapName`, already in `mapsDir`, where it's published), its
    /// settings; `map` and `mode` unused (the director picks them), `name` the host's for the
    /// listed "[EDIT] <name>". Not a solo or quest game.
    std::optional<nox::server::EditSessionDirector::Options> editSession;
};

/// What the server reads of our own client from frame to frame (the host client's state GAME.EXE's
/// server reads directly), as the main thread last published it (LocalHost::publishClient).
struct HostClientView {
    bool mapDrawn = true;          ///< gameDisableMapDraw off (Cinematics::mapDrawn)
    bool wideScreenOn = false;     ///< HostClient::wideScreenOn
    bool wideScreenMoving = false; ///< HostClient::wideScreenMoving
    bool briefingShown = false;    ///< HostClient::briefingShown
    bool bookFlying = false;       ///< HostClient::bookFlying
    bool grid = false;             ///< an inventory grid (else inventoryRoom answers kNoGrid)
    noxsdl::RoomCells cells{};     ///< its cells, for inventoryRoom
    /// The inventory window's secondary weapon and last quiver (a quest save's inventoryWindow:
    /// the cells are a solo save's, read with the main thread parked).
    std::uint16_t secondaryWeapon = 0;
    std::uint16_t lastQuiver = 0;
    std::uint64_t applied = 0;     ///< (LocalHost's: the last queued client call run before it)
};

class LocalHost {
public:
    /// Opens the port and loads the map, then starts the server's thread, which waits for the
    /// first beginFrame. Throws on failure (the port taken, no such map). Main thread.
    LocalHost(const nox::things::ThingDb& things, const nox::things::ModifierDb* modifiers, const nox::things::GameData& gameData,
              const nox::strings::StringTable* strings, const std::filesystem::path& gameDir, const HostSettings& settings);
    /// Stops and joins the server's thread (main thread, outside withGame). A mode host's levels
    /// still being built ahead are left to finish (finishReleased).
    ~LocalHost();
    /// Waits for the levels mode hosts left building as they went: at quit, before the thing
    /// database goes (the builds read it).
    static void finishReleased();
    LocalHost(const LocalHost&) = delete;
    LocalHost& operator=(const LocalHost&) = delete;

    // ---------------------------------------------------------------------- the main thread's
    /// At each frame's start: the server let go on the first; the host client's calls queued ahead
    /// of the first datagram not yet taken run (and a park with no datagram ahead of it, or one
    /// that already waited a frame).
    void beginFrame();
    /// A datagram from the server about to go to our client (LiveSession::beforeDatagram): the
    /// host client's calls queued before the server sent it run first.
    void datagramArriving(std::span<const std::uint8_t> datagram);
    /// Our client has taken every datagram waiting (LiveSession::afterDatagrams): the calls queued
    /// after the last it took run, and a park the server waits for is served.
    void datagramsTaken();
    /// The client's view (setClientView's) published for the server: after queued calls ran,
    /// before the client sends (LiveSession::beforeSend), at the frame's end.
    void publishClient();
    /// What gathers the client's view (main thread, or the server thread while the main one is
    /// parked); and what runs before a batch of queued calls (the client's own pending screen
    /// messages handled first, for their order). Empty: none.
    void setClientView(std::function<HostClientView()> gather, std::function<void()> beforeCalls);

    /// fn(GameHost&) with the server lock held, its result returned (see above).
    template <class F>
    decltype(auto) withGame(F&& fn)
    {
        if (onServerThread() || mainHolds_) {
            return std::forward<F>(fn)(*game_);
        }
        MainLock lock(*this);
        return std::forward<F>(fn)(*game_);
    }
    /// fn(GameHost&) on the server's thread at its next pump (either thread may post).
    void post(std::function<void(nox::server::GameHost&)> job);
    /// A closure for the server's side (a fade's `done`) at its next pump (either thread).
    void post(std::function<void()> job);

    /// The server's game flags as its last pump left them (World::gameFlags).
    std::uint32_t gameFlags() const noexcept { return gameFlags_.load(std::memory_order_acquire); }
    /// A solo game's saved map the client reads at its map change (World::soloPath while
    /// World::noMultiplayer is up), as the last pump left them; nullopt otherwise.
    std::optional<std::filesystem::path> soloPath() const;
    /// The client read it (gameChangeMap's map read): the server's "not multiplayer" lowered.
    void soloPathRead();

    /// The server's thread's last error, taken (it logs and runs on, as the loop's pump did).
    std::optional<std::string> takeError();
    /// A console line as the host (issuer: our own player's net code), run on the server's thread
    /// at its next pump; what it printed handed to `printed` on the main thread at a later
    /// beginFrame (a map load it starts doesn't hold the frame up).
    void command(std::string line, std::uint32_t issuer, std::function<void(std::vector<std::string>)> printed);
    /// What the server's console printed on its own (a sysop's commands...) since the last call.
    std::vector<std::string> takeConsole();
    /// A console line to the hosted NoX Core mode (ServerMode::console: a script mode's "mode
    /// reload keep"), run on the server's thread at its next pump; its answer handed to `answered`
    /// on the main thread at a later beginFrame (empty: no mode, or it didn't take the line).
    void modeCommand(std::string line, std::function<void(std::vector<std::string>)> answered);

    // The host leaving (GameHost::beginQuit, endSession): MSG_SERVER_QUIT to the other players at
    // the quit's start, their acks awaited, then (the destructor) every stream closed with
    // SERVER_CLOSE, so stock and noxsdl clients leave at once instead of waiting on a silent server.
    /// Starts the quit on the server's thread (its next pump); any thread, once.
    void beginQuit();
    /// Whether the quit started and every ack awaited came (or none was).
    bool quitDone() const noexcept { return quitDone_.load(std::memory_order_acquire); }
    /// A host leaving its game (main thread): our client let go (the server calls into it no
    /// more), the quit begun, then kept running in the background till its players acked (at most
    /// kQuitWaitMs) and destroyed (pumpRetired, finishRetired), its streams closed.
    static void retire(std::unique_ptr<LocalHost> host);
    /// Each frame (main thread): retired hosts whose players all acked, or that waited long
    /// enough, destroyed.
    static void pumpRetired();
    /// At quit (main thread): the retired hosts waited for (at most `waitMs` more) and destroyed.
    static void finishRetired(std::uint32_t waitMs);
    /// The longest a retired host waits for its players' MSG_SERVER_QUIT_ACK (the exe's host waits
    /// behind its "Shutting down server..." dialog with no limit).
    static constexpr std::uint32_t kQuitWaitMs = 5000;
    /// A mode's generated maps swept, once (retire, before a new game of it may write them again;
    /// else the destructor).
    void sweepModeMaps();
    std::string address() const { return "127.0.0.1:" + std::to_string(port_); }
    std::uint16_t port() const { return port_; }
    /// The port the settings asked for was another program's here: the game is on port() instead
    /// (UdpSocket::openHosting).
    bool portMoved() const { return askedPort_ != 0 && port_ != askedPort_; }
    std::uint16_t askedPort() const { return askedPort_; }
    /// The game itself: only on the server thread, or inside withGame on the main thread.
    nox::server::GameHost& game();
    /// Hosting a mode whose director picks the maps (a NoX Core mode or a map editor session): no
    /// Server Settings.
    bool directed() const noexcept { return serverMode_ != nullptr || edit_.has_value(); }
    /// The NoX Core mode hosted (HostSettings::modeId); empty: none. Set once, as it's made.
    const std::string& modeId() const noexcept { return modeId_; }
    /// Hosting a map editor session (HostSettings::editSession).
    bool editSession() const noexcept { return edit_.has_value(); }
    /// A map editor session's director (nullptr: not one). As game(): only on the server thread,
    /// or inside withGame on the main thread.
    nox::server::EditSessionDirector* editDirector() noexcept { return edit_ ? &*edit_ : nullptr; }

    // The host's own client (GAME.EXE's solo game: its server and client one process).
    /// Attaches it (GameHost::setHostClient, through the bridge); nullptr detaches, dropping
    /// what's queued for it.
    void setHostClient(nox::server::HostClient* client);
    /// Our client's game socket (GameHost::setHostPeer): the datagrams to it are the ones the host
    /// client's calls keep their order with.
    void setHostPeer(std::optional<nox::net::Endpoint> at);
    /// Game_SetPaused (1 / 0) on a solo game, or the timed pause (-1: forgotten,
    /// then unpaused); `why` for a NoX Core mode that may hold or refuse it (ModeHooks::mayPause).
    void pause(int how, nox::server::PauseReason why = nox::server::PauseReason::Menu);
    /// The saved game `slot` (Save\<slot>) loaded; false where it fails.
    bool loadSoloGame(const std::string& slot);
    /// setSaveFileName(name) and a save asked for now (the server's next tick saves it when its
    /// host may: the Server's part).
    void requestSave(const std::string& name);
    /// Whether the host may open its in-game menu (solo.hpp hostMayOpenMenu).
    bool mayOpenMenu();
    /// Game_IsPaused.
    bool paused();
    /// The client's stage (Gui_Brief_ShowLockScreen's chapter + 1: World::briefingByte).
    void setStage(std::uint8_t stage);

private:
    class Bridge;
    friend class Bridge;

    using ToMain = noxsdl::HostQueue::Entry;
    /// The park's state (toMainMutex_): asked by the server, the main thread parked, let go.
    enum class Park : std::uint8_t { None, Asked, Parked, Released };

    /// The server lock held by the main thread for withGame's fn (or not needed: the server
    /// waiting for the queued call that's running to be done and the main thread parked).
    struct MainLock {
        explicit MainLock(LocalHost& host) : host_(host), locked_(host_.lockForMain()) {}
        ~MainLock() { host_.unlockForMain(locked_); }
        MainLock(const MainLock&) = delete;
        MainLock& operator=(const MainLock&) = delete;
        LocalHost& host_;
        bool locked_;
    };

    bool onServerThread() const noexcept { return std::this_thread::get_id() == serverId_; }
    /// The server lock taken (true), or the server found stopped for a park it waits for while
    /// a queued call runs (false: no lock needed).
    bool lockForMain();
    void unlockForMain(bool locked);
    /// The server waits for a park the main thread will serve once the call it's running is done.
    bool serverAwaitsPark();

    /// Its datagrams in, its frames run, its datagrams out (the server's thread, with the lock).
    void pump(std::uint32_t nowMs);
    void run();
    /// World's flags and the solo path published for the main thread (with the lock).
    void publishView();
    /// The server's copy of the client's view brought up to date (server thread).
    void refreshClientCopy();

    // The server thread's side of the queue.
    std::uint64_t queueCall(std::function<void()> call);
    void queueMarker(std::span<const std::uint8_t> bytes);
    /// The main thread parked (true at once when it already is; false when it can't be: stopping).
    bool parkMain();
    /// A park let go (the client's view gathered first, the main thread still parked).
    void releaseMain();

    // The main thread's side.
    /// Runs taken entries in order (the calls, a park), then publishes the client's view.
    void runTaken(std::vector<ToMain>& taken, bool beforeCalls);
    /// Every queued call run now (the bridge on the main thread, before it calls the client).
    void runAllQueued();
    /// One taken call run (its number noted as applied).
    void runCall(ToMain& e);
    /// A park asked for served (from a wait for the server lock).
    void serveParks();
    /// Waits, parked, till the server lets go.
    void parkHere();

    std::timed_mutex mutex_;   ///< the server lock
    bool mainHolds_ = false;   ///< the main thread is inside withGame (only the main thread reads or sets it)
    bool touched_ = false;     ///< the bridge called the client directly inside withGame (main thread's)
    // (main thread's) Running queued calls: how deep, and whether the batch ends in a park the
    // server waits for; a park left waiting at a frame's start for its datagrams.
    int inCalls_ = 0;
    bool parkTaken_ = false;
    bool parkWaited_ = false;
    std::thread::id serverId_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> quitBegun_{false};
    std::atomic<bool> quitDone_{false}; ///< (the server thread's pump: GameHost::quitDone)
    bool readMapChoices_ = false; ///< (a network game's: the maps' modes read as the thread starts)
    std::mutex startMutex_;
    std::condition_variable startCv_;
    bool started_ = false;
    std::mutex errorMutex_;
    std::optional<std::string> error_;
    std::mutex consoleMutex_;
    std::vector<std::string> console_;
    std::mutex jobsMutex_;
    std::vector<std::function<void()>> jobs_;
    /// What the server's jobs hand back to the main thread (a console command's lines), run at
    /// beginFrame (jobsMutex_).
    std::vector<std::function<void()>> results_;
    std::thread thread_;

    // The server-to-main queue.
    std::mutex toMainMutex_;
    std::condition_variable parkCv_;
    noxsdl::HostQueue queue_;         ///< (toMainMutex_)
    Park park_ = Park::None;
    bool parked_ = false;             ///< (server thread's) the main thread is parked for it
    std::atomic<std::uint64_t> appliedSeq_{0}; ///< the last call the main thread ran
    std::function<HostClientView()> gather_;
    std::function<void()> beforeCalls_;
    std::mutex clientMutex_;
    HostClientView client_;           ///< as published (clientMutex_)
    HostClientView clientCopy_;       ///< the server thread's copy for its pump
    std::optional<nox::net::Endpoint> hostPeer_; ///< (with the server lock)

    // The views.
    std::atomic<std::uint32_t> gameFlags_{0};
    mutable std::mutex viewMutex_;
    bool noMultiplayer_ = false;
    std::string soloPath_;

    /// Held by the closures that may outlive us (a fade's `done` wrapped for the server's side).
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::unique_ptr<Bridge> bridge_;
    std::filesystem::path gameDir_;
    std::filesystem::path saveRoot_;
    std::optional<nox::things::MonsterDb> monsterDb_;
    std::optional<nox::things::SoundSets> soundSets_;
    /// A NoX Core mode's server side (HostSettings::modeId: the registry's; before game_: the host's
    /// hooks hold it), and its id (its log lines').
    std::unique_ptr<nox::modes::ServerMode> serverMode_;
    std::string modeId_;
    bool swept_ = false; ///< the mode's generated maps swept (sweepModeMaps: at retire, else as it goes)
    /// A map editor session's director (before game_, as a mode's: the host's prepareMap and
    /// edit messages hold it).
    std::optional<nox::server::EditSessionDirector> edit_;
    std::unique_ptr<nox::server::GameHost> game_;
    std::optional<nox::net::UdpSocket> socket_;
    std::uint16_t port_ = 0;
    std::uint16_t askedPort_ = 0; ///< HostSettings::port (0: any free one)
};
