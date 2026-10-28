// LocalHost: builds a GameHost from HostSettings and runs it on its own thread; the server lock,
// the queue to the main thread and the host client's bridge (local_host.hpp has the rules).
#include "local_host.hpp"
#include "mods_folder.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/solo_save.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

// The host's own client as GameHost reaches it (local_host.hpp: what's queued, what's read from
// the client's view, what parks the main thread). Its guesses are the server thread's; `client`
// is set with the server lock held.
class LocalHost::Bridge final : public nox::server::HostClient {
public:
    explicit Bridge(LocalHost& host) : host_(host) {}

    /// The client the calls go to (the main thread's); nullptr: none.
    nox::server::HostClient* client = nullptr;
    /// The guesses forgotten (a client attached or gone).
    void forget() { mapDrawn_ = wideOn_ = wideMoving_ = Guess{}; }

    void blind() override { call([](HostClient& c) { c.blind(); }); }
    void unblind() override { guess(mapDrawn_, true, call([](HostClient& c) { c.unblind(); })); }
    void fadeToBlack(int frames, bool overMenu, std::function<void()> done) override
    {
        if (host_.onServerThread() && !host_.parked_ && !seen(mapDrawn_, &HostClientView::mapDrawn)) {
            // Draw_FadeInScreen with the map not drawn: `done` there and then, in the server's tick
            // (the client's own call logs as Cinematics' does).
            SDL_Log("cinematics: fade to black, %d frames%s", frames, overMenu ? " over the GUI" : "");
            if (done) {
                done();
            }
            return;
        }
        // `done` is the server's: run on its thread (at once there, else posted to it).
        std::function<void()> serverSide;
        if (done) {
            serverSide = [&host = host_, alive = std::weak_ptr<int>(host_.alive_), done = std::move(done)] {
                if (alive.expired()) {
                    return;
                }
                if (host.onServerThread()) {
                    done();
                } else {
                    host.post(done);
                }
            };
        }
        call([frames, overMenu, serverSide](HostClient& c) { c.fadeToBlack(frames, overMenu, serverSide); });
    }
    void setMapDrawn(bool drawn) override { guess(mapDrawn_, drawn, call([drawn](HostClient& c) { c.setMapDrawn(drawn); })); }
    bool wideScreenMoving() const override
    {
        return read(wideMoving_, &HostClientView::wideScreenMoving, [](const HostClient& c) { return c.wideScreenMoving(); });
    }
    void wideScreen(bool on) override
    {
        // (as when the bars got their record: the HUD switched, the bars moving in)
        const auto seq = call([on](HostClient& c) { c.wideScreen(on); });
        guess(wideOn_, on, seq);
        if (on) {
            guess(wideMoving_, true, seq);
        }
    }
    void playMapIntro(bool immediate) override
    {
        guess(mapDrawn_, false, call([immediate](HostClient& c) { c.playMapIntro(immediate); }));
    }
    void endGame(int kind) override
    {
        // (the map off, the records cleared, the HUD back)
        const auto seq = call([kind](HostClient& c) { c.endGame(kind); });
        guess(mapDrawn_, false, seq);
        guess(wideOn_, false, seq);
        guess(wideMoving_, false, seq);
    }
    void musicPush() override { call([](HostClient& c) { c.musicPush(); }); }
    void musicPop() override { call([](HostClient& c) { c.musicPop(); }); }
    void music(int track, int volume) override { call([track, volume](HostClient& c) { c.music(track, volume); }); }
    void playVoice(std::string_view file, int volume) override
    {
        call([file = std::string(file), volume](HostClient& c) { c.playVoice(file, volume); });
    }
    void stopAllFades() override { guess(mapDrawn_, true, call([](HostClient& c) { c.stopAllFades(); })); }

