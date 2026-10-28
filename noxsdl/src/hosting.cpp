// A game hosted here (app.hpp): its settings from nox.cfg, the server made, its XWIS post, its
// bots, its router port, and a solo game's new character.
#include "app.hpp"

#include "noxcore/bot/bot_squad.hpp"
#include "noxcore/client/solo_menu.hpp"
#include "noxcore/client/solo_saves.hpp"
#include "noxcore/lobby/listing.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace noxsdl {

/// The game we host as XWIS lists it (the exe's WOL record, makeWolRecord), renewed every 2 s
/// (players come and go, the map changes), and the connection moved on. Its player count is
/// people only: the Bots tab's bots aren't counted (by design: the list shows who there is
/// to play with). They're known by the serials they joined with, not by name: anyone can call
/// themselves "[BOT] ...". (Bots from elsewhere, a bot manager's, count as players.)
void postToXwis(App& app)
{
    const std::uint64_t now = SDL_GetTicks();
    if (now >= app.xwisRecordAt) {
        app.xwisRecordAt = now + 2000;
        std::set<std::string> ours;
        if (app.localBots.squad) {
            for (const auto& b : app.localBots.squad->status()) {
                ours.insert(b.serial);
            }
        }
        nox::lobby::GameListing g;
        app.host->withGame([&](nox::server::GameHost& game) {
            // The name, map, mode, limits and the Access settings (a password game's record says
            // so, as GAME.EXE's does).
            g = game.listing();
            // People only: a player whose join serial (NUL-padded) is one of our bots' isn't counted.
            g.players = static_cast<int>(std::ranges::count_if(game.players(), [&](const auto& p) {
                const auto& s = p.second.accept.serial;
                const char* text = reinterpret_cast<const char*>(s.data());
                return !ours.contains(std::string(text, strnlen(text, s.size())));
            }));
        });
        g.port = app.host->port();
        app.xwisHost->setRecord(nox::lobby::makeWolRecord(g));
    }
    app.xwisHost->poll(static_cast<std::uint32_t>(now));
    // What became of the post, logged as it changes (a refused login, a channel taken...).
    if (const bool posted = app.xwisHost->hosting(); posted != app.xwisPosted) {
        app.xwisPosted = posted;
        SDL_Log(posted ? "XWIS: the game is listed (its channel made, its record posted)" : "XWIS: the game is off the list (connecting again)");
    }
    if (app.xwisHost->error() != app.xwisError) {
        app.xwisError = app.xwisHost->error();
        if (!app.xwisError.empty()) {
            SDL_Log("XWIS: the game couldn't be posted: %s (trying again every 30 s)", app.xwisError.c_str());
        }
    }
}

/// The game we host posted to XWIS under the menus' login (GAME.EXE's host's game is: WOLAPI's
/// channel and topic, xwis_host.hpp), from the next frame's postToXwis on.
void startXwisPost(App& app, const nox::io::SavedLogin& login)
{
    app.xwisHost = std::make_unique<nox::lobby::XwisHost>(login.nick, login.password, static_cast<std::uint32_t>(SDL_GetTicks()));
    app.xwisRecordAt = 0;
    app.xwisPosted = false;
    app.xwisError.clear();
}

/// Where a game the menus host is listed (host_listing.hpp): on XWIS with the menus' login, else
/// the LAN only; `listed` false: a local one (a test's, Solo Quest's).
HostListing menusHostListing(const App& app, bool listed)
{
    const auto& login = app.menus ? app.menus->wolLogin() : std::optional<nox::io::SavedLogin>{};
    // Network (LAN play): never posted, the reason being Network's (not --no-xwis, nor an
    // earlier Skip Login's).
    if (app.menus && app.menus->lanOnly()) {
        return hostListing(listed, true, false, {});
    }
    return hostListing(listed, app.online, app.menus && app.menus->skippedLogin(), login ? login->nick : std::string());
}

