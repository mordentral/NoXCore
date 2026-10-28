// The hosted game: GameHost joins players to the Server, reads their messages, and sends each
// frame's updates, reports and important messages back through the server transport. The Server
// Settings window's records, the kick votes and noxcpp's map vote live here too; the console is
// host_console.cpp and map downloads map_send.cpp.
#include "noxcore/server/game_host.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/net/framing.hpp"
#include "noxcore/net/player_input.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/server/collide.hpp"
#include "noxcore/server/dialog.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/player_save.hpp"
#include "noxcore/server/profile.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/solo_save.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace nox::server {

namespace {

// A message's bytes, its opcode first.
using net::bytesOf;

void append(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

// Text into a fixed byte field, cut to fit and padded with NULs.
template <std::size_t N>
void copyText(std::array<std::uint8_t, N>& to, std::string_view text)
{
    to.fill(0);
    std::memcpy(to.data(), text.data(), std::min(text.size(), N - 1));
}

// A UTF-16LE name, up to its NUL, as ASCII (other letters as '?').
std::string nameOf(std::span<const std::uint8_t> wide)
{
    std::string out;
    for (std::size_t i = 0; i + 1 < wide.size(); i += 2) {
        const char16_t c = static_cast<char16_t>(wide[i] | wide[i + 1] << 8);
        if (c == 0) {
            break;
        }
        out += c < 0x80 ? static_cast<char>(c) : '?';
    }
    return out;
}

// UTF-16LE bytes up to the first NUL.
using util::utf16Until;

/// How many things the first load's outfitting makes a solo game's new host: its
/// playerMakeDefItems under game flag 0x800 (game flag 0x200,
/// which would skip it, is set only by that load's read): StreetShirt (0xA00),
/// StreetPants, StreetSneakers (none worn yet) and the campaign's weapon (Sword,
/// StaffWooden, StaffWooden), each playerRespawnItem's new object.
std::int32_t firstLoadOutfitSize(const things::ThingDb& things)
{
    std::int32_t n = 0;
    for (const char* type : {"StreetShirt", "StreetPants", "StreetSneakers"}) {
        n += things.findThing(type) ? 1 : 0;
    }
    return n + (things.findThing("Sword") || things.findThing("StaffWooden") ? 1 : 0);
}

/// SrvNet_SendUnitHeight: MSG_REPORT_Z_PLUS {u16 the object, u8 its height}, or
/// MSG_REPORT_Z_MINUS with the depth below the floor (FCHS, then __ftol: truncated).
std::vector<std::uint8_t> heightReport(const ServerObject& o)
{
    if (o.xStatus & 0x20u) {
        // Under a DeltaZ spell (a fist's, a meteor's): MSG_FX_DELTAZ_SPELL_START 0x9F {u16 its net code
        // (its extent), its height, its fall per frame, its bounce}, each __ftol'd to a byte.
        return {0x9F, static_cast<std::uint8_t>(o.extent), static_cast<std::uint8_t>(o.extent >> 8), static_cast<std::uint8_t>(static_cast<int>(o.z)),
                static_cast<std::uint8_t>(static_cast<int>(o.zVel)), static_cast<std::uint8_t>(static_cast<int>(o.bounce))};
    }
    const std::uint16_t code = netCodeOf(o);
    const bool below = o.z < 0;
    return {static_cast<std::uint8_t>(below ? 0x5F : 0x5E), static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
            static_cast<std::uint8_t>(static_cast<int>(below ? -o.z : o.z))};
}

/// A game's numbers come from the install's gamedata.bin alone (nothing is built in), so a host
/// made without it is the calling program's mistake: noxsdl and the dedicated servers load
/// the file at start-up, as they load thing.bin.
GameHost::Options requireGameData(GameHost::Options o)
{
    if (!o.gameData) {
        throw std::invalid_argument("GameHost: no gamedata.bin loaded");
    }
    return o;
}
} // namespace

// Builds the server and loads the first map, then sets up the settings records, the rules, the
// map's copy for downloads and the lobby's checks.
GameHost::GameHost(const things::ThingDb& things, const std::filesystem::path& mapPath, Options options)
    : things_(things)
    , options_(requireGameData(std::move(options)))
    , server_(things, {.multiplier = options_.multiplier,
                       .modifiers = options_.modifiers,
                       .gameData = options_.gameData,
                       // (A network game's host chose it from the main menu's Multiplayer or Solo
                       // Quest: game flags 0x2000 and 0x10000, the minimap's every wall: windowMainMenuProc's
                       // 0x70 / 0x83. Then 0x20000, the server up, from its first map's load till
                       // its reset (servInitialMapLoad / servResetPlayers):
                       // a stock client in a game that gets MSG_GAME_SETTINGS without it leaves
                       // its game loop (CliNet_OnPacket 0xAF).)
                       .gameFlags = options_.solo ? GameFlag::Solo : options_.gameFlags | GameFlag::Network | 0x10000u | 0x20000u,
                       .strings = options_.strings,
                       .gameplayFlags = options_.gameplayFlags,
                       .lessonLimits = options_.lessonLimits,
                       .timeLimits = options_.timeLimits,
                       .serverOptions = options_.serverOptions,
                       .monsterDb = options_.monsterDb,
                       .autoRespawn = options_.autoRespawn,
                       .wolGame = options_.wolGame,
                       .soundSets = options_.soundSets,
                       .frameSounds = 0x80, // (the "AudEvent" class: afterFrame frees it)
                       .questScreens = options_.questScreens,
                       .questSaves = options_.questSaves,
                       .forceClass = options_.forceClass,
                       .multiclass = options_.multiclass,
                       .monstersAlert = options_.monstersAlert,
                       .keepThingsOnDeath = options_.keepThingsOnDeath,
                       .playersCantDie = options_.playersCantDie,
                       .noScoring = options_.sandboxNoScoring,
                       .modeOwnsLimits = options_.modeOwnsLimits,
                       .exactMaths = options_.exactMaths})
    , transport_({.maxPlayers = options_.solo ? 1 + options_.soloGuests : options_.maxPlayers, .password = options_.password})
{
    mapCycleOn_ = options_.mapCycleOn;
    voteRng_.seed(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    server_.setPacketHook([this] {
        // (the game tick: the streams that never accepted closed, then the packets read)
        transport_.setFrame(server_.clock().frame());
        transport_.closeUnaccepted();
        takePackets();
    });
    server_.setUpdateServerHook([this] {
        // (updateServer: more than 500 ms since the last, the next player's latency reported)
        if (!latencyReportedAt_ || 500 < nowMs_ - *latencyReportedAt_) {
            reportNextLatency();
            latencyReportedAt_ = nowMs_;
        }
    });
    server_.setMapLoadHook([this] { loadNextMap(); });
    server_.setClientsHook([this] { frameClients(); });
    server_.setQuestSaveHooks([this](Fight& f, ServerObject& u) { questSendSave(f, u); },
                              [this](Fight& f, ServerObject& u) { questSaveAndKick(f, u); });
    if (options_.solo && !options_.saveDir.empty()) {
        // A solo game's saves under the game's folder's Save\, a visited map's
        // saved copy loaded from there.
        server_.setSaveFolder(options_.saveDir);
        server_.setSaveGame([this](const std::string& name, ServerObject* exit) {
            return saveSoloGame(server_, {.gameDir = options_.saveDir, .clientSections = options_.clientSections}, name, exit);
        });
    }
    const auto file = map::loadMapFile(mapPath);
    mapPath_ = mapPath;
    mapFile_ = mapPath.filename().string();
    mapCrc_ = file.crc;
    pristineCrc_ = file.crc;
    if (options_.solo && options_.soloGuests > 0 && !options_.startFromSave.empty()) {
        // (noxcpp: a saved game's copy hosted with room for guests: they're told the map's own file)
        const std::string stem = mapPath.stem().string();
        std::error_code ec;
        if (const std::filesystem::path own = resolveNoCase(mapsDir() / stem / (stem + ".map")); std::filesystem::exists(own, ec)) {
            try {
                pristineCrc_ = map::loadMapFile(own).crc;
            } catch (const std::exception&) {
                // (unreadable: the copy's CRC stands; a guest's download takes it)
            }
        }
    }
    scanMaps();
    if (options_.solo && !options_.startFromSave.empty()) {
        server_.startFromSave(options_.startFromSave);
    }
    if (options_.scriptCounter) {
        server_.world().setNextScriptId(*options_.scriptCounter);
    }
    if (options_.solo) {
        // A solo game's host player made before the first load (the solo start's
        // Plr_CreateFromJoinData(0x1F)), a new game's or a saved one's: its unit's id the
        // counter's next (not under game flag 0x200000), ahead of the map's; ours is
        // made at its CLIENT_ACCEPT, no frame run before it (tick). The first load
        // then dresses it before the map's read: its outfit's ids the next ones, ahead of
        // the ImaginaryCaster's and the map's (made in the join: soloOutfit).
        hostScriptId_ = server_.world().nextScriptId();
        server_.world().setNextScriptId(*hostScriptId_ + 1 + firstLoadOutfitSize(things_));
        awaitingHost_ = true;
    }
    server_.loadMap(file, mapFile_);
    server_.world().questChosen = options_.quest; // (setQuest, set as the host was chosen)
    mapLoadedSettings();
    // (a network game hosted, after the first load: posts the game, and
    // ends with slot 1's game flags' types but Solo's turned into Arena's: & 0xE90F | 0x100.)
    if (!options_.solo) {
        SettingsRecord& one = settingsSlots_[1];
        const auto flags = static_cast<std::uint16_t>(((one[0x34] | one[0x35] << 8) & 0xE90F) | 0x100);
        one[0x34] = static_cast<std::uint8_t>(flags);
        one[0x35] = static_cast<std::uint8_t>(flags >> 8);
    }

    if (!options_.solo) {
        loadRules();
    }
    if (!(server_.world().gameFlags & GameFlag::Solo)) {
        mapSendLoadMap(); // (servInitialMapLoad's not in a solo game: the map's .nxz for downloads)
    }
    if (!options_.password.empty()) {
        access_.privateGame = true; // (a password given: a private game)
        access_.password = options_.password.substr(0, 9);
    }
    syncAccess();
    transport_.setExtended({lobby::kExtendedVersion, static_cast<std::uint8_t>(options_.multiplier), extendedFeatures()});
    // The object types the content packs add: only games with the same ones are let in. (The
    // process's: its thing.bin loads once, with the installed packs, and every host shares it.)
    const content::NewThingTypes types = content::newThingTypes();
    if (types.hash != 0) {
        std::string packs;
        for (const auto& id : types.packs) {
            packs += (packs.empty() ? "" : ", ") + id;
        }
        transport_.setNewThingTypes(types.hash, std::move(packs));
    }
    // The lowest client level let in (0: any).
    transport_.setMinClientLevel(options_.minClientLevel);
    if (types.hash != 0 || options_.minClientLevel != 0) {
        // (the joins' refusals, why, in the process's log: the content packs' log sink)
        transport_.setLog([](const std::string& line) { content::writeContentPackLog(line); });
    }
}

void GameHost::refreshServerInfo()
{
    // server_makeServerInfoPacket's, as it stands (the map, its mode, the name and the
    // host's Access settings: classes refused, closed, private, experience, resolution and ping limits).
    const World& world = server_.world();
    // (the mode the map took; noxcpp: a mode's listed one)
    const std::uint32_t gameFlags =
        (options_.gameFlags & ~0x17F0u) | (options_.listedMode ? (*options_.listedMode & 0x17F0u) : (world.gameFlags & 0x17F0u));
    std::uint8_t flags = access_.restrictedClasses & 7u;
    if (access_.closed) {
        flags |= 0x10;
    }
    if (access_.privateGame) {
        flags |= 0x20;
    }
    // (Made again only when what it's made of changed: every tick asks.)
    InfoMadeFrom from{options_.maxPlayers, gameFlags, flags, access_.limitResolution, access_.minPing, access_.maxPing};
    if (infoMadeFrom_ && *infoMadeFrom_ == from && infoMap_ == mapPath_.native() && infoName_ == options_.serverName) {
        return;
    }
    infoMadeFrom_ = from;
    infoMap_ = mapPath_.native();
    infoName_ = options_.serverName;
    net::msg::ServerInfo info;
    info.maxPlayers = static_cast<std::uint8_t>(options_.maxPlayers);
    copyText(info.mapName, mapPath_.stem().string());
    info.gameFlags = gameFlags;
    info.serverFlags = flags;
    info.closed = flags & 0x10;
    info.settings101lo = 0x0F; // (the experience limits: none)
    info.settings101hi = 0x0F;
    info.videoFlags = access_.limitResolution ? 0x80 : 0;
    info.pingMin = static_cast<std::uint16_t>(access_.minPing ? *access_.minPing : 0xFFFF); // (a u16 field, as the transport's)
    info.pingMax = static_cast<std::uint16_t>(access_.maxPing ? *access_.maxPing : 0xFFFF);
    const auto name = options_.serverName.substr(0, 15);
    info.serverName.assign(name.begin(), name.end());
    info.serverName.push_back(0);
    transport_.setServerInfo(info);
}

lobby::GameListing GameHost::listing()
{
    // The WOL record's fields as GAME.EXE posts its game (makeWolRecord): the server's name, the
    // map, the mode (the game flags' low 16 bits), the player limit, the quest's stage and the
    // Access settings (closed, private, the classes refused, the ping limits).
    World& world = server_.world();
    lobby::GameListing g;
    g.name = options_.serverName.substr(0, 15);
    g.map = std::filesystem::path(world.mapName).stem().string();
    g.players = static_cast<int>(players_.size());
    g.maxPlayers = options_.maxPlayers;
    g.gameFlags = world.gameFlags & 0xFFFFu;
    if (options_.listedMode) { // (noxcpp: a mode's listed one, the world's solo bit not shown)
        g.gameFlags = (g.gameFlags & ~(0x17F0u | GameFlag::Solo)) | (*options_.listedMode & 0x17F0u);
    }
    g.questStage = world.questStage;
    g.bannedClasses = access_.restrictedClasses & 7u;
    g.access = access_.closed ? lobby::GameAccess::Closed : access_.privateGame ? lobby::GameAccess::Password : lobby::GameAccess::Open;
    g.pingMin = access_.minPing;
    g.pingMax = access_.maxPing;
    return g;
}

void GameHost::syncAccess()
{
    // The lobby's checks (ServerTransport::Access) from the Access settings and the lists.
    net::ServerTransport::Access a;
    a.closed = access_.closed;
    for (const auto& n : access_.allowed) {
        a.allowed.push_back(strings::fromUtf8(n));
    }
    for (const auto& [name, until] : bans_) {
        a.banned.push_back({name, {}, until == 0xFFFFFFFFu ? 0u : until});
    }
    a.bannedClasses = access_.restrictedClasses & 7u;
    a.privateGame = access_.privateGame;
    a.password = strings::fromUtf8(access_.password);
    if (access_.minPing) {
        a.minPing = static_cast<std::uint16_t>(*access_.minPing);
    }
    if (access_.maxPing) {
        a.maxPing = static_cast<std::uint16_t>(*access_.maxPing);
    }
    transport_.setAccess(std::move(a));
    transport_.setMaxPlayers(options_.solo ? 1 + options_.soloGuests : options_.maxPlayers);
    refreshServerInfo();
}

void GameHost::receive(const net::Endpoint& from, std::span<const std::uint8_t> datagram, std::uint32_t nowMs)
{
    transport_.receive(from, datagram, nowMs);
}

void GameHost::takePackets()
{
    // netlist / SrvNet_OnPacket: what came since, in the server's frame (Server's
    // packet hook: after updateServer, before the units).
    // (Into the kept list, held here for the walk: a read under it would start one of its own.)
    std::vector<net::ServerTransport::Event> events = std::move(transportEvents_);
    transport_.takeEvents(events);
    for (auto& e : events) {
        switch (e.kind) {
        case net::ServerTransport::Event::Kind::Payload:
            onPayload(e.peer, e.bytes);
            break;
        case net::ServerTransport::Event::Kind::Closed:
            leave(e.peer);
            break;
        case net::ServerTransport::Event::Kind::Connected:
            break; // (the player comes with its CLIENT_ACCEPT)
        }
    }
    events.clear();
    transportEvents_ = std::move(events);
}
// Runs the server for the real time passed: as many substeps as the clock's rate calls for (at
// most 64 a call), each frame's settings, votes and map changes, then the transport.
void GameHost::tick(std::uint32_t nowMs)
{
    // (The server's figuring in the game's x87 rounding, the clients' updates too: x87.hpp.)
    const util::RoundingScope rounding(server_.exactMaths());
    nowMs_ = nowMs;
    if (awaitingHost_) {
        // A solo game: no frame before its host's player is made (the game's main loop starts
        // after); what came taken as it comes (its CLIENT_ACCEPT).
        takePackets();
        transport_.poll(nowMs);
        return;
    }
    // Real time: the clock's rate of substeps since the start.
    if (!started_) {
        started_ = true;
        startMs_ = nowMs;
    }
    const auto rate = static_cast<std::uint64_t>(server_.clock().rate());
    if (rate != clockRate_) {
        // (the update rate changed at a frame's start: counted afresh from now, not caught up)
        clockRate_ = rate;
        startMs_ = nowMs;
        substepsRun_ = 0;
    }
    const std::uint64_t due = static_cast<std::uint64_t>(nowMs - startMs_) * rate / 1000;
    for (int guard = 0; substepsRun_ < due && guard < 64; ++guard) {
        server_.setPlayersPresent(std::ranges::any_of(players_, [](const auto& p) { return p.second.ready; }));
        server_.step(); // (a frame's clients' update runs at its tick end: frameClients)
        ++substepsRun_;
        if (!server_.steppedFrame()) {
            syncClasses(); // (noxcpp's forced class: a run's start, its end)
            {
                ProfileScope timed(ProfileSection::HostSubstep);
                sendSubstep(); // (noxcpp's: identified clients' positions between frames)
            }
            flushDrops();
        }
        if (server_.steppedFrame()) {
            settingsLoop(); // (mainloop: SrvNet_BroadcastGameSettings, else the window's)
        }
        if (server_.steppedFrame()) {
            voteFrame(); // (noxcpp's map vote: before a finished match's next map is taken)
        }
        if (server_.takeNextMapDue()) {
            nextMap();
        } else if (auto exit = std::exchange(server_.world().exitTo, std::nullopt)) {
            nextMap(std::move(exit));
        } // (asked: the next frame loads it, Server::requestMapLoad)
    }
    substepsRun_ = std::max(substepsRun_, due); // (a long stall isn't run back)
    if (!refused_.empty()) {
        putOutRefused(); // (noxcpp: Options::stockRefusal's holds that are up)
    }
    // (game flags 1 and 8: no connects; but while our end of match vote holds it)
    transport_.setMatchOver((server_.world().gameFlags & GameFlag::GameOver) != 0 && !endVoteHolds());
    refreshServerInfo(); // (the game list's view of it, as it stands)
    ProfileScope timed(ProfileSection::HostPoll);
    transport_.poll(nowMs);
}

void GameHost::frameClients()
{
    // A frame's tick end, before the new things' pass (Server::tickEnd).
    syncClasses(); // (noxcpp's forced class: a run's start, its end)
    flushDrops();
    if (server_.updatedWorld()) { // (voteUptate: in the world's update, not paused nor over)
        updateVotes();
    }
    flushDrops(); // (a quest's kick vote's)
    // The transfers and SrvNet_UpdateAllClients are the update branch's
    // (paused too): not in a match's end nor a map's load tick. (Our map vote's end of
    // a match, held past the stock 10 s, keeps them: a client joining or loading meanwhile gets in
    // rather than waiting in the dark for the next map.)
    if (server_.ranUpdate() || endVoteHolds()) {
        if ((server_.clock().frame() & 1u) == 0) {
            mapSendFrame(); // (just before the transfers: skipped on odd frames)
        }
        xfers_.frame(server_.clock().frame());
        ProfileScope timed(ProfileSection::HostAfterFrame);
        afterFrame();
    }
}

void GameHost::tellModeChange()
{
    // A frame that changed the game's mode (a quest begun: startSoloQuest's countdown) tells the
    // settings before its other news: the clients dress players by the mode
    // (Cli_ResetPlayerDefaultOutfit's quest weapons, MSG_PLAYER_RESPAWN), and the exe's host's
    // client shares the server's flags in-process; ours is told them.
    if (const std::uint32_t flags = server_.world().gameFlags & 0x7FFF0u; flags != settingsFlags_) {
        if (settingsFlags_ != kNoSettingsFlags) {
            // (SrvNet_BroadcastGameSettings: Net_QueueImportantSeq(0x9F), held for one not ready.)
            sendSettings(false);
        }
        settingsFlags_ = flags;
    }
}

void GameHost::flushDrops()
{
    for (const std::uint8_t peer : std::exchange(dropAfterFrame_, {})) {
        if (players_.contains(peer)) {
            disconnect(peer, 4); // (Plr_DisconnectByIndex, reason 4)
        }
    }
}

void GameHost::syncClasses()
{
    World& world = server_.world();
    if (!world.forceClass) {
        return;
    }
    const bool forced = forcedClass(world).has_value();
    for (auto& [peer, p] : players_) {
        ServerObject* unit = world.byExtent(p.extent);
        if (!unit || !unit->player) {
            continue;
        }
        if (!forced && unit->player->ownClass <= 2 && unit->player->classId != unit->player->ownClass) {
            Fight f = server_.fight();
            forceClass(f, *unit, unit->player->ownClass);
        }
        if (unit->player->info[0x42] == p.info[0x42]) {
            continue;
        }
        // Its class changed: everyone told (at once, ahead of a map to load: its client builds that
        // map's meters and bars from it).
        p.info[0x42] = unit->player->info[0x42];
        p.accept.classId = unit->player->classId;
        net::msg::NewPlayer told;
        told.netCode = static_cast<std::uint16_t>(p.extent);
        told.playerInfo = p.info;
        told.accountName = p.accept.account;
        const auto bytes = bytesOf(told);
        for (const auto& [other, q] : players_) {
            if (other == peer || q.joined) {
                transport_.sendReliable(other, bytes);
            }
        }
    }
}

void GameHost::FrameIndex::build(World& world)
{
    const auto units = world.playerUnits();
    players.assign(units.begin(), units.end());
    const auto globes = world.sentryUnits();
    sentries.assign(globes.begin(), globes.end());
    keys.clear();
    if (world.quest()) {
        // (What each holds can't change while the frame's updates go out: worked out once.)
        for (const ServerObject* o : players) {
            std::uint8_t k = 0;
            for (const std::uint32_t e : o->inventory) {
                if (const ServerObject* i = world.byExtent(e)) {
                    k |= i->typeName == NameId::SilverKey ? 1 : i->typeName == NameId::GoldKey ? 2 : 0;
                }
            }
            keys.push_back(k);
        }
    }
}
// Sends what the frame made: silent players dropped, removals and wall notices told, then for
// each player its broadcasts, its update, the sounds it hears and its changed reports.
void GameHost::afterFrame()
{
    // (SrvNet_UpdateAllClients) A player not heard from for 90 s, 30 s once it's in (status 0x10):
    // everyone told (MSG_INFORM 3, its code), it told (0xC6, at once), dropped (reason 3).
    {
        const std::uint32_t now = server_.clock().frame();
        std::vector<std::uint8_t> silent;
        for (const auto& [peer, p] : players_) {
            const ServerObject* unit = server_.world().byExtent(p.extent);
            if (!unit || !unit->player) {
                continue;
            }
            const std::uint32_t limit = 30u * ((unit->player->statusFlags & 0x10u) ? 30u : 90u);
            if (limit < now - p.heardAt) {
                silent.push_back(peer);
            }
        }
        for (const std::uint8_t peer : silent) {
            // (SrvNet_BroadcastInform 3: Net_QueueMsg, the frame's datagram of each player with a
            // unit, ready or not; the silent ones' own netlists never go, Net_CloseStream's flush
            // being Net_FlushStream(.., 1): no netlist.)
            const std::uint32_t code = players_[peer].extent;
            const std::vector<std::uint8_t> inform = {0xA9, 0x03, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                      static_cast<std::uint8_t>(code >> 16), static_cast<std::uint8_t>(code >> 24)};
            for (const auto& [q, h] : players_) {
                if (std::ranges::find(silent, q) == silent.end()) {
                    transport_.sendUnreliable(q, inform);
                }
            }
            transport_.sendReliableNow(peer, std::vector<std::uint8_t>{0xC6}, nowMs_);
            disconnect(peer, 3);
        }
    }
    tellModeChange();
    // What the frame told the clients (deaths, the kill feed, respawns, inventories), in order;
    // a player's own before it's ready wait for it.
    auto broadcasts = std::exchange(server_.world().broadcasts(), {});
    const bool forPeer = static_cast<bool>(options_.forPeer); // (noxcpp: a mode's per-client wording)
    // What left the world: the clients that knew it hear it's gone (MSG_DESTROY_OBJECT 0x31:
    // SrvNet_ReportDestroyObject's Net_QueueImportant(its index, .., onlyJoined 1)).
    for (const std::uint32_t gone : std::exchange(server_.world().removed(), {})) {
        for (auto& [peer, p] : players_) {
            if (p.knows.erase(gone) != 0) {
                sendImportant(peer,
                              std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::DestroyObject), static_cast<std::uint8_t>(gone),
                                                        static_cast<std::uint8_t>(gone >> 8)},
                              true);
            }
        }
    }
    for (auto& [peer, p] : players_) {
        if (!p.ready) {
            // Its own important ones, and once it's sent INCOMING_CLIENT (in g_playersInGameMask)
            // those to everyone: Net_QueueImportant(0xFF)'s node waits till each in-game player has
            // it, so one still loading gets it at its CLIENT_READY (the map start's
            // MSG_FADE_BEGIN {1, 1} too). (Those that aren't reliable go now, below.)
            for (const auto& b : broadcasts) {
                if (b.reliable && (b.to == p.extent || (b.to == 0 && p.joined))) {
                    auto other = forPeer ? options_.forPeer(*this, peer, b.bytes) : std::nullopt;
                    p.pending.push_back({other ? std::move(*other) : b.bytes, false});
                }
            }
        }
    }
    // The Wall spell's ends, to those who were sent the walls (SrvNet_BroadcastMagicWallRevert
    // SrvNet_BroadcastMagicWallRevert: Net_QueueImportant(each one's index, .., onlyJoined 1)).
    for (const auto& notice : std::exchange(server_.world().wallNotices(), {})) {
        for (auto& [peer, p] : players_) {
            if (notice.players & (1u << (p.slot & 31))) {
                sendImportant(peer, notice.bytes, true);
            }
        }
    }
    // A player moved at a stroke: everything's owed to it afresh, and it knows
    // nothing but the creatures and the always-sent (0x20400006) and the destroyed.
    for (const std::uint32_t moved : std::exchange(server_.world().forgetting(), {})) {
        for (auto& [peer, p] : players_) {
            if (p.extent != moved) {
                continue;
            }
            const std::uint32_t bit = 1u << (p.slot & 31);
            for (auto& [extent, o] : server_.world().objects()) {
                o.syncMask |= bit;
                if (!(o.flags & ObjFlag::Destroyed) && !(o.classes & 0x20400006u)) {
                    o.knownBy &= ~bit;
                    p.knows.erase(extent);
                }
            }
        }
    }
    // SrvNet_UpdateAllClients: each player in the game gets its update, and its
    // reports (stamina: SrvNet_SendReportStamina; its health when it changed: 0x43, all of it when it's new or
    // back: 0xDD; all reliable).
    // In a WOL game a player's stream (not the host's own) is flushed only
    // every `rate` frames or with the match over (game flag 8): Net_FlushStream(.., 0) at the
    // loop's end; between, what it's sent waits in the transport (holdFlush).
    frameIndex_.build(server_.world()); // (the players' updates' look-ups: the world as the frame left it)
    const std::uint32_t frameNow = server_.clock().frame();
    const bool flushDue = !server_.world().wolGame || frameNow % static_cast<std::uint32_t>(std::max(netRate_, 1)) == 0 ||
                          (server_.world().gameFlags & GameFlag::GameOver);
    for (auto& [peer, p] : players_) {
        transport_.holdFlush(peer, !flushDue && !isHostPlayer(p));
        if (!p.ready) {
            // Not in the game yet: what the frame sent everyone that isn't important (Net_QueueMsg's
            // netlist: the effects, SrvNet_SendToPlayersSeeingPoint...) and its own, then
            // MSG_TIMESTAMP (0x27, the frame's low u16) instead of the update, flushed. Its
            // datagram's header is what acks the client's reliable packets (its join data,
            // MSG_INCOMING_CLIENT); without it a stock client resends them, and its connect, for
            // seconds before going on.
            for (const auto& b : broadcasts) {
                if (!b.reliable && (b.to == 0 || b.to == p.extent)) {
                    transport_.sendUnreliable(peer, b.bytes);
                }
            }
            transport_.sendUnreliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::Timestamp),
                                                                       static_cast<std::uint8_t>(frameNow),
                                                                       static_cast<std::uint8_t>(frameNow >> 8)});
            if (flushDue || isHostPlayer(p)) {
                transport_.flushUnreliable(peer);
            }
            continue;
        }
        const auto send = [&](const World::Broadcast& b) {
            if (b.reliable) {
                if (auto other = forPeer ? options_.forPeer(*this, peer, b.bytes) : std::nullopt) {
                    transport_.sendReliable(peer, std::move(*other));
                } else {
                    transport_.sendReliable(peer, b.bytes);
                }
            } else {
                transport_.sendUnreliable(peer, b.bytes);
            }
        };
        releasePending(p); // (released at its CLIENT_READY already: what's left, none)
        for (const auto& b : broadcasts) {
            if (b.to == 0 || b.to == p.extent) {
                send(b);
            }
        }
        {
            ProfileScope timed(ProfileSection::HostUpdates);
            updatePlayer(p);
        }
        ServerObject* unit = server_.world().byExtent(p.extent);
        if (!unit || !unit->player) {
            continue;
        }
        {
            ProfileScope timed(ProfileSection::HostSounds);
            sendHeardSounds(peer, p, *unit);
        }
        sendReports(peer, *unit);
    }
    server_.world().sounds().clear(); // (the frame's list freed)
}

