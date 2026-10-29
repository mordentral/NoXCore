// The game's windows made at start-up (app.hpp): Options and the key bindings, the console and
// chat, the in-game menu, the host's Server Settings, the message of the day, the lag window, the
// scoreboard, the map vote, the modes' own UIs (mode_ui.hpp), the map download.
#include "app.hpp"

#include <string>
#include <vector>

namespace noxsdl {

namespace {

/// The Server Settings' hands on the game we host (and on nox.cfg and noxcore.cfg for what they keep).
ServerOptions::Callbacks serverOptionsCallbacks(App* app)
{
    const auto onHost = [app](auto fn, auto orElse) { return app->host ? app->host->withGame(fn) : orElse; };
    const auto inHost = [app](auto fn) {
        if (app->host) {
            app->host->withGame(fn);
        }
    };
    ServerOptions::Callbacks serverCallbacks;
    serverCallbacks.current = [onHost] {
        return onHost([](nox::server::GameHost& g) { return g.serverSettings(); }, ServerOptions::Settings{});
    };
    serverCallbacks.maps = [onHost] {
        return onHost([](nox::server::GameHost& g) { return g.mapChoices(); }, std::vector<ServerOptions::MapChoice>{});
    };
    serverCallbacks.limits = [onHost](std::uint32_t mode) {
        return onHost([mode](nox::server::GameHost& g) { return g.limitsFor(mode); }, std::pair<int, int>{});
    };
    serverCallbacks.defaults = [onHost](std::uint32_t mode) {
        return onHost([mode](nox::server::GameHost& g) { return g.defaultLimits(mode); }, std::pair<int, int>{});
    };
    serverCallbacks.apply = [app](const ServerOptions::Settings& s) {
        if (app->host) {
            // (on the server's side, before its next tick)
            app->host->post([s](nox::server::GameHost& g) { g.applyServerSettings(s); });
        }
    };
    // (no world map to pick one from when hosting yet: So_Druid, as Solo Quest's click)
    serverCallbacks.chatMap = [] { return std::string("So_Druid"); };
    // The settings' slot 1 and the window as the other players are told it (MSG_UPDATE_GUI_GAME_SETTINGS):
    // at once.
    serverCallbacks.windowOpened = [inHost](const ServerOptions::SettingsWindow& w) {
        inHost([&w](nox::server::GameHost& g) { g.settingsWindowOpened(w); });
    };
    serverCallbacks.windowState = [inHost](const ServerOptions::SettingsWindow& w) {
        inHost([&w](nox::server::GameHost& g) { g.settingsWindowState(w); });
    };
    serverCallbacks.windowClosed = [inHost] { inHost([](nox::server::GameHost& g) { g.settingsWindowClosed(); }); };
    serverCallbacks.editSettings = [inHost](const ServerOptions::SettingsEdit& e) {
        inHost([&e](nox::server::GameHost& g) { g.editSettings(e); });
    };
    serverCallbacks.windowGo = [inHost](const ServerOptions::SettingsWindow& w) {
        inHost([&w](nox::server::GameHost& g) { g.settingsWindowGo(w); });
    };
    serverCallbacks.setName = [app](const std::string& name) {
        if (app->host) {
            app->host->post([name](nox::server::GameHost& g) { g.setServerName(name); });
        }
    };
    serverCallbacks.switches = [onHost] {
        return onHost([](nox::server::GameHost& g) { return g.hostSwitches(); }, ServerOptions::Switches{});
    };
    serverCallbacks.setSwitches = [app](const ServerOptions::Switches& s) {
        if (!app->host) {
            return;
        }
        const std::uint32_t options = app->host->withGame([&s](nox::server::GameHost& g) {
            g.setHostSwitches(s);
            return g.options().serverOptions; // (the flags themselves)
        });
        if (hostedCampaign(*app)) {
            return; // (a campaign's session: the files keep the normal games' settings)
        }
        // (nox.cfg keeps them: its writer's server part)
        if (app->input) {
            auto c = app->input->config();
            c.set("SendMessageOfTheDay", s.sendMotd ? "1" : "0");
            c.set("MapCycle", s.mapCycle ? "1" : "0");
            c.set("ItemRespawn", (options & 2u) ? "1" : "0");
            c.set("CamperAlarm", (options & 0x2000u) ? "1" : "0");
            storeConfig(*app, c);
        }
        // (noxcore.cfg keeps the update rate: noxcpp's, not the exe's)
        if (s.updateRate != app->serverUpdateRate || s.mapVote != app->serverMapVote) {
            app->serverUpdateRate = s.updateRate;
            app->serverMapVote = s.mapVote;
            saveExtras(*app, currentExtras(*app));
        }
    };
    serverCallbacks.teams = [onHost] {
        return onHost([](nox::server::GameHost& g) { return g.teamsView(); }, ServerOptions::TeamsView{});
    };
    serverCallbacks.setTeamOption = [inHost](int option, bool on) {
        inHost([option, on](nox::server::GameHost& g) {
            option == 0 ? g.setUseTeams(on) : option == 1 ? g.setAutoAssign(on) : g.setTeamDamage(on);
        });
    };
    serverCallbacks.shuffleTeams = [inHost] { inHost([](nox::server::GameHost& g) { g.shuffleTeams(); }); };
    serverCallbacks.assignTeam = [inHost](const std::vector<std::string>& players, std::uint8_t team) {
        inHost([&players, team](nox::server::GameHost& g) { g.assignToTeam(players, team); });
    };
    serverCallbacks.joinTeam = [app, inHost](std::uint8_t team) {
        if (app->live) {
            const std::uint32_t me = app->live->client().netCode();
            inHost([me, team](nox::server::GameHost& g) { g.joinTeam(me, team); });
        }
    };
    serverCallbacks.renameTeam = [inHost](std::uint8_t team, const std::string& name) {
        inHost([team, &name](nox::server::GameHost& g) { g.renameTeam(team, name); });
    };
    serverCallbacks.access = [onHost] {
        return onHost([](nox::server::GameHost& g) { return g.accessSettings(); }, ServerOptions::AccessSettings{});
    };
    serverCallbacks.setAccess = [app](const ServerOptions::AccessSettings& a) {
        if (!app->host) {
            return;
        }
        const int maxPlayers = app->host->withGame([&a](nox::server::GameHost& g) {
            g.setAccessSettings(a);
            return g.options().maxPlayers;
        });
        if (hostedCampaign(*app)) {
            return; // (a campaign's session: nox.cfg keeps the normal games' limit and password)
        }
        // (nox.cfg keeps the player limit and the sysop's password)
        if (app->input) {
            auto c = app->input->config();
            c.set("MaxPlayers", std::to_string(maxPlayers));
            c.set("SysopPassword", a.sysopPassword);
            storeConfig(*app, c);
        }
    };
    serverCallbacks.banned = [onHost] {
        return onHost(
            [](nox::server::GameHost& g) {
                std::vector<std::string> names;
                for (const auto& [name, timed] : g.bannedNames()) {
                    names.push_back(name);
                }
                return names;
            },
            std::vector<std::string>{});
    };
    serverCallbacks.addAccessName = [inHost](const std::string& name, bool allowedList) {
        inHost([&name, allowedList](nox::server::GameHost& g) { g.addAccessName(name, allowedList); });
    };
    serverCallbacks.removeAccessName = [inHost](std::size_t index, bool allowedList) {
        inHost([index, allowedList](nox::server::GameHost& g) { g.removeAccessName(index, allowedList); });
    };
    serverCallbacks.kickPlayers = [app, inHost](const std::vector<std::string>& players, bool ban) {
        if (app->live) {
            const std::uint32_t me = app->live->client().netCode();
            inHost([&players, me, ban](nox::server::GameHost& g) { g.kickPlayers(players, me, ban); });
        }
    };
    serverCallbacks.ownNetCode = [app] { return app->live ? app->live->client().netCode() : 0u; };
    serverCallbacks.rulesFor = [onHost](const std::string& map, std::uint32_t mode, const std::optional<std::string>& file) {
        return onHost([&](nox::server::GameHost& g) { return g.rulesFor(map, mode, file); }, nox::server::RuleSet{});
    };
    serverCallbacks.internetRules = [onHost]() -> std::optional<nox::server::RuleSet> {
        return onHost([](nox::server::GameHost& g) { return g.internetRules(); }, std::optional<nox::server::RuleSet>{});
    };
    serverCallbacks.ruleSets = [onHost](const std::string& map) {
        return onHost([&map](nox::server::GameHost& g) { return g.ruleSetsOf(map); }, std::vector<std::string>{});
    };
    serverCallbacks.saveRuleSet = [onHost](const std::string& map, const std::string& name, const nox::server::RuleSet& rules,
                                           std::uint32_t mode) {
        return onHost([&](nox::server::GameHost& g) { return g.saveRuleSet(map, name, rules, mode); }, false);
    };
    serverCallbacks.deleteRuleSet = [inHost](const std::string& map, const std::string& name) {
        inHost([&](nox::server::GameHost& g) { g.deleteRuleSet(map, name); });
    };
    serverCallbacks.campaign = [app] { return hostedCampaign(*app); };
    serverCallbacks.things = &app->things;
    serverCallbacks.bots = [app] { return localBotsView(*app); };
    serverCallbacks.setBots = [app](const std::array<int, 3>& wanted, int minTier, int maxTier) {
        setLocalBots(*app, wanted, minTier, maxTier);
    };
    return serverCallbacks;
}

} // namespace

/// The game's windows made, their callbacks on pp (SDL_AppInit, once the data and font are in).
void makeGameWindows(App* app, const gui::Resources& res)
{
    const std::filesystem::path& gameDir = app->gameDir;
    // The game's Options window, from the main menu or the in-game menu.
    OptionsWindow::Callbacks optionCallbacks;
    optionCallbacks.onChange = [app](const nox::config::Config& c) { applySettings(*app, c); };
    optionCallbacks.onClose = [app](const nox::config::Config& c) {
        applySettings(*app, c);
        saveSettings(*app);
    };
    optionCallbacks.onSound = [app](const OptionsWindow::SoundOn& on) {
        if (app->audio) {
            app->audio->setSwitches({on[0], on[1], on[2]});
        }
    };
    optionCallbacks.onExtras = [app](const OptionsWindow::Extras& e) {
        applyExtras(*app, e);
        saveExtras(*app, e);
    };
    optionCallbacks.onInputConfig = [app] {
        if (app->inputConfig && app->input) {
            app->inputConfig->open(app->input->config());
        }
    };
    app->options = std::make_unique<OptionsWindow>(res, gameDir / "window", std::move(optionCallbacks));
    // The input configuration (InputCfg.wnd): Back sets the bindings and writes nox.cfg;
    // either way Options comes back.
    InputConfigWindow::Callbacks inputCallbacks;
    inputCallbacks.onSave = [app](const nox::config::Config& c) {
        if (app->input) {
            app->input->setConfig(c);
            saveSettings(*app);
        }
    };
    inputCallbacks.onClose = [app] { openOptions(*app); };
    app->inputConfig = std::make_unique<InputConfigWindow>(res, gameDir, std::move(inputCallbacks));

    // The console (F1), with the game's commands and noxsdl's debug switches.
    app->console = std::make_unique<Console>(app->font.get(), app->strings.get());
    app->console->setCommands(consoleCommands(*app));
    // Chat: the line goes to the server (MSG_TEXT_MESSAGE), which sends it back as a bubble.
    app->chat = std::make_unique<ChatEntry>(res, gameDir / "window", [app](const std::string& text, bool team) { sendChat(*app, text, team); });

    QuitMenu::Callbacks quitCallbacks;
    quitCallbacks.options = [app] { openOptions(*app); };
    quitCallbacks.observe = [app] {
        if (app->live) {
            app->live->client().observe();
        }
    };
    quitCallbacks.quit = [app] { clientQuit(*app); };
    quitCallbacks.solo = [app] { return app->live && soloGame(*app); };
    quitCallbacks.refused = [app] {
        // The open's refusals: a solo game's player dying or dead; an award flying; paused.
        const int state = ownAnimState(*app);
        if (app->live && soloGame(*app) && (state == 2 || state == 1 || state == 0x33)) {
            return true;
        }
        return app->flight.has_value() || (app->host && app->host->paused());
    };
    quitCallbacks.pause = [app](int how) {
        if (app->host) {
            app->host->pause(how);
        }
    };
    quitCallbacks.deadFlag = [app] { return ownDeadFlag(*app); };
    quitCallbacks.autoLoad = [app] {
        if (app->cinematics) {
            app->cinematics->autoLoadFromMenu();
        }
    };
    quitCallbacks.autoSave = [app] {
        if (app->host) {
            app->host->requestSave("AUTOSAVE");
        }
    };
    quitCallbacks.save = [app] {
        if (app->cinematics) {
            app->cinematics->openSaveMenu();
        }
    };
    // (a quest's own settings, not done yet: off there)
    quitCallbacks.hosting = [app] {
        return app->host && !app->host->directed() && app->live && !(app->live->client().world().game().gameFlags & 0x1000);
    };
    quitCallbacks.gameplay = [app] {
        if (app->host && app->serverOptions) {
            app->serverOptions->toggle();
        }
    };
    quitCallbacks.campaignHost = [app] { return app->live && hostedCampaign(*app).has_value(); };
    quitCallbacks.canVoteMap = [app] { return app->live && app->live->client().mapVote().available; };
    quitCallbacks.voteMap = [app] {
        if (app->live) {
            app->live->client().callMapVote();
        }
    };
    app->quitMenu = std::make_unique<QuitMenu>(res, gameDir / "window", std::move(quitCallbacks));

    // The host's Server Settings (servopts.wnd) on the game we host: each at once with the
    // server's lock held (LocalHost::withGame), so what one sets the next reads.
    ServerOptions::Callbacks serverCallbacks = serverOptionsCallbacks(app);
    app->serverOptions = std::make_unique<ServerOptions>(res, gameDir / "window", std::move(serverCallbacks));
    app->motdWindow = std::make_unique<MotdWindow>(res, gameDir / "window");
    app->discon = std::make_unique<DisconWindow>(
        res, gameDir / "window",
        [app] {
            if (app->live) {
                app->live->client().lagWait(); // (Wait: another 20 s)
            }
        },
        [app] {
            if (app->live) {
                app->live->client().lagExit(); // (Exit: the game left, then the Timeout notice)
            }
        });
    app->rankBoard = std::make_unique<RankBoard>(app->font.get(), app->strings.get());
    app->mapPictures = std::make_unique<MapThumbnails>(gameDir, app->mapCache, app->things);
    app->mapVote = std::make_unique<MapVoteWindow>(*app->mapPictures, app->font.get());
    makeModeUis(*app); // (each mode's own windows: mode_ui.hpp)
    // The map download screen; Cancel gives up the game.
    app->mapDownload = std::make_unique<MapDownload>(res, gameDir / "window", [app] {
        if (app->live) {
            app->live->quit();
        }
    });
}

} // namespace noxsdl