/// The Bots tab's choice for the game we host: the squad made on first use (joined to our port
/// over the loopback, its brains' item kinds from thing.bin and modifier.bin), then the classes
/// and difficulty wanted (each bot a level from minTier to maxTier).
void setLocalBots(App& app, const std::array<int, 3>& wanted, int minTier, int maxTier)
{
    auto& b = app.localBots;
    b.wanted = wanted;
    b.minTier = std::clamp(minTier, 0, 2);
    b.maxTier = std::clamp(maxTier, b.minTier, 2);
    if (!app.host) {
        return;
    }
    const auto lo = static_cast<nox::bot::Tier>(b.minTier);
    const auto hi = static_cast<nox::bot::Tier>(b.maxTier);
    if (!b.squad) {
        if (wanted[0] + wanted[1] + wanted[2] == 0) {
            return;
        }
        const auto server = nox::net::Endpoint::resolve("127.0.0.1", app.host->port());
        if (!server) {
            return;
        }
        if (!b.kinds) {
            b.kinds = std::make_unique<nox::bot::ItemKinds>(app.things, &app.modifiers, &app.gameData);
        }
        nox::bot::SquadOptions o;
        o.server = *server;
        app.host->withGame([&](nox::server::GameHost& game) {
            o.password = game.options().password;
            o.playerLimit = game.options().maxPlayers;
        });
        o.minTier = lo;
        o.maxTier = hi;
        o.nagHost = false; // (we're the host)
        o.mapsDir = app.gameDir / "maps";
        std::error_code ec;
        o.mapCache = app.mapCache.empty() ? fs::temp_directory_path(ec) / "noxsdl-botmaps" : app.mapCache;
        b.squad = std::make_unique<LocalBots>(o, app.things, *b.kinds);
        SDL_Log("bots: for our game at %s", server->toString().c_str());
    }
    std::vector<std::uint8_t> classes;
    // Each class repeated as many times as it's wanted (0 warrior, 1 wizard, 2 conjurer).
    for (std::uint8_t c = 0; c < 3; ++c) {
        classes.insert(classes.end(), static_cast<std::size_t>(std::max(wanted[c], 0)), c);
    }
    // Handed to the bots' thread, which applies it there.
    b.squad->post([lo, hi, classes](nox::bot::BotSquad& s, std::uint32_t now) {
        if (s.minTier() != lo || s.maxTier() != hi) {
            s.setTiers(lo, hi, now);
        }
        s.setWanted(classes, now);
    });
}

/// What the Bots tab shows.
ServerOptions::BotsView localBotsView(App& app)
{
    ServerOptions::BotsView v;
    const auto& b = app.localBots;
    v.wanted = b.wanted;
    v.minTier = b.minTier;
    v.maxTier = b.maxTier;
    const int limit = app.host ? app.host->withGame([](nox::server::GameHost& game) { return game.options().maxPlayers; }) : 16;
    v.most = std::max(0, limit - nox::bot::SquadOptions::kSeatsKept);
    if (b.squad) {
        for (const auto& s : b.squad->status()) { // (the bots' thread's last snapshot)
            if (s.state == "left" || s.state == "failed") {
                continue;
            }
            if (s.state == "playing" || s.state == "dead" || s.state == "watching") {
                ++v.in;
            }
            v.lines.push_back(s.name + "\t" + s.state + "  " + std::to_string(s.kills) + "-" + std::to_string(s.deaths));
        }
    }
    return v;
}

/// A game hosted on our own server thread with `settings` (LocalHost; throws as it does).
std::unique_ptr<LocalHost> makeHost(App& app, const HostSettings& settings)
{
    // (Every game this program hosts, solo, quest and editor sessions too: Options' Exact Original
    // Maths, noxcore.cfg's ExactMaths.)
    HostSettings s = settings;
    s.exactMaths = app.exactMaths;
    return std::make_unique<LocalHost>(app.things, &app.modifiers, app.gameData,
                                       app.strings.get(), app.gameDir, s);
}