    bool wideScreenOn() const override
    {
        return read(wideOn_, &HostClientView::wideScreenOn, [](const HostClient& c) { return c.wideScreenOn(); });
    }
    bool briefingShown() const override
    {
        return read(Guess{}, &HostClientView::briefingShown, [](const HostClient& c) { return c.briefingShown(); });
    }
    bool bookFlying() const override
    {
        return read(Guess{}, &HostClientView::bookFlying, [](const HostClient& c) { return c.bookFlying(); });
    }
    int inventoryRoom(std::uint16_t typeId, int count) const override
    {
        if (direct()) {
            return client->inventoryRoom(typeId, count);
        }
        const HostClientView& v = host_.clientCopy_;
        return v.grid ? noxsdl::roomIn(v.cells, typeId, count) : kNoGrid;
    }
    InventoryWindow inventoryWindow() const override
    {
        // The cells only count in a solo game's save (read with the main thread parked); a
        // quest's save takes the secondary weapon and quiver alone, from the client's view.
        if (host_.onServerThread() && !host_.parked_ && !(host_.game_->server().world().gameFlags & nox::server::GameFlag::Solo)) {
            InventoryWindow w;
            w.secondaryWeapon = host_.clientCopy_.secondaryWeapon;
            w.lastQuiver = host_.clientCopy_.lastQuiver;
            return w;
        }
        return parked() ? client->inventoryWindow() : InventoryWindow{};
    }
    void clearInventoryWeapons() override { call([](HostClient& c) { c.clearInventoryWeapons(); }); }
    bool wallDrawn(int x, int y) const override { return parked() && client->wallDrawn(x, y); }
    bool minimapMarked(std::uint16_t netCode) const override { return parked() && client->minimapMarked(netCode); }
    void saveStarting() override
    {
        if (parked()) {
            client->saveStarting();
        }
    }
    std::optional<bool> saveClientSections(const std::filesystem::path& plr, const ClientSave& info) override
    {
        if (!parked()) {
            return std::nullopt;
        }
        const std::optional<bool> written = client->saveClientSections(plr, info);
        letGo(); // (the save's last call into its client)
        return written;
    }
    bool loadClientSections(const std::filesystem::path& plr, ClientLoad& out) override
    {
        return parked() && client->loadClientSections(plr, out);
    }
    void gameLoaded() override
    {
        call([](HostClient& c) { c.gameLoaded(); });
        letGo(); // (the load's last call into its client)
    }

private:
    /// What a queued call will set, and the call's number: the client's view says so once the
    /// main thread has run it.
    struct Guess {
        bool value = false;
        std::uint64_t seq = 0;
    };

    /// Whether the client may be called directly: on the main thread (GameHost used inside
    /// withGame: what's queued runs first) or with the main thread parked.
    bool direct() const
    {
        if (!host_.onServerThread()) {
            host_.runAllQueued();
            host_.touched_ = true;
            return true;
        }
        return host_.parked_;
    }
    /// As direct(), the main thread parked first on the server's (false: it can't be, stopping).
    bool parked() const
    {
        if (!host_.onServerThread()) {
            host_.runAllQueued();
            host_.touched_ = true;
            return true;
        }
        return host_.parkMain();
    }
    /// After a save's or load's last call into the client, the parked main thread let go.
    void letGo()
    {
        if (host_.onServerThread()) {
            host_.releaseMain();
        }
    }
    /// A call of the client's: directly where it may be, else queued (its number returned).
    std::optional<std::uint64_t> call(std::function<void(HostClient&)> f)
    {
        if (direct()) {
            f(*client);
            return std::nullopt;
        }
        return host_.queueCall([c = client, f = std::move(f)] { f(*c); });
    }
    static void guess(Guess& g, bool value, std::optional<std::uint64_t> seq)
    {
        if (seq) {
            g = {value, *seq};
        }
    }
    /// The client's view of `field`, or the guess while its call's still queued.
    bool seen(const Guess& g, bool HostClientView::*field) const
    {
        const HostClientView& v = host_.clientCopy_;
        return g.seq > v.applied ? g.value : v.*field;
    }
    template <class F>
    bool read(const Guess& g, bool HostClientView::*field, F&& directly) const
    {
        return direct() ? directly(*client) : seen(g, field);
    }

    LocalHost& host_;
    Guess mapDrawn_, wideOn_, wideMoving_;
};