// The frame's sounds a player in the game hears (netUpdateRemotePlr), to it.
void GameHost::sendHeardSounds(std::uint8_t peer, const HostedPlayer& p, ServerObject& unit)
{
    PlayerControl& pc = *unit.player;
    // The frame's sounds it hears (netUpdateRemotePlr): from its camera
    // (camX / camY: loudness and pan), in the polygon group of what it watches (an
    // observer following something) or its own.
    World& world = server_.world();
    if (pc.polygon == kPolygonUnknown) {
        updatePolygon(world, unit);
    }
    const bool observing = (pc.statusFlags & 3u) != 0;
    SoundListener l;
    l.extent = unit.extent;
    l.team = unit.team;
    l.camX = pc.camX;
    l.camY = pc.camY;
    l.halfWidth = p.viewHalfWidth;
    l.group = pc.polygonGroup;
    l.hearsOwnPhonemes = pc.hearsOwnPhonemes;
    if (const ServerObject* f = observing && pc.cameraFollow ? world.byExtent(pc.cameraFollow) : nullptr) {
        if (f->player) {
            l.group = f->player->polygonGroup;
        } else {
            const std::uint32_t at = world.polygonAt(static_cast<int>(f->x), static_cast<int>(f->y), 0);
            l.group = at != 0 ? world.polygons()[at - 1].group : std::uint8_t{0};
        }
    }
    std::vector<std::uint8_t>& heard = updateScratch_.sounds;
    soundsFor(world, server_.soundTable(), l, heard);
    if (!heard.empty()) {
        transport_.sendUnreliable(peer, heard); // (Net_QueueMsg: the frame's packet)
    }
}

// A player's own reports (stamina, armour, worn enchantments, gold, a quest's lives and keys,
// health, mana, numbers), each sent when it changed since it was last sent.
void GameHost::sendReports(std::uint8_t peer, ServerObject& unit)
{
    PlayerControl& pc = *unit.player;
    // Its own reports, each sent only when the value changed since it was last sent.
    if (pc.reportStamina) {
        pc.reportStamina = false;
        transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportStamina), pc.stamina});
    }
    if (pc.armorSent != pc.armorValue) { // (playerReportAnything: its float)
        const auto bits = std::bit_cast<std::uint32_t>(pc.armorValue);
        transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportArmorValue),
                                                                static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8),
                                                                static_cast<std::uint8_t>(bits >> 16), static_cast<std::uint8_t>(bits >> 24)});
        pc.armorSent = pc.armorValue;
    }
    if (pc.itemBuffsSent != unit.itemBuffs) { // (its worn enchantments, SrvNet_SendReportItemEnchantment)
        transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportItemEnchantment), unit.itemBuffs});
        pc.itemBuffsSent = unit.itemBuffs;
    }
    if (pc.goldSent != pc.gold) { // (SrvNet_SendReportGold: MSG_REPORT_GOLD 0x4A)
        transport_.sendReliable(peer, std::vector<std::uint8_t>{0x4A, static_cast<std::uint8_t>(pc.gold), static_cast<std::uint8_t>(pc.gold >> 8),
                                                                static_cast<std::uint8_t>(pc.gold >> 16), static_cast<std::uint8_t>(pc.gold >> 24)});
        pc.goldSent = pc.gold;
    }
    if (server_.world().quest()) {
        ProfileScope timed(ProfileSection::HostReports);
        // Each player's lives as this one was last told them (SrvNet_SendGauntletLives: MSG_GAUNTLET 4,
        // its lives and code).
        for (ServerObject* po : frameIndex_.players) {
            ServerObject& o = *po;
            if (o.gone()) {
                continue;
            }
            auto& sent = o.player->livesSent[pc.index & 31u];
            if (sent != static_cast<std::uint8_t>(o.player->questLives)) {
                transport_.sendReliable(peer, std::vector<std::uint8_t>{0xF0, 0x04, static_cast<std::uint8_t>(o.player->questLives),
                                                                        static_cast<std::uint8_t>(o.extent), static_cast<std::uint8_t>(o.extent >> 8)});
                sent = static_cast<std::uint8_t>(o.player->questLives);
            }
        }
        // Whether each player holds a SilverKey / GoldKey as this one was last told
        // (the gauntlet key reports: MSG_GAUNTLET 0x16 / 0x17, the byte and code).
        for (std::size_t n = 0; n < frameIndex_.players.size(); ++n) {
            ServerObject& o = *frameIndex_.players[n];
            if (o.gone()) {
                continue;
            }
            const std::uint8_t keys = frameIndex_.keys[n];
            const std::pair<std::uint8_t, std::uint8_t> kinds[2] = {{0x16, static_cast<std::uint8_t>(keys & 1u)},
                                                                    {0x17, static_cast<std::uint8_t>((keys >> 1) & 1u)}};
            for (const auto& [sub, has] : kinds) {
                auto& sent = sub == 0x16 ? o.player->silverKeySent[pc.index & 31u] : o.player->goldKeySent[pc.index & 31u];
                if (sent != has) {
                    transport_.sendReliable(peer, std::vector<std::uint8_t>{0xF0, sub, has, static_cast<std::uint8_t>(o.extent),
                                                                            static_cast<std::uint8_t>(o.extent >> 8)});
                    sent = has;
                }
            }
        }
    }
    const auto code = static_cast<std::uint16_t>(unit.extent);
    if (pc.healthSent < 0) {
        transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportTotalHealth),
                                                                static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                                static_cast<std::uint8_t>(unit.health), static_cast<std::uint8_t>(unit.health >> 8),
                                                                static_cast<std::uint8_t>(unit.maxHealth),
                                                                static_cast<std::uint8_t>(unit.maxHealth >> 8)});
        pc.healthSent = unit.health;
    } else if (pc.healthSent != unit.health) {
        transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportPlayerHealth),
                                                                static_cast<std::uint8_t>(unit.health),
                                                                static_cast<std::uint8_t>(unit.health >> 8)});
        pc.healthSent = unit.health;
        if (server_.world().quest()) { // (SrvNet_SendPlayerHealth: its share to everyone else, index | 0x80, onlyJoined 1)
            sendImportantToAll(questTeamHealthMessage(unit), true, peer);
        }
    }
    // Its mana (a caster's): all of it when new or back (0xDE), else when it changed (0x45).
    if (pc.maxMana > 0) {
        if (pc.manaSent < 0) {
            transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportTotalMana),
                                                                    static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                                    static_cast<std::uint8_t>(pc.mana), static_cast<std::uint8_t>(pc.mana >> 8),
                                                                    static_cast<std::uint8_t>(pc.maxMana),
                                                                    static_cast<std::uint8_t>(pc.maxMana >> 8)});
            pc.manaSent = pc.mana;
        } else if (pc.manaSent != pc.mana) {
            transport_.sendReliable(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportMana),
                                                                    static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                                    static_cast<std::uint8_t>(pc.mana), static_cast<std::uint8_t>(pc.mana >> 8)});
            pc.manaSent = pc.mana;
        }
    }
    // Its numbers, after the totals (reportStats: SrvNet_SendReportStats, important).
    if (pc.reportStats) {
        transport_.sendReliable(peer, bytesOf(net::msg::ReportStats{.netCode = code,
                                                                    .maxHealth = unit.maxHealth,
                                                                    .maxMana = pc.maxMana,
                                                                    .carryCapacity = unit.carryCapacity,
                                                                    .speed = static_cast<std::uint16_t>(pc.speed),
                                                                    .strength = static_cast<std::uint16_t>(pc.strength),
                                                                    .level = pc.level}));
        pc.reportStats = false;
    }
}

namespace {

/// A player's record (SrvNet_SendPlayerObj): its position (Math_FloatToInt: truncated), its
/// facing, and what it's doing (no frame for the states so far).
UpdateRecord playerRecord(const ServerObject& o, const World& world)
{
    const auto coord = [](float v) { return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 65535.0f)); };
    UpdateRecord r{UpdateRecord::Kind::Player, static_cast<std::uint16_t>(o.extent), o.type, coord(o.x), coord(o.y),
                   directionNibble(o.direction), 0xFF, 0};
    if (o.player) {
        r.state = playerAnimation(o, r.frame, &world);
    }
    return r;
}

} // namespace