/// The Host button's game as the exe hosts it, from nox.cfg's server settings (as the exe reads them):
/// ServerName, MaxPlayers, SysopPassword, LessonLimit and TimeLimit (by mode:
/// arena, elimination, CTF, flagball, KotR, lessons at most 999),
/// SendMessageOfTheDay, MapCycle, ItemRespawn, CamperAlarm.
void hostSettingsFromConfig(const App& app, HostSettings& s)
{
    s.updateRate = app.serverUpdateRate; // (noxcore.cfg's, not nox.cfg's)
    s.mapVote = app.serverMapVote;
    if (!app.input) {
        return;
    }
    const auto& c = app.input->config();
    const auto number = [&](const char* key) -> std::optional<int> {
        if (const auto v = c.setting(key); v && !v->empty()) {
            return std::atoi(v->c_str());
        }
        return std::nullopt;
    };
    if (const auto v = c.setting("ServerName"); v && !v->empty()) {
        s.name = v->substr(0, 15);
    }
    if (const auto v = number("MaxPlayers"); v && *v > 0) {
        s.maxPlayers = std::clamp(*v, 1, 32);
    }
    if (const auto v = c.setting("SysopPassword")) {
        s.sysopPassword = *v;
    }
    s.sendMotd = number("SendMessageOfTheDay").value_or(0) != 0;
    s.mapCycle = number("MapCycle").value_or(0) != 0;
    s.itemRespawn = number("ItemRespawn").value_or(0) != 0;
    s.camperAlarm = number("CamperAlarm").value_or(0) != 0;
    static constexpr std::array<int, 5> kIndex = {0, 1, 2, 4, 3}; // (the file's order to modes.hpp's)
    const auto list = [&](const char* key) {
        std::vector<int> out;
        if (const auto v = c.setting(key)) {
            std::stringstream in(*v);
            for (std::string part; std::getline(in, part, ',');) {
                out.push_back(std::atoi(part.c_str()));
            }
        }
        return out;
    };
    if (const auto l = list("LessonLimit"); !l.empty()) {
        std::array<std::uint16_t, 5> a{};
        for (std::size_t i = 0; i < l.size() && i < 5; ++i) {
            a[static_cast<std::size_t>(kIndex[i])] = static_cast<std::uint16_t>(std::clamp(l[i], 0, 999));
        }
        s.lessonLimits = a;
    }
    if (const auto l = list("TimeLimit"); !l.empty()) {
        std::array<std::uint8_t, 5> a{};
        for (std::size_t i = 0; i < l.size() && i < 5; ++i) {
            a[static_cast<std::size_t>(kIndex[i])] = static_cast<std::uint8_t>(std::clamp(l[i], 0, 255));
        }
        s.timeLimits = a;
    }
}

/// The game we host's port on the router (UPnP): mapped as it starts (a network game's, not a
/// solo one's), the outcome logged; not mapped, a reminder to forward it once we're in.
void startPortMapping(App& app, bool network)
{
    PortMapper::retire(std::move(app.portMapper));
    app.portReported = false;
    if (app.host && network && !app.hostLoopback) { // (a loopback-only test host has nothing to open)
        app.portMapper = std::make_unique<PortMapper>(app.host->port());
    }
}

/// Each frame: once the UPnP mapping is done, its outcome logged; if it failed, a centred reminder once
/// we're in the game.
void tickPortMapping(App& app)
{
    if (!app.host) {
        PortMapper::retire(std::move(app.portMapper));
        return;
    }
    if (!app.portMapper || app.portReported || app.portMapper->state() == PortMapper::State::Working) {
        return;
    }
    const bool in = app.live && app.live->client().stage() == nox::client::HeadlessClient::Stage::InGame && app.autoDone;
    if (app.portMapper->state() == PortMapper::State::Mapped) {
        SDL_Log("UPnP: %s", app.portMapper->detail().c_str());
        app.portReported = true;
    } else if (in) {
        const std::string port = std::to_string(app.host->port());
        SDL_Log("UPnP: port %s not mapped: %s", port.c_str(), app.portMapper->detail().c_str());
        centredMessage(app, "Players outside your network may not be able to join: forward UDP port " + port +
                                " on your router to this computer (UPnP: " + app.portMapper->detail() + ").");
        app.portReported = true;
    }
}