LocalHost::LocalHost(const nox::things::ThingDb& things, const nox::things::ModifierDb* modifiers, const nox::things::GameData& gameData,
                     const nox::strings::StringTable* strings, const fs::path& gameDir, const HostSettings& settings)
{
    fs::path path = settings.map;
    if (!fs::exists(path)) { // a map's name: the game's maps folder
        path = gameDir / "maps" / settings.map / (settings.map + ".map");
    }
    const nox::modes::Mode* mode = nullptr;
    if (!settings.modeId.empty()) {
        mode = nox::modes::registry().find(settings.modeId);
        if (!mode || !mode->makeServer) {
            throw std::runtime_error("no game mode " + settings.modeId);
        }
    }
    // (a generated mode's lobby is written below; an edit session's map is its own, checked there)
    const bool generated = mode && mode->info.levels == nox::modes::ModeInfo::Levels::Generated;
    if (!generated && !settings.editSession && !fs::exists(nox::resolveNoCase(path))) {
        throw std::runtime_error("no map " + settings.map);
    }
    if (fs::exists(nox::resolveNoCase(gameDir / "monster.bin"))) {
        monsterDb_ = nox::things::loadMonsterDb(gameDir / "monster.bin", settings.solo); // (a solo or network game's numbers)
    }
    if (fs::exists(nox::resolveNoCase(gameDir / "soundset.bin"))) {
        soundSets_ = nox::things::loadSoundSets(gameDir / "soundset.bin");
    }
    nox::server::GameHost::Options o;
    o.serverName = settings.name;
    o.maxPlayers = settings.maxPlayers;
    o.password = settings.password;
    o.gameFlags = (o.gameFlags & ~0x17F0u) | settings.mode;  // the game-type bits cleared, then the chosen mode
    o.gameData = &gameData;
    o.modifiers = modifiers;
    o.monsterDb = monsterDb_ ? &*monsterDb_ : nullptr;
    o.soundSets = soundSets_ ? &*soundSets_ : nullptr;
    o.strings = strings;
    o.lessonLimits.fill(settings.lessons);
    o.timeLimits.fill(settings.minutes);
    if (settings.lessonLimits) {
        std::copy(settings.lessonLimits->begin(), settings.lessonLimits->end(), o.lessonLimits.begin());
    }
    if (settings.timeLimits) {
        std::copy(settings.timeLimits->begin(), settings.timeLimits->end(), o.timeLimits.begin());
    }
    if (settings.teams) {
        o.gameplayFlags |= 4u;  // teams on
    }
    // The message of the day (motd: motd.txt, whole) and whether it's sent.
    if (std::ifstream motd(nox::resolveNoCase(gameDir / "motd.txt"), std::ios::binary); motd) {
        o.motd = std::string(std::istreambuf_iterator<char>(motd), std::istreambuf_iterator<char>());
    }
    o.sendMotd = settings.sendMotd;
    o.mapCycleOn = settings.mapCycle;
    if (settings.itemRespawn) {
        o.serverOptions |= 2u;
    }
    if (settings.camperAlarm) {
        o.serverOptions |= 0x2000u;
    }
    o.wolGame = settings.wolGame;
    o.multiplier = std::clamp(settings.updateRate / 30, 1, 4);  // the update rate as a multiple of 30 Hz
    o.mapVote = settings.mapVote;
    o.testCommands = settings.testCommands;
    o.solo = settings.solo;
    o.quest = settings.quest;
    o.playersCantDie = settings.playersCantDie;
    o.exactMaths = settings.exactMaths;
    o.hostClient = true; // (our own client joins it: the game flags' 2)
    saveRoot_ = settings.saveRoot.empty() ? gameDir : settings.saveRoot;
    if (settings.solo) {
        o.saveDir = saveRoot_; // (Save\WORKING... as GAME.EXE keeps them; the client's own .plr sections: its HostClient's)
        // The character's file (clientCurSave), loaded onto our player as it's made (GameHost:
        // soloMapLoaded with the host in, its game start's SAVE_SERVER and client sections).
        o.hostPlayerFile = !settings.characterFile.empty()   ? settings.characterFile
                           : !settings.continueSlot.empty() ? saveRoot_ / "Save" / settings.continueSlot / "Player.plr"
                                                            : fs::path{};
    }
    if (settings.solo && !settings.continueSlot.empty()) {
        // windowSelCharProc's Load: the map the save's copy (switch-to-solo, not multiplayer, its path, no
        // AUTOSAVE at its load), the script ids from the slot's Player.plr.
        o.startFromSave = path.string();
        std::ifstream f(saveRoot_ / "Save" / settings.continueSlot / "Player.plr", std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        o.scriptCounter = nox::server::savedScriptCounter(bytes, true);
    }
    o.sysopPassword = nox::strings::fromUtf8(settings.sysopPassword);
    o.mapsDir = settings.mapsDir.empty() ? gameDir / "maps" : settings.mapsDir;
    o.questMapsOnly = settings.questMapsOnly;
    o.gameDir = gameDir; // (execrul's files: the game's folder whatever the maps folder)
    if (fs::exists(nox::resolveNoCase(gameDir / "mapcycle.txt"))) {
        o.mapCycle = nox::resolveNoCase(gameDir / "mapcycle.txt");
    }
    if (mode) {
        // A NoX Core mode (the registry's: built in, or a mod's): its
        // server side made, the options its own (a generated mode's lobby written in the game's
        // maps folder; a campaign mode's a solo game with room for guests); its seed the
        // clock's; no records kept (only the bot manager's pool keeps them).
        modeId_ = mode->info.id;
        serverMode_ = mode->makeServer(nox::modes::ServerContext{
            .things = &things,
            .gameDir = gameDir,
            .mapsDir = o.mapsDir,
            .seed = static_cast<std::uint32_t>(SDL_GetTicksNS() ^ static_cast<std::uint64_t>(std::time(nullptr))),
            .options = settings.modeOptions,
            .log = [id = modeId_](std::string_view line) { SDL_Log("%s: %.*s", id.c_str(), static_cast<int>(line.size()), line.data()); },
            .modData = noxsdl::modsFolderPath().parent_path() / "moddata"}); // (a mod's nox.store: beside mods\, never the game's folder)
        if (const fs::path p = serverMode_->configure(o); !p.empty() && !settings.mapOverMode) {
            path = p;
        }
    } else if (settings.editSession) {
        // A map editor session (edit_session.hpp): the session's map, already written in its maps
        // folder (the editor's session dialog: prepareSessionMap), hosted as "[EDIT] <name>" in the
        // sandbox's mode; the director reads it as its base and logs what the session does.
        nox::server::EditSessionDirector::Options eo = *settings.editSession;
        const fs::path sessionMap = eo.mapsDir / eo.mapName / (eo.mapName + ".map");
        if (eo.mapName.empty() || !fs::exists(nox::resolveNoCase(sessionMap))) {
            throw std::runtime_error("no map " + (eo.mapName.empty() ? std::string("(unnamed)") : eo.mapName) + " for the edit session (" +
                                     sessionMap.string() + ")");
        }
        if (!eo.log) {
            eo.log = [](const std::string& line) { SDL_Log("session: %s", line.c_str()); };
        }
        edit_.emplace(things, std::move(eo));
        path = nox::server::setUpEditSession(*edit_, o, settings.name);
    }
    try {
        socket_ = nox::net::UdpSocket::open(settings.port, settings.loopbackOnly);
    } catch (const std::exception&) {
        socket_ = nox::net::UdpSocket::open(0, settings.loopbackOnly); // (taken, e.g. by another server here: any free one)
    }
    port_ = socket_->localPort();
    if (serverMode_) {
        serverMode_->setServerPort(port_); // (a mode's own bots join here: a boss, say)
    }
    gameDir_ = gameDir;
    game_ = std::make_unique<nox::server::GameHost>(things, path, o);
    if (serverMode_) {
        serverMode_->attach(*game_);
    }
    readMapChoices_ = !settings.solo && !settings.quest && settings.modeId.empty() && !settings.editSession;
    bridge_ = std::make_unique<Bridge>(*this);
    publishView(); // (its views at once: the thread pumps from the first frame)
    thread_ = std::thread([this] { run(); });
    serverId_ = thread_.get_id(); // (read by the thread only after the first beginFrame)
}

namespace {

// Hosts that left their game, waiting for their players' acks (LocalHost::retire): each with the
// time it gives up waiting (main thread only).
struct Retired {
    std::unique_ptr<LocalHost> host;
    std::uint64_t until = 0;
};
std::vector<Retired>& retiredHosts()
{
    static std::vector<Retired> v;
    return v;
}

} // namespace

void LocalHost::beginQuit()
{
    if (quitBegun_.exchange(true)) {
        return;
    }
    SDL_Log("hosting: the game ends (MSG_SERVER_QUIT to its players)");
    post([this] {
        game_->beginQuit();
        quitDone_.store(game_->quitDone(), std::memory_order_release);
    });
}

void LocalHost::retire(std::unique_ptr<LocalHost> host)
{
    if (!host) {
        return;
    }
    host->setHostClient(nullptr); // (our client is going: the server calls into it no more)
    if (!host->started_) {
        return; // (its thread never ran a pump: nobody to tell)
    }
    // The quit begun now if it wasn't (a game left another way than our quit); with nobody to
    // wait for (a solo game, a game alone) the host goes at once.
    host->quitBegun_ = true;
    const bool done = host->withGame([](nox::server::GameHost& game) {
        game.beginQuit();
        return game.quitDone();
    });
    host->quitDone_ = done;
    if (done) {
        return;
    }
    // A mode's generated maps gone now, not as the host goes: a new game of the same mode (its maps
    // of the same names) may be hosted meanwhile (bug hunt 3: the old host's sweep took its maps).
    host->withGame([&h = *host](nox::server::GameHost&) { h.sweepModeMaps(); });
    SDL_Log("hosting: the game ends; waiting for its players to take MSG_SERVER_QUIT");
    retiredHosts().push_back({std::move(host), SDL_GetTicks() + kQuitWaitMs});
}

void LocalHost::sweepModeMaps()
{
    if (serverMode_ && !swept_) {
        swept_ = true;
        serverMode_->sweepOwn();
    }
}

void LocalHost::pumpRetired()
{
    auto& v = retiredHosts();
    const std::uint64_t now = SDL_GetTicks();
    std::erase_if(v, [now](const Retired& r) { return r.host->quitDone() || now >= r.until; });
}

void LocalHost::finishRetired(std::uint32_t waitMs)
{
    const std::uint64_t until = SDL_GetTicks() + waitMs;
    while (!retiredHosts().empty()) {
        pumpRetired();
        if (retiredHosts().empty() || SDL_GetTicks() >= until) {
            break;
        }
        SDL_Delay(5);
    }
    retiredHosts().clear();
}

void LocalHost::finishReleased()
{
    // (Each mode's work left running after its hosts went: its levels being built.)
    for (const nox::modes::Mode* mode : nox::modes::registry().all()) {
        if (mode->finishBackground) {
            mode->finishBackground();
        }
    }
}

LocalHost::~LocalHost()
{
    {
        std::lock_guard lock(startMutex_);
        stop_ = true;
    }
    startCv_.notify_all();
    {
        std::lock_guard lock(toMainMutex_); // (a park the server waits for given up)
    }
    parkCv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    // The server's end session: every client's stream closed with SERVER_CLOSE (the thread is
    // gone: the game is ours here), sent before the socket goes.
    try {
        game_->endSession();
        for (auto& d : game_->takeOutgoing()) {
            socket_->sendTo(d.to, d.bytes);
        }
    } catch (const std::exception& ex) {
        SDL_Log("hosting: closing the game's streams: %s", ex.what());
    }
    if (serverMode_) {
        serverMode_->detach(*game_);
        sweepModeMaps(); // (a mode's generated maps gone with it, if retire hadn't)
        // Its work still under way (a mode's levels being built ahead) isn't waited for here
        // (seconds of map generation on the thread leaving the game): finishReleased waits at quit.
        serverMode_->releaseBackground();
    }
}

// ------------------------------------------------------------------------- the server thread

void LocalHost::run()
{
    {
        // (not before the main thread's first frame: what it set up after the constructor first)
        std::unique_lock lock(startMutex_);
        startCv_.wait(lock, [this] { return started_ || stop_.load(); });
    }
    if (readMapChoices_ && !stop_) {
        // The maps' modes (GameHost::mapChoices reads every map's header once, then keeps them)
        // read first, as our client joins: not under the lock at the Server Settings window's
        // first opening, nor in the tick that opens the first map vote.
        std::lock_guard lock(mutex_);
        game_->mapChoices();
    }
    // Each millisecond (SDL_DelayNS: a high-resolution timer on Windows): the server's tick,
    // which runs the substeps due by then (GameHost::tick's real-time clock).
    while (!stop_) {
        {
            std::lock_guard lock(mutex_);
            if (stop_) {
                break;
            }
            try {
                pump(static_cast<std::uint32_t>(SDL_GetTicks()));
            } catch (const std::exception& ex) {
                std::lock_guard errorLock(errorMutex_);
                error_ = ex.what();
            }
            releaseMain(); // (a park the pump left let go)
        }
        SDL_DelayNS(1'000'000);
    }
}

// One pump on the server's thread: our client's queued jobs, every packet waiting, the server's tick,
// the edit session director's and a mode's, then everything the server has to send.
void LocalHost::pump(std::uint32_t nowMs)
{
    refreshClientCopy();
    // What the main thread posted since the last pump (a fade's `done`, the Server Settings'
    // apply), in order.
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard lock(jobsMutex_);
        jobs.swap(jobs_);
    }
    for (auto& job : jobs) {
        job();
    }
    std::uint8_t buffer[4096];
    while (const auto got = socket_->receive(buffer)) {
        game_->receive(got->from, std::span(buffer, got->size), nowMs);
    }
    refreshClientCopy(); // (the client's view as it was when it sent what just came: the tick reads them together)
    game_->tick(nowMs);
    if (quitBegun_.load(std::memory_order_relaxed)) {
        quitDone_.store(game_->quitDone(), std::memory_order_release); // (the players' acks counted)
    }
    if (serverMode_) {
        serverMode_->tick(*game_, nowMs);
        if (serverMode_->takeLockChanged()) {
            SDL_Log("%s: %s", modeId_.c_str(), serverMode_->locked() ? "locked" : "open");
        }
    }
    if (edit_) {
        edit_->tick(*game_, nowMs); // (between frames; it logs its publishes itself)
    }
    publishView(); // (before what the client is sent: what it reads then is at least this new)
    if (auto lines = game_->takeConsole(); !lines.empty()) {
        std::lock_guard lock(consoleMutex_);
        console_.insert(console_.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
    }
    for (auto& d : game_->takeOutgoing()) {
        if (hostPeer_ && d.to == *hostPeer_) {
            queueMarker(d.bytes); // (the host client's calls before it run before our client takes it)
        }
        socket_->sendTo(d.to, d.bytes);
    }
}

void LocalHost::publishView()
{
    const auto& world = game_->server().world();
    gameFlags_.store(world.gameFlags, std::memory_order_release);
    std::lock_guard lock(viewMutex_);
    noMultiplayer_ = world.noMultiplayer;
    if (soloPath_ != world.soloPath) {
        soloPath_ = world.soloPath;
    }
}

void LocalHost::refreshClientCopy()
{
    std::lock_guard lock(clientMutex_);
    clientCopy_ = client_;
}

std::uint64_t LocalHost::queueCall(std::function<void()> call)
{
    std::lock_guard lock(toMainMutex_);
    return queue_.pushCall(std::move(call));
}

void LocalHost::queueMarker(std::span<const std::uint8_t> bytes)
{
    std::lock_guard lock(toMainMutex_);
    queue_.pushMarker(bytes);
}

bool LocalHost::parkMain()
{
    if (parked_) {
        return true;
    }
    std::unique_lock lock(toMainMutex_);
    if (stop_) {
        return false;
    }
    queue_.pushPark();
    park_ = Park::Asked;
    parkCv_.wait(lock, [this] { return park_ == Park::Parked || stop_.load(); });
    if (park_ != Park::Parked) {
        queue_.dropParks();
        park_ = Park::None;
        return false;
    }
    parked_ = true;
    return true;
}

void LocalHost::releaseMain()
{
    if (!parked_) {
        return;
    }
    publishClient(); // (gathered here: the main thread is still parked)
    refreshClientCopy();
    {
        std::lock_guard lock(toMainMutex_);
        park_ = Park::Released;
    }
    parkCv_.notify_all();
    parked_ = false;
}

// --------------------------------------------------------------------------- the main thread

void LocalHost::beginFrame()
{
    if (!started_) {
        {
            std::lock_guard lock(startMutex_);
            started_ = true;
        }
        startCv_.notify_all();
    }
    std::vector<ToMain> taken;
    {
        // The calls queued after the last datagram our client took (and a park with none ahead of
        // it). A park still behind datagrams waits for them (LiveSession takes them this frame:
        // datagramsTaken), but not past one more frame's start.
        std::lock_guard lock(toMainMutex_);
        queue_.takeDue(taken);
        if (queue_.parkQueued()) {
            if (std::exchange(parkWaited_, true)) {
                queue_.takeToPark(taken);
            }
        }
    }
    runTaken(taken, true);
    // What the server's jobs handed back (a console command's lines).
    std::vector<std::function<void()>> results;
    {
        std::lock_guard lock(jobsMutex_);
        results.swap(results_);
    }
    for (auto& r : results) {
        r();
    }
}

void LocalHost::datagramArriving(std::span<const std::uint8_t> datagram)
{
    // The calls before it (a park after it waits for the client to have taken it: datagramsTaken).
    std::vector<ToMain> taken;
    {
        std::lock_guard lock(toMainMutex_);
        queue_.takeForDatagram(datagram, taken);
    }
    runTaken(taken, true);
}

void LocalHost::datagramsTaken()
{
    // Every datagram waiting taken: the calls after the last (and a park after them); a park
    // still behind markers is behind datagrams lost on the way, and the server waits.
    std::vector<ToMain> taken;
    {
        std::lock_guard lock(toMainMutex_);
        queue_.takeDue(taken);
        queue_.takeToPark(taken);
    }
    runTaken(taken, true);
}

void LocalHost::runCall(ToMain& e)
{
    try {
        if (e.call) {
            e.call();
        }
    } catch (const std::exception& ex) {
        SDL_Log("hosting: the host client's call: %s", ex.what());
    }
    appliedSeq_.store(e.seq, std::memory_order_release);
}

void LocalHost::runTaken(std::vector<ToMain>& taken, bool beforeCalls)
{
    if (taken.empty()) {
        return;
    }
    // (a park last: the server waits in its tick from here till it's served; what runs before it,
    // the client's own screen messages too (a chapter's end sets the stage through withGame),
    // must not wait for the server's lock)
    const bool parks = taken.back().kind == ToMain::Kind::Park;
    const bool wasTaken = std::exchange(parkTaken_, parkTaken_ || parks);
    ++inCalls_;
    if (beforeCalls && beforeCalls_ && std::ranges::any_of(taken, [](const ToMain& e) { return e.kind == ToMain::Kind::Call; })) {
        beforeCalls_();
    }
    for (auto& e : taken) {
        if (e.kind == ToMain::Kind::Park) {
            parkTaken_ = wasTaken;
            parkWaited_ = false;
            parkHere();
            continue;
        }
        runCall(e);
    }
    --inCalls_;
    parkTaken_ = wasTaken;
    publishClient();
}

void LocalHost::runAllQueued()
{
    // (inside withGame: the server between pumps, or stopped for a park while a queued call runs)
    std::vector<ToMain> taken;
    {
        std::lock_guard lock(toMainMutex_);
        queue_.takeCalls(taken);
    }
    ++inCalls_;
    for (auto& e : taken) {
        runCall(e);
    }
    --inCalls_;
}

void LocalHost::serveParks()
{
    std::vector<ToMain> taken;
    {
        std::lock_guard lock(toMainMutex_);
        queue_.takeToPark(taken); // (the server may wait for it holding the lock: not for its datagrams)
    }
    runTaken(taken, false);
}

bool LocalHost::serverAwaitsPark()
{
    if (parkTaken_) {
        return true;
    }
    std::lock_guard lock(toMainMutex_);
    return queue_.parkQueued();
}

void LocalHost::parkHere()
{
    std::unique_lock lock(toMainMutex_);
    park_ = Park::Parked;
    parkCv_.notify_all();
    parkCv_.wait(lock, [this] { return park_ != Park::Parked; });
    if (park_ == Park::Released) {
        park_ = Park::None;
    }
}

void LocalHost::publishClient()
{
    if (!gather_) {
        return;
    }
    HostClientView v;
    try {
        v = gather_();
    } catch (const std::exception& ex) {
        SDL_Log("hosting: the client's view: %s", ex.what());
        return;
    }
    v.applied = appliedSeq_.load(std::memory_order_acquire);
    std::lock_guard lock(clientMutex_);
    client_ = v;
}

void LocalHost::setClientView(std::function<HostClientView()> gather, std::function<void()> beforeCalls)
{
    withGame([&](nox::server::GameHost&) {
        gather_ = std::move(gather);
        beforeCalls_ = std::move(beforeCalls);
    });
    publishClient();
}

bool LocalHost::lockForMain()
{
    while (!mutex_.try_lock_for(std::chrono::milliseconds(1))) {
        if (inCalls_ == 0) {
            serveParks(); // (a park asked for meanwhile: the server may be waiting for it with the lock)
            continue;
        }
        // Inside a queued call (EndGame's unpause): a park the server waits for comes after this
        // call (served now it would run the calls after it first). The server is stopped in that
        // wait with the lock (as GAME.EXE's tick is inside the call): fn runs without it.
        if (serverAwaitsPark()) {
            mainHolds_ = true;
            return false;
        }
    }
    mainHolds_ = true;
    return true;
}

void LocalHost::unlockForMain(bool locked)
{
    if (std::exchange(touched_, false)) {
        publishClient(); // (the client called directly: the server reads it as it is now)
    }
    publishView();
    mainHolds_ = false;
    if (locked) {
        mutex_.unlock();
    }
}

void LocalHost::post(std::function<void(nox::server::GameHost&)> job)
{
    post([this, job = std::move(job)] { job(*game_); });
}

void LocalHost::post(std::function<void()> job)
{
    std::lock_guard lock(jobsMutex_);
    jobs_.push_back(std::move(job));
}

std::optional<fs::path> LocalHost::soloPath() const
{
    std::lock_guard lock(viewMutex_);
    if (!noMultiplayer_ || soloPath_.empty()) {
        return std::nullopt;
    }
    return fs::path(soloPath_);
}

void LocalHost::soloPathRead()
{
    // At once, as the exe's client lowers it in its map read (posted, a withGame's publishView
    // before the job ran had raised the view's flag again).
    withGame([](nox::server::GameHost& game) { game.server().world().noMultiplayer = false; });
}

std::optional<std::string> LocalHost::takeError()
{
    std::lock_guard lock(errorMutex_);
    return std::exchange(error_, std::nullopt);
}

std::vector<std::string> LocalHost::takeConsole()
{
    std::lock_guard lock(consoleMutex_);
    return std::exchange(console_, {});
}

nox::server::GameHost& LocalHost::game()
{
    if (!onServerThread() && !mainHolds_) {
        static bool told = false;
        if (!std::exchange(told, true)) {
            SDL_Log("hosting: the game used off its thread without withGame");
        }
    }
    return *game_;
}

void LocalHost::command(std::string line, std::uint32_t issuer, std::function<void(std::vector<std::string>)> printed)
{
    post([this, line = std::move(line), issuer, printed = std::move(printed)] {
        auto lines = game_->command(line, issuer);
        if (printed) {
            std::lock_guard lock(jobsMutex_);
            results_.push_back([printed, lines = std::move(lines)] { printed(lines); });
        }
    });
}

void LocalHost::modeCommand(std::string line, std::function<void(std::vector<std::string>)> answered)
{
    post([this, line = std::move(line), answered = std::move(answered)] {
        std::vector<std::string> lines;
        if (serverMode_) {
            lines = serverMode_->console(*game_, line).value_or(std::vector<std::string>{});
        }
        if (answered) {
            std::lock_guard lock(jobsMutex_);
            results_.push_back([answered, lines = std::move(lines)] { answered(lines); });
        }
    });
}

void LocalHost::setHostClient(nox::server::HostClient* client)
{
    withGame([&](nox::server::GameHost& game) {
        {
            std::lock_guard lock(toMainMutex_);
            queue_.dropCalls();
        }
        bridge_->client = client;
        bridge_->forget();
        game.setHostClient(client ? bridge_.get() : nullptr);
    });
    publishClient();
}

void LocalHost::setHostPeer(std::optional<nox::net::Endpoint> at)
{
    withGame([&](nox::server::GameHost& game) {
        game.setHostPeer(at);
        hostPeer_ = at;
        std::lock_guard lock(toMainMutex_);
        queue_.dropMarkers();
    });
}

void LocalHost::pause(int how, nox::server::PauseReason why)
{
    withGame([how, why](nox::server::GameHost& game) {
        auto& world = game.server().world();
        if (how < 0) {
            nox::server::pauseFor(world, 0);
        }
        nox::server::setPaused(world, how > 0, why);
    });
}

bool LocalHost::loadSoloGame(const std::string& slot)
{
    return withGame([&](nox::server::GameHost& game) {
        nox::server::SoloSaveOptions options;
        options.gameDir = saveRoot_;
        return nox::server::loadSoloGame(game.server(), options, slot);
    });
}

void LocalHost::requestSave(const std::string& name)
{
    withGame([&](nox::server::GameHost& game) {
        auto& server = game.server();
        server.world().saveName = name;
        nox::server::requestSave(server.world(), true, 0, 0, server.clock().frame());
    });
}

bool LocalHost::mayOpenMenu()
{
    return withGame([](nox::server::GameHost& game) {
        auto& server = game.server();
        return nox::server::hostMayOpenMenu(server.world(), server.clock().frame());
    });
}

bool LocalHost::paused()
{
    return withGame([](nox::server::GameHost& game) { return (game.server().world().gameFlags & nox::server::GameFlag::Paused) != 0; });
}

void LocalHost::setStage(std::uint8_t stage)
{
    withGame([stage](nox::server::GameHost& game) { game.server().world().briefingByte = stage; });
}