// In order: the magic walls, its own height, the objects in view into the update stream, the
// secret walls, the minimap marks, the out-of-sight notices and the sentry beams. The whole
// payload goes as one unreliable datagram.
void GameHost::updatePlayer(HostedPlayer& p)
{
    namespace oc = things::ObjectClass;
    World& world = server_.world();
    ServerObject* unit = world.byExtent(p.extent);
    if (!unit) {
        return;
    }
    const std::uint32_t frame = server_.clock().frame();
    const std::uint32_t bit = 1u << (p.slot & 31);
    // The Wall spell's walls it hasn't had, once it's ready (SrvNet_SendMagicWallsToPlayer
    // SrvNet_SendMagicWallsToPlayer: MSG_CHANGE_OR_ADD_WALL_MAGIC 0x3D, the material, shape, look and cell).
    if (unit->player && (unit->player->statusFlags & 0x10u)) {
        for (MagicWall& mw : world.magicWalls()) {
            if (mw.sentTo & bit) {
                continue;
            }
            if (const WallCell* c = world.wall(mw.x, mw.y)) {
                transport_.sendReliable(p.peer, std::vector<std::uint8_t>{0x3D, c->material, c->direction, c->variant,
                                                                          static_cast<std::uint8_t>(mw.x), static_cast<std::uint8_t>(mw.y)});
            }
            mw.sentTo |= bit;
        }
    }
    // What's owed of an object it's sent (netUpdateObjectSpecial): its
    // animation frame (MSG_REPORT_ANIMATION_FRAME 0x6B: the object and the frame, u32), its
    // enchantments (MSG_REPORT_ENCHANTMENT 0x5A: the object and its bits).
    const auto special = [&](ServerObject& o) {
        const std::uint16_t code = netCodeOf(o); // (a map object's: bit 15)
        if (o.animOwed & bit) {
            o.animOwed &= ~bit;
            transport_.sendReliable(p.peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportAnimationFrame),
                                                                      static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                                      static_cast<std::uint8_t>(o.animFrame), static_cast<std::uint8_t>(o.animFrame >> 8),
                                                                      static_cast<std::uint8_t>(o.animFrame >> 16),
                                                                      static_cast<std::uint8_t>(o.animFrame >> 24)});
        }
        // Turned on or off (SrvNet_SendEnableDisableObject: MSG_ENABLE / DISABLE_OBJECT 0x37 /
        // 0x38, the object; only the VISIBLE_ENABLE kinds, 0x800000), its xStatus
        // (SrvNet_SendReportXStatus: MSG_REPORT_X_STATUS 0x65, the object and the bits, u32).
        if (o.enableOwed & bit) {
            o.enableOwed &= ~bit;
            if (o.classes & oc::VisibleEnable) {
                transport_.sendReliable(p.peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(o.enabled() ? net::Op::EnableObject
                                                                                                                 : net::Op::DisableObject),
                                                                          static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8)});
            }
        }
        if (o.xStatusOwed & bit) {
            o.xStatusOwed &= ~bit;
            transport_.sendReliable(p.peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportXStatus),
                                                                      static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                                      static_cast<std::uint8_t>(o.xStatus), static_cast<std::uint8_t>(o.xStatus >> 8),
                                                                      static_cast<std::uint8_t>(o.xStatus >> 16),
                                                                      static_cast<std::uint8_t>(o.xStatus >> 24)});
        }
        // Its height (SrvNet_SendUnitHeight: MSG_REPORT_Z_PLUS 0x5E, or _MINUS 0x5F
        // below the floor with its depth, the object and the height truncated to a byte; under a
        // DeltaZ spell, xStatus 0x20, MSG_FX_DELTAZ_SPELL_START 0x9F instead: heightReport).
        if (o.heightOwed & bit) {
            o.heightOwed &= ~bit;
            transport_.sendReliable(p.peer, heightReport(o));
        }
        if (o.enchantOwed & bit) {
            o.enchantOwed &= ~bit;
            transport_.sendReliable(p.peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportEnchantment),
                                                                      static_cast<std::uint8_t>(code),
                                                                      static_cast<std::uint8_t>(code >> 8),
                                                                      static_cast<std::uint8_t>(o.buffs), static_cast<std::uint8_t>(o.buffs >> 8),
                                                                      static_cast<std::uint8_t>(o.buffs >> 16),
                                                                      static_cast<std::uint8_t>(o.buffs >> 24)});
        }
        // Its modifiers (SrvNet_SendReportModifier, 0x2000000: MSG_REPORT_MODIFIER 0x67, the
        // object and its four ids, 0xFF for none), owed as a weapon, armour or flag (class
        // 0x13001000) gets known, or as anything's are set (Items::setModifiers, a TeamBase's
        // too). On the ground the client colours it by them (a CTF flag by its MaterialTeam...).
        if (o.modifierOwed & bit) {
            o.modifierOwed &= ~bit;
            transport_.sendReliable(p.peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportModifier),
                                                                      static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                                                      o.modifiers[0], o.modifiers[1], o.modifiers[2], o.modifiers[3]});
        }
        // An NPC's colours (SrvNet_SendReportNPC, 0x4000000), then each worn thing's
        // class to this player (SrvNet_SendReportEquipClass).
        if (o.npcOwed & bit) {
            o.npcOwed &= ~bit;
            transport_.sendReliable(p.peer, npcReport(o));
            for (const std::uint32_t e : o.inventory) {
                const ServerObject* item = world.byExtent(e);
                if (item && (item->flags & ObjFlag::Equipped)) {
                    if (auto m = equipClassReport(o, *item); !m.empty()) {
                        transport_.sendReliable(p.peer, std::move(m));
                    }
                }
            }
        }
    };
    // SrvNet_UpdatePlayer: its own height every 15 s, staggered by its slot (not the
    // host's own, 0x1F).
    if ((p.slot & 31) != 0x1F && (frame + (p.slot & 31)) % (30u * 15u) == 0) {
        transport_.sendReliable(p.peer, heightReport(*unit));
    }
    // The payload: the frame's timestamp, then the update stream, then the notices. (Its buffers
    // are the host's, kept from update to update: updateScratch_.)
    UpdateScratch& scratch = updateScratch_;
    std::vector<std::uint8_t>& payload = scratch.payload;
    payload.clear();
    net::msg::Timestamp ts;
    ts.frame16 = static_cast<std::uint16_t>(frame);
    net::appendBytesOf(payload, ts);

    // Its camera (camX / camY): an observer's own, else what it follows or its unit as the
    // frame began (updatePlayer). An observer is sent the camera alone
    // (SrvNet_SendObserverCamera: net code and type 0, frame 0xFF), else its player
    // record.
    const bool observing = unit->player && (unit->player->statusFlags & 3u);
    const float camX = unit->player ? unit->player->camX : unit->x;
    const float camY = unit->player ? unit->player->camY : unit->y;
    // SrvNet_SendHealthDelta, from SrvNet_SendPlayerObj, _SendComplexObj and
    // _SendSimpleObj as each object goes: one with health, more than 2 frames past its last hurt
    // (its hurt frame), whose health isn't what this player last had: a fall goes to it as a floating
    // number (MSG_REPORT_HEALTH_DELTA 0x42, the object and the change: Net_QueueImportant,
    // onlyJoined 1), and what it has now is kept either way.
    const std::size_t slot = p.slot & 31u;
    const auto healthDelta = [&](const ServerObject& o, std::uint16_t& seen) {
        if (o.maxHealth == 0 || !(2u < frame - o.hurtFrame) || o.health == seen) {
            return;
        }
        if (const auto delta = static_cast<std::int16_t>(static_cast<int>(o.health) - static_cast<int>(seen)); delta < 0) {
            const std::uint16_t code = netCodeOf(o);
            const auto d = static_cast<std::uint16_t>(delta);
            sendImportant(p.peer,
                          std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportHealthDelta), static_cast<std::uint8_t>(code),
                                                    static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(d), static_cast<std::uint8_t>(d >> 8)},
                          true);
        }
        seen = o.health;
    };
    UpdateRecord self = playerRecord(*unit, world);
    if (!observing) {
        healthDelta(*unit, unit->healthSeen[slot]); // (SrvNet_UpdatePlayer's SrvNet_SendPlayerObj of its own unit)
    }
    if (observing) {
        const auto coord = [](float v) { return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 65535.0f)); };
        self = UpdateRecord{UpdateRecord::Kind::Player, 0, 0, coord(camX), coord(camY), 0, 0xFF, 0};
    }
    // The view: its half screen and 100 more each way about the camera (SrvNet_UpdatePlayer's first
    // rectangle). What's in it, as getUnitsInRectAdv finds it: a missile
    // by the map grid's cells (85 units: chopped; below 0, Math_FloatToInt's), its
    // position's cell among those the view's corners are in; anything else by its collide box
    // (CollideP1 / P2; a shapeless one's stays 0, 0, 0, 0) overlapping the view.
    // (On the x87: the half screen by FILD, the camera and the margin added there, each edge
    // stored: chopped.)
    using util::X87;
    const auto edge = [](float cam, float half, float margin) { return (X87(cam) + X87::fromDouble(half) + margin).f(); };
    constexpr float kViewMargin = 100;
    const float left = edge(camX, -p.viewHalfWidth, -kViewMargin);
    const float right = edge(camX, p.viewHalfWidth, kViewMargin);
    const float top = edge(camY, -p.viewHalfHeight, -kViewMargin);
    const float bottom = edge(camY, p.viewHalfHeight, kViewMargin);
    // The second, 128 more each way: what's out of it is out of sight; a mark out of it
    // goes now and then all the same.
    constexpr float kSightMargin = 128;
    const float farLeft = edge(camX, -p.viewHalfWidth, -kSightMargin);
    const float farRight = edge(camX, p.viewHalfWidth, kSightMargin);
    const float farTop = edge(camY, -p.viewHalfHeight, -kSightMargin);
    const float farBottom = edge(camY, p.viewHalfHeight, kSightMargin);
    // (Its collide box as the grid has it: CollideP1 / P2 as last linked.)
    const auto boxOf = [&](const ServerObject& o) {
        const MapGrid::Rect* b = world.grid().box(o);
        return b ? std::array<float, 4>{b->left, b->top, b->right, b->bottom} : std::array<float, 4>{0, 0, 0, 0};
    };
    std::vector<UpdateRecord>& others = scratch.records;
    others.clear();
    // Whose each record is, and whether the player knew it before this update (a record the
    // stream can't take is owed again, as below).
    std::vector<std::pair<ServerObject*, bool>>& recordOf = scratch.recordOf;
    recordOf.clear();
    std::vector<std::uint8_t>& notices = scratch.notices;
    notices.clear();
    PlayerControl* control = unit->player ? &*unit->player : nullptr;
    const auto marked = [&](std::uint32_t extent) {
        return control && std::ranges::find(control->minimapMarks, extent, &PlayerControl::MinimapMark::object) != control->minimapMarks.end();
    };
    // An object's record in this update (netSendObjects2Plr's), it known to the player from now.
    const auto sendRecord = [&](ServerObject& o, bool known) {
        const std::uint32_t extent = o.extent;
        UpdateRecord r;
        r.netCode = static_cast<std::uint16_t>(extent);
        r.typeId = o.type;
        r.x = static_cast<std::uint16_t>(std::clamp(o.x, 0.0f, 65535.0f));
        r.y = static_cast<std::uint16_t>(std::clamp(o.y, 0.0f, 65535.0f));
        if (o.classes & oc::Player) {
            r = playerRecord(o, world);
            healthDelta(o, o.healthSeen[slot]); // (SrvNet_SendPlayerObj)
        } else if (o.classes & 0x200000u) {
            r.kind = UpdateRecord::Kind::Complex;
            r.anim = directionNibble(o.direction);
            if ((o.classes & oc::Missile) && (o.subclasses & 0x30u)) {
                // (SrvNet_SendComplexObjGeneric) An arrow, bolt or harpoon (subclass 0x30):
                // its direction / 8 as its frame, ArrowDraw's / HarpoonDraw's image;
                // the other complex things 0xFF.
                r.frame = static_cast<std::uint8_t>(o.direction >> 3);
            }
            if (o.monster) { // (SrvNet_SendComplexObj: its action's animation state and frame)
                r.anim = static_cast<std::uint8_t>(r.anim | monsterAnimState(o));
                r.frame = o.monster->animFrame;
                // An NPC shocked twitches now and then: 3 times in 11, state 0xE at a
                // frame drawn from the player's animation 0x32.
                if ((o.subclasses & 0x10u) && o.monster->field523 != 0) {
                    auto& rng = server_.fight().rng;
                    if (7 < nox::util::uniform_int_distribution<int>(0, 10)(rng)) {
                        const int frames = world.playerAnimations ? (*world.playerAnimations)[0x32].first : 0;
                        r.anim = static_cast<std::uint8_t>(directionNibble(o.direction) | 0xE);
                        r.frame = static_cast<std::uint8_t>(nox::util::uniform_int_distribution<int>(0, std::max(frames, 0))(rng));
                    }
                }
                healthDelta(o, o.monster->healthSeen[slot]); // (SrvNet_SendComplexObj)
            }
        } else if (o.classes & oc::MonsterGenerator) {
            healthDelta(o, o.healthSeen[slot]); // (SrvNet_SendSimpleObj: class 0x20000)
        }
        others.push_back(r);
        recordOf.emplace_back(&o, known);
        if (!known && !(o.classes & 0x20400004u) && o.buffs != 0) {  // (those bits: ClientPersist, Immobile, Player)
            o.enchantOwed |= bit; // (newly known: its standing reports owed, netSendObjects2Plr)
        }
        if (!known && !(o.classes & 0x20400004u) && !o.enabled()) {
            // Off, unlike its type (every type's ENABLED: the owed bits' 4): MSG_DISABLE_OBJECT
            // owed (a chat map's team flags, hidden till its teams turn them on).
            o.enableOwed |= bit;
        }
        if (!known && !(o.classes & 0x20400004u) && o.z != 0) {
            o.heightOwed |= bit; // (its height off the floor standing: the owed bits' 0x40)
        }
        if (!known && !(o.classes & 0x20400004u) && o.npcState) {
            o.npcOwed |= bit; // (an NPC's report: its state 0x400 standing)
        }
        if (!known && !(o.classes & 0x20400004u) && (o.classes & 0x13001000u)) {
            o.modifierOwed |= bit; // (a weapon's, armour's or flag's modifiers: the owed bits' 0x200)
        }
        if (!known && !(o.classes & 0x20400004u) && o.xStatus != 0) {
            // Its xStatus standing (the owed bits' 8: unlike its type's, which is 0 for
            // the types here): an NPC with something to say (0x10, SetDialog), a chest's state.
            o.xStatusOwed |= bit;
        }
        p.knows.insert(extent);
        o.knownBy |= bit;
        o.syncMask &= ~bit;
        special(o); // (sent in this update: approximately, as the stream may drop the last records)
    };
    // An immobile thing's state, owed to the player (netSendObjects2Plr's SrvNet_SendSpriteState):
    // a door its direction (MSG_DOOR_ANGLE); an elevator its height / 4, a shaft its elevator's
    // (none: 0), as its frame (MSG_DRAW_FRAME 0x39); a pressure plate whether it's pressed
    // (MSG_PENTAGRAM_ACTIVATE 0xB4); a teleport pentagram its frame (MSG_DRAW_FRAME), a Spike
    // whether it's off; the rest what's owed (special: a lever's frame); an obelisk its mana
    // (MSG_OBELISK_CHARGE). Never a record in the stream: the client draws these from its own map.
    const auto spriteState = [&](ServerObject& o) {
        const std::uint16_t code = netCodeOf(o); // (extent | 0x8000: the client's own copy)
        if (o.subclasses & 0x18u) {
            // An obelisk: its mana (MSG_OBELISK_CHARGE 0xB3, reliable).
            transport_.sendReliable(p.peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ObeliskCharge), static_cast<std::uint8_t>(code),
                                                                      static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(o.value)});
        } else if (o.classes & oc::Door) {
            notices.insert(notices.end(), {static_cast<std::uint8_t>(net::Op::DoorAngle), static_cast<std::uint8_t>(code),
                                           static_cast<std::uint8_t>(code >> 8), o.doorDirection});
        } else if (o.typeName == NameId::Spike) {
            // (a Spike: frame 1 while off)
            notices.insert(notices.end(), {static_cast<std::uint8_t>(net::Op::DrawFrame), static_cast<std::uint8_t>(code),
                                           static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(o.enabled() ? 0 : 1)});
        } else if (o.typeName == NameId::TeleportPentagram) {
            notices.insert(notices.end(), {static_cast<std::uint8_t>(net::Op::DrawFrame), static_cast<std::uint8_t>(code),
                                           static_cast<std::uint8_t>(code >> 8), o.transport ? o.transport->frame : std::uint8_t{0}});
        } else if (o.typeName == NameId::PressurePlate) {
            notices.insert(notices.end(), {static_cast<std::uint8_t>(net::Op::PentagramActivate), static_cast<std::uint8_t>(code),
                                           static_cast<std::uint8_t>(code >> 8),
                                           static_cast<std::uint8_t>(o.trigger && (o.trigger->flags & 1u) ? 1 : 0)});
        } else if (!(o.classes & 0xC000u)) {  // (0xC000: elevators and their shafts)
            special(o);
        } else {
            int level = o.elevatorLevel;
            if (o.classes & 0x8000u) {
                const ServerObject* e = o.elevatorLink ? world.byExtent(o.elevatorLink) : nullptr;
                level = e ? e->elevatorLevel : 0;
            }
            notices.insert(notices.end(), {static_cast<std::uint8_t>(net::Op::DrawFrame), static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8),
                                           static_cast<std::uint8_t>(static_cast<std::uint8_t>(level) >> 2)});
        }
        o.syncMask &= ~bit;
    };
    // (What's in view, as getUnitsInRectAdv finds it in the map grid, in its order:
    // the records go out so, unitAroundPlayerFn on each.)
    std::vector<ServerObject*>& candidates = scratch.candidates;
    candidates.clear();
    world.unitsInRectAdv(left, top, right, bottom, [&](ServerObject& o) { candidates.push_back(&o); });
    // Each object the view's rectangle found: its state, its record, or its out-of-sight notice.
    for (ServerObject* candidate : candidates) {
        ServerObject& o = *candidate;
        const std::uint32_t extent = o.extent;
        const bool own = extent == p.extent;
        if (own) {
            // (unitAroundPlayerFn) Its own unit, as the walk reaches it: its
            // specials (netUpdateObjectSpecial); an observer's (status & 1) then goes
            // on as any object (netSendObjects2Plr: its own record), else that's all.
            special(o);
            if (!(unit->player && (unit->player->statusFlags & 1u))) {
                continue;
            }
        }
        if (o.pendingDelete || o.holder != 0) { // (netSendObjects2Plr: not 0x20, ours' pendingDelete)
            continue;
        }
        const bool inView = true; // (the grid found it in view)
        // A change in its friendship to this player (netSendObjects2Plr's first: SrvNet_SendFriendAddRemove,
        // MSG_OBJECT_FRIEND_ADD 0x34 / _REMOVE 0x35, its code), not its own: the client marks its
        // friends (our creatures' dot, MonsterDraw).
        if (inView && !own && (o.friendOwed & bit) && !(o.classes & 0x40000000u)) {  // (that bit: ClientPredict)
            const std::uint16_t code = netCodeOf(o);
            sendImportant(p.peer, {static_cast<std::uint8_t>((o.friendOf & bit) ? 0x34 : 0x35), static_cast<std::uint8_t>(code),
                                   static_cast<std::uint8_t>(code >> 8)},
                          true);
            o.friendOwed &= ~bit;
        }
        // The immobile in view that changed send their state (spriteState); no wall check for them.
        if ((o.classes & 0x400000u) && !(o.classes & 0x40000000u)) {  // (Immobile, not ClientPredict)
            if (inView && (o.syncMask & bit)) {
                spriteState(o);
            }
            continue;
        }
        // What goes in the stream (netSendObjects2Plr): simple and complex objects, not the hidden kind
        // (0x40000000).
        const bool streamed = (o.classes & (0x100000u | 0x200000u)) && !(o.classes & 0x40000000u);
        if (!streamed) {
            continue;
        }
        if (!inView) {
            continue;
        }
        const bool known = p.knows.contains(extent);
        const bool isMarked = marked(extent);
        if (!(o.syncMask & bit) && !(o.classes & oc::Missile)) {
            continue; // (nothing new for it)
        }
        // OWNER_VISIBLE (0x800: a glyph, a teleport mark) goes only to the player it's of
        // (unitHasThatParent), but in a co-op game (0x200).
        if ((o.flags & 0x800u) && !(world.gameFlags & GameFlag::Coop)) {
            const ServerObject* parent = &o;
            for (int guard = 0; parent && parent != unit && guard < 16; ++guard) {
                parent = parent->owner ? world.byExtent(parent->owner) : nullptr;
            }
            if (parent != unit) {
                continue;
            }
        }
        // Walls hide it from the player's eye (its camera), not for the always-seen kinds
        // (0x20400000) nor its marks.
        if (!(o.classes & 0x20400000u) && !isMarked && !world.traceRay(camX, camY, o.x, o.y, Trace::Sight)) {  // (those bits: ClientPersist, Immobile)
            // Behind a wall: a creature is out of sight, anything else in shadows (netSendObjects2Plr).
            if (known) {
                notices.push_back(static_cast<std::uint8_t>((o.classes & (oc::Monster | oc::Player)) ? net::Op::ObjectOutOfSight
                                                                                                     : net::Op::ObjectInShadows));
                notices.push_back(static_cast<std::uint8_t>(extent));
                notices.push_back(static_cast<std::uint8_t>(extent >> 8));
                p.knows.erase(extent);
                o.knownBy &= ~bit;
                o.syncMask |= bit;
            }
            continue;
        }
        sendRecord(o, known);
    }
    // The secret walls about it (secretWallCheckUnits: the list's, the cell's
    // corner strictly within the second rectangle) whose state isn't what it was last told
    // (2 and 3 open, 1 and 4 shut; any other open, always): MSG_OPEN_WALL (0x3B) /
    // MSG_CLOSE_WALL (0x3C) with the wall's id to every player (SrvNet_BroadcastWallOpen /
    // Close), its own bit in the record set or cleared. (A broken one, Map_GetWallAtGrid's none,
    // is passed over: an original bug.)
    for (const auto& [x, y] : world.secretWalls()) {
        const auto cx = static_cast<float>(x * 23);
        const auto cy = static_cast<float>(y * 23);
        if (!(farLeft < cx && cx < farRight && farTop < cy && cy < farBottom)) {
            continue;
        }
        WallCell* c = world.wall(x, y);
        if (!c) {
            continue;
        }
        const std::uint8_t state = c->secretState;
        const bool shut = state == 1 || state == 4;
        const bool told = (c->secretSeen & bit) != 0;
        // (Any other state leaves the compared value the player's unit pointer: never equal.)
        const bool owed = shut ? told : (state == 2 || state == 3) ? !told : true;
        if (owed) {
            const WallCell* standing = world.gridWall(x, y);
            if (!standing) {
                continue;
            }
            // (Net_QueueImportant(each player unit's index, .., onlyJoined 1): held for one not
            // ready, as its own.)
            const std::uint16_t id = standing->wallId;
            const std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(shut ? 0x3C : 0x3B), static_cast<std::uint8_t>(id),
                                                 static_cast<std::uint8_t>(id >> 8)};
            for (const auto& [q, h] : players_) {
                sendImportant(q, m, true);
            }
            if (shut) {
                c->secretSeen &= ~bit;
            } else {
                c->secretSeen |= bit;
            }
        }
    }
    // Then its minimap's marks: one a while (each 60 / their count frames, every
    // frame past 60 of them), the list's head, which moves on; gone, it's unmarked for
    // all (netMinimapUnmark4All); by its position out of the second rectangle, it goes in this update with
    // its height (SrvNet_SendUnitHeight, MSG_REPORT_Z_PLUS / _MINUS; under a DeltaZ
    // spell, xStatus 0x20, MSG_FX_DELTAZ_SPELL_START: heightReport).
    if (control && !control->minimapMarks.empty()) {
        const auto count = static_cast<std::uint32_t>(control->minimapMarks.size());
        if (0x3D <= count || 60u / count < frame - control->markSentAt) {
            auto& marks = control->minimapMarks;
            const std::uint32_t head = marks.front().object;
            std::rotate(marks.begin(), marks.begin() + 1, marks.end());
            ServerObject* m = world.byExtent(head);
            if (!m || (m->flags & ObjFlag::Destroyed)) {
                for (auto& [e, q] : world.objects()) {
                    if (q.player) {
                        std::erase_if(q.player->minimapMarks, [&](const PlayerControl::MinimapMark& k) { return k.object == head && (k.bits & ~3u) == 0; });
                    }
                }
                if (m) {
                    m->xStatus &= ~1u;
                }
            } else if (m->x < farLeft || farRight < m->x || m->y < farTop || farBottom < m->y) {
                // (netSendObjects2Plr, as for what's in view: a hidden kind (ClientPredict) nothing;
                // an immobile thing its state, not a record: an obelisk a Drain Mana ray marked
                // has its charge sent, and the client keeps drawing its own copy of it.)
                m->syncMask |= bit;
                if (m->classes & 0x40000000u) {
                    // (nothing sent: its bit stays owed)
                } else if (m->classes & 0x400000u) {
                    spriteState(*m);
                } else {
                    sendRecord(*m, p.knows.contains(m->extent));
                }
                const auto height = heightReport(*m);
                notices.insert(notices.end(), height.begin(), height.end());
                control->markSentAt = frame;
            }
        }
    }
    // Every 8 frames of 16 ((frame & 8): SrvNet_UpdatePlayer), what it knows whose collide box is wholly out
    // of the second rectangle is told so: not the always-seen kinds (0x20400000) nor its marks
    // (playerMapTracksObj).
    if (frame & 8) {
        // (The exe walks the object list, then the missiles' list, no 0x20 test: what it knows, in
        // the objects' order (every object's walk's) but the missiles, then those it knows in the
        // missiles' list's.)
        const auto outOfSight = [&](ServerObject& o) {
            const std::uint32_t extent = o.extent;
            if (extent == p.extent || o.holder != 0 || (o.classes & 0x20400000u) || marked(extent)) {
                return false;
            }
            const auto b = boxOf(o);
            if (!(farRight < b[0] || b[2] < farLeft || farBottom < b[1] || b[3] < farTop)) {
                return false;
            }
            notices.push_back(static_cast<std::uint8_t>(net::Op::ObjectOutOfSight));
            notices.push_back(static_cast<std::uint8_t>(extent));
            notices.push_back(static_cast<std::uint8_t>(extent >> 8));
            o.knownBy &= ~bit;
            o.syncMask |= bit;
            return true;
        };
        for (auto it = p.knows.begin(); it != p.knows.end();) {
            ServerObject* known = world.byExtent(*it);
            if (known && !(known->classes & things::ObjectClass::Missile) && outOfSight(*known)) {
                it = p.knows.erase(it);
            } else {
                ++it;
            }
        }
        world.grid().eachMissile([&](ServerObject& m) {
            if (p.knows.contains(m.extent) && world.byExtent(m.extent) == &m && outOfSight(m)) {
                p.knows.erase(m.extent);
            }
        });
    }
    // The sentry globes' beams that cross its view (the beam's box against its
    // half screen about it) go every frame (SrvNet_SendSentryRay: MSG_FX_SENTRY_RAY
    // 0x95, the globe and the beam's end).
    for (const ServerObject* so : frameIndex_.sentries) {
        const ServerObject& o = *so;
        if (o.gone() || !o.enabled()) {
            continue;
        }
        const float bx0 = std::min(o.x, o.homeX);
        const float bx1 = std::max(o.x, o.homeX);
        const float by0 = std::min(o.y, o.homeY);
        const float by1 = std::max(o.y, o.homeY);
        // (Each edge on the x87, the half screen by FILD, stored: chopped.)
        const float vl = (X87(camX) - X87::fromDouble(p.viewHalfWidth)).f();
        const float vr = (X87::fromDouble(p.viewHalfWidth) + camX).f();
        const float vt = (X87(camY) - X87::fromDouble(p.viewHalfHeight)).f();
        const float vb = (X87::fromDouble(p.viewHalfHeight) + camY).f();
        if (vl < bx1 && bx0 < vr && vt < by1 && by0 < vb) {
            const auto u16 = [&](float v) {
                const auto i = static_cast<std::uint16_t>(static_cast<int>(v));
                notices.push_back(static_cast<std::uint8_t>(i));
                notices.push_back(static_cast<std::uint8_t>(i >> 8));
            };
            notices.push_back(0x95);
            u16(o.x);
            u16(o.y);
            u16(o.homeX);
            u16(o.homeY);
        }
    }
    // noxcpp's (no stock counterpart): an identified client is also told its own unit's whole place
    // when the stream's header (the exe's, kept for every client) drops its odd unit: a substep at
    // the frame itself (MSG_NOXCPP_SUBSTEP, substep 0), in the same datagram after the stream, so
    // the client takes both before it draws. Without it the frame said one place and the substeps
    // between frames another (a unit apart), a back and forth at a stop and an uneven run. A stock
    // client's datagram is as GAME.EXE sends it.
    std::vector<std::uint8_t>& exact = scratch.exact;
    exact.clear();
    if (!observing && transport_.identified(p.peer) && ((self.x | self.y) & 1u)) {
        exact.assign({static_cast<std::uint8_t>(net::Op::NoxcppSubstep), 0, static_cast<std::uint8_t>(server_.lastMultiplier()), 1,
                      static_cast<std::uint8_t>(frame), static_cast<std::uint8_t>(frame >> 8),
                      static_cast<std::uint8_t>(self.netCode), static_cast<std::uint8_t>(self.netCode >> 8),
                      static_cast<std::uint8_t>(self.x), static_cast<std::uint8_t>(self.x >> 8),
                      static_cast<std::uint8_t>(self.y), static_cast<std::uint8_t>(self.y >> 8)});
    }
    // The stream gets what's left of a 1000-byte datagram after the timestamp and the notices.
    std::vector<bool>& written = scratch.written;
    appendUpdateStream(payload, self, others, p.aliases, frame, 1000 - payload.size() - notices.size() - exact.size(), written, scratch.used);
    // A record the stream had no room for wasn't taken: still owed to the player and not known by
    // it, as netSendObjects2Plr leaves an object whose record the update list refused (its
    // needSync bit set, its known bit not), so it goes in a later update. (Taken as sent, a
    // creature's last records were lost for good once it lay dead: off the updatable list nothing
    // owes it again, and the client kept it mid-death.)
    for (std::size_t i = 0; i < recordOf.size(); ++i) {
        if (written[i]) {
            continue;
        }
        auto& [o, wasKnown] = recordOf[i];
        o->syncMask |= bit;
        if (!wasKnown) {
            p.knows.erase(o->extent);
            o->knownBy &= ~bit;
        }
    }
    append(payload, notices);
    append(payload, exact);
    transport_.sendUnreliable(p.peer, payload);
}