/// The bots of the game we host: their log, each frame (they run on their own thread); gone with
/// the game.
void tickLocalBots(App& app)
{
    auto& b = app.localBots;
    if (!app.host) {
        b.squad.reset();
        b.wanted = {};
        return;
    }
    if (b.squad) {
        for (const auto& line : b.squad->takeLog()) {
            SDL_Log("bots: %s", line.c_str());
        }
    }
}

/// The music section a new solo character's file gets (plr_rwMusic's write out of the
/// network: 1, the track playing and the music stack).
nox::player::Music soloMusic(App& app)
{
    nox::player::Music music;
    music.version = 11;
    music.solo = 1;
    if (app.audio) {
        const auto c = app.audio->musicCurrent();
        music.current = {c.extra, c.position, c.track, c.volume};
        for (int i = 0; i < app.audio->musicStackSize(); ++i) {
            const auto e = app.audio->musicStackEntry(i);
            music.stack.push_back({e.extra, e.position, e.track, e.volume});
        }
    }
    return music;
}

/// --solo's character: made as the class and looks screens' Accept makes a solo one (its
/// solo branch: Save\WORKING\Player.plr with the client's sections, its bars empty), from the
/// command line's name, class and colours; map the map the game starts on. Held as the client's
/// and its file the character's.
void newSoloCharacter(App& app, const nox::client::PlayerProfile& profile, const std::string& map)
{
    nox::player::FileInfo held;
    held.flags = 0;
    held.name = nox::strings::fromUtf8(profile.name);
    held.characterClass = static_cast<nox::player::CharacterClass>(profile.playerClass);
    for (std::size_t i = 0; i < 5; ++i) {
        held.colors[i] = {profile.colors[i * 3], profile.colors[i * 3 + 1], profile.colors[i * 3 + 2]};
        held.clothing[i] = profile.clothing[i];
    }
    const auto plr = nox::client::solo_menu::writeNewCharacter(app.saveRoot, held, nox::client::SpellBars{}.toGui(held.characterClass),
                                                               soloMusic(app), fs::path(map).stem().string(),
                                                               nox::client::solo_saves::localTime());
    if (plr.empty()) {
        SDL_Log("solo: the new character couldn't be written under %s", app.saveRoot.string().c_str());
        return;
    }
    SDL_Log("solo: a new character, %s", plr.string().c_str());
    app.characterFile = plr;
    app.heldFileInfo = nox::client::solo_saves::readPlr(plr).info;
}

/// Each frame: the game we host's calls into our client that are due (it runs on its own thread:
/// LocalHost), what it printed, and its XWIS post.
void tickHost(App& app)
{
    if (!app.host) {
        return;
    }
    // The game we host runs on its own thread (its own clock, whatever our screen is
    // doing: LocalHost); here the calls into our client it queued that are due, then what
    // it printed.
    app.host->beginFrame();
    if (const auto error = app.host->takeError()) {
        SDL_Log("hosting: %s", error->c_str());
    }
    try {
        for (const auto& line : app.host->takeConsole()) {
            SDL_Log("host: %s", line.c_str()); // (the map downloads' lines too, for the log)
            if (app.console) {
                app.console->print(line, White);
            }
        }
        if (app.xwisHost) {
            postToXwis(app);
        }
    } catch (const std::exception& ex) {
        SDL_Log("hosting: %s", ex.what());
    }
}

} // namespace noxsdl
