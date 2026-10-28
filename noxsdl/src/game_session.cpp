// A game joined (app.hpp): the live session made and its hooks, the game's windows for it (the
// shop and dialog screens, the cinematics, the book), its chat and events, and the messages
// and saves the server sends.
#include "app.hpp"
#include "new_game.hpp"

#include "noxcore/client/inform_text.hpp"
#include "noxcore/client/solo_saves.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/version.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <utility>

namespace fs = std::filesystem;

namespace noxsdl {

/// A chat line into the game, as the chat box sends it (MSG_TEXT_MESSAGE; `team`: to our team
/// only): the HUD's chat box and the map editor's Chat panel alike. A map editor session's own
/// chat commands are never sent as chat: "/edit [reason]" asks the host for edit rights, the host's
/// "/revoke NAME" takes them away.
void sendChat(App& app, const std::string& typed, bool team)
{
    // Leading spaces skipped and nothing sent for a blank line, as the game's
    // chat box does (CliNet_SendChatText): the editor's Chat panel sent its box as typed.
    const auto start = typed.find_first_not_of(' ');
    if (start == std::string::npos) {
        return;
    }
    const std::string text = typed.substr(start);
    if (app.editSession && app.editSession->inSession() && (text == "/edit" || text.starts_with("/edit ") || text.starts_with("/revoke "))) {
        if (text.starts_with("/revoke ")) {
            const std::string name = text.substr(8);
            for (const auto& r : app.editSession->roster()) {
                if (r.name == name) app.editSession->revoke(r.peer);
            }
        } else {
            app.editSession->requestEdit(text.size() > 6 ? text.substr(6) : std::string());
            if (app.console) app.console->print("Asked the host for edit rights.", Yellow);
        }
        return;
    }
    if (app.live) {
        app.live->client().say(text, team);
    }
}

namespace {

/// What happens in the game, in the console too: chat, players coming and going, notices.
void consoleEvent(App& app, const nox::client::ClientEvent& e)
{
    using Kind = nox::client::ClientEvent::Kind;
    if (e.kind == Kind::PlayerLeft) {
        app.muted.erase(e.netCode); // (the player's record goes, and its status with it)
    }
    if (e.kind == Kind::Kill) {
        // The kill feed's record (the console gets the notice that comes with it).
        App::KillLine k{e.attacker, e.assist, e.netCode, e.weapon, e.killKind,
                        gameFrame(app)};
        if (k.kind == 1) {
            // A bolt or an arrow shows as the bow that shot it.
            const auto& db = app.things;
            const auto nameOf = [&](std::uint16_t type) {
                return type >= 1 && type <= db.things.size() ? db.things[type - 1u].name : std::string();
            };
            if (nameOf(k.weapon) == "ArcherBolt") {
                k.weapon = typeIdOf(db, "CrossBow");
            } else if (nameOf(k.weapon) == "ArcherArrow") {
                k.weapon = typeIdOf(db, "Bow");
            }
        }
        app.kills.push_back(k);
        while (app.kills.size() > 100) {  // keep the last 100
            app.kills.pop_front();
        }
    }
    // A map editor session's Chat panel: the game's chat as the game shows it here (team lines only
    // to the team, as the host relays them; a muted player's left out, as the console leaves them),
    // players coming and going in the game's words.
    if (app.editSession && app.editSession->inSession() && !(e.kind == Kind::Chat && app.muted.contains(e.netCode))) {
        if (auto line = editor::chatLineFrom(e)) {
            line->text = resolveStringId(app, line->text);
            if (e.kind == Kind::PlayerJoined) {
                line->text = nox::client::inform::format(gameString(app, "cdecode.c:PlayerJoined"), {e.name});
            } else if (e.kind == Kind::PlayerLeft) {
                line->text = e.name.empty() ? gameString(app, "cdecode.c:UnknownLeft")
                                            : nox::client::inform::format(gameString(app, "cdecode.c:PlayerLeft"), {e.name});
            }
            if (app.editor && app.editor->inSession()) {
                app.editor->addChat(std::move(*line));
            } else {
                // (Kept till the editor attaches: a joining editor's first lines, or ours while
                // the game is in front before the editor was ever opened.)
                app.sessionChat.push_back(std::move(*line));
                if (app.sessionChat.size() > 500) app.sessionChat.pop_front();
            }
        }
    }
    if (!app.console) {
        return;
    }
    switch (e.kind) {
    case Kind::Chat:
        if (e.netCode == 0 && !(e.textFlags & 0x10)) {
            // The server's own line (CliNet_OnPacket 0xA8, net code 0): in the middle of the screen.
            centredMessage(app, chatLine(app, e.text, e.team));
            break;
        }
        if (!app.muted.contains(e.netCode)) {
            app.console->print((e.name.empty() ? std::string() : e.name + ": ") + chatLine(app, e.text, e.team), White);
        }
        break;
    case Kind::PlayerJoined:
        // MSG_PLAYER_JOIN (CliNet_OnPacket 0x2D): cdecode.c:PlayerJoined, centred (the client's
        // new players here; the exe's every 0x2D).
        centredMessage(app, nox::client::inform::format(gameString(app, "cdecode.c:PlayerJoined"), {e.name}));
        break;
    case Kind::PlayerLeft:
        // MSG_PLAYER_QUIT (0x2E): cdecode.c:PlayerLeft, or UnknownLeft for a player it didn't know.
        centredMessage(app, e.name.empty() ? gameString(app, "cdecode.c:UnknownLeft")
                                           : nox::client::inform::format(gameString(app, "cdecode.c:PlayerLeft"), {e.name}));
        break;
    case Kind::Notice:
        if (e.informSub != 0xFF && e.informSub != 0x0E) {
            break; // (CliNet_OnInform's: shown centred from the message itself, onInform)
        }
        if (e.text == "can't use that as a secondary weapon") {
            centredMessage(app, gameString(app, "GuiInv.c:Weapon2CantUse")); // (Gui_Inv_SetSecondaryWeapon)
            break;
        }
        app.console->print(resolveStringId(app, e.text), Yellow);
        break;
    case Kind::Disconnected:
        app.console->print(e.text, Red);
        break;
    default:
        break;
    }
}
/// The shop, dialog and trade windows for a new game, sending through the client.
void startTradeScreens(App& app)
{
    if (!app.live || !app.inventory || !app.itemNames) {
        return;
    }
    auto& client = app.live->client();
    const gui::Resources res = guiResources(app);
    TradeScreens::Actions a;
    a.shopExit = [&client] { client.shopExit(); };
    a.buy = [&client](std::uint16_t nc) { client.shopBuy(nc); };
    a.buyStack = [&client](std::uint16_t type, std::uint8_t n) { client.shopBuyStack(type, n); };
    a.sell = [&client](std::uint16_t nc) { client.shopSell(nc); };
    a.sellStack = [&client](std::uint16_t type, std::uint8_t n) { client.shopSellStack(type, n); };
    a.repair = [&client](std::uint16_t nc) { client.shopRepair(nc); };
    a.askSellPrice = [&client](std::uint16_t nc) { client.shopAskSellPrice(nc); };
    a.askRepairCost = [&client](std::uint16_t nc) { client.shopAskRepairCost(nc); };
    a.tradeAccept = [&client] { client.tradeAccept(); };
    a.tradeCancel = [&client] { client.tradeCancel(); };
    a.tradeWithdraw = [&client](std::uint16_t nc) { client.tradeWithdraw(nc); };
    a.dialogAnswer = [&client](std::uint8_t answer) { client.dialogAnswer(answer); };
    a.askBook = [&client](std::uint16_t nc, std::uint8_t kind) { client.askBook(nc, kind); };
    a.spoken = [&app](const std::string& id) -> std::string {
        // The variant being spoken (speech() picked it), else the first.
        if (!app.strings) {
            return id;
        }
        const auto* entry = app.strings->find(id);
        if (!entry || entry->variants.empty()) {
            const auto t = app.strings->text(id);
            return t ? nox::strings::toUtf8(*t) : id;
        }
        const bool speaking = app.speaking.size() > id.size() && app.speaking.ends_with(":" + id);
        const std::size_t v = speaking && app.speakingVariant < entry->variants.size() ? app.speakingVariant : 0;
        return nox::strings::toUtf8(entry->variants[v].text);
    };
    a.replay = [&app] {
        // Gui_NpcDialog_WndProc's Replay: the same line again.
        if (!app.audio || !app.strings || app.speaking.empty()) {
            return;
        }
        const auto* entry = app.strings->find(app.speaking.substr(app.speaking.find(':') + 1));
        if (entry && app.speakingVariant < entry->variants.size() && !entry->variants[app.speakingVariant].extra.empty()) {
            app.audio->dialog(entry->variants[app.speakingVariant].extra);
        }
    };
    a.canTake = [&app](std::uint16_t type, int count) {
        return app.inventory->room(type, count, app.live->client().world(), gameFlags(app)) > 0;
    };
    a.centredMessage = [&app](const std::string& words) { centredMessage(app, words); };
    app.tradeScreens = std::make_unique<TradeScreens>(res, app.gameDir / "window", *app.inventory, *app.itemNames,
                                                      *app.art, std::move(a));
    app.inventory->setGameData(&app.gameData); // the stats page's maximums
    app.inventory->setItemNames(app.itemNames.get());
    app.inventory->setSwapPending([&app] { return app.live && app.live->client().weaponSwapPending(); });
    app.inventory->setAskAmount([&app](const std::string& label, std::uint16_t type, const std::array<std::uint8_t, 4>& mods,
                                       int most, SDL_FPoint at, std::function<void(int)> yes) {
        app.tradeScreens->askAmount(label, type, mods, most, at, app.camera.viewWidth, app.camera.viewHeight,
                                    std::move(yes));
    });
    // The shop's sell and repair modes ask about the inventory item clicked.
    app.inventory->setShopPick([&app](std::uint16_t nc) {
        auto& c = app.live->client();
        if (app.inventory->shopMode() == Inventory::ShopMode::Sell) {
            c.shopAskSellPrice(nc); // clientTradeMB
        } else {
            c.shopAskRepairCost(nc);
        }
    });
}

/// What CliNet_OnPacket prints in the middle of the screen for a message from the server:
/// MSG_INFORM (CliNet_OnInform: inform_text.hpp; its sound, 0x11's message box), 0xF0's
/// awards and lock notices (decodeReport) and MSG_MESSAGES_KILL (0xCB: Gui_ClearCenteredMessages).
void serverMessageText(App& app, std::span<const std::uint8_t> m)
{
    if (m[0] == 0xCB) {
        app.centred.clear();
        return;
    }
    if ((m[0] != 0xA9 && m[0] != 0xF0) || !app.live) {
        return;
    }
    const auto& world = app.live->client().world();
    nox::client::inform::Lookups l;
    l.text = [&app](std::string_view label) { return gameString(app, label); };
    l.playerName = [&world](std::uint32_t nc) -> std::optional<std::string> {
        const auto it = nc <= 0xFFFF ? world.players().find(static_cast<std::uint16_t>(nc)) : world.players().end();
        return it != world.players().end() ? std::optional<std::string>(it->second.name) : std::nullopt;
    };
    l.teamName = [&world](std::uint32_t id) -> std::optional<std::string> {
        const auto it = world.teams().find(id);
        return it != world.teams().end() ? std::optional<std::string>(it->second.name) : std::nullopt;
    };
    l.spellTitle = [&app](std::uint32_t id) {
        const auto* s = app.things.findSpell(id);
        return s && !s->title.empty() ? gameString(app, s->title) : std::string();
    };
    l.guideTitle = [&app](std::uint32_t id) {
        return id >= 1 && id <= nox::things::kGuideNames.size()
                   ? gameString(app, "creature:" + std::string(nox::things::kGuideNames[id - 1]))
                   : std::string();
    };
    l.abilityTitle = [&app](std::uint32_t id) {
        return id >= 1 && id <= app.things.abilities.size() ? gameString(app, app.things.abilities[id - 1].title) : std::string();
    };
    l.localNetCode = app.live->client().netCode();
    const auto n = m[0] == 0xA9 ? nox::client::inform::decode(m, l) : nox::client::inform::decodeReport(m, l);
    for (const auto& line : n.centred) {
        centredMessage(app, line);
    }
    if (n.sound != 0) {
        uiSound(app, n.sound);
    }
    if (n.dialog && app.cinematics) {
        app.cinematics->notice(n.dialogTitle, n.dialogText);
    }
}

} // namespace

/// client_quit: out of a quest, gone; in one as a client (not the host), our save asked
/// for first and gone once it's kept (HeadlessClient::questQuit), at once if asked again. (The
/// host's wait, everyone's save sent before it closes, isn't reproduced: it goes at once.)
void clientQuit(App& app)
{
    if (!app.live) {
        return;
    }
    if ((gameFlags(app) & 0x1000u) && !app.host) {
        SDL_Log(app.live->client().closingForSave() ? "quit: at once" : "quit: our save asked for first");
        app.live->client().questQuit();
        return;
    }
    app.live->quit();
}

namespace {

/// The SAVEDATA our host sent kept as our character's file (clientCurSave), our own
/// sections after it, and XferPlyr's "CharacterSaved" on the console (colour 12)
/// and in the middle; one too short for a client refused with Wol.c:WolApierror the same way.
void keepSaveData(App& app, std::span<const std::uint8_t> data)
{
    if (!app.live || app.characterFile.empty()) {
        SDL_Log("save: no character file to keep the host's save in");
        return;
    }
    namespace saves = nox::client::solo_saves;
    const auto kept = saves::keepSaveData(app.saveRoot / "Save", app.characterFile, data, app.host != nullptr);
    if (kept == saves::KeptSave::TooShort) {
        if (app.console) {
            app.console->print(gameString(app, "Wol.c:WolApierror"), Blue);
        }
        centredMessage(app, gameString(app, "Wol.c:Wolapierror"));
        return;
    }
    if (kept == saves::KeptSave::Unwritten) {
        SDL_Log("save: unable to open %s", app.characterFile.string().c_str());
        return;
    }
    const auto& client = app.live->client();
    const std::uint32_t flags = gameFlags(app);
    nox::server::HostClient::ClientSave info;
    info.stage = (flags & 0x1000u) ? static_cast<std::uint8_t>(client.world().self().questStage) : std::uint8_t{0};
    info.level = client.world().self().level;
    info.summonOrder = 4; // (a client's own Player: its setup's, nothing else sets it)
    const std::string& map = client.mapName();
    info.mapName = map.size() > 4 ? map.substr(0, map.size() - 4) : map; // (Map_GetCurrentName)
    if (!app.cinematics || !app.cinematics->appendNetworkSave(app.characterFile, info, flags)) {
        SDL_Log("save: unable to save %s", app.characterFile.string().c_str());
        return;
    }
    SDL_Log("save: kept the host's save in %s", app.characterFile.string().c_str());
    if (app.console) {
        app.console->print(gameString(app, "CharacterSaved"), Blue);
    }
    centredMessage(app, gameString(app, "CharacterSaved"));
}

/// What the game we host reads of our client from frame to frame (LocalHost's client view: the
/// HostClient queries its server makes between its calls).
HostClientView hostClientView(App& app)
{
    HostClientView v;
    if (app.cinematics) {
        const nox::server::HostClient& client = app.cinematics->hostClient();
        v.mapDrawn = app.cinematics->mapDrawn();
        v.wideScreenOn = client.wideScreenOn();
        v.wideScreenMoving = client.wideScreenMoving();
        v.briefingShown = client.briefingShown();
        v.bookFlying = client.bookFlying();
    }
    if (app.inventory && app.live) {
        // (as the inventoryRoom and inventoryWindow hooks read them)
        v.grid = true;
        v.cells = app.inventory->roomCells(gameFlags(app));
        v.secondaryWeapon = app.live->client().secondaryWeapon();
        v.lastQuiver = app.live->client().lastQuiver();
    }
    return v;
}

/// Our client joined to the game we host: the host told our game socket, and the client's
/// datagrams and sends kept in step with the host client's calls and view (local_host.hpp).
void joinOwnHost(App& app)
{
    app.host->setHostPeer(nox::net::Endpoint{0x7F000001u, app.live->gamePort()}); // 127.0.0.1, the loopback address
    app.live->beforeDatagram = [&app](std::span<const std::uint8_t> datagram) {
        if (app.host) {
            app.host->datagramArriving(datagram);
        }
    };
    app.live->afterDatagrams = [&app] {
        if (app.host) {
            app.host->datagramsTaken();
        }
    };
    app.live->beforeSend = [&app] {
        if (app.host) {
            app.host->publishClient();
        }
    };
}

/// The game's screen effects and briefings (cinematics.hpp) for this game; when we host it, our
/// host's own client (GAME.EXE's server calling its client directly in a solo game).
void startCinematics(App& app)
{
    if (!app.live) {
        return;
    }
    const gui::Resources res = guiResources(app);
    Cinematics::Hooks hooks;
    hooks.hideHud = [&app](bool hide) {
        // The HUD switch: the book, inventory and map put away (back after), the console, menus
        // and a spell waiting for its target too, and the HUD off; after, RenderGUI's setting.
        auto& was = app.hudBefore;
        if (hide) {
            was = {};
            if (app.book && app.book->isOpen()) {
                app.book->hide();
                was.book = true;
            }
            if (app.inventory && app.inventory->isOpen()) {
                app.inventory->toggle();
                was.inventory = true;
            }
            app.renderGui = false;
            if (app.minimap && app.minimap->shown()) {
                app.minimap->toggle();
                was.map = true;
            }
            if (app.console && app.console->isOpen()) {
                app.console->close(app.window);
            }
            if (app.quitMenu) {
                app.quitMenu->close();
            }
            if (app.serverOptions) {
                app.serverOptions->close();
            }
            if (app.options && app.options->isOpen()) {
                app.options->close();
            }
            app.armed.reset();
            app.barDrag.reset();
            return;
        }
        if (was.book && app.book) {
            app.book->show(bookContext(app));
        }
        if (was.inventory && app.inventory && !app.inventory->isOpen()) {
            app.inventory->toggle();
        }
        if (was.map && app.minimap && !app.minimap->shown()) {
            app.minimap->toggle();
        }
        const auto setting = app.input ? app.input->config().setting("RenderGUI") : std::nullopt;
        if (!setting || std::atoi(setting->c_str()) != 0) {
            app.renderGui = true;
        }
    };
    // Our own game (hosted or solo): hooks that reach into our server.
    if (app.host) {
        hooks.hostPause = [&app](int how, nox::server::PauseReason why) {
            if (app.host) {
                app.host->pause(how, why);
            }
        };
        hooks.toHost = [&app](std::function<void()> job) {
            if (app.host) {
                app.host->post(std::move(job));
            }
        };
        hooks.loadGame = [&app](const std::string& slot) { return app.host && app.host->loadSoloGame(slot); };
        hooks.detach = [&app] {
            if (app.host) {
                app.host->setHostClient(nullptr);
            }
        };
        hooks.bookFlying = [&app] { return app.flight.has_value(); };
        // Room for a pickup in our inventory, for the host's checks (kNoGrid without an inventory).
        hooks.inventoryRoom = [&app](std::uint16_t typeId, int count) {
            if (!app.inventory || !app.live) {
                return nox::server::HostClient::kNoGrid;
            }
            return app.inventory->room(typeId, count, app.live->client().world(), gameFlags(app));
        };
        hooks.inventoryWindow = [&app] {
            nox::server::HostClient::InventoryWindow w;
            if (app.inventory && app.live) {
                w.cells = app.inventory->cellOrder();
                w.secondaryWeapon = app.live->client().secondaryWeapon();
                w.lastQuiver = app.live->client().lastQuiver();
            }
            return w;
        };
        hooks.clearInventoryWeapons = [&app] {
            if (app.live) {
                app.live->client().clearInventoryWeapons();
            }
        };

        // The minimap's memory (the exe's one wall grid and its client's marker list): only of the
        // view's map when it is the server's (not the old one before the client has the next).
        // (Asked by a solo save: on the server's thread with ours parked, or inside withGame.)
        const auto onServersMap = [&app] {
            if (!app.host || !app.live || !app.view || app.liveMapLoads != app.live->client().mapLoads()) {
                return false;
            }
            const std::string ours = fs::path(app.liveMap).stem().string();
            const std::string& theirs = app.host->game().server().world().mapName;
            return nox::util::equalsNoCase(ours, theirs);
        };
        hooks.wallDrawn = [&app, onServersMap](int x, int y) { return onServersMap() && app.view->wallDrawn(x, y); };
        hooks.minimapMarked = [&app, onServersMap](std::uint16_t netCode) {
            return onServersMap() && app.minimap && app.minimap->marked(netCode, app.live->client().world());
        };
        // gameChangeMap: while the server's "not multiplayer" flag is up, the host's
        // client reads the solo path (a visited map's saved copy); its map read lowers the flag.
        if (app.live) {
            app.live->client().setSoloMap({
                .path = [&app]() -> std::optional<fs::path> { return app.host ? app.host->soloPath() : std::nullopt; },
                .read = [&app] {
                    if (app.host) {
                        app.host->soloPathRead();
                    }
                },
            });
        }

        hooks.requestSave = [&app](const std::string& name) {
            if (app.host) {
                app.host->requestSave(name);
            }
        };
        hooks.setStage = [&app](std::uint8_t stage) {
            if (app.host) {
                app.host->setStage(stage);
            }
        };
    }
    // Every game, hosted or joined.
    hooks.animState = [&app] { return ownAnimState(app); };
    hooks.deadFlag = [&app] { return ownDeadFlag(app); };
    hooks.landFlight = [&app] { landFlightAtOnce(app); };
    hooks.cancelDrags = [&app] {
        // Gui_CancelDragAndDrop (a dragged thing back in its cell), quickBarClose
        if (app.inventory) {
            app.inventory->cancelDrag();
        }
        app.barDrag.reset();
        expandBars(app, false);
    };
    hooks.bars = [&app] {
        return app.bars.toGui(static_cast<nox::player::CharacterClass>(app.profile.playerClass));
    };
    hooks.clearBars = [&app] {
        // The bars cleared: the quick bar closed, every slot and trap slot emptied.
        expandBars(app, false);
        app.bars.rows = {};
        app.bars.traps = {};
    };
    hooks.setBars = [&app](const nox::player::Gui& gui) {
        // plr_rwGUI's read: the bars (plr_rwSpellBars), the set
        // (clientUpdateButtonRow) and trap set chosen.
        app.bars = nox::client::SpellBars::fromGui(gui);
    };
    hooks.fileInfo = [&app] {
        if (app.heldFileInfo) {
            return *app.heldFileInfo;
        }
        // (The character the game began with: its name, class, colours and clothes.)
        nox::player::FileInfo f;
        f.flags = 1;
        f.name = nox::strings::fromUtf8(app.profile.name);
        f.characterClass = static_cast<nox::player::CharacterClass>(app.profile.playerClass);
        for (std::size_t i = 0; i < 5; ++i) {
            f.colors[i] = {app.profile.colors[i * 3], app.profile.colors[i * 3 + 1], app.profile.colors[i * 3 + 2]};  // colours: 5 RGB triples, packed
            f.clothing[i] = app.profile.clothing[i];
        }
        return f;
    };
    hooks.setFileInfo = [&app](const nox::player::FileInfo& f) { app.heldFileInfo = f; };
    hooks.clearConsole = [&app] {
        if (app.console) {
            app.console->clear();
        }
        app.centred.clear(); // (the solo load's Gui_ClearCenteredMessages)
    };
    hooks.quitGame = [&app] {
        if (app.live) {
            app.live->quit();
        }
    };
    hooks.hideBook = [&app] {
        if (app.book) {
            app.book->particles.clear();
            if (app.book->isOpen()) {
                app.book->hide();
            }
        }
    };
    hooks.playerName = [&app](std::uint16_t netCode) -> std::optional<std::string> {
        if (!app.live) {
            return std::nullopt;
        }
        const auto& players = app.live->client().world().players();
        const auto it = players.find(netCode);
        return it != players.end() ? std::optional<std::string>(it->second.name) : std::nullopt;
    };
    hooks.ownNetCode = [&app] { return app.live ? static_cast<std::uint16_t>(app.live->client().netCode()) : std::uint16_t{0}; };
    hooks.trackData = [&app] { return trackData(app); };
    hooks.setTrackData = [&app](int mask) { setTrackData(app, mask); };
    hooks.playMovie = [&app](const std::string& name, std::function<void()> done) {
        // The main menu's <Class>.vqa (the player with 1: only Escape stops it); without the file,
        // straight on to the credits.
        playMovie(app, name, movies::Skip::EscapeOnly, std::move(done));
    };
    if (app.quitMenu) {
        app.quitMenu->newGame(); // (game_guiInit: the in-game menu made anew)
    }
    if (app.serverOptions) {
        app.serverOptions->close();
    }
    app.cinematics = std::make_unique<Cinematics>(res, app.gameDir, app.audio.get(), std::move(hooks));
    app.cinematics->setSaveRoot(app.saveRoot);
    if (app.host) {
        // (what the host reads of us between its calls, and our screen messages handled before
        // its queued calls run: local_host.hpp)
        app.host->setClientView([&app] { return hostClientView(app); },
                                [&app] {
                                    if (app.cinematics) {
                                        app.cinematics->flushMessages();
                                    }
                                });
        app.host->setHostClient(&app.cinematics->hostClient());
    }
    // Every message from the server also goes to the cinematics and to the centred-message texts.
    app.live->client().setTracer([&app](bool outgoing, std::span<const std::uint8_t> m) {
        if (!outgoing && app.cinematics) {
            app.cinematics->message(m);
        }
        if (!outgoing && !m.empty()) {
            serverMessageText(app, m);
        }
    });
    // A quest's windows: Continue asks the server to go on (questContinue), Quit leaves (clientQuit).
    app.questWindows = std::make_unique<QuestWindows>(
        res, app.gameDir / "window",
        QuestWindows::Hooks{[&app] {
                                if (app.live) {
                                    app.live->client().questContinue();
                                }
                            },
                            [&app] { clientQuit(app); }});
    app.live->client().setSaveDataHandler([&app](std::span<const std::uint8_t> data) { keepSaveData(app, data); });
    // (a quest client's character file, clientCurSave, sent as SAVE_SERVER once in.)
    if (!app.characterFile.empty()) {
        std::ifstream f(app.characterFile, std::ios::binary);
        app.live->client().setSaveServerFile(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
    }
}

/// A game's spellbook and spell bars (bookInit, Gui_Quickbar_Init).
void startBook(App& app)
{
    loadBars(app);
    app.trapOpen = false;
    app.trapArt = false;
    app.trapSlide = 0;
    app.trapSlideDir = 0;
    if (!app.hud) {
        return;
    }
    app.book = std::make_unique<Spellbook>(app.things, app.strings.get(), *app.sprites, app.font.get(), *app.hud);
    app.book->setSound([&app](std::uint32_t id) { uiSound(app, id); });
    app.book->setMessage([&app](const std::string& text) { centredMessage(app, text); });
}

/// The solo and quest starting weapons' modifiers the client dresses players with
/// (Cli_ResetPlayerDefaultOutfit: ArmorQuality1, Material1, Replenishment1).
void setOutfitModifiers(App& app)
{
    const auto id = [&app](const char* name) {
        const auto* m = app.modifiers.findModifier(name);
        return m ? static_cast<std::uint8_t>(m->id) : std::uint8_t{0xFF};
    };
    app.live->client().setOutfitModifiers(id("ArmorQuality1"), id("Material1"), id("Replenishment1"));
}

/// The new live session's hooks and the game's windows (joinLive), and the server it joined
/// (`show game`, the browser's "Left ...").
void setUpLiveSession(App& app, const std::string& host)
{
    app.liveHost = host;
    // The map editor session's client side (idle unless the server is an edit session: a stock
    // or other noxcpp server never sends MSG_NOXCPP_EDIT). A full publish's map is written to the
    // map cache, so the map change finds it (the host's CRC) without a download.
    {
        nox::client::HeadlessClient* c = &app.live->client();
        app.editSession = std::make_unique<nox::client::EditSessionClient>(
            app.things, [c](std::vector<std::uint8_t> bytes, bool reliable) { c->sendEdit(std::move(bytes), reliable); }, app.mapCache);
        nox::client::EditSessionClient* e = app.editSession.get();
        c->setEditHandler([e](std::span<const std::uint8_t> m) { e->onMessage(m); });
        app.editSessionOn = c;
        app.sessionNoticed = false;
    }
    bindModeClients(app); // (each mode's client side and UI: idle on a server not playing it)
    app.live->onEvent = [&app](const nox::client::ClientEvent& e) { consoleEvent(app, e); };
    if (const auto* userColor1 = app.modifiers.findModifier("UserColor1")) {
        app.live->client().setUserColorBase(userColor1->id); // the street clothes' colours
    }
    setOutfitModifiers(app);
    app.live->client().setModifiers(&app.modifiers);
    app.inventory = std::make_unique<Inventory>(app.things, *app.art, app.modifiers, app.strings.get(), *app.sprites,
                                                app.font.get());
    app.inventory->setSound([&app](std::uint32_t id) { uiSound(app, id); });
    app.inventory->setClearSecondary([&app] { app.live->client().setSecondaryWeapon(0); });
    app.summon = std::make_unique<SummonWindow>(app.things, app.strings.get(), *app.sprites, app.font.get());
    app.summon->setSound([&app](std::uint32_t id) { uiSound(app, id); });
    startBook(app);
    startTradeScreens(app);
    startCinematics(app);
}

} // namespace

/// Joins `host` as app.profile: the live session made, its hooks and the game's windows set up
/// (setUpLiveSession), the audio into the game (startLive, and --connect at start).
void joinLive(App& app, const std::string& host)
{
    // The server is told the game's screen size, the view it sends objects for (half of it each
    // way); here that's the resolution the view is fixed to, not the window.
    app.profile.screenWidth = static_cast<std::uint32_t>(app.viewWidth);
    app.profile.screenHeight = static_cast<std::uint32_t>(app.viewHeight);
    if (app.host && host == app.host->address()) {
        // Our player is the host's own (GAME.EXE's 0x1F, its client sharing the settings: it isn't
        // sent MSG_UPDATE_GUI_GAME_SETTINGS), known by our game socket's address (told before the
        // server takes our first datagram).
        app.host->withGame([&](nox::server::GameHost&) {
            app.live = std::make_unique<LiveSession>(host, app.profile, app.things, app.gameDir, app.mapCache);
            joinOwnHost(app);
        });
    } else {
        // (A NoX Core-only game, as the list has it: the join waits till the host knows us. A
        // game the list saw as stock: joined at once, as before.)
        const nox::lobby::ServerEntry* listed = app.browser ? app.browser->selectedEntry() : nullptr;
        const bool identifyFirst = joinWaitsForIdentity(listed) && BrowserScreen::address(listed->game) == host;
        const bool knownStock = app.browser && app.browser->sawStock(host);
        app.live = std::make_unique<LiveSession>(host, app.profile, app.things, app.gameDir, app.mapCache, identifyFirst,
                                                 knownStock);
    }
    setUpLiveSession(app, host);
    if (app.audio) {
        app.audio->enterGame();
    }
    SDL_Log("noxsdl %s: joining %s as %s", nox::version(), host.c_str(), app.profile.name.c_str());
}

/// Joins `host` from the menus, the game state started afresh.
void startLive(App& app, const std::string& host)
{
    SDL_StopTextInput(app.window); // the game's keys are keys again
    gui::setShellUp(false);        // (client_xxx_switchChatMap: the game's windows small)
    freshGameState(app);           // (nothing of the last game's: new_game.hpp)
    app.view.reset();
    app.liveMap.clear();
    app.liveMapLoads = 0;
    app.serverOptionsLoad = 0;
    app.effects->clear();
    app.interpolator = Interpolator{};
    app.interpolator.setSmoothing(app.smoothMotion);
    app.interpolator.setScrolling(app.smoothScrolling);
    app.interpolator.setEasing(app.smoothing == 1);
    app.interpolator.setPrediction(app.predictMovement);
    setOtherUnits(app); // (the centred messages cleared above too: the join's after-load Gui_ClearCenteredMessages)
    joinLive(app, host);
}

} // namespace noxsdl