// A packet from a player: CLIENT_ACCEPT is a join; anything else is read as game messages back
// to back, each handled by its opcode.
void GameHost::onPayload(std::uint8_t peer, std::span<const std::uint8_t> bytes)
{
    // (SrvNet_OnPacket) Any packet, an empty one too, is word from it.
    if (const auto it = players_.find(peer); it != players_.end()) {
        it->second.heardAt = server_.clock().frame();
    }
    if (bytes.empty()) {
        return;
    }
    const auto op = static_cast<net::Op>(bytes[0]);
    if (op == net::Op::ClientAccept) {
        join(peer, bytes);
        return;
    }
    if (!players_.contains(peer)) {
        return;
    }
    // Game messages back to back (SrvNet_OnPacket's loop); stop at one we can't size.
    std::size_t off = 0;
    while (off < bytes.size()) {
        const auto rest = bytes.subspan(off);
        const std::size_t size = net::messageSize(rest, net::Direction::ClientToServer);
        if (size == 0 || size == net::kVariableSize) {
            return;
        }
        const auto m = rest.first(size);
        switch (static_cast<net::Op>(m[0])) {
        case net::Op::RequestMap: // (map_send.cpp)
            onRequestMap(peer);
            break;
        case net::Op::CancelMap:
            onCancelMap(peer);
            break;
        case net::Op::ReceivedMap:
            onReceivedMap(peer);
            break;
        case net::Op::ClientReady:
            onClientReady(peer);
            break;
        case net::Op::ServerQuitAck: // (SrvNet_OnPacket 0xC8: Game_OnQuitAckReceived, whoever sent it, quitting or not)
            --quitPendingAcks_;
            break;
        case net::Op::IncomingClient:
            onIncomingClient(peer);
            break;
        case net::Op::PlayerInput: // (SrvNet_OnPlayerInput: only once it's in)
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && players_[peer].ready) {
                try {
                    net::decodeControls(m.subspan(2, std::min<std::size_t>(m[1], m.size() - 2)), controlEntries_);
                    addControls(*unit, controlEntries_);
                } catch (const std::exception&) {
                    // (a malformed batch is dropped)
                }
            }
            break;
        case net::Op::TryGet: { // picking something up (SrvNet_OnPacket 0x73: no reach check)
            // (An original bug, kept: no reach check, so a modified client can pick up items
            // anywhere. Not to be fixed without the project's say.)
            ServerObject* unit = server_.world().byExtent(players_[peer].extent);
            ServerObject* item = server_.world().byExtent(static_cast<std::uint32_t>(m[1] | m[2] << 8));
            // Not while a save's asked for (Game_IsSavePending), observing (status & 3),
            // trading (PlayerControl::trade), talking to an NPC (DialogWith, dialog.hpp) or frozen.
            if (unit && unit->player && item && players_[peer].ready && !server_.world().saveRequest.pending && !(unit->player->statusFlags & 3u) &&
                !trading(*unit) && unit->player->dialogWith == 0 && !(unit->flags & ObjFlag::NoUpdate)) {
                int carried = 0;
                for (const std::uint32_t e : unit->inventory) {
                    if (const ServerObject* o = server_.world().byExtent(e)) {
                        carried += o->weight;
                    }
                }
                if (unit->carryCapacity < item->weight + carried) {
                    // (SrvNet_SendInformString: Net_QueueMsg, the frame's datagram; none while its
                    // .plr is applied.)
                    if (!unit->player->transfer) {
                        transport_.sendUnreliable(peer, informStringBytes("pickup.c:CarryingTooMuch"));
                    }
                } else {
                    server_.pickUp(*unit, *item);
                }
            }
            break;
        }
        case net::Op::TryCollide: { // (0x7B: u16 object) working a lever, button... (SrvNet_OnPacket)
            // Its COLLIDE with the player's unit, wherever it is (no reach check: an original
            // bug). Not an observer's (status & 3), one trading (PlayerControl::trade)
            // nor one talking to an NPC (DialogWith, dialog.hpp).
            ServerObject* unit = server_.world().byExtent(players_[peer].extent);
            // (packetDynamicUnitCode: a map object's code is its extent with bit 15.)
            ServerObject* o = server_.world().byExtent(static_cast<std::uint32_t>(m[1] | m[2] << 8) & 0x7FFFu);
            if (unit && unit->player && players_[peer].ready && !(unit->player->statusFlags & 3u) && !trading(*unit) && unit->player->dialogWith == 0 && o &&
                !o->gone() && o->collides) {
                Fight f = server_.fight();
                collideFunction(f, *o, unit);
            }
            break;
        }
        case net::Op::TryDrop:    // (0x72: u16 item, u16 x, u16 y)
        case net::Op::TryEquip:   // (0x75: u16 item)
        case net::Op::TryDequip: { // (0x76: u16 item)
            ServerObject* unit = server_.world().byExtent(players_[peer].extent);
            ServerObject* item = server_.world().byExtent(static_cast<std::uint32_t>(m[1] | m[2] << 8));
            if (!unit || !unit->player || !item || !players_[peer].ready || item->holder != unit->extent) {
                break;
            }
            auto items = server_.items();
            const bool observing = (unit->player->statusFlags & 3u) != 0; // (status & 3)
            if (m[0] == static_cast<std::uint8_t>(net::Op::TryDrop)) {
                // Not observing, trading, talking nor frozen.
                if (!observing && !trading(*unit) && unit->player->dialogWith == 0 && !(unit->flags & ObjFlag::NoUpdate)) {
                    items.tryDrop(*unit, *item, static_cast<float>(m[3] | m[4] << 8), static_cast<float>(m[5] | m[6] << 8));
                }
            } else if (m[0] == static_cast<std::uint8_t>(net::Op::TryEquip)) {
                // Not while a save's asked for (Game_IsSavePending) nor observing.
                if (!server_.world().saveRequest.pending && !observing) {
                    items.tryEquip(*unit, *item);
                }
            } else if (!observing) { // (0x76: not observing)
                items.tryDequip(*unit, *item);
            }
            break;
        }
        case net::Op::TryUse: { // (0x74: u16 item) using a thing it holds (Obj_UseItem)
            // Not while a save's asked for (Game_IsSavePending) nor an observer (status 1
            // or 2); the item in its inventory.
            ServerObject* unit = server_.world().byExtent(players_[peer].extent);
            ServerObject* item = server_.world().byExtent(static_cast<std::uint32_t>(m[1] | m[2] << 8) & 0x7FFFu);
            if (unit && unit->player && players_[peer].ready && !server_.world().saveRequest.pending && !(unit->player->statusFlags & 3u) && item &&
                item->holder == unit->extent) {
                server_.items().use(*unit, *item);
            }
            break;
        }
        case net::Op::TryCreatureCommand: { // (0x78: u16 creature (0: all), u8 order) AI_OrderCreatures
            // Not while observing. (Any creature by its code, its owner unchecked but for a
            // banishing: an original bug.)
            ServerObject* unit = server_.world().byExtent(players_[peer].extent);
            if (unit && unit->player && players_[peer].ready && !(unit->player->statusFlags & 1u) && m.size() >= 4) {
                const std::uint32_t code = static_cast<std::uint32_t>(m[1] | m[2] << 8);
                ServerObject* creature = nullptr;
                if (code != 0) {
                    creature = server_.world().byExtent(code & 0x7FFFu);
                    if (!creature) {
                        break;
                    }
                }
                auto f = server_.fight();
                orderCreatures(f, *unit, creature, m[3]);
            }
            break;
        }
        case net::Op::TrySpell: // (0x79: five u32 spells, then "on itself")
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player && players_[peer].ready) {
                std::array<std::uint32_t, 5> spells{};
                for (std::size_t i = 0; i < spells.size(); ++i) {
                    const std::size_t at = 1 + i * 4;
                    spells[i] = util::u32At(m, at);
                }
                server_.spells().request(*unit, spells, m[21] != 0);
            }
            break;
        case net::Op::TryAbility: // (0x7A: the ability, 1-5)
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player && players_[peer].ready) {
                server_.tryAbility(*unit, m[1]);
            }
            break;
        case net::Op::ReportSecondaryWeapon: // (0xE0: u16 its net code, 0 none) the client's secondary
            // weapon (Plr_SetSecondaryWeapon): one its class may not use
            // (playerClassCanUseItem) or it isn't strong enough for
            // (playerCheckStrength: the weapon's or armour's record's strength) is
            // refused: SrvNet_SendReportSecondaryWeapon(.., null, 1), its client's "Weapon2CantUse".
            // (Kept in the exe's player record: what reads it, the saves, takes the host's window.)
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player && m.size() >= 3) {
                const std::uint16_t code = static_cast<std::uint16_t>(m[1] | (m[2] << 8));
                if (const ServerObject* item = code != 0 ? server_.world().byExtent(code) : nullptr) {
                    if (!server_.items().classMay(*unit, *item) || !server_.items().strongEnough(*unit, *item)) {
                        sendImportant(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ReportSecondaryWeapon), 0, 0, 1});
                    }
                }
            }
            break;
        case net::Op::InfoBookData: // (0xE2: u16 a book's net code, u8 its kind) what a book holds
            // (SrvNet_OnPacket 0xE2) The book among its things (Obj_FindInventoryItemByNetCode), the
            // shop's (Trade_FindShopItemByNetCode) or anywhere (Obj_GetByNetCode): our extents find
            // them all. The answer {0xE2, its code, the entry}: kind 2 a field guide's creature's
            // index in the guide (Game_GetCreatureGuideIndex, 0 unknown), else a spell's or an
            // ability's number (its UseData's first byte), important (Net_QueueImportant).
            if (m.size() >= 4) {
                const std::uint32_t code = static_cast<std::uint32_t>(m[1] | (m[2] << 8)) & 0x7FFFu;
                const ServerObject* book = server_.world().byExtent(code);
                if (!book) { // (a shop's stock: in its trade's keeping, out of the world)
                    const auto kept = server_.world().tradeObjects.find(code);
                    book = kept != server_.world().tradeObjects.end() ? &kept->second : nullptr;
                }
                if (book && !book->gone()) {
                    std::uint8_t entry = static_cast<std::uint8_t>(book->bookEntry);
                    if (m[3] == 2) {
                        entry = 0;
                        for (std::size_t i = 0; i < things::kGuideNames.size(); ++i) {
                            if (book->guideCreature == things::kGuideNames[i]) {
                                entry = static_cast<std::uint8_t>(i + 1);
                                break;
                            }
                        }
                    }
                    const std::uint16_t nc = netCodeOf(*book);
                    sendImportant(peer, std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::InfoBookData), static_cast<std::uint8_t>(nc),
                                                                  static_cast<std::uint8_t>(nc >> 8), entry},
                                  true); // (Net_QueueImportant's onlyJoined 1)
                }
            }
            break;
        case net::Op::Social: // (0xE3: u8 1 laugh, 2 taunt, 4 point) an emote, whenever it's in
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player) {
                social(server_.world(), *unit, m[1], server_.clock().frame());
            }
            break;
        case net::Op::Gauntlet: // (SrvNet_OnPacket 0xF0: sub 3, game over's respawn while dead; 0x1B, its save in)
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player && m.size() > 1 && m[1] == 3 &&
                (unit->flags & ObjFlag::Dead)) {
                unit->player->questGameOverAt = 0;
                Fight f = server_.fight();
                f.respawn(*unit);
            } else if (const ServerObject* readyUnit = server_.world().byExtent(players_[peer].extent); readyUnit && m.size() > 1 && m[1] == 0x1B) {
                // SrvNet_OnGauntletQuestReady: its client has its save. One to go once it
                // had it, out (reason 4); else no save pending, MSG_GAUNTLET 0x1C {0}.
                const std::uint32_t bit = 1u << (players_[peer].slot & 31u);
                if (kickAfterSave_ & bit) {
                    disconnect(peer, 4);
                    return;
                }
                savePending_ &= ~bit;
                // (SrvNet_SendGauntletSaveState: Net_QueueImportantSeq, onlyJoined 0: held for one
                // still loading, after its held 0x1C {1}.)
                sendImportant(peer, std::vector<std::uint8_t>{0xF0, 0x1C, 0});
            }
            break;
        case net::Op::RequestSavePlayer: // (0xC1) its saved player sent it (SrvNet_OnPacket)
            // In a quest, one not in the game, with no unit, or whose own save isn't loaded yet is
            // let go (reason 2) instead (not the host's own: SrvNet_OnPacket's 0xC1).
            if (server_.world().quest() && !isHostPlayer(players_[peer])) {
                const ServerObject* unit = server_.world().byExtent(players_[peer].extent);
                if (!players_[peer].ready || !unit || !unit->player || unit->player->questAwaitingSave) {
                    disconnect(peer, 2);
                    return;
                }
            }
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player) {
                Fight f = server_.fight();
                if (!server_.world().questSaves || refused(peer)) { // (noxcpp: a stock player held out never played)
                    // (noxcpp's: a mode whose characters are the players' own, never the quest's
                    // (World::questSaves off), in its runs and its lobby alike (a chat map: the
                    // in-game Save is enabled there): a save the client refuses
                    // (under its 700 bytes: writes nothing) so its .plr
                    // stays as it was, and it leaves as after a save.
                    // The unit here is the mode's: stripped, its run's. Stock: the unit's own.)
                    sendSaveData(peer, players_[peer], std::vector<std::uint8_t>(kRefusedSaveBytes, 0));
                } else if (auto file = savedPlayer(f, *unit, peer)) {
                    sendSaveData(peer, players_[peer], std::move(*file));
                }
            }
            break;
        case net::Op::XferMsg: // (0xC2: the file transfers, xfer.hpp)
            if (auto file = xfers_.receive(peer, m, server_.clock().frame())) {
                onFile(*file);
            }
            break;
        case net::Op::Mouse: // its cursor in the world (0xAC: u16 x, u16 y)
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player) {
                unit->player->cursorX = static_cast<float>(m[1] | m[2] << 8);
                unit->player->cursorY = static_cast<float>(m[3] | m[4] << 8);
            }
            break;
        case net::Op::NewAlias: {
            // The client's name for an object in our streams (SrvNet_OnPacket 0xA5).
            const std::uint32_t expire = util::u32At(m, 6);
            players_[peer].aliases.set(m[1], {util::u16At(m, 2), util::u16At(m, 4), expire});
            break;
        }
        case net::Op::TeamMsg: { // (SrvNet_OnPacket 0xC4: sub 10 join a team, sub 11 switch)
            // {u32 team, u16 net code}: whoever the code names, no questions asked (an original
            // bug).
            World& world = server_.world();
            const std::uint32_t teamId = m[2] | m[3] << 8 | m[4] << 16 | static_cast<std::uint32_t>(m[5]) << 24;
            ServerObject* o = world.byExtent(static_cast<std::uint32_t>(m[6] | m[7] << 8) & 0x7FFFu);
            ServerTeam* team = teamId < 256 ? teamById(world, static_cast<std::uint8_t>(teamId)) : nullptr;
            Fight f = server_.fight();
            if (m[1] == 10) {
                if (team && o) {
                    addTeamMember(world, team->id, *o, true, true, f.rng, f.frame);
                }
            } else if (m[1] == 11 && o) {
                if (!(world.gameFlags & GameFlag::Chat)) {
                    const auto [x, y] = findPlayerStart(world, o, f.rng);
                    setPosition(world, *o, x, y, f.frame);
                }
                if (team) {
                    moveTeamMember(world, *o, *team, true, f.rng, f.frame);
                }
            }
            break;
        }
        case net::Op::Dialog: // (SrvNet_OnPacket 0xD0: sub 1 {u16 the NPC's code}, sub 2 {u8 the answer})
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player) {
                World& world = server_.world();
                Fight f = server_.fight();
                if (m[1] == 1) {
                    // Not while a save's asked for (Game_IsSavePending) nor observing (status
                    // & 3); the NPC by its code (Obj_GetByNetCode: none destroyed).
                    const std::uint32_t code = static_cast<std::uint32_t>(m[2] | m[3] << 8);
                    ServerObject* npc = world.byExtent(code & 0x7FFFu);
                    if (npc && (npc->gone() || netCodeOf(*npc) != code)) {
                        npc = nullptr;
                    }
                    if (!world.saveRequest.pending && !(unit->player->statusFlags & 3u) && npc &&
                        (!world.modeHooks || world.modeHooks->mayStartDialog(f, *unit, *npc))) { // (noxcpp: a mode may refuse it)
                        dialogStart(f, *unit, *npc); // (SrvNet_OnDialogStart: dialog.hpp)
                    }
                } else if (m[1] == 2) {
                    dialogAnswer(f, *unit, m[2]); // (SrvNet_OnDialogAnswer)
                }
                // (Any other sub: the exe's loop reads the same message again for ever, a hang:
                // an original bug. Ours has no size for it and stops reading the packet.)
            }
            break;
        case net::Op::Vote:
            onVote(peer, m);
            break;
        case net::Op::Trade: // (0xC9: shops and trading, trade.hpp)
            if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player) {
                Fight f = server_.fight();
                onTradeMessage(f, *unit, m);
            }
            break;
        case net::Op::RequestTimerStatus: // (0xD4) MSG_TIMER_STATUS back
            // (SrvNet_SendTimerStatus: Net_QueueImportantSeq, onlyJoined 1: held for one not ready,
            // none for one not in the game.)
            sendImportant(peer, timerStatus(server_.world(), server_.clock().frame()), true);
            break;
        case net::Op::TextMessage:
            onTextMessage(peer, m);
            break;
        case net::Op::NoxcppMapVote: // (noxcpp's, from a noxsdl player: its vote, or a vote called)
            if (const auto v = mapvote::parse(m)) {
                if (v->first == mapvote::kVote && !v->second.empty()) {
                    castVote(peer, v->second[0]);
                } else if (v->first == mapvote::kCall) {
                    callVote(peer); // (the rules of a stock client's "?MapVote")
                }
            }
            break;
        case net::Op::NoxcppModeF4: // (a NoX Core mode's, from a noxsdl player: a pick made in its window)
        case net::Op::NoxcppModeF5:  // (another NoX Core mode's, from a noxsdl player: a pick likewise)
        case net::Op::NoxcppEdit:  // (noxcpp's map editor session, from a noxsdl client: mapedit/session_proto.hpp)
        case net::Op::NoxcppCoop:  // (a NoX Core mode's, from a noxsdl player)
        case net::Op::NoxcppModeUi: // (a script mode's UI, from a noxsdl player that asked for it)
            // (each mode's own, by its opcode: Options::noxcppMessages)
            if (const auto h = options_.noxcppMessages.find(m[0]); h != options_.noxcppMessages.end() && h->second && transport_.identified(peer)) {
                h->second(*this, peer, m);
            }
            break;
        case net::Op::ServerCmd: // (0xBB) {cmd, u16 its code, u8 chars, UTF-16 text}
            if (m.size() >= 5) {
                onServerCmd(peer, m[1], utf16Until(m.subspan(5)));
            }
            break;
        case net::Op::SysopPw: // (0xBC) the password, UTF-16
            onSysopPassword(peer, utf16Until(m.subspan(1)));
            break;
        default:
            break;
        }
        off += size;
    }
}

// CLIENT_READY (SrvNet_OnClientReady): the player in; what waited for it, the map's walls, doors and
// vote as it stands now sent it.
void GameHost::onClientReady(std::uint8_t peer)
{
    players_[peer].ready = true;
    // (SrvNet_OnClientReady) Its status 0x10, ready (the magic walls are sent it
    // from now); in a network game but a chat one, 5 s invulnerable.
    if (ServerObject* unit = server_.world().byExtent(players_[peer].extent); unit && unit->player) {
        unit->player->statusFlags |= 0x10u;
    }
    // What waited for it (the join's fade or quest start, INCOMING_CLIENT's news) goes
    // before what its ready queues below: one important queue, oldest first.
    releasePending(players_[peer]);
    if (ServerObject* unit = server_.world().byExtent(players_[peer].extent);
        unit && server_.world().network() && !(server_.world().gameFlags & GameFlag::Chat)) {
        disenchant(server_.world(), *unit, Enchant::Invulnerable);
        enchant(server_.world(), *unit, Enchant::Invulnerable, 30 * 5, 5);
        // Every flag (class 0x10000000) on its minimap.
        for (auto& [extent, o] : server_.world().objects()) {
            if ((o.classes & things::ObjectClass::Flag) && !o.gone()) {
                markMinimap(server_.world(), *unit, o, 1);
            }
        }
    }
    {
        // (wall) The map's broken walls (the breakable list, its head
        // first; flag 0x20): MSG_DESTROY_WALL (0x3A, the wall's id) to it
        // (SrvNet_SendWallDestroyed). Then each secret wall as it knows it: open by the
        // map (flag 8) or shut (its bit in the wall's record).
        World& world = server_.world();
        for (const auto& [x, y] : world.breakableWalls()) {
            const auto it = world.walls().find({x, y});
            if (it != world.walls().end() && it->second.broken) {
                const std::uint16_t id = it->second.wallId;
                std::vector<std::uint8_t> m{0x3A, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8)};
                if (auto other = options_.forPeer ? options_.forPeer(*this, peer, m) : std::nullopt) {
                    m = std::move(*other);
                }
                transport_.sendReliable(peer, m);
            }
        }
        const std::uint32_t bit = 1u << (players_[peer].slot & 31);
        for (const auto& [x, y] : world.secretWalls()) {
            if (WallCell* c = world.wall(x, y)) {
                c->secretSeen = (c->secretFlags & 8) ? (c->secretSeen | bit) : (c->secretSeen & ~bit);
            }
        }
        // A vote going on (the vote list not empty): MSG_VOTE 6 {1} to it
        // (SrvNet_SendVoteState).
        if (!votes_.empty()) {
            transport_.sendReliable(peer, std::vector<std::uint8_t>{0xEE, 0x06, 1});
        }
    }
    if (server_.world().quest()) {
        // (wall's quest part) The doors opened, their markers cleared
        // (SrvNet_SendGauntletClearMarker), and the warp window when it's shown
        // (SrvNet_SendGauntletWarpGui); then the exit's countdown under way, the HUD
        // (SrvNet_SendGauntletShowHud).
        for (const auto& [extent, o] : server_.world().objects()) {
            if ((o.classes & things::ObjectClass::Door) && o.doorQuestOpened && !o.gone()) {
                transport_.sendReliable(peer, std::vector<std::uint8_t>{0xF0, 0x0F, static_cast<std::uint8_t>(extent),
                                                                        static_cast<std::uint8_t>(extent >> 8)});
            }
        }
        if (server_.world().questWarpGui) {
            transport_.sendReliable(peer, std::vector<std::uint8_t>{0xF0, 0x18, 1});
        }
        if (server_.world().countdown) {
            transport_.sendReliable(peer, std::vector<std::uint8_t>{0xF0, 0x14});
        }
    }
    if (inVoteWindow(peer)) {
        // (noxcpp's) Whether a map vote can be called here, and one under way as it stands,
        // to a noxsdl player now in the game (at its join, and after each map's load).
        sendVoteAvailable(peer);
        if (vote_.active()) {
            sendVoteTo(peer);
        }
    }
    if (const auto r = refused_.find(peer); r != refused_.end()) {
        // (noxcpp: Options::stockRefusal) Still held out (heldOut keeps it observing), its hold
        // counted from its first ready, when it's told why: a text line after the informs held for
        // it (the newest of the centred lines). The mode never hears of it.
        keepHeldOut(peer); // (the ready's invulnerability above ended its invisibility)
        if (!r->second.until) {
            r->second.until = nowMs_ + (options_.stockRefusal ? options_.stockRefusal->holdMs : 15000u);
            if (options_.stockRefusal) {
                sendImportant(peer, World::textMessage(strings::fromUtf8(options_.stockRefusal->notice), 0));
            }
        }
        return;
    }
    if (options_.onClientReady) {
        options_.onClientReady(*this, peer); // (noxcpp: a mode told, e.g. to send its multiclass word)
    }
}

// INCOMING_CLIENT (SrvNet_OnIncomingClient): the player into the game, told of everyone and everything
// in it.
void GameHost::onIncomingClient(std::uint8_t peer)
{
    // (SrvNet_OnIncomingClient's part so far) It and each other player on each
    // other's minimaps, its record sent each of them (MSG_SIMPLE_OBJ); the ball and crowns
    // (ACTIVE) on its (servMinimapRevealFlag).
    // Everything it's told here goes through its important queue (Net_QueueImportant /
    // ..Seq), which waits for its CLIENT_READY (sendImportant); the MOTD is a transfer.
    World& world = server_.world();
    // A quest's first: MSG_GAUNTLET 0 (the level's start) to it, its tallies cleared. (questAwaitingSave,
    // waiting for its saved character: with the save transfer, not yet.)
    if (ServerObject* unit = world.byExtent(players_[peer].extent); unit && unit->player && world.quest()) {
        unit->player->questAwaitingSave = true; // (its client's saved player to come)
        sendImportant(peer, std::vector<std::uint8_t>{0xF0, 0x00});
        questClearTallies(world, *unit);
    }
    // In the game (g_playersInGameMask's bit); each other player's MSG_NEW_PLAYER to it
    // (SrvNet_SendAllPlayersToNewPlayer, the player list's order).
    players_[peer].joined = true;
    for (const auto& [other, q] : players_) {
        if (other == peer) {
            continue;
        }
        net::msg::NewPlayer them;
        them.netCode = static_cast<std::uint16_t>(q.extent);
        them.playerInfo = q.info;
        them.accountName = q.accept.account;
        sendImportant(peer, bytesOf(them));
        // What it wears (flag 0x100) and a Flag it carries, its inventory's
        // order, each one's class (SrvNet_SendReportEquipClass: 0x50 / 0x51 a weapon, the
        // Flag one of them; 0x4F / 0x52 armour; anything else nothing).
        if (const ServerObject* holder = world.byExtent(q.extent)) {
            for (const std::uint32_t e : holder->inventory) {
                const ServerObject* item = world.byExtent(e);
                if (item && ((item->flags & ObjFlag::Equipped) || things::weaponBitOf(item->typeName) == 1u)) { // (weapon bit 1: the Flag)
                    if (auto report = equipClassReport(*holder, *item); !report.empty()) {
                        sendImportant(peer, std::move(report));
                    }
                }
            }
        }
    }
    // (Its unit's Init, PlayerInit: playerMakeDefItems's camper watch begun,
    // the rest of it the join's giveDefaultItems.)
    if (ServerObject* unit = world.byExtent(players_[peer].extent); unit && unit->player) {
        camperReset(world, *unit, server_.clock().frame());
    }
    // The message of the day, in a network game not a quest (playerSendMOTD). (noxcpp: a stock
    // player held out by Options::stockRefusal gets the refusal's notice as its message instead,
    // in any game: its text line goes too, should a client not show this one.)
    if (options_.stockRefusal && refused_.contains(peer)) {
        // (Each of its lines at most 255 bytes, cut on a whole UTF-8 character: the stock client
        // copies a line into 256 bytes unchecked, OB-370. The host's own motd.txt goes as it is.)
        std::vector<std::uint8_t> text;
        std::size_t line = 0;
        const std::string& notice = options_.stockRefusal->notice;
        for (std::size_t i = 0; i < notice.size();) {
            const auto c = static_cast<std::uint8_t>(notice[i]);
            const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            const std::size_t len = std::min(n, notice.size() - i);
            if (c == '\n') {
                line = 0;
            } else if (line + len > 255) {
                text.push_back('\n');
                line = 0;
            }
            text.insert(text.end(), notice.begin() + static_cast<std::ptrdiff_t>(i), notice.begin() + static_cast<std::ptrdiff_t>(i + len));
            line += c == '\n' ? 0 : len;
            i += len;
        }
        text.push_back(0);
        xfers_.send(peer, Xfers::Motd, "MOTD", text);
    } else if (options_.sendMotd && options_.motd && world.network() && !world.quest()) {
        std::vector<std::uint8_t> text(options_.motd->begin(), options_.motd->end());
        text.push_back(0);
        xfers_.send(peer, Xfers::Motd, "MOTD", text);
    }
    if (ServerObject* unit = world.byExtent(players_[peer].extent); unit && unit->player) {
        for (auto& [extent, o] : world.objects()) {
            if (o.player && &o != unit && !o.gone()) {
                markMinimap(world, *unit, o, 1);
                markMinimap(world, o, *unit, 1);
                simpleObjTo(world, o, *unit); // (its record to the other at once: SrvNet_SendSimpleObjReliable)
                if (world.quest()) { // (each one's health to the other: SrvNet_SendTeamHealthPercent)
                    for (const auto& [q, h] : players_) {
                        if (h.extent == o.extent) {
                            sendImportant(q, questTeamHealthMessage(*unit), true);
                        }
                    }
                    sendImportant(peer, questTeamHealthMessage(o), true);
                }
            }
        }
        for (auto& [extent, o] : world.objects()) {
            if ((o.flags & ObjFlag::Active) && (o.typeName == "GameBall" || o.typeName == "Crown") && !o.gone()) {
                markMinimap(world, *unit, o, 1);
            }
        }
    }
    // The teams and their players (MSG_TEAM_MSG sub 0 each, then sub 1 for
    // each player in it).
    for (const ServerTeam& t : world.teams) {
        if (!t.active) {
            continue;
        }
        sendImportant(peer, teamMessage(world, t));
        for (const auto& [extent, o] : world.objects()) {
            if (o.player && inTeam(world, o, t.id)) {
                sendImportant(peer, teamMemberMessage(t, o));
            }
        }
    }
    // Every creature's and player's friendship to it afresh, each owed (the join's walk, before
    // the flags' status): its summons, NPCs and the like told it as they're next sent
    // (MSG_OBJECT_FRIEND_ADD).
    if (ServerObject* unit = world.byExtent(players_[peer].extent); unit && unit->player) {
        friendsOf(world, *unit);
    }
    // In flagball the ball's status; in CTF each team's flag status
    // (the flags' status table).
    if (world.gameFlags & GameFlag::FlagBall) {
        sendImportant(peer, ballStatusMessage(world));
    } else if (world.gameFlags & GameFlag::CaptureTheFlag) {
        for (const ServerTeam& t : world.teams) {
            if (t.active) {
                sendImportant(peer, flagStatusMessage(world, t.id));
            }
        }
    }
    // Every player's status to it, itself too (SrvNet_SendAllClientStatus:
    // MSG_REPORT_CLIENT_STATUS 0x6A, its code and status & 0x423, the player list's order).
    for (const ServerObject* o : playersInOrder(world, [](const ServerObject& u) { return !u.gone(); })) {
        const std::uint32_t s = o->player->statusFlags & 0x423u;
        sendImportant(peer, std::vector<std::uint8_t>{0x6A, static_cast<std::uint8_t>(o->extent), static_cast<std::uint8_t>(o->extent >> 8),
                                                      static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(s >> 8),
                                                      static_cast<std::uint8_t>(s >> 16), static_cast<std::uint8_t>(s >> 24)});
    }
    // With the camper alarm on, each other player marked a camper (SrvNet_SendAllCamperMarkers
    // SrvNet_SendAllCamperMarkers, after the clients' status), in Plr_GetFirstUnit's order. (in
    // CTF and elimination always.)
    if (world.serverOption(0x2000u)) {
        const std::uint32_t self = players_[peer].extent;
        const auto campers = playersInOrder(world, [&](const ServerObject& u) {
            return (u.classes & things::ObjectClass::Player) && u.extent != self && u.player->camping;
        });
        for (const ServerObject* o : campers) {
            sendImportant(peer, camperMarker(*o, true));
        }
    }
    // A quest's last: each player in it (MSG_GAUNTLET 1, the player table's order) to it, and
    // its slot marked.
    if (world.quest()) {
        for (const ServerObject* o : playersInOrder(world, [](const ServerObject& u) { return !u.gone() && u.player->questActive == 1; })) {
            sendImportant(peer, std::vector<std::uint8_t>{0xF0, 0x01, static_cast<std::uint8_t>(o->extent),
                                                         static_cast<std::uint8_t>(o->extent >> 8)});
        }
        world.questSlotSeenAt[players_[peer].slot & 31u] = server_.clock().frame();
    }
}

// TEXT_MESSAGE (SrvNet_OnPacket 0xA8): a chat line, relayed; noxcpp's chat commands read from it.
void GameHost::onTextMessage(std::uint8_t peer, std::span<const std::uint8_t> m)
{
    // Relayed as it came (SrvNet_OnPacket 0xA8): not at all while the sender's status
    // has 4 (nothing in the exe sets it); team chat (flag 1) only to the
    // players whose unit is in the team of the object whose net code the line carries
    // (Team_GetObjectTeamByNetCode(data + 1), Team_IsObjectInTeam: none when it has no
    // team); anything else to every player. Each copy goes out at once in a datagram of its
    // own (Net_Send(.., 0) then Net_FlushStream(.., 1): not with the frame's update).
    if (chat_.size() >= kChatKept) { // (the host's log of it: the oldest half let go)
        chat_.erase(chat_.begin(), chat_.begin() + kChatKept / 2);
    }
    chat_.emplace_back(players_[peer].name, std::vector<std::uint8_t>(m.begin(), m.end()));
    // A line as text (noxcpp's: a stock client's map vote, and a mode's own answers and
    // "?" commands, a noxsdl player's too).
    std::u16string text;
    // (TEXT_MESSAGE: +1 the speaker's code, +3 flags (1 team, 2 8-bit text), +11 the text.)
    if (m.size() > 11) {
        const bool narrow = (m[3] & 2) != 0;
        for (std::size_t i = 11; i < m.size(); i += narrow ? 1 : 2) {
            const char16_t c = narrow ? m[i] : static_cast<char16_t>(m[i] | (i + 1 < m.size() ? m[i + 1] << 8 : 0));
            if (c == 0) {
                break;
            }
            text += c;
        }
        while (!text.empty() && text.back() == u' ') {
            text.pop_back();
        }
        while (!text.empty() && text.front() == u' ') {
            text.erase(text.begin());
        }
        // (noxcpp, a mode's: an answer of its own taken, not relayed.)
        if (options_.onChat && !text.empty() && options_.onChat(*this, peer, text)) {
            return;
        }
    }
    World& world = server_.world();
    const auto relay = [&](std::uint8_t to) {
        transport_.sendUnreliable(to, m);
        transport_.flushUnreliable(to);
    };
    const ServerObject* sender = world.byExtent(players_[peer].extent);
    if (!(sender && sender->player && (sender->player->statusFlags & 4u))) {
        if (m[3] & 1) {
            // (Obj_GetByNetCode: the server's net code as it came, bit 15 and all, none
            // destroyed; not the client's form of a map object's code.)
            const std::uint32_t code = static_cast<std::uint32_t>(m[1] | m[2] << 8);
            const ServerObject* speaker = world.byExtent(code);
            if (speaker && speaker->gone()) {
                speaker = nullptr;
            }
            const std::uint8_t team = speaker ? speaker->team : 0;
            if (team != 0 && teamById(world, team)) {
                for (auto& [other, p] : players_) {
                    const ServerObject* unit = world.byExtent(p.extent);
                    if (unit && unit->player && inTeam(world, *unit, team)) {
                        relay(other);
                    }
                }
            }
        } else {
            for (auto& [other, p] : players_) {
                relay(other);
            }
        }
    }
    // (noxcpp's map vote by chat, for a stock client: a candidate's number while one is
    // open, "?MapVote" to call one. A noxsdl player's goes by MSG_NOXCPP_MAPVOTE instead;
    // its chat is only chat.)
    if (m.size() > 11 && !transport_.identified(peer)) {
        std::u16string lower;
        for (const char16_t c : text) {
            lower += (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + 32) : c;
        }
        if (vote_.active() && text.size() == 1 && text[0] >= u'1' && text[0] <= u'9') {
            castVote(peer, text[0] - u'1');
        } else if (lower == u"?mapvote") {
            callVote(peer);
        }
    }
}

// CLIENT_ACCEPT: a banned name or a quest mismatch is refused; else its unit is made at a start,
// the join sequence sent, and everyone else told of it.
void GameHost::join(std::uint8_t peer, std::span<const std::uint8_t> bytes)
{
    net::msg::ClientAccept accept;
    try {
        ByteReader r(bytes.subspan(1), "CLIENT_ACCEPT");
        accept = net::msg::ClientAccept::read(r);
    } catch (const std::exception&) {
        transport_.close(peer);
        return;
    }
    // (noxcpp: a solo game with guests, Options::soloGuests) While its host is awaited, nobody
    // else: with the host's own client's address known, only that one comes in first.
    if (options_.solo && options_.soloGuests > 0 && awaitingHost_ && hostPeerAt_ && transport_.endpointOf(peer) != hostPeerAt_) {
        transport_.close(peer);
        return;
    }
    // Kicked by a vote: not back for its 15 minutes; banned from the console: not back (the ban
    // list, by name).
    if (const auto ban = bans_.find(utf16Until(accept.playerName)); ban != bans_.end()) {
        if (ban->second == 0xFFFFFFFFu || static_cast<std::int32_t>(ban->second - nowMs_) > 0) {
            transport_.close(peer);
            return;
        }
        bans_.erase(ban);
        syncAccess();
    }
    // (Plr_CreateFromJoinData) A quest client (its flags' bit 7) only in a quest, and only a quest
    // client in one. (noxcpp: in the game the client's told it's in: Options::clientGameFlags, a
    // guest's, slot not yet taken.)
    bool questGame = server_.world().quest();
    if (!questGame && options_.clientGameFlags && (accept.flags & 0x80u)) {
        const HostedPlayer probe{};
        questGame = (options_.clientGameFlags(*this, peer, probe, server_.world().gameFlags) & GameFlag::Quest) != 0;
    }
    if (((accept.flags & 0x80u) != 0) != questGame) {
        transport_.close(peer);
        return;
    }
    // noxcpp: a forced class (a mode's conjurers) over the character's, before it's made: its
    // own MSG_NEW_PLAYER tells its client so, which takes its class from there (its meters, book,
    // bars and what it may wear: CliNet_OnPacket's 0x2D), and the others hear it too.
    const std::uint8_t ownClass = accept.classId; // (its character's)
    const std::optional<std::uint8_t> forced = forcedClass(server_.world());
    if (forced) {
        accept.classId = *forced;
    }
    HostedPlayer p;
    p.peer = peer;
    p.accept = accept;
    std::copy_n(bytes.begin() + 1, std::min<std::size_t>(97, bytes.size() - 1), p.info.begin());
    if (forced) {
        p.info[0x42] = *forced;
    }
    p.name = nameOf(accept.playerName);
    p.slot = options_.solo ? std::uint8_t{0x1F} : static_cast<std::uint8_t>((peer - 1) & 31); // (a solo game's player the host's)
    if (options_.solo && options_.soloGuests > 0 && !awaitingHost_) {
        // (noxcpp: a solo game's guest, Options::soloGuests: the lowest slot no player has, below
        // the host's 0x1F)
        std::uint8_t free = 0;
        while (free < 0x1F && std::ranges::any_of(players_, [&](const auto& q) { return (q.second.slot & 31u) == free; })) {
            ++free;
        }
        p.slot = free;
    }
    // The view the client draws (Plr_CreateFromJoinData: half its screen size).
    if (accept.screenWidth > 0 && accept.screenHeight > 0) {
        p.viewHalfWidth = static_cast<float>(accept.screenWidth / 2);
        p.viewHalfHeight = static_cast<float>(accept.screenHeight / 2);
    }
    const std::int32_t scriptIds = server_.world().nextScriptId();
    // A female join (the join data's flag): stock thing.bin has no PlayerFemale, so the exe's
    // create fails and the join is refused, while this spawns a unit with no type. Kept for a
    // female player model later; until then no stock client sends it.
    ServerObject& unit = server_.world().spawn(things_, accept.isFemale ? "PlayerFemale" : "NewPlayer", 2944.0f, 2944.0f);
    const std::optional<std::int32_t> soloHostId = std::exchange(hostScriptId_, std::nullopt);
    if (soloHostId) {
        unit.scriptId = *soloHostId; // (a solo game's host: the exe's made before the first load)
        server_.world().setNextScriptId(scriptIds);
    }
    // (Its PlayerInit, unitInitPlayer's) Its level: a network game's
    // 10, else by its experience, XPTable's (a new unit's none: level 1).
    std::uint8_t level = 10;
    if (!server_.world().network()) {
        int l = 0;
        while (l < 0xB && !(unit.experience < server_.world().number("XPTable", static_cast<std::size_t>(l)))) {
            ++l;
        }
        level = static_cast<std::uint8_t>(l - 1);
    }
    server_.makePlayer(unit, accept.classId, level);
    unit.player->viewHalfWidth = p.viewHalfWidth;
    unit.player->viewHalfHeight = p.viewHalfHeight;
    unit.player->index = p.slot;
    unit.player->name = utf16Until(accept.playerName);
    unit.player->info = p.info; // (the join's PlayerInfo, the .plr's Attrib)
    unit.player->ownClass = ownClass;
    // noxcpp: a mode's multiclass, a noxsdl player's alone (its client asked the extended
    // query): its numbers again, a conjurer's mana among them.
    if (options_.multiclass && transport_.identified(peer)) {
        unit.player->multiclass = true;
        readVals(server_.world(), unit, false);
    }
    unit.player->serial.assign(reinterpret_cast<const char*>(accept.serial.data()), strnlen(reinterpret_cast<const char*>(accept.serial.data()), accept.serial.size()));
    // Where it starts (Map_FindPlayerStart; joining a quest, by the soul gate touched last).
    float x = 0;
    float y = 0;
    if (Fight f = server_.fight(); !server_.world().quest() || !joinAtSoulGate(f, unit, x, y)) {
        std::tie(x, y) = findPlayerStart(server_.world(), &unit, server_.fight().rng);
    }
    // (Plr_CreateFromJoinData: made at (2944, 2944), the new things' pass at once,
    // unitsNewAddToList, then Obj_SetPosition: relinked where it starts.)
    queueNewObjects(server_.world());
    unit.x = unit.frameStartX = unit.newX = x;
    unit.y = unit.frameStartY = unit.newY = y;
    server_.world().gridRelink(unit, x, y);
    unit.player->cursorX = x;
    unit.player->cursorY = y;
    unit.player->clothing = accept.clothing;
    if (soloHostId) {
        // (The map load's outfitting, the exe's host there before the first load's read, game
        // flag 0x200 not yet up) Plr_SetState 0xD, playerMakeDefItems (healed but
        // continuing a save, keeping nothing: its keep flag is only mapLoad's) and
        // stateFrame the frame: its outfit's ids those reserved after its own. (Its trade and
        // control buffer: none yet.)
        World& world = server_.world();
        const std::int32_t after = world.nextScriptId();
        world.setNextScriptId(*soloHostId + 1);
        Fight f = server_.fight();
        setPlayerState(world, unit, 0xD, f.frame);
        makeDefItems(f, unit, !world.switchToSolo, false);
        unit.player->stateFrame = f.frame;
        world.setNextScriptId(after);
    }
    p.extent = unit.extent;
    const std::uint32_t frame = server_.clock().frame();
    // (Plr_CreateFromJoinData, after the protected values) SrvNet_SendRateChange: MSG_RATE_CHANGE
    // {0xEC, the rate} (Net_QueueImportantSeq, onlyJoined 1): its first important message.
    p.pending.push_back({{static_cast<std::uint8_t>(net::Op::RateChange), static_cast<std::uint8_t>(netRate_)}, true});

    // The join sequence (SrvNet_SendJoinSequence), the first five in one reliable packet, then USE_MAP; its
    // GAME_SETTINGS from slot 0 as the slot's refresh leaves it.
    refreshSettingsSlot0();
    std::vector<std::uint8_t> first;
    net::msg::FullTimestamp full;
    full.frame = frame;
    append(first, bytesOf(full));
    net::msg::JoinData joinData;
    joinData.playerNetCode = static_cast<std::uint16_t>(p.extent);
    joinData.joinToken = accept.joinToken;
    append(first, bytesOf(joinData));
    {
        net::msg::GameSettings settings = gameSettings(frame);
        if (options_.clientGameFlags) {
            settings.gameFlags = clientFlags(peer, p) & 0x7FFF0u; // (noxcpp: a mode's flags for this client)
        }
        append(first, bytesOf(settings));
    }
    append(first, bytesOf(gameSettings2()));
    net::msg::NewPlayer newPlayer;
    newPlayer.netCode = static_cast<std::uint16_t>(p.extent);
    newPlayer.playerInfo = p.info;
    newPlayer.accountName = accept.account;
    const auto announce = bytesOf(newPlayer);
    append(first, announce);
    transport_.sendReliable(peer, first);
    transport_.sendReliable(peer, useMapBytes(frame, soloGuest(p)));

    // The others hear of it (it hears of them at its INCOMING_CLIENT: SrvNet_SendAllPlayersToNewPlayer):
    // Net_QueueImportant(its index | 0x80): those in the game, at their ready.
    const std::uint32_t extent = p.extent;
    p.heardAt = frame;
    p.joinSerial = ++joinSerials_;
    players_[peer] = std::move(p);
    sendImportantToAll(announce, false, peer);
    // Not the host's own (0x1F): the settings the window has (2: shut, slot 1; open,
    // a read of it into the stack's uninitialised bytes, here 0) to it
    // (SrvNet_SendUpdateGuiGameSettings, slot 1: Net_QueueImportantSeq, onlyJoined 0).
    if (!isHostPlayer(players_[peer])) {
        SettingsRecord r{};
        if (window_) {
            fillFromWindow(r, *window_);
        } else {
            r = settingsSlots_[1];
        }
        sendGuiSettings(peer, 1, r);
    }
    // Its outfit (its client hears once it's in; the others dress it from MSG_PLAYER_RESPAWN).
    // (A solo game's host: its PlayerInit's after the first load's soloHostIn.)
    const bool soloHost = std::exchange(awaitingHost_, false);
    // (noxcpp: Options::stockRefusal) A stock client (it didn't identify itself) in a NoX Core-only
    // game: held out below.
    const bool refusing = options_.stockRefusal && !transport_.identified(peer) && !isHostPlayer(players_[peer]);
    if (ServerObject* u = server_.world().byExtent(extent)) {
        if (!soloHost) {
            server_.giveDefaultItems(*u);
        }
        // (Plr_CreateFromJoinData) In a network game not a quest, coop, chat or solo one, it
        // comes in observing, free to join (Action); in a quest observing (in the quest by its
        // Action: Plr_UpdateObserver). (Elimination's: with that.) Then not in a solo game, a
        // quest's start (MSG_GAUNTLET 0x0E) or the screen faded in (MSG_FADE_BEGIN {1, 1}).
        World& world = server_.world();
        if (world.quest() || refusing) {
            // (noxcpp: a stock player held out, Options::stockRefusal: an observer that may not
            // join, as one eliminated)
            Fight f = server_.fight();
            goObserver(f, *u, false, true);
        } else if (world.network() && !(world.gameFlags & (GameFlag::Coop | GameFlag::Chat | GameFlag::Solo))) {
            Fight f = server_.fight();
            goObserver(f, *u, true, true);
        }
        // Both through the player's important queue (Net_QueueImportant / ..Seq), which
        // netImportant holds until its CLIENT_READY (status 0x10): a client that
        // downloads the map first blanks its screen as it loads it (gameChangeMap),
        // so the fade-in must come after that, not during the download.
        if (!(world.gameFlags & GameFlag::Solo)) {
            if (world.quest() && world.questScreens) {
                sendImportant(peer, questBriefingMessage(world, 0x0E, false));
            } else {
                sendImportant(peer, std::vector<std::uint8_t>{0xE4, 1, 1});
            }
        }
    }
    if (soloHost) {
        soloHostIn(extent);
    }
    // (noxcpp: a NoX Core-only mode's, Options::stockRefusal) A stock client is held out and told
    // why, then put out; the mode never hears of it.
    if (refusing) {
        if (ServerObject* u = server_.world().byExtent(extent); u && u->player) {
            refuse(peer, *u);
            return;
        }
    }
    // (noxcpp: a mode told, before the player's first frame)
    if (options_.onJoin) {
        if (ServerObject* u = server_.world().byExtent(extent); u && u->player) {
            options_.onJoin(*this, peer, *u);
        }
    }
}

void GameHost::refuse(std::uint8_t peer, ServerObject& unit)
{
    refused_[peer] = HeldOut{nowMs_, std::nullopt}; // (its hold starts at its CLIENT_READY)
    // Out of play: an observer whose Action never brings it in (heldOut: server-only, never
    // sent; the join made it an observer that may not join). Why: as the message of the day at
    // its INCOMING_CLIENT, and a text line at its CLIENT_READY, after the informs held for it.
    Fight f = server_.fight();
    if (!unit.player->observing) {
        goObserver(f, unit, false, true);
    }
    unit.player->heldOut = true;
}

void GameHost::keepHeldOut(std::uint8_t peer)
{
    const auto p = players_.find(peer);
    ServerObject* unit = p != players_.end() ? server_.world().byExtent(p->second.extent) : nullptr;
    if (!unit || !unit->player) {
        return;
    }
    if (!unit->player->observing) {
        Fight f = server_.fight();
        goObserver(f, *unit, false, true);
        return;
    }
    // Unseen and out of reach, as goObserver left it: any enchantment since (a ready's
    // invulnerability, a map switch's) ends an observer's invisibility.
    if (!enchanted(*unit, Enchant::Invisible)) {
        enchant(server_.world(), *unit, Enchant::Invisible, 0, 5);
    }
    unit->flags |= ObjFlag::NoCollide;
}

void GameHost::putOutRefused()
{
    // Its hold up, or (a client that never readies: a stuck download) readyWithinMs from its join.
    const std::uint32_t stuckMs = options_.stockRefusal ? options_.stockRefusal->readyWithinMs : 120000u;
    std::vector<std::uint8_t> due;
    for (const auto& [peer, held] : refused_) {
        if (held.until ? static_cast<std::int32_t>(nowMs_ - *held.until) >= 0 : nowMs_ - held.since >= stuckMs) {
            due.push_back(peer);
        }
    }
    for (const std::uint8_t peer : due) {
        kickPeer(peer); // (leave forgets it)
        refused_.erase(peer);
    }
}

void GameHost::soloHostIn(std::uint32_t extent)
{
    // The exe's first load ends in the saved game's restore (switch-to-solo) with its host there.
    server_.firstLoadHostIn();
    ServerObject* host = server_.world().byExtent(extent);
    if (!host || !host->player) {
        return;
    }
    // The solo host's join, SrvNet_OnIncomingClient(0x1F): its PlayerInit's outfit (again
    // playerMakeDefItems(1, 0)): the first load's weapon gone and a new one, the
    // clothes it wears kept.
    server_.giveDefaultItems(*host);
    if (options_.hostPlayerFile.empty()) {
        return;
    }
    // Then (after SrvNet_OnIncomingClient(0x1F)), a solo game's: clientCurSave sent as
    // SAVE_SERVER (CliNet_SendSaveServerXfer), to the host itself at once (the received
    // file's case 3: written out, SavePlayerDataFromClient,
    // and loaded, cliPlrInfoLoadFromFile; not a quest's: nothing dropped), then
    // Plr_LoadClientPlayerData(clientCurSave) by its own client. A new character's file has only
    // the client's sections: the load is cliPlrInfoLoadFromFile's god mode off and a reset (its book,
    // guide and abilities anew, level 1, full, idle) and its numbers by its experience. (No map
    // intro, no "Game Loaded.": those are the in-game load's.)
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream f(options_.hostPlayerFile, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    Fight f = server_.fight();
    loadSentPlayerData(f, *host, saveServerSent(bytes));
    if (ServerObject* h = server_.world().byExtent(extent)) {
        loadHostClientSections(server_.world(), *h, options_.hostPlayerFile);
    }
}

void GameHost::beginQuit()
{
    // Game_BeginQuit on a host, once (g_quitStartFrame): the acks awaited are the players but
    // the host's own (playerInfoCount - 1, those not in yet among them, who never get
    // it); with any, MSG_SERVER_QUIT to all but the host's (Net_QueueImportant(0x9F, .., 1)),
    // the "Shutting down server..." dialog up till they're in; with none, Game_FinishQuit now.
    if (quitStarted_) {
        return;
    }
    quitStarted_ = true;
    std::optional<std::uint8_t> hostPeer;
    int others = 0;
    for (const auto& [peer, p] : players_) {
        if (isHostPlayer(p)) {
            hostPeer = peer;
        } else {
            ++others;
        }
    }
    quitPendingAcks_ = others;
    if (others > 0) {
        sendImportantToAll(std::vector<std::uint8_t>{static_cast<std::uint8_t>(net::Op::ServerQuit)}, true, hostPeer);
    }
}

void GameHost::endSession()
{
    // servEndSession (not in a solo game): server_netCloseHandler's Net_CloseStream of
    // the host's stream, which closes each client's (flushed, SERVER_CLOSE, flushed). The players
    // aren't removed one by one: the server is going. (noxcpp: a solo game's guests, Options::soloGuests,
    // are closed as a network game's.)
    if (options_.solo && options_.soloGuests == 0) {
        return;
    }
    transport_.closeAll();
}

// A player's stream closed: its votes and saves dropped, its unit off the minimaps and out of the
// world (what it held dropped), and everyone told it quit.
void GameHost::leave(std::uint8_t peer)
{
    const auto it = players_.find(peer);
    if (it == players_.end()) {
        return;
    }
    // (Plr_OnDisconnect's first: its save flags cleared) Nothing waits on its save.
    kickAfterSave_ &= ~(1u << (it->second.slot & 31u));
    savePending_ &= ~(1u << (it->second.slot & 31u));
    if (vote_.active() && vote_.choiceOf(peer)) {
        vote_.forget(peer); // (noxcpp's map vote: its vote gone with it)
        sendTallies();
    }
    if (ServerObject* unit = server_.world().byExtent(it->second.extent); unit && unit->player) {
        // Its votes withdrawn, head first; a vote left with none is over.
        const std::uint32_t bit = 1u << (it->second.slot & 31u);
        for (std::size_t i = 0; i < votes_.size();) {
            if (votes_[i].voters & bit) {
                votes_[i].voters &= ~bit;
                --votes_[i].votes;
            }
            if (votes_[i].votes == 0) {
                endVote(i);
            } else {
                ++i;
            }
        }
    }
    const bool wasRefused = refused_.erase(peer) != 0; // (noxcpp: one held out; the mode never heard of it)
    if (ServerObject* unit = server_.world().byExtent(it->second.extent)) {
        // (noxcpp: a mode told, the unit still there with what it holds)
        if (options_.onLeave && unit->player && !wasRefused) {
            options_.onLeave(*this, peer, *unit);
        }
        // (Plr_OnDisconnect) A quest's player out of it (quest.hpp).
        if (server_.world().quest()) {
            Fight f = server_.fight();
            questPlayerLeaves(f, *unit);
        }
        // Off the others' minimaps and they off its (bits 1 and 2).
        for (auto& [extent, o] : server_.world().objects()) {
            if (o.player && &o != unit) {
                unmarkMinimap(server_.world(), *unit, o, 3);
                unmarkMinimap(server_.world(), o, *unit, 3);
            }
        }
        // The doors it locked by magic free (their ObjOwner simply cleared).
        for (auto& [extent, o] : server_.world().objects()) {
            if ((o.classes & things::ObjectClass::Door) && o.owner == unit->extent) {
                o.owner = 0;
            }
        }
    }
    // Its file transfers given up (by its index + 1: after the doors, whether it had a unit
    // or not).
    xfers_.peerGone(peer);
    // Then, by its index, every object owed to its slot afresh, known only of the always-sent
    // kinds (the next player in the slot starts clean). Its map sender isn't touched: a send under
    // way goes on into the closed stream (an original bug).
    forgetObjects(it->second.slot);
    if (ServerObject* unit = server_.world().byExtent(it->second.extent)) {
        // MSG_OUTGOING_CLIENT {its code} to the others (the first of the two notices: 0xAE here,
        // Plr_RemoveFromGame's MSG_PLAYER_QUIT below).
        net::msg::OutgoingClient outgoing;
        outgoing.netCode = static_cast<std::uint16_t>(unit->extent);
        sendImportantToAll(bytesOf(outgoing), false, peer); // (Net_QueueImportant(its index | 0x80))
        removeSpawned(server_.world(), *unit);
        // What it holds: on the ground about it, in a quest gone.
        if (unit->player && !server_.world().quest()) {
            server_.items().dropAll(*unit);
        } else {
            for (const std::uint32_t e : std::vector<std::uint32_t>(unit->inventory)) {
                if (ServerObject* i = server_.world().byExtent(e)) {
                    deleteObject(server_.world(), *i); // (delayedDeleteObject)
                }
            }
        }
        unit->pendingDelete = true;
        // (Plr_RemoveFromGame: modes.hpp's playerRemoved.)
        if (unit->player) {
            Fight f = server_.fight();
            playerRemoved(f, *unit);
        }
    }
    net::msg::PlayerQuit quit;
    quit.netCode = static_cast<std::uint16_t>(it->second.extent);
    players_.erase(it);
    sysops_.erase(peer);
    sendImportantToAll(bytesOf(quit)); // (Plr_RemoveFromGame: Net_QueueImportant(its index | 0x80))
    if (server_.world().wolGame) {
        refreshSettingsSlot0(); // (Plr_RemoveFromGame's in a WOL game)
    }
}

void GameHost::onFile(const Xfers::Received& file)
{
    // A received file, on the server: 3 (SAVE_SERVER) a client's saved player, written out
    // (SavePlayerDataFromClient: encrypted, padded to the cipher's block) and loaded
    // onto it (cliPlrInfoLoadFromFile); in a quest one that won't load drops the
    // player (reason 4), one that does is waited for no more (questAwaitingSave). 1 and 2 are the
    // client's (the MOTD, its own save).
    if (file.action != Xfers::SaveServer || !players_.contains(file.peer)) {
        return;
    }
    ServerObject* unit = server_.world().byExtent(players_[file.peer].extent);
    if (!unit || !unit->player) {
        return;
    }
    bool drop = false;
    Fight f = server_.fight();
    // (noxcpp: a solo game's guest brings its character, not its game: GameData stays the host's;
    // one told another game's flags brings that game's character, read as that game's.)
    bool loaded = false;
    {
        std::optional<GameFlagsAs> as;
        if (const std::uint32_t told = clientFlags(file.peer); told != server_.world().gameFlags) {
            as.emplace(server_.world(), told);
        }
        loaded = loadSentPlayerData(f, *unit, file.data, &drop, soloGuest(players_[file.peer]));
        if (as) {
            as.reset();
            if (loaded) {
                relevel(server_.world(), *unit); // (its level by the world's own rules, not the told game's)
            }
        }
    }
    // (Its .plr gave it its character's class back, and that class's book: the forced one again.)
    if (const auto forced = forcedClass(server_.world()); forced && loaded && !drop) {
        forceClass(f, *unit, *forced);
        players_[file.peer].info[0x42] = *forced;
        players_[file.peer].accept.classId = *forced;
    }
    if (drop || (server_.world().quest() && !loaded)) {
        disconnect(file.peer, drop ? 1 : 4); // (the Attrib's mode check: reason 1)
    } else if (server_.world().quest()) {
        unit->player->questAwaitingSave = false;
    }
    if (options_.onCharacter && !drop) {
        options_.onCharacter(*this, file.peer, loaded); // (noxcpp: a mode told)
    }
}

std::optional<std::vector<std::uint8_t>> GameHost::savedPlayer(Fight& fight, ServerObject& unit, std::optional<std::uint8_t> peer)
{
    // (noxcpp: a client told another game's flags is sent that game's file.)
    std::optional<GameFlagsAs> as;
    if (peer) {
        if (const std::uint32_t told = clientFlags(*peer); told != server_.world().gameFlags) {
            as.emplace(server_.world(), told);
        }
    }
    // (A quest's told a character of the world's: only what a quest's read of it will take.)
    const bool asQuest = as && (server_.world().gameFlags & GameFlag::Quest);
    const auto file = asQuest ? buildQuestFile(fight, unit) : buildPlayerFile(fight, unit);
    if (!file) {
        return std::nullopt;
    }
    return saveDataSent(*file);
}

bool GameHost::sendSaveData(std::uint8_t peer, const HostedPlayer& p, std::vector<std::uint8_t> file)
{
    // SrvNet_SendSaveDataToClient: not while one is on its way; marked
    // on its way, sent as "SAVEDATA" (action 2).
    const std::uint32_t bit = 1u << (p.slot & 31u);
    if ((savePending_ & bit) || file.empty()) {
        return false;
    }
    savePending_ |= bit;
    xfers_.send(peer, Xfers::SaveData, "SAVEDATA", file);
    return true;
}

bool GameHost::sendSaveTo(std::uint8_t peer)
{
    // (noxcpp: a mode's) the quest's map-start step for one player: its save written as its
    // client's game, MSG_GAUNTLET 0x1C {1}, sent (SAVEDATA; its client's 0x1B comes back).
    const auto it = players_.find(peer);
    if (it == players_.end() || isHostPlayer(it->second)) {
        return false;
    }
    ServerObject* unit = server_.world().byExtent(it->second.extent);
    if (!unit || !unit->player) {
        return false;
    }
    Fight f = server_.fight();
    questSendSave(f, *unit);
    return true;
}

void GameHost::questSendSave(Fight& fight, ServerObject& unit)
{
    // The map start's step: saved (Plr_SaveServerPlayerData); MSG_GAUNTLET 0x1C {1}; sent; not
    // sent and none pending, 0x1C {0}.
    for (auto& [peer, p] : players_) {
        if (p.extent != unit.extent) {
            continue;
        }
        auto file = savedPlayer(fight, unit, peer);
        if (!file) {
            return;
        }
        const bool pending = (savePending_ & (1u << (p.slot & 31u))) != 0;
        fight.world.broadcasts().push_back({{0xF0, 0x1C, 1}, true, unit.extent});
        if (!sendSaveData(peer, p, std::move(*file)) && !pending) {
            fight.world.broadcasts().push_back({{0xF0, 0x1C, 0}, true, unit.extent});
        }
        return;
    }
}

void GameHost::questSaveAndKick(Fight& fight, ServerObject& unit)
{
    // one in the quest whose save is loaded: its save to its client (unless one is
    // pending) and out once its client has it; a save that wasn't sent
    // keeps it. Anyone else out at once (Plr_DisconnectByIndex, reason 4: after the frame here).
    for (auto& [peer, p] : players_) {
        if (p.extent != unit.extent) {
            continue;
        }
        const std::uint32_t bit = 1u << (p.slot & 31u);
        if (unit.player->questActive != 0 && !unit.player->questAwaitingSave && server_.world().questSaves) {
            bool sent = true;
            if (!(savePending_ & bit)) {
                if (auto file = savedPlayer(fight, unit, peer)) {
                    sent = sendSaveData(peer, p, std::move(*file));
                }
            }
            if (sent) {
                kickAfterSave_ |= bit;
            }
            return;
        }
        if (std::ranges::find(dropAfterFrame_, peer) == dropAfterFrame_.end()) {
            dropAfterFrame_.push_back(peer);
        }
        return;
    }
}

void GameHost::onVote(std::uint8_t peer, std::span<const std::uint8_t> m)
{
    World& world = server_.world();
    ServerObject* voter = world.byExtent(players_[peer].extent);
    if (!voter || !voter->player || m.size() < 2) {
        return;
    }
    // (SrvNet_OnPacket's MSG_VOTE: 0 cast a kick (in a quest the quest's, kind 3), 1 cast kind 1,
    // 2 withdraw a kick (in a quest kind 3), 3 withdraw (kind 1: as 2's), each with
    // a name; 4 / 5 cast / withdraw the quest's restart, kind 2, without one.)
    const std::uint8_t sub = m[1];
    // The voter's bit in a vote's `voters` mask, by its player slot (a vote counts each voter once).
    const std::uint32_t bit = 1u << (players_[peer].slot & 31);
    const auto newVote = [&](int kind, std::uint32_t target, std::uint8_t needed, bool team) {
        // (at the list's head; the first, MSG_VOTE 6 {1} to everyone).
        const bool first = votes_.empty();
        votes_.insert(votes_.begin(), Vote{kind, target, 0, 0, needed, team});
        if (first) {
            sendImportantToAll(std::vector<std::uint8_t>{0xEE, 0x06, 1}, true); // (SrvNet_SendVoteState(0xFF, 1))
        }
        return votes_.begin();
    };
    if (sub == 4 || sub == 5) {
        // The quest's restart: ResetQuestMinVotes not 0, the voter in
        // the quest (a withdrawal: anyone); the one kind-2 vote, needing 6 (the players in, in fact).
        auto it = std::ranges::find_if(votes_, [](const Vote& v) { return v.kind == 2; });
        if (sub == 4) {
            if (options_.resetQuestVotes == 0 || voter->player->questActive == 0) {
                return;
            }
            if (it == votes_.end()) {
                it = newVote(2, 0, 6, false);
            }
            if (!(it->voters & bit)) {
                it->voters |= bit;
                ++it->votes;
            }
        } else if (it != votes_.end() && (it->voters & bit)) {
            it->voters &= ~bit;
            if (--it->votes == 0) {
                endVote(static_cast<std::size_t>(it - votes_.begin()));
            }
        }
        return;
    }
    if (sub > 3 || m.size() < 0x34) {  // (+2: the name, 25 UTF-16 letters; 0x34 = 2 + 50)
        return;
    }
    const std::u16string name = utf16Until(m.subspan(2, 50));
    // The player of that name (_wcscmp), not itself.
    HostedPlayer* target = nullptr;
    for (auto& [other, p] : players_) {
        if (utf16Until(p.accept.playerName) == name) {
            target = &p;
            break;
        }
    }
    ServerObject* unit = target ? world.byExtent(target->extent) : nullptr;
    if (!unit || unit == voter) {
        return;
    }
    const bool questKick = world.quest() && (sub == 0 || sub == 2);
    if (questKick && sub == 0) {
        // KickQuestPlayerMinVotes not 0, voter and target in the quest; the kind-3
        // vote on it (needing 6), no team rule.
        if (options_.kickQuestPlayerVotes == 0 || voter->player->questActive == 0 || !unit->player || unit->player->questActive == 0) {
            return;
        }
        auto it = std::ranges::find_if(votes_, [&](const Vote& v) { return v.kind == 3 && v.target == unit->extent; });
        if (it == votes_.end()) {
            it = newVote(3, unit->extent, 6, false);
        }
        if (!(it->voters & bit)) {
            it->voters |= bit;
            ++it->votes;
        }
        return;
    }
    if (sub >= 2) { // (the kind-0 vote on it with this voter in; a quest's kind 3)
        const int kind = questKick ? 3 : 0;
        for (auto it = votes_.begin(); it != votes_.end(); ++it) {
            if (it->kind == kind && it->target == unit->extent && (it->voters & bit)) {
                it->voters &= ~bit;
                if (--it->votes == 0) {
                    endVote(static_cast<std::size_t>(it - votes_.begin()));
                }
                return;
            }
        }
        return;
    }
    // needs a vote count set (1-32); in team play only a team-mate's.
    if (options_.kickVotes <= 0 || 0x20 < options_.kickVotes) {
        return;
    }
    const bool teams = (world.gameplayFlags & 4u) != 0;  // gameplayFlags bit 2: team play
    if (teams && !(voter->team != 0 && voter->team == unit->team)) {
        return;
    }
    const int kind = sub;
    auto it = std::ranges::find_if(votes_, [&](const Vote& v) { return v.kind == kind && v.target == unit->extent; });
    if (it == votes_.end()) {
        it = newVote(kind, unit->extent, static_cast<std::uint8_t>(options_.kickVotes), teams);
    }
    if (!(it->voters & bit)) {
        it->voters |= bit;
        ++it->votes;
    }
}

void GameHost::updateVotes()
{
    World& world = server_.world();
    // Vote kinds: 0 and 1 kicks (handled alike), 2 the quest's restart, 3 a quest kick. endVote erases, so `i` only
    // moves on past a vote that stays.
    for (std::size_t i = 0; i < votes_.size();) {
        Vote& v = votes_[i];
        if (v.kind == 2) {
            // everyone in the quest has voted: each respawned (Plr_Respawn), stage 0
            // and the next quest map (getQuestMapFile, mapLoad).
            if (questPlayers(world) <= v.votes) {
                Fight f = server_.fight();
                // (Plr_GetFirstUnit's order, the players' index: each respawn picks its start.)
                for (ServerObject* o : playersInOrder(world, [](const ServerObject& u) { return !u.gone(); })) {
                    if (o->player->questActive == 1) {
                        f.respawn(*o);
                    }
                }
                world.questStage = 0;
                const std::optional<std::string> next = world.questMaps ? world.questMaps->pick(f.rng) : std::nullopt;
                if (next && !next->empty()) {
                    world.exitTo = *next;
                }
                endVote(i);
            } else {
                ++i;
            }
            continue;
        }
        if (v.kind == 3) {
            // its target still in the quest; 6 votes, or (two players in or more)
            // all of them but one and at least 2: its save and out, its name kept
            // out 15 minutes. Fewer than two in the quest: the vote's over.
            ServerObject* unit = world.byExtent(v.target);
            if (!unit || !unit->player || unit->gone() || unit->player->questActive == 0) {
                endVote(i);
                continue;
            }
            if (v.votes < v.needed) {
                const int in = questPlayers(world);
                if (in < 2) {
                    endVote(i);
                    continue;
                }
                if (v.votes < in - 1 || v.votes < 2) {
                    ++i;
                    continue;
                }
            }
            Fight f = server_.fight();
            questSaveAndKick(f, *unit);
            for (auto& [peer, p] : players_) {
                if (p.extent == v.target) {
                    bans_[utf16Until(p.accept.playerName)] = nowMs_ + 15u * 60000u; // (banned for 15 minutes)
                    syncAccess();
                }
            }
            endVote(i);
            continue;
        }
        ServerObject* unit = world.byExtent(v.target);
        bool done = !unit;
        if (unit) {
            // enough, or (two or more) all but one of the players (its team's).
            int players = 0;
            for (const auto& [extent, o] : world.objects()) {
                if (o.player && !o.gone() && (!v.team || (o.team != 0 && o.team == unit->team))) {
                    ++players;
                }
            }
            if (v.needed <= v.votes || (players - 1 <= v.votes && 1 < v.votes)) {
                // (noxcpp: the kick's leave() ends every vote it leaves without a voter, so the list
                // may shrink under `i` and `v`: this vote and those still to look at are found again
                // by kind and target, which name one vote.)
                const int kind = v.kind;
                const std::uint32_t target = v.target;
                std::vector<std::pair<int, std::uint32_t>> later;
                for (std::size_t j = i + 1; j < votes_.size(); ++j) {
                    later.emplace_back(votes_[j].kind, votes_[j].target);
                }
                for (auto& [peer, p] : players_) {
                    if (p.extent == target) {
                        bans_[utf16Until(p.accept.playerName)] = nowMs_ + 15u * 60000u; // (banned for 15 minutes)
                        syncAccess();
                        const std::uint8_t kicked = peer;
                        disconnect(kicked, 4); // (Plr_DisconnectByIndex, reason 4)
                        break;
                    }
                }
                if (const auto at = std::ranges::find_if(votes_, [&](const Vote& w) { return w.kind == kind && w.target == target; });
                    at != votes_.end()) {
                    endVote(static_cast<std::size_t>(at - votes_.begin()));
                }
                i = static_cast<std::size_t>(std::ranges::find_if(votes_, [&](const Vote& w) {
                                                 return std::ranges::find(later, std::pair{w.kind, w.target}) != later.end();
                                             }) - votes_.begin());
                continue;
            }
        }
        if (done) {
            endVote(i);
        } else {
            ++i;
        }
    }
}

void GameHost::endVote(std::size_t index)
{
    // a restart vote's voters told it's over (SrvNet_SendVoteReset, MSG_VOTE 7);
    // gone from the list; none left, MSG_VOTE 6 {0} to everyone.
    const Vote v = votes_[index];
    if (v.kind == 2) {
        for (const auto& [peer, p] : players_) {
            if (v.voters & (1u << (p.slot & 31u))) {
                sendImportant(peer, std::vector<std::uint8_t>{0xEE, 0x07}, true); // (Net_QueueImportantSeq(its index, .., 1))
            }
        }
    }
    votes_.erase(votes_.begin() + static_cast<std::ptrdiff_t>(index));
    if (votes_.empty()) {
        sendImportantToAll(std::vector<std::uint8_t>{0xEE, 0x06, 0}, true); // (SrvNet_SendVoteState(0xFF, 0))
    }
}

// MSG_GAME_SETTINGS as the game stands: its flags, server options, player limit and the mode's
// limits.
net::msg::GameSettings GameHost::gameSettings(std::uint32_t frame) const
{
    net::msg::GameSettings settings;
    settings.settingsSerial = frame;
    settings.version = net::kGameVersion;
    // (SrvNet_BroadcastGameSettings: the game flags & 0x7FFF0, the game's flags as they are:
    // the network game's 0x2000 among them, which the clients' F9 and Home wait for.)
    settings.gameFlags = const_cast<Server&>(server_).world().gameFlags & 0x7FFF0;
    settings.serverSubFlags = options_.serverOptions;
    settings.playerLimit = static_cast<std::uint8_t>(options_.maxPlayers);
    const World& world = const_cast<Server&>(server_).world();
    settings.fragLimit = static_cast<std::uint8_t>(lessonLimit(world)); // (the settings record's lesson and time limits)
    settings.timeLimit = timeLimit(world);
    return settings;
}

std::uint32_t GameHost::mapModesOf(const std::string& name)
{
    namespace fs = std::filesystem;
    if (const auto it = mapModes_.find(name); it != mapModes_.end()) {
        return it->second;
    }
    const fs::path mapsDir = !options_.mapsDir.empty() ? options_.mapsDir : mapPath_.parent_path().parent_path();
    const fs::path path = mapsDir / name / (name + ".map");
    std::uint32_t modes = 0;
    try {
        if (fs::exists(resolveNoCase(path))) { // (in any case: maps/Fortress/fortress.map)
            const auto file = map::loadMapFile(path);
            modes = file.find(map::MapInfo::kName) ? mapToGameFlags(map::decodeSection<map::MapInfo>(file).gameFlags) : 0;
        }
    } catch (const std::exception&) {
        modes = 0;
    }
    mapModes_[name] = modes;
    return modes;
}

GameHost::ServerSettings GameHost::serverSettings()
{
    const World& world = server_.world();
    ServerSettings s;
    s.name = options_.serverName;
    s.mode = world.gameFlags & 0x17F0u;
    s.map = mapPath_.stem().string();
    s.lessonLimit = lessonLimit(world);
    s.timeLimit = timeLimit(world);
    s.teams = teamCount(world);
    return s;
}

std::vector<GameHost::MapChoice> GameHost::mapChoices()
{
    namespace fs = std::filesystem;
    const fs::path mapsDir = !options_.mapsDir.empty() ? options_.mapsDir : mapPath_.parent_path().parent_path();
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(mapsDir, ec)) {
        // (A mode's generated maps aren't anyone's to choose: ModeInfo::ownsMap.)
        if (e.is_directory(ec) && !modes::registry().ownsMap(e.path().filename().string())) {
            names.push_back(e.path().filename().string());
        }
    }
    std::ranges::sort(names);
    std::vector<MapChoice> out;
    for (const auto& name : names) {
        if (const auto it = mapChoices_.find(name); it != mapChoices_.end()) {
            if (it->second.modes != 0) {
                out.push_back(it->second);
            }
            continue;
        }
        MapChoice c;
        c.name = name;
        try {
            const fs::path path = mapsDir / name / (name + ".map");
            if (fs::exists(resolveNoCase(path))) {
                const auto file = map::loadMapFile(path);
                if (file.find(map::MapInfo::kName)) {
                    const auto info = map::decodeSection<map::MapInfo>(file);
                    // (checkMapFile: a network map; not a solo one, 0x200's)
                    if (!(info.gameFlags & 0x200u) && !(info.gameFlags & 1u)) {
                        c.modes = mapToGameFlags(info.gameFlags);
                        c.minPlayers = info.minPlayers;
                        c.maxPlayers = info.maxPlayers;
                    }
                }
            }
        } catch (const std::exception&) {
            c.modes = 0;
        }
        mapChoices_[name] = c;
        if (c.modes != 0) {
            out.push_back(c);
        }
    }
    return out;
}

std::pair<int, int> GameHost::defaultLimits(std::uint32_t mode) const
{
    const auto i = static_cast<std::size_t>(modeIndex(mode));
    return {options_.lessonLimits[i], options_.timeLimits[i]};
}

std::pair<int, int> GameHost::limitsFor(std::uint32_t mode)
{
    const World& world = server_.world();
    const auto i = static_cast<std::size_t>(modeIndex(mode));
    return {world.lessonLimits[i], world.timeLimits[i]};
}

GameHost::HostSwitches GameHost::hostSwitches()
{
    const World& world = server_.world();
    HostSwitches s;
    s.forceRespawn = world.autoRespawn && !world.quest(); // (server_doPlayersAutoRespawn)
    s.sendMotd = options_.sendMotd;
    s.mapCycle = mapCycleOn_;
    s.itemRespawn = (options_.serverOptions & 2u) != 0;
    s.camperAlarm = (options_.serverOptions & 0x2000u) != 0 || (world.gameFlags & 0x420u) != 0;  // (0x420: elimination or CTF)
    s.bones = world.leaveBones;
    s.gestures = world.gestures;
    s.latency = latency_;
    s.latencyAmount = latencyAmount_;
    s.wol = world.wolGame;
    s.updateRate = kFrameRate * options_.multiplier;
    s.mapVote = options_.mapVote;
    return s;
}

namespace {

/// Compared in the "C" locale: equal but for the case of A-Z.
template <typename S>
bool sameIgnoringCase(const S& a, const S& b)
{
    const auto fold = [](auto c) { return (c >= 'A' && c <= 'Z') ? static_cast<decltype(c)>(c + 0x20) : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](auto x, auto y) { return fold(x) == fold(y); });
}

} // namespace

void GameHost::setServerName(const std::string& name)
{
    // gameSetServername: 15 letters at most; the same but for case, kept as it was.
    const std::string n = name.substr(0, 15);
    if (!n.empty() && !sameIgnoringCase(n, options_.serverName)) {
        options_.serverName = n;
        settingsChanged_ = true;
    }
}

void GameHost::setHostSwitches(const HostSwitches& s)
{
    World& world = server_.world();
    const HostSwitches was = hostSwitches();
    if (s.forceRespawn != was.forceRespawn && !world.quest()) {
        world.autoRespawn = s.forceRespawn; // (0x283D: ruleSetNoRespawn)
    }
    options_.sendMotd = s.sendMotd; // (0x283E)
    mapCycleOn_ = s.mapCycle;       // (0x2840)
    const auto toggle = [&](std::uint32_t bit) {
        // the option flipped, the clients told.
        options_.serverOptions ^= bit;
        world.serverOptions = options_.serverOptions;
        settingsChanged_ = true;
    };
    if (s.itemRespawn != was.itemRespawn) {
        toggle(2u); // (0x2841)
    }
    if (s.camperAlarm != was.camperAlarm && !(world.gameFlags & 0x420u)) {
        // (0x2842; on or off, each player's watch started anew: by the option's
        // toggle as it's turned on and by 0x2842 as it's turned off)
        toggle(0x2000u);
        camperResetAll(world, server_.clock().frame());
    }
    world.leaveBones = s.bones;  // (advserv 0x836)
    world.gestures = s.gestures; // (0x837)
    latencyAmount_ = std::max(0, s.latencyAmount); // (0x83E's entry, below 0 none)
    if (s.latency != latency_ || s.announceLatency) {
        // (0x83A-0x83D: the mode, and the announce flag set: the server's next update announces it,
        // the report.c line to everyone; a click on the mode there was too.)
        latency_ = std::clamp(s.latency, 0, 3);
        static constexpr const char* kReports[4] = {"report.c:NoComp", "report.c:MinComp", "report.c:AveComp", "report.c:UserComp"};
        // (SrvNet_BroadcastInformString: SrvNet_SendInformString to each player unit, Net_QueueMsg:
        // the frame's datagram, ready or not; none to one whose .plr is being applied.)
        const std::vector<std::uint8_t> m = informStringBytes(kReports[latency_]);
        for (const auto& [peer, p] : players_) {
            if (const ServerObject* u = world.byExtent(p.extent); u && u->player && !u->player->transfer) {
                transport_.sendUnreliable(peer, m);
            }
        }
    }
    // noxcpp's update rate: the clock's substeps from the next frame, and what the game list's
    // extended reply says the server does.
    const int multiplier = std::clamp(s.updateRate / kFrameRate, 1, kMaxMultiplier);
    if (multiplier != options_.multiplier) {
        options_.multiplier = multiplier;
        server_.clock().setMultiplier(multiplier);
        transport_.setExtended({lobby::kExtendedVersion, static_cast<std::uint8_t>(multiplier), extendedFeatures()});
    }
    // noxcpp's map vote: off, a vote under way ends without a result.
    options_.mapVote = s.mapVote;
    if (!s.mapVote && vote_.active()) {
        if (vote_.kind() == MapVote::Kind::EndOfMatch) {
            server_.holdGameOver(0); // (the stock 10 s again)
        }
        closeVote(std::nullopt);
    }
}

void GameHost::sendSubstep()
{
    // noxcpp's (no stock counterpart): after a substep between frames, each identified client
    // (ServerTransport::identified) that's in is told where the important objects it knows are
    // that the substep carried on (MSG_NOXCPP_SUBSTEP, unreliable, its own datagram). Classic
    // clients get nothing between frames.
    const int k = server_.lastSubstep();
    const int m = server_.lastMultiplier();
    World& world = server_.world();
    for (auto& [peer, p] : players_) {
        if (!p.ready || !transport_.identified(peer)) {
            continue;
        }
        const std::uint32_t frame = server_.clock().frame();
        // MSG_NOXCPP_SUBSTEP: {op, substep, substeps a frame, count, u16 frame}, then {u16 code, u16 x,
        // u16 y} for each object.
        std::vector<std::uint8_t> m0 = {static_cast<std::uint8_t>(net::Op::NoxcppSubstep), static_cast<std::uint8_t>(k),
                                        static_cast<std::uint8_t>(m), 0, static_cast<std::uint8_t>(frame),
                                        static_cast<std::uint8_t>(frame >> 8)};
        std::vector<std::uint8_t> msg = m0;
        // One goes each substep even with nothing moving (the header alone): the client's clock
        // is fitted to them, and gaps would put it back to the frame's rate.
        bool sent = false;
        const auto flush = [&](bool last) {
            if (msg[3] != 0 || (last && !sent)) {
                transport_.sendUnreliable(peer, msg);
                transport_.flushUnreliable(peer);
                sent = true;
            }
            msg = m0;
        };
        // Its own unit first (the update stream's header carries it, not its known objects), then
        // what it knows.
        std::vector<std::uint32_t> extents{p.extent};
        for (const std::uint32_t e : p.knows) {
            if (e != p.extent) {
                extents.push_back(e);
            }
        }
        for (const std::uint32_t extent : extents) {
            const ServerObject* o = world.byExtent(extent);
            // (Those moving, or carried off their frame's place: a player setting off from standing
            // has no motion yet, its frame's push to come: predictedFrameMove.)
            if (!o || o->priority != Priority::High || o->gone() || o->holder != 0 ||
                (o->vx == 0 && o->vy == 0 && o->x == o->frameStartX && o->y == o->frameStartY)) {
                continue;
            }
            // (positions as the update stream truncates them: the FPU's chop mode)
            const auto x = static_cast<std::uint16_t>(static_cast<std::int32_t>(o->x));
            const auto y = static_cast<std::uint16_t>(static_cast<std::int32_t>(o->y));
            msg.insert(msg.end(), {static_cast<std::uint8_t>(extent), static_cast<std::uint8_t>(extent >> 8), static_cast<std::uint8_t>(x),
                                   static_cast<std::uint8_t>(x >> 8), static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8)});
            if (++msg[3] == 100) {  // (at most 100 objects a datagram)
                flush(false);
            }
        }
        flush(true);
    }
}

GameHost::AccessSettings GameHost::accessSettings()
{
    AccessSettings a = access_;
    a.maxPlayers = options_.maxPlayers;
    a.sysopPassword = strings::toUtf8(options_.sysopPassword);
    return a;
}

void GameHost::setAccessSettings(const AccessSettings& a)
{
    // windowAccessProc's fields as they're left (0x4003 / 0x401F) and boxes.
    access_.closed = a.closed;                   // (0x2776)
    access_.privateGame = a.privateGame;         // (0x2777)
    access_.password = a.password.substr(0, 9);  // (0x2778)
    access_.minPing = a.minPing;                 // (0x2791 / 0x2792)
    access_.maxPing = a.maxPing;                 // (0x2793 / 0x2794)
    access_.limitResolution = a.limitResolution; // (0x278C)
    access_.restrictedClasses = a.restrictedClasses & 7u; // (0x278B)
    access_.allowed = a.allowed;
    const int limit = std::clamp(a.maxPlayers, 1, 32); // (0x2795: Game_SetMaxPlayers)
    if (limit != options_.maxPlayers) {
        options_.maxPlayers = limit;
        settingsChanged_ = true;
    }
    options_.sysopPassword = strings::fromUtf8(a.sysopPassword); // (0x2798: sysopSetPass)
    syncAccess();
}

std::vector<std::pair<std::string, bool>> GameHost::bannedNames()
{
    std::vector<std::pair<std::string, bool>> out;
    for (const auto& [name, until] : bans_) {
        out.emplace_back(strings::toUtf8(name), until != 0xFFFFFFFFu);
    }
    return out;
}

void GameHost::addAccessName(const std::string& name, bool allowedList)
{
    // 0x2780: onto the list shown (the allowed(0) the banned: for good).
    if (name.empty()) {
        return;
    }
    if (allowedList) {
        access_.allowed.push_back(name);
    } else {
        bans_[strings::fromUtf8(name)] = 0xFFFFFFFFu;
    }
    syncAccess();
}

void GameHost::removeAccessName(std::size_t index, bool allowedList)
{
    // 0x2781: the shown list's chosen entry.
    if (allowedList) {
        if (index < access_.allowed.size()) {
            access_.allowed.erase(access_.allowed.begin() + static_cast<std::ptrdiff_t>(index));
        }
    } else if (index < bans_.size()) {
        bans_.erase(std::next(bans_.begin(), static_cast<std::ptrdiff_t>(index)));
    }
    syncAccess();
}

void GameHost::kickPlayers(const std::vector<std::string>& names, std::uint32_t issuer, bool ban)
{
    // 0x27CF / 0x27D0: each named, but the host's own; out (Plr_DisconnectByIndex(4); a ban's
    // playerDisconnByPlrID), in a quest by the save-and-kick (its save first); a ban also
    // its name banned for good (with its serial).
    for (const std::string& name : names) {
        HostedPlayer* p = playerNamed(name);
        if (!p || (issuer != 0 && p->extent == issuer)) {
            continue;
        }
        if (ban) {
            bans_[strings::fromUtf8(p->name)] = 0xFFFFFFFFu;
        }
        if (server_.world().quest()) {
            if (ServerObject* unit = server_.world().byExtent(p->extent); unit && unit->player) {
                Fight f = server_.fight();
                questSaveAndKick(f, *unit);
                flushDrops();
            }
        } else {
            disconnect(p->peer, 4);
        }
    }
    syncAccess();
}

void GameHost::kickPeer(std::uint8_t peer)
{
    // (noxcpp: a mode's) Plr_DisconnectByIndex(4), as kickPlayers', by its peer: never the host's own.
    const auto it = players_.find(peer);
    if (it == players_.end() || isHostPlayer(it->second)) {
        return;
    }
    disconnect(peer, 4);
}

std::filesystem::path GameHost::mapsDir() const
{
    return !options_.mapsDir.empty() ? options_.mapsDir : mapPath_.parent_path().parent_path();
}

void GameHost::loadRules()
{
    // The map's load, after server_loadMapFile (a network game's, not a chat game's):
    // the rules read (slot 0, user.rul, flags 3) for the mode.
    World& world = server_.world();
    if (!(world.gameFlags & GameFlag::Network) || (world.gameFlags & GameFlag::Chat)) {
        rules_ = RuleSet{};
    } else {
        rules_ = readRules(mapsDir(), mapPath_.stem().string(), std::nullopt, 3, world.gameFlags, things_);
    }
    applyRules();
}

void GameHost::applyRules()
{
    // The rules' spells, weapons and armour off in the world (a deviation: the exe's
    // clients keep to them; here the server does): spells all on and types all allowed first
    // (spellEnableAll), then those the rules leave out off.
    World& world = server_.world();
    world.spellDisabled.fill(false);
    // (Spell ids 1-0x88; those flags: a spell some class can have, rules.cpp kClassSpell.)
    for (std::uint32_t id = 1; id < 0x89; ++id) {
        const things::SpellDef* def = things_.findSpell(id);
        if (def && (def->flags & 0x7000000u) && !rules_.spellAllowed(static_cast<int>(id))) {
            world.spellDisabled[id] = true;
        }
    }
    applyRuleTypes(rules_, world.disabledTypes);
    settingsChanged_ = true; // (the clients told: MSG_GAME_SETTINGS_2's bitmaps)
}

RuleSet GameHost::rulesFor(const std::string& map, std::uint32_t mode, const std::optional<std::string>& file)
{
    return readRules(mapsDir(), map, file, 7, mode, things_, true, server_.world().wolGame);
}

std::optional<RuleSet> GameHost::internetRules()
{
    if (!server_.world().wolGame) {
        return std::nullopt;
    }
    return readRules(mapsDir(), std::string(), std::nullopt, 4, 0x17F0, things_, true, true);
}

std::vector<std::string> GameHost::ruleSetsOf(const std::string& map)
{
    return ruleSets(mapsDir(), map);
}

bool GameHost::saveRuleSet(const std::string& map, const std::string& name, const RuleSet& rules, std::uint32_t mode)
{
    return writeRules(mapsDir(), map, name + ".rul", rules, mode, things_, server_.world().wolGame);
}

void GameHost::deleteRuleSet(const std::string& map, const std::string& name)
{
    // maps\<map>\<name>.rul gone.
    std::error_code ec;
    std::filesystem::remove(mapsDir() / map.substr(0, 8) / (name + ".rul"), ec);
}

GameHost::TeamsView GameHost::teamsView()
{
    World& world = server_.world();
    TeamsView v;
    v.teams = (world.gameplayFlags & 4u) != 0;
    v.autoAssign = (world.gameplayFlags & 2u) != 0;
    v.teamDamage = (world.gameplayFlags & 1u) != 0;
    v.chat = (world.gameFlags & GameFlag::Chat) != 0;
    for (const ServerTeam& t : world.teams) {
        if (t.active) {
            v.list.push_back({t.id, strings::toUtf8(t.name), t.color});
        }
    }
    for (const auto& [peer, p] : players_) {
        const ServerObject* unit = world.byExtent(p.extent);
        v.players.push_back({p.name, p.extent, unit ? unit->team : std::uint8_t{0}});
    }
    return v;
}

void GameHost::setUseTeams(bool on)
{
    // 0x285A.
    World& world = server_.world();
    if (on == ((world.gameplayFlags & 4u) != 0)) {
        return;
    }
    Fight f = server_.fight();
    if (on) {
        createFlagTeams(f); // (wndGuiTeamCreate)
        if (world.gameplayFlags & 2u) {
            shareOutPlayers(f, false);
        } else {
            setTeamBases(world, true);
        }
    } else {
        setTeamBases(world, false);
        removeAllTeams(world, true); // (Team_RemoveAll(1))
    }
}

void GameHost::setAutoAssign(bool on)
{
    // 0x285B.
    World& world = server_.world();
    if (on == ((world.gameplayFlags & 2u) != 0)) {
        return;
    }
    if (on) {
        setTeamBases(world, false);
        // with teams, gameplay flag 2 and the players shared out.
        if (teamCount(world) != 0) {
            world.gameplayFlags |= 2u;
            Fight f = server_.fight();
            shareOutPlayers(f, false);
        }
    } else {
        world.gameplayFlags &= ~2u;
        setTeamBases(world, true);
    }
}

void GameHost::setTeamDamage(bool on)
{
    // 0x285D.
    World& world = server_.world();
    world.gameplayFlags = on ? (world.gameplayFlags | 1u) : (world.gameplayFlags & ~1u);
}

void GameHost::shuffleTeams()
{
    // 0x285C.
    Fight f = server_.fight();
    shareOutPlayers(f, true);
}

void GameHost::assignToTeam(const std::vector<std::string>& names, std::uint8_t team)
{
    // The window's 0x2907: each player named, not observing (status bit 1) nor its first status
    // bit, added (Team_AddMember) or moved (Team_MoveMember), told and at its base.
    World& world = server_.world();
    ServerTeam* to = teamById(world, team);
    if (!to) {
        return;
    }
    Fight f = server_.fight();
    for (const std::string& name : names) {
        for (const auto& [peer, p] : players_) {
            ServerObject* unit = p.name == name ? world.byExtent(p.extent) : nullptr;
            if (!unit || !unit->player || (unit->player->statusFlags & 1u)) {
                continue;
            }
            if (unit->team == 0 || !inTeam(world, *unit, unit->team)) {
                addTeamMember(world, team, *unit, true, true, f.rng, f.frame);
            } else {
                moveTeamMember(world, *unit, *to, true, f.rng, f.frame);
            }
        }
    }
}

void GameHost::joinTeam(std::uint32_t netCode, std::uint8_t team)
{
    // On the host: not carrying a flag (weapon bit 1) nor observing; added or moved
    // (to its base); out of a chat game, to a player start (Map_FindPlayerStart).
    World& world = server_.world();
    ServerTeam* to = teamById(world, team);
    ServerObject* unit = world.byExtent(netCode);
    if (!to || !unit || !unit->player || (unit->player->weaponEquip & 1u) || (unit->player->statusFlags & 1u)) {
        return;
    }
    Fight f = server_.fight();
    if (unit->team == 0 || !inTeam(world, *unit, unit->team)) {
        addTeamMember(world, team, *unit, true, true, f.rng, f.frame);
        return;
    }
    moveTeamMember(world, *unit, *to, true, f.rng, f.frame);
    if (!(world.gameFlags & GameFlag::Chat)) {
        const auto [x, y] = findPlayerStart(world, unit, f.rng);
        setPosition(world, *unit, x, y, f.frame);
    }
}

void GameHost::renameTeam(std::uint8_t team, const std::string& name)
{
    World& world = server_.world();
    ServerTeam* t = teamById(world, team);
    const std::u16string wide = strings::fromUtf8(name);
    if (!t || wide.empty() ||
        std::ranges::any_of(world.teams, [&](const ServerTeam& o) { return o.active && sameIgnoringCase(o.name, wide); })) {
        return; // (a team has it, by _wcsicmp)
    }
    server::renameTeam(world, *t, wide);
}

void GameHost::applyServerSettings(const ServerSettings& s)
{
    World& world = server_.world();
    // (a map named) Slot 1 copied to slot 0, its "Form a new game" cleared there.
    copySettings(1, 0);
    settingsSlots_[0][0x39] = 0;
    // with Capture the Flag or Flag Ball chosen (the settings' 0x60), teams made
    // till there are two, and team play on (the host was asked first, ServOpts.c's NeedTeams).
    if ((s.mode & 0x60u) || s.flagTeams) {
        flagGameTeams(world);
    }
    setServerName(s.name); // (gameSetServername)
    if (s.mode != 0) {
        const auto i = static_cast<std::size_t>(modeIndex(s.mode));
        world.lessonLimits[i] = static_cast<std::uint16_t>(std::clamp(s.lessonLimit, 0, 0xFFFF));
        world.timeLimits[i] = static_cast<std::uint8_t>(std::clamp(s.timeLimit, 0, 255));
    }
    settingsChanged_ = true; // (the clients told, as the console's set does)
    // the settings kept for the next map (Game_CopySettingsData(1, 0)); only another
    // map is loaded now (mapLoad), the same one plays on.
    if (s.mode != 0) {
        world.chosenMode = s.mode;
    }
    const std::string map = s.map.empty() ? mapPath_.stem().string() : s.map;
    if (s.rules && !(s.mode & GameFlag::Chat)) {
        // The rules chosen into the map's user.rul for the mode, its other lines kept
        // (read, then written).
        RuleSet out = *s.rules;
        out.other = readRules(mapsDir(), map, std::nullopt, 3, s.mode ? s.mode : world.gameFlags, things_).other;
        writeRules(mapsDir(), map, "user.rul", out, s.mode ? s.mode : world.gameFlags, things_, world.wolGame);
    }
    if (!sameIgnoringCase(map, mapPath_.stem().string())) {
        nextMap(map);
    } else if (s.rules) {
        loadRules(); // (the same map: its rules read into slot 0)
    }
}

void GameHost::scanMaps()
{
    namespace fs = std::filesystem;
    if (options_.mapsDir.empty()) {
        return;
    }
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(options_.mapsDir, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        std::string name = e.path().filename().string();
        const auto prefix = [&](const char* p) {
            for (int i = 0; i < 3; ++i) {
                if (std::tolower(static_cast<unsigned char>(i < static_cast<int>(name.size()) ? name[static_cast<std::size_t>(i)] : '\0')) != p[i]) {
                    return false;
                }
            }
            return true;
        };
        const bool solo = (prefix("wiz") || prefix("war") || prefix("con")) && 3 < name.size() && '0' <= name[3] && name[3] <= '9';
        if (solo || name == "." || name == "..") {
            continue;
        }
        name = name.substr(0, 8);
        if (mapModesOf(name) != 0) { // (checkMapFile)
            names.push_back(std::move(name));
        }
    }
    std::ranges::sort(names); // (maplist_add: strcmp order)
    std::vector<std::pair<std::string, std::uint32_t>> list;
    if (!options_.questMapsOnly.empty()) {
        // (A mode's: the quest's maps are its generated one alone, written before each load.)
        for (const auto& n : options_.questMapsOnly) {
            list.emplace_back(n, 0x1000u);
        }
    } else {
        for (const auto& n : names) {
            if (!modes::registry().ownsMap(n)) {
                list.emplace_back(n, mapModesOf(n));
            }
        }
    }
    server_.questMaps().build(list);
}

void GameHost::nextMap(std::optional<std::string> exitTo)
{
    const util::RoundingScope rounding(server_.exactMaths());
    // mapLoad only asks; the server's next frame is the load's tick (loadNextMap).
    // Map downloads under way are aborted (reason 0).
    if (mapSendsInProgress_ != 0) {
        mapSendAbortAll(0);
    }
    pendingExit_ = std::move(exitTo);
    server_.requestMapLoad();
}
void GameHost::loadNextMap()
{
    const util::RoundingScope rounding(server_.exactMaths());
    // A mode changed at the last tick's end, after its clients' update (the quest's
    // start), is told ahead of the news held through the load (its respawns), as afterFrame does.
    tellModeChange();
    std::optional<std::string> exitTo = std::exchange(pendingExit_, std::nullopt);
    // (noxcpp's map vote) A map a vote chose is this load's or nobody's, and a vote still open
    // (one called, when the host, a sysop or an exit changes the map) ends with its map.
    std::optional<std::string> voted = std::exchange(voteNext_, std::nullopt);
    if (vote_.active()) {
        const MapVote::Kind kind = vote_.kind();
        vote_.close();
        for (auto& [peer, p] : players_) {
            if (transport_.identified(peer)) {
                transport_.sendReliable(peer, mapvote::closeMessage(kind, std::nullopt));
            }
        }
    }
    namespace fs = std::filesystem;
    const fs::path mapsDir = !options_.mapsDir.empty() ? options_.mapsDir : mapPath_.parent_path().parent_path();
    const auto pathOf = [&](const std::string& name) { return mapsDir / name / (name + ".map"); };
    fs::path next = mapPath_;
    std::string waypoint; // (mapLoad: "map:waypoint", the players put there)
    if (exitTo) {
        std::string name = exitTo->substr(0, exitTo->find('\0')); // (a C string: the file's ends in its NUL)
        if (const auto colon = name.find(':'); colon != std::string::npos) {
            waypoint = name.substr(colon + 1);
            name.resize(colon);
        }
        const std::string stem = name.substr(0, name.find('.'));
        next = pathOf(stem);
    } else if (voted) {
        // (noxcpp's map vote) Its map; the map cycle moves on as though it had chosen it when it's
        // the cycle's next, else stays where it was.
        if (mapCycleOn_ && !options_.mapCycle.empty()) {
            cycle_.load(options_.mapCycle, [&](const std::string& name) { return mapModesOf(name); });
            MapCycle peek = cycle_;
            if (peek.next(server_.world().gameFlags, mapPath_.stem().string()) == *voted) {
                cycle_ = peek;
            }
        }
        next = pathOf(*voted);
    } else if (mapCycleOn_ && !options_.mapCycle.empty()) {
        // (loadMapCycle again, then getSomeMapName: a map's types
        // from its MapInfo, checkMapFile)
        cycle_.load(options_.mapCycle, [&](const std::string& name) { return mapModesOf(name); });
        if (const auto name = cycle_.next(server_.world().gameFlags, mapPath_.stem().string())) {
            next = pathOf(*name);
        }
    }
    if (options_.prepareMap) {
        options_.prepareMap(next, server_.world()); // (a mode's: its next level written there)
    }
    map::MapFile file;
    try {
        file = map::loadMapFile(next);
    } catch (const std::exception&) {
        next = mapPath_;
        file = map::loadMapFile(next);
    }
    mapPath_ = next;
    mapFile_ = next.filename().string();
    mapCrc_ = file.crc;
    pristineCrc_ = file.crc; // (noxcpp: the map's own, for a solo game's guests)
    // Switch-to-solo: the map's saved copy is what loads
    // (mapFilenameGetSolo), and its CRC is the map's (server_loadMapFile:
    // Map_SetCrc of the file read). The clients are told the map's name with that CRC; a solo
    // game's one client is the host's own, which reads the copy too (gameChangeMap
    // while gameIsNotMultiplayer: HeadlessClient::setSoloMap).
    if (exitTo && server_.world().switchToSolo && !server_.world().soloPath.empty()) {
        file = map::loadMapFile(fs::path(server_.world().soloPath));
        mapCrc_ = file.crc;
    }
    // What was told before the load and not sent yet (Go's teams, SrvNet_SendTeamCreate: in the
    // exe already in the important queue, which the load keeps) goes on ahead of the load's own.
    auto earlier = std::exchange(server_.world().broadcasts(), {});
    server_.switchMap(file, mapFile_, waypoint);
    for (const auto& [peer, held] : refused_) {
        keepHeldOut(peer); // (noxcpp: Options::stockRefusal's, through the switch's outfit and starts)
    }
    server_.world().broadcasts().insert(server_.world().broadcasts().begin(), std::make_move_iterator(earlier.begin()),
                                        std::make_move_iterator(earlier.end()));
    mapLoadedSettings(); // (its mode set)
    settingsChanged_ = true; // (Map_SetCurrentPath: server_gameSettingsUpdated)
    loadRules(); // (the new map's user.rul into the settings' slot 0)
    if (!(server_.world().gameFlags & GameFlag::Solo)) {
        mapSendLoadMap(); // (and, not in a solo game, its .nxz for downloads)
    }
    // What the load tells the players goes through the important queue, and the load tick began
    // with every player's status 0x10 cleared (before): each gets it at
    // its next CLIENT_READY, after what it was owed already (the queue's kept through the load, but
    // for the nodes of MSG_DESTROY_OBJECT, _OUT_OF_SIGHT and _IN_SHADOWS, 0x31-0x33:
    // mapSwitchLevel's). That's the outfit's (its
    // playerMakeDefItems: MSG_PLAYER_RESPAWN, SrvNet_SendReportDrop / Pickup /
    // ModifiablePickup / ItemHealth / Equip, SrvNet_SendResetAbilities: the client's inventory then
    // the new map's), the switch's (the creatures lost, the trades' ends, the teams), the timer's,
    // Obj_SetPosition's MSG_FORGET_DRAWABLES, the scores cleared (MSG_REPORT_LESSON 0x4E,
    // Team_ResetAllScores' MSG_TEAM_MSG 9) and the teams' news (0xC4: players shared out to the
    // flags' teams). Ours go by kind around the load's SrvNet_BroadcastFadeBegin(0, 1): all but
    // 0x4E / 0xC4, the fade, then those; the exe's are in the order made (the outfit, the fade, the
    // rest), no client effect found. What wasn't reliable (the frame's packet) and the
    // load's text, informs and speech, which the exe sends with MSG_USE_MAP before the client
    // loads, ours hold or drop.
    for (auto& [peer, p] : players_) {
        p.ready = false;
        std::erase_if(p.pending, [](const HostedPlayer::Held& h) { return !h.bytes.empty() && h.bytes[0] > 0x30 && h.bytes[0] < 0x34; });
    }
    const auto forward = [&](bool respawns) {
        for (const auto& b : server_.world().broadcasts()) {
            if (b.bytes.empty() || !b.reliable || (b.bytes[0] > 0x30 && b.bytes[0] < 0x34)) {
                continue;
            }
            const bool kept = b.bytes[0] == 0x4E || b.bytes[0] == 0xC4;
            if (respawns == kept) {
                continue;
            }
            for (auto& [peer, p] : players_) {
                if (b.to == p.extent || (b.to == 0 && p.joined)) {
                    p.pending.push_back({b.bytes, false});
                }
            }
        }
    };
    forward(true);
    // (SrvNet_BroadcastFadeBegin(0, 1), Net_QueueImportantSeq(0xFF, .., onlyJoined 1))
    sendImportantToAll(std::vector<std::uint8_t>{0xE4, 0, 1}, true);
    forward(false);
    // Every player the map to load (SrvNet_SendUseMap: Net_Send at once), and from its CLIENT_READY
    // everything afresh; the settings (its mode may have changed) the main loop's
    // (server_gameSettingsUpdated: settingsLoop), held with the rest.
    const std::uint32_t frame = server_.clock().frame();
    syncClasses(); // (a forced class's change told before the map, which its client builds from it)
    const std::vector<std::uint8_t> useMap = useMapBytes(frame);
    for (auto& [peer, p] : players_) {
        transport_.sendReliable(peer, soloGuest(p) ? useMapBytes(frame, true) : useMap);
        p.knows.clear();
        p.aliases = {};
    }
    // Nobody's in the new map till a CLIENT_READY: its start (the tick's end right
    // after this load) waits for one, as the exe's waits for its host's client's ready
    // (gameChangeMap's SrvNet_OnClientReady(0x1F)).
    server_.setPlayersPresent(false);
    server_.world().broadcasts().clear(); // (the old map's news: nobody's to hear)
    server_.world().removed().clear();
}

void GameHost::sendImportant(std::uint8_t peer, std::vector<std::uint8_t> bytes, bool onlyJoined)
{
    // (netImportant returns at once for a player without status 0x10: its
    // messages stay queued, oldest first, till its CLIENT_READY's frame; an onlyJoined one is
    // dropped then for a player not in the game.)
    const auto it = players_.find(peer);
    if (it == players_.end()) {
        return;
    }
    if (!it->second.ready) {
        it->second.pending.push_back({std::move(bytes), onlyJoined});
        return;
    }
    if (!onlyJoined || it->second.joined) {
        transport_.sendReliable(peer, bytes);
    }
}

void GameHost::sendImportantToAll(std::span<const std::uint8_t> bytes, bool onlyJoined, std::optional<std::uint8_t> except)
{
    for (auto& [peer, p] : players_) {
        if (except && peer == *except) {
            continue;
        }
        if (p.ready) {
            if (!onlyJoined || p.joined) {
                transport_.sendReliable(peer, bytes);
            }
        } else if (p.joined) {
            p.pending.push_back({std::vector<std::uint8_t>(bytes.begin(), bytes.end()), onlyJoined});
        }
    }
}

void GameHost::releasePending(HostedPlayer& p)
{
    for (const auto& h : std::exchange(p.pending, {})) {
        if (!h.onlyJoined || p.joined) {
            transport_.sendReliable(p.peer, h.bytes);
        }
    }
}

bool GameHost::isHostPlayer(const HostedPlayer& p) const
{
    // (A solo game's one player is slot 0x1F; a network game's 32nd peer has slot 31 too, which
    // isn't the host's own.)
    if (options_.solo && (p.slot & 31u) == 0x1F) {
        return true;
    }
    const auto at = hostPeerAt_ ? transport_.endpointOf(p.peer) : std::nullopt;
    return at && *at == *hostPeerAt_;
}

int GameHost::latencyOf(std::uint8_t peer) const
{
    const auto it = players_.find(peer);
    if (it == players_.end()) {
        return 0;
    }
    if (!isHostPlayer(it->second)) {
        return transport_.latency(peer); // (its stream's record: player index + 1)
    }
    // The host's own (player 0x1F) has no stream: the latency compensation's value. The least
    // and the average are of the other players' latencies over 0 (none: 0).
    int least = 0;
    int sum = 0;
    int count = 0;
    for (const auto& [q, p] : players_) {
        if (isHostPlayer(p)) {
            continue;
        }
        const int l = transport_.latency(q);
        if (0 < l) {
            least = count == 0 ? l : std::min(least, l);
            sum += l;
            ++count;
        }
    }
    switch (latency_) {
    case 1:
        return least;
    case 2:
        return count != 0 ? sum / count : 0;
    case 3:
        return latencyAmount_;
    default:
        return 0;
    }
}

void GameHost::reportNextLatency()
{
    // The players in Plr_GetFirst / Plr_GetNext's order (by slot); the cursor moves on from the
    // last one reported (back to the first past the end).
    std::vector<const HostedPlayer*> list;
    list.reserve(players_.size());
    for (const auto& [peer, p] : players_) {
        list.push_back(&p);
    }
    if (list.empty()) {
        latencyCursor_.reset();
        return;
    }
    std::ranges::sort(list, {}, &HostedPlayer::slot);
    std::size_t i = 0;
    if (latencyCursor_) {
        const auto after = std::ranges::find_if(list, [&](const HostedPlayer* p) { return *latencyCursor_ < p->slot; });
        i = after == list.end() ? 0 : static_cast<std::size_t>(after - list.begin());
    }
    // From there round the list, the first that's the host's own or has a latency over 0. With
    // neither anywhere (no host's player: a dedicated server) the exe's search never ends
    // (OB-226); ours gives up after one lap and reports nothing.
    for (std::size_t looked = 1; !isHostPlayer(*list[i]) && transport_.latency(list[i]->peer) == 0; ++looked) {
        if (looked == list.size()) {
            latencyCursor_ = list[i]->slot;
            return;
        }
        i = (i + 1) % list.size();
    }
    const HostedPlayer& p = *list[i];
    latencyCursor_ = p.slot;
    // {0xD7, u16 its net code, u16 its latency} to every player (Net_QueueMsg, the host's own too).
    const auto code = static_cast<std::uint16_t>(p.extent);
    const auto ms = static_cast<std::uint16_t>(latencyOf(p.peer));
    const std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(net::Op::ReportAllLatency), static_cast<std::uint8_t>(code),
                                         static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(ms),
                                         static_cast<std::uint8_t>(ms >> 8)};
    for (const auto& [q, h] : players_) {
        transport_.sendUnreliable(q, m);
    }
}

// ------------------------------------------------------------- the settings' records (g_gameSettingsData)

namespace {

// Little-endian fields of a settings record.
using util::u16At;
using util::u32At;

void putU16(GameHost::SettingsRecord& r, std::size_t at, std::uint16_t v)
{
    r[at] = static_cast<std::uint8_t>(v);
    r[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void putU32(GameHost::SettingsRecord& r, std::size_t at, std::uint32_t v)
{
    for (std::size_t i = 0; i < 4; ++i) {
        r[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

/// The record's C string at `at` (to its NUL, or the record's end).
std::string cStringAt(const GameHost::SettingsRecord& r, std::size_t at)
{
    std::string s;
    for (std::size_t i = at; i < r.size() && r[i] != 0; ++i) {
        s += static_cast<char>(r[i]);
    }
    return s;
}

/// strcpy into the record (its NUL too; past the field into the next, as the exe's, but not past
/// the record).
void strcpyAt(GameHost::SettingsRecord& r, std::size_t at, std::string_view s)
{
    std::size_t i = 0;
    for (; i < s.size() && at + i < r.size(); ++i) {
        r[at + i] = static_cast<std::uint8_t>(s[i]);
    }
    if (at + i < r.size()) {
        r[at + i] = 0;
    }
}

/// strncpy(r + at, s, n): `s` up to its NUL, the rest of the n bytes 0.
void strncpyAt(GameHost::SettingsRecord& r, std::size_t at, std::string_view s, std::size_t n)
{
    const std::size_t len = std::min(s.find('\0'), s.size());
    for (std::size_t i = 0; i < n; ++i) {
        r[at + i] = i < len ? static_cast<std::uint8_t>(s[i]) : std::uint8_t{0};
    }
}

/// "%S" of a wide field: its letters as bytes (the C locale's: past 0x7F, '?').
std::string narrow(std::string_view utf8)
{
    std::string out;
    for (const char16_t c : strings::fromUtf8(std::string(utf8))) {
        out += c < 0x80 ? static_cast<char>(c) : '?';
    }
    return out;
}

} // namespace

std::vector<std::uint8_t> GameHost::useMapBytes(std::uint32_t frame, bool guest) const
{
    net::msg::UseMap useMap;
    copyText(useMap.mapName, mapFile_);
    // (noxcpp: a solo game's guest, Options::soloGuests: a visited map's saved copy is the host's
    // own client's to read; a guest is told the map's own file, its own copy of it loaded with no
    // download, and a mode puts right what the saved copy changed: Options::onClientReady.)
    useMap.crc = guest ? pristineCrc_ : mapCrc_;
    useMap.frame = frame;
    return bytesOf(useMap);
}

bool GameHost::soloGuest(const HostedPlayer& p) const
{
    return options_.solo && options_.soloGuests > 0 && (p.slot & 31u) != 0x1F;
}

void GameHost::mapLoadedSettings()
{
    // (server_loadMapFile) Slot 0 refreshed, every spell, weapon and armour on as the map's load
    // left them, and copied to slot 1 but for a chat map; neither under game flag 0x200000.
    if (!(server_.world().gameFlags & GameFlag::SoloOther)) {
        refreshSettingsSlot0(true);
        if (!(server_.world().gameFlags & GameFlag::Chat)) {
            copySettings(0, 1);
        }
    }
}

void GameHost::refreshSettingsSlot0(bool allOn)
{
    // Slot 0 refreshed (the player counts it also keeps are the game list's: refreshServerInfo).
    SettingsRecord& s = settingsSlots_[0];
    const World& world = server_.world();
    // gameFlags_getVal: the host's 1 (server), 2 with a client of its own.
    const std::uint32_t flags = (world.gameFlags & ~3u) | 1u | (options_.hostClient ? 2u : 0u);
    if (((flags ^ u32At(s, 0x34)) & 0xFFF0u) != 0) {  // (a change in bits 4-15)
        putU16(s, 0x34, static_cast<std::uint16_t>(flags));
    }
    const auto mode = static_cast<std::size_t>(modeIndex(u16At(s, 0x34)));
    if (const auto lesson = static_cast<std::uint16_t>(world.lessonLimits[mode]); u16At(s, 0x36) != lesson) {
        putU16(s, 0x36, lesson);
    }
    if (const auto time = static_cast<std::uint8_t>(world.timeLimits[mode]); s[0x38] != time) {
        s[0x38] = time;
    }
    const std::string name = options_.serverName;
    if (cStringAt(s, 9).substr(0, 15) != name) {
        strncpyAt(s, 9, name, 15);
    }
    if (const std::string map = mapPath_.stem().string(); cStringAt(s, 0) != map) {
        strcpyAt(s, 0, map);
    }
    // a spell of a class (flags 0x7000000) not on, its bit off (bit n spell n).
    std::array<std::uint32_t, 5> spells{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    for (std::uint32_t id = 1; id < 0x89 && !allOn; ++id) {
        const things::SpellDef* def = things_.findSpell(id);
        if (def && (def->flags & 0x7000000u) && world.spellDisabled[id]) {
            spells[id >> 5] &= ~(1u << (id & 31u));
        }
    }
    for (std::size_t i = 0; i < 5; ++i) {
        putU32(s, 0x18 + 4 * i, spells[i]);
    }
    // (A server's) a weapon's or armour's thing not allowed, its bit off.
    std::uint32_t weapons = 0xFFFFFFFFu;
    std::uint32_t armor = 0xFFFFFFFFu;
    if (!allOn) {
        for (const auto& i : ruleWeapons()) {
            if (i.bit < (1u << 27) && world.disabledTypes.contains(i.thing)) {
                weapons &= ~i.bit;
            }
        }
        for (const auto& i : ruleArmour()) {
            if (i.bit < (1u << 26) && world.disabledTypes.contains(i.thing)) {
                armor &= ~i.bit;
            }
        }
    }
    putU32(s, 0x2C, weapons);
    putU32(s, 0x30, armor);
}

void GameHost::fillFromWindow(SettingsRecord& a, const SettingsWindow& w)
{
    // The window read into a record.
    strncpyAt(a, 9, narrow(w.name), 15);
    putU16(a, 0x34, w.type);
    for (std::size_t i = 0; i < 5; ++i) {
        putU32(a, 0x18 + 4 * i, w.rules.spells[i]);
    }
    putU32(a, 0x2C, w.rules.weapons);
    putU32(a, 0x30, w.rules.armor);
    if (w.lessonLimit) {
        putU16(a, 0x36, static_cast<std::uint16_t>(*w.lessonLimit));
    }
    if (w.timeLimit) {
        a[0x38] = static_cast<std::uint8_t>(*w.timeLimit);
    }
    a[0x39] = w.newGame ? 1 : 0;
    // The chosen line's first word (strtok on a tab), else none.
    if (w.mapLine) {
        const std::string line = narrow(*w.mapLine);
        const auto from = line.find_first_not_of('\t');
        if (from != std::string::npos) {
            strcpyAt(a, 0, line.substr(from, line.find('\t', from) - from));
            return;
        }
    }
    a[0] = 0;
}

void GameHost::sendGuiSettings(std::uint8_t peer, std::uint8_t slot, const SettingsRecord& r)
{
    std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(net::Op::UpdateGuiGameSettings), slot};
    m.insert(m.end(), r.begin(), r.end());
    sendImportant(peer, std::move(m));
}

void GameHost::settingsWindowOpened(const SettingsWindow& w)
{
    // guiServerOptsLoad (a server's): the window marked changed, slot 0 refreshed, then (after
    // the window's filled from slot 0) slot 0 copied to slot 1, the current slot.
    window_ = w;
    windowChanged_ = true;
    refreshSettingsSlot0();
    copySettings(0, 1);
}

void GameHost::editSettings(const SettingsEdit& e)
{
    SettingsRecord& s = settingsSlots_[1];
    if (e.fill) {
        fillFromWindow(s, *e.fill);
    }
    if (e.name) {
        strcpyAt(s, 9, narrow(*e.name)); // (the field's 15 letters and the NUL: + 0x18's first byte)
        setServerName(*e.name);
    }
    if (e.lessonLimit) {
        putU16(s, 0x36, static_cast<std::uint16_t>(std::max(0, *e.lessonLimit)));
        windowChanged_ = true;
    }
    if (e.timeLimit) {
        s[0x38] = static_cast<std::uint8_t>(std::clamp(*e.timeLimit, 0, 0xFF));
        windowChanged_ = true;
    }
    if (e.type) {
        putU16(s, 0x34, static_cast<std::uint16_t>((u16At(s, 0x34) & 0xE80Fu) | *e.type));
        const auto mode = static_cast<std::size_t>(modeIndex(u16At(s, 0x34)));
        putU16(s, 0x36, static_cast<std::uint16_t>(server_.world().lessonLimits[mode]));
        s[0x38] = static_cast<std::uint8_t>(server_.world().timeLimits[mode]);
        windowChanged_ = true;
    }
    if (e.map) {
        strcpyAt(s, 0, narrow(*e.map));
        windowChanged_ = true;
    }
    if (e.rules) {
        for (std::size_t i = 0; i < 5; ++i) {
            putU32(s, 0x18 + 4 * i, e.rules->spells[i]);
        }
        putU32(s, 0x2C, e.rules->weapons);
        putU32(s, 0x30, e.rules->armor);
    }
    if (e.changed) {
        windowChanged_ = true;
    }
}

void GameHost::settingsWindowGo(const SettingsWindow& w)
{
    // 0x27A1: the window read into slot 1, Game_CopySettingsData(1, 0).
    fillFromWindow(settingsSlots_[1], w);
    copySettings(1, 0);
}

void GameHost::setNetRate(int rate)
{
    // rateUpdate: a new rate told (game flag 0x20000: the server up) to everyone but
    // the host's own (SrvNet_SendRateChange(0x9F): Net_QueueImportantSeq, onlyJoined 1).
    if (rate != netRate_) {
        const std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(net::Op::RateChange), static_cast<std::uint8_t>(rate)};
        for (auto& [peer, p] : players_) {
            if (isHostPlayer(p)) {
                continue;
            }
            if (p.ready) {
                if (p.joined) {
                    transport_.sendReliable(peer, m);
                }
            } else if (p.joined) {
                p.pending.push_back({m, true});
            }
        }
    }
    netRate_ = rate;
}

void GameHost::settingsLoop()
{
    // mainloop, the host's (game flag 1) part.
    if (std::exchange(settingsChanged_, false)) {
        broadcastSettings(); // (server_gameDoSwitchMap: SrvNet_BroadcastGameSettings)
    } else if (server_.world().network() && windowChanged_) {
        // not while the host's own client loads a map (game flags 0x900000).
        const bool hostLoading = std::ranges::any_of(players_, [&](const auto& kv) { return !kv.second.ready && isHostPlayer(kv.second); });
        if (!hostLoading) {
            if (window_) {
                // the window's state (into the stack's uninitialised bytes, here 0),
                // when it isn't what was last told, to every player but the host's own.
                SettingsRecord r{};
                fillFromWindow(r, *window_);
                if (r != guiSent_) {
                    for (const auto& [peer, p] : players_) {
                        if (!isHostPlayer(p)) {
                            sendGuiSettings(peer, 1, r);
                        }
                    }
                    guiSent_ = r;
                }
            }
            windowChanged_ = false;
        }
    }
    // Each minute (from the loop's start): slot 0 refreshed (and WOL's refresh).
    if (server_.world().network() && refreshSettingsAt_ <= server_.clock().frame()) {
        refreshSettingsSlot0();
        refreshSettingsAt_ = server_.clock().frame() + 30u * 60u;
    }
}


// ------------------------------------------------------------------------------- the map vote
// noxcpp's (no stock counterpart: map_vote.hpp).

namespace {

constexpr std::uint32_t kEndVoteFrames = 30u * 20u;    // a match's end waits 20 s for it (else 10)
constexpr std::uint32_t kCalledVoteFrames = 30u * 30u; // a called one stays open 30 s
constexpr std::uint32_t kCallAgainFrames = 30u * 60u * 5u; // one called every 5 minutes, whatever came of it

std::u16string widened(const std::string& s)
{
    return strings::fromUtf8(s);
}

} // namespace

std::vector<std::string> GameHost::mapsForMode()
{
    // (A chat map's: the other chat maps, the So_ social maps.)
    const std::uint32_t mode = server_.world().gameFlags & (GameFlag::KingOfTheRealm | GameFlag::CaptureTheFlag | GameFlag::FlagBall |
                                                            GameFlag::Arena | GameFlag::Elimination | GameFlag::Chat);
    std::vector<std::string> out;
    for (const MapChoice& c : mapChoices()) {
        if (c.modes & mode) {
            out.push_back(c.name);
        }
    }
    return out;
}

std::optional<std::string> GameHost::cycleNextPeek()
{
    if (!mapCycleOn_ || options_.mapCycle.empty()) {
        return std::nullopt;
    }
    cycle_.load(options_.mapCycle, [&](const std::string& name) { return mapModesOf(name); });
    MapCycle peek = cycle_; // (the cycle moves on only when it's used: loadNextMap)
    return peek.next(server_.world().gameFlags, mapPath_.stem().string());
}

void GameHost::announce(const std::u16string& text)
{
    // (As the server's other news to the players: held for one still loading, none for one not
    // in the game yet.)
    const std::vector<std::uint8_t> m = World::textMessage(text, 0);
    for (auto& [peer, p] : players_) {
        sendImportant(peer, m, true);
    }
}

bool GameHost::openVote(MapVote::Kind kind, std::uint8_t caller)
{
    std::vector<std::string> maps = MapVote::pick(mapsForMode(), cycleNextPeek(), mapPath_.stem().string(), voteRng_);
    if (maps.size() < 2) {
        return false;
    }
    const std::uint32_t frame = server_.clock().frame();
    vote_.open(kind, std::move(maps), frame + (kind == MapVote::Kind::EndOfMatch ? kEndVoteFrames : kCalledVoteFrames), caller);
    // In chat for everyone (a stock player votes by the number), then the window for noxsdl's.
    std::u16string head = kind == MapVote::Kind::EndOfMatch
                              ? u"Vote for the next map: say its number."
                              : widened(players_.count(caller) ? players_[caller].name : std::string("A player")) +
                                    u" calls a map vote: say a map's number (more than half changes the map).";
    announce(head);
    for (std::size_t i = 0; i < vote_.candidates().size(); ++i) {
        announce(widened(std::to_string(i + 1) + ": " + vote_.candidates()[i]));
    }
    for (auto& [peer, p] : players_) {
        if (inVoteWindow(peer)) {
            sendVoteTo(peer);
        }
    }
    return true;
}

bool GameHost::inVoteWindow(std::uint8_t peer) const
{
    // The vote's window goes to a noxsdl player (identified) that's in the game (ready); one still
    // loading gets the vote as it stands at its CLIENT_READY (sendVoteTo), the time left with it.
    const auto it = players_.find(peer);
    return it != players_.end() && it->second.ready && transport_.identified(peer);
}

void GameHost::sendVoteTo(std::uint8_t peer)
{
    if (!vote_.active()) {
        return;
    }
    const std::uint32_t frame = server_.clock().frame();
    const std::uint32_t left = vote_.endsAt() > frame ? (vote_.endsAt() - frame + 29) / 30 : 0;
    transport_.sendReliable(peer, mapvote::openMessage(vote_.kind(), static_cast<std::uint16_t>(left), vote_.candidates()));
    transport_.sendReliable(peer, mapvote::tallyMessage(vote_.tally(), players_.size(), vote_.choiceOf(peer)));
}

void GameHost::sendTallies()
{
    const std::vector<int> tally = vote_.tally();
    for (auto& [peer, p] : players_) {
        if (inVoteWindow(peer)) {
            transport_.sendReliable(peer, mapvote::tallyMessage(tally, players_.size(), vote_.choiceOf(peer)));
        }
    }
}

void GameHost::castVote(std::uint8_t peer, int choice)
{
    if (vote_.vote(peer, choice)) {
        sendTallies();
    }
}

void GameHost::callVote(std::uint8_t peer)
{
    World& world = server_.world();
    const auto tell = [&](const std::u16string& text) { sendImportant(peer, World::textMessage(text, 0), true); };
    if (!voteAvailable()) {
        return; // (no vote here: the line was only chat, as the stock server has it)
    }
    if (vote_.active() || (world.gameFlags & GameFlag::GameOver)) {
        tell(u"A map vote is already under way.");
        return;
    }
    if (const std::uint32_t frame = server_.clock().frame(); frame < callAgainAt_) {
        // (One every 5 minutes, from the last one called.)
        const std::uint32_t seconds = (callAgainAt_ - frame + kFrameRate - 1) / kFrameRate;
        const std::string left = std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
        tell(widened("A map vote can be called once every 5 minutes: the next in " + left + "."));
        return;
    }
    if (!openVote(MapVote::Kind::Called, peer)) {
        tell(u"There's no other map for this game's mode.");
        return;
    }
    callAgainAt_ = server_.clock().frame() + kCallAgainFrames;
}

void GameHost::closeVote(std::optional<int> winner)
{
    const MapVote::Kind kind = vote_.kind();
    const std::vector<std::string> maps = vote_.candidates();
    const std::vector<int> tally = vote_.tally();
    vote_.close();
    for (auto& [peer, p] : players_) {
        if (inVoteWindow(peer)) {
            transport_.sendReliable(peer, mapvote::closeMessage(kind, winner));
        }
    }
    if (winner) {
        const std::string& name = maps[static_cast<std::size_t>(*winner)];
        const int votes = tally[static_cast<std::size_t>(*winner)];
        voteNext_ = name;
        announce(widened(std::string(kind == MapVote::Kind::EndOfMatch ? "Next map: " : "The vote changes the map to ") + name + " (" +
                         std::to_string(votes) + (votes == 1 ? " vote)." : " votes).")));
        if (kind == MapVote::Kind::Called) {
            nextMap(); // (now, as the host's "load")
        }
        return;
    }
    if (kind == MapVote::Kind::EndOfMatch) {
        announce(u"No map votes: the map cycle's next.");
    } else {
        announce(u"The map vote failed.");
    }
}

bool GameHost::endVoteHolds()
{
    return (server_.world().gameFlags & GameFlag::GameOver) && vote_.active() && vote_.kind() == MapVote::Kind::EndOfMatch;
}

bool GameHost::voteAvailable()
{
    // (A chat map's vote too: among the other chat maps, mapsForMode.)
    const World& world = server_.world();
    return options_.mapVote && world.network() && !world.quest();
}

bool GameHost::sendToIdentified(std::uint8_t peer, std::vector<std::uint8_t> bytes)
{
    if (!players_.contains(peer) || !transport_.identified(peer)) {
        return false;
    }
    transport_.sendReliable(peer, std::move(bytes));
    return true;
}

bool GameHost::sendToIdentifiedUnreliable(std::uint8_t peer, std::vector<std::uint8_t> bytes)
{
    if (!players_.contains(peer) || !transport_.identified(peer)) {
        return false;
    }
    transport_.sendUnreliable(peer, bytes);
    return true;
}

ServerObject* GameHost::unitOf(std::uint8_t peer)
{
    const auto it = players_.find(peer);
    return it != players_.end() ? server_.world().byExtent(it->second.extent) : nullptr;
}

bool GameHost::isHostPeer(std::uint8_t peer) const
{
    const auto it = players_.find(peer);
    return it != players_.end() && isHostPlayer(it->second);
}

void GameHost::refreshMapFile()
{
    // (noxcpp's map editor only: never in a game the exe could host.) The file as written now:
    // its CRC what USE_MAP tells a joining client, its .nxz what a download sends.
    map::MapFile file = map::loadMapFile(mapPath_);
    mapCrc_ = file.crc;
    mapSendLoadMap();
}

void GameHost::loadMapNamed(const std::string& name, std::optional<std::uint32_t> mode)
{
    if (mode) {
        server_.world().chosenMode = *mode; // (as applyServerSettings keeps it for the next map)
    }
    nextMap(name);
}

std::uint32_t GameHost::extendedFeatures() const
{
    // (A map editor session says so: noxsdl's browser marks it with a pencil. Stock clients never
    // ask: they see the "[EDIT]" name alone.)
    return lobby::ServerFeature::HighRate | options_.features |
           (options_.noxcppMessages.contains(static_cast<std::uint8_t>(net::Op::NoxcppEdit)) ? lobby::ServerFeature::EditSession : 0u) |
           (options_.stockRefusal ? lobby::ServerFeature::NoxCoreOnly : 0u); // (noxsdl's browser marks it NoX Core only)
}

void GameHost::setSandboxNoScoring(bool on)
{
    options_.sandboxNoScoring = on;
    server_.setNoScoring(on);
}

void GameHost::setPlayersCantDie(bool on)
{
    options_.playersCantDie = on;
    server_.setPlayersCantDie(on);
}

void GameHost::sendVoteAvailable(std::uint8_t peer)
{
    transport_.sendReliable(peer, mapvote::availableMessage(voteAvailable()));
}

void GameHost::voteFrame()
{
    World& world = server_.world();
    const std::uint32_t frame = server_.clock().frame();
    // Whether one can be called, to the noxsdl players when it changes (the switch, a quest map).
    if (const bool available = voteAvailable(); available != voteAvailableSent_) {
        voteAvailableSent_ = available;
        for (auto& [peer, p] : players_) {
            if (inVoteWindow(peer)) {
                sendVoteAvailable(peer);
            }
        }
    }
    const bool over = (world.gameFlags & GameFlag::GameOver) != 0;
    if (vote_.active() && vote_.kind() == MapVote::Kind::Called && over) {
        closeVote(std::nullopt); // (the match's own vote takes over)
    }
    if (!over) {
        votedThisMatch_ = false;
    } else if (!votedThisMatch_ && !vote_.active() && options_.mapVote && world.network() && !world.quest() &&
               !(world.gameFlags & GameFlag::Chat)) {
        votedThisMatch_ = true;
        if (openVote(MapVote::Kind::EndOfMatch, 0)) {
            server_.holdGameOver(vote_.endsAt()); // (its wait to the vote's end)
        }
    }
    if (!vote_.active()) {
        return;
    }
    if (vote_.kind() == MapVote::Kind::Called) {
        if (const auto w = vote_.winner(players_.size())) {
            closeVote(w); // (a majority already)
            return;
        }
    }
    if (vote_.endsAt() <= frame) {
        closeVote(vote_.winner(players_.size()));
    }
}

} // namespace nox::server
