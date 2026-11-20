// noxcore.exe's side of the co-op campaign (noxcore/coop), as its ModeUi (mode_ui.hpp): the live
// game's co-op client (MSG_NOXCPP_COOP: the greeting, the party, the rules' notices, a guest's
// cinematics and chapter screens), a guest's view on the host's scenes, and what the game list
// says when a co-op game refuses a character of another class. Its hosting goes through the solo
// screens (SoloScreensStart: the class screen picks the campaign, its saves under Save\Coop).
#include "app.hpp"
#include "character_play.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/solo_saves.hpp"
#include "noxcore/coop/coop.hpp"
#include "noxcore/coop/coop_client.hpp"
#include "noxcore/lobby/browser.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/net/server_transport.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/strings/l10n.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace noxsdl {

namespace {

/// One of the host's client's calls made on ours (a guest's own Cinematics, as the host's server
/// calls the host's: cinematics.hpp's HostClient).
void applyCine(nox::server::HostClient& h, const nox::coop::coopmsg::Cine& c)
{
    using Call = nox::coop::coopmsg::CineCall;
    switch (c.call) {
    case Call::Blind: h.blind(); break;
    case Call::UnBlind: h.unblind(); break;
    case Call::FadeToBlack: h.fadeToBlack(c.b, c.a != 0, {}); break; // (its end the host's server's)
    case Call::MapDrawn: h.setMapDrawn(c.a != 0); break;
    case Call::WideScreen: h.wideScreen(c.a != 0); break;
    case Call::MapIntro: h.playMapIntro(c.a != 0); break;
    case Call::EndGame: h.endGame(c.a); break;
    case Call::Voice: h.playVoice(c.text, c.a); break;
    case Call::StopAllFades: h.stopAllFades(); break;
    }
}

/// Where our co-op character for the campaign the host named is kept (nox::coop::guestCharacterFolder:
/// a mod's campaign's in that mod's players' saves); empty: beside the character we joined with, as
/// for the game's own campaign.
std::filesystem::path coopCharacterFolder(const nox::coop::CoopClient* coop)
{
    if (!coop || !coop->hello() || coop->hello()->campaignId.empty()) {
        return {};
    }
    std::filesystem::path dir = nox::coop::guestCharacterFolder(nox::modes::registry(), coop->hello()->campaignId);
    if (dir.empty()) {
        SDL_Log("coop: the campaign %s isn't one of our mods (or its saves can't be written): its co-op character kept beside ours",
                coop->hello()->campaignId.c_str());
    }
    return dir;
}

/// Our campaign character as the host's save left it, kept beside the one we joined with as
/// "<name> (Co-op).plr" (never over it): the server's sections as a quest's SAVEDATA is kept, then
/// our own sections as a network character's, so the list of network characters shows it. A mod's
/// campaign's goes in `folder` (coopCharacterFolder) instead.
void keepCoopCharacter(App& app, std::span<const std::uint8_t> data, const std::filesystem::path& folder)
{
    if (app.characterFile.empty() || !app.cinematics || !app.live) {
        SDL_Log("coop: no character file to keep the campaign's character beside");
        return;
    }
    const std::filesystem::path plr = nox::coop::campaignCharacterFile(app.characterFile, folder);
    if (plr == app.characterFile && !folder.empty()) {
        return; // (never over the character we joined with)
    }
    namespace saves = nox::client::solo_saves;
    if (saves::keepSaveData(app.saveRoot / "Save", plr, data, true) != saves::KeptSave::Written) {
        SDL_Log("coop: unable to write %s", reinterpret_cast<const char*>(plr.u8string().c_str()));
        return;
    }
    const auto& client = app.live->client();
    nox::server::HostClient::ClientSave info;
    info.level = client.world().self().level;
    info.summonOrder = 4;
    const std::string& map = client.mapName();
    info.mapName = map.size() > 4 ? map.substr(0, map.size() - 4) : map;
    if (!app.cinematics->appendNetworkSave(plr, info, 0x2000u)) { // (a network character's sections)
        SDL_Log("coop: unable to finish %s", reinterpret_cast<const char*>(plr.u8string().c_str()));
        return;
    }
    SDL_Log("coop: kept the campaign's character in %s", reinterpret_cast<const char*>(plr.u8string().c_str()));
}

/// A guest's spell bars once it's greeted (nox::coop::guestBars). A runner the campaign kept gets
/// its campaign character's bars, from the "(Co-op).plr" the host's saves write beside its own
/// file. A guest that came with a character no solo game saved (a network character: no GameData of
/// its own; the server gives it a new solo character's empty book) gets empty bars, as a new solo
/// character's are (the class screen's), so no spell it doesn't know is on them. A "(Co-op).plr"
/// keeps its own bars. From then on the bars are saved into the "(Co-op).plr" as the game is left
/// (saveBars), as well as at each of the host's saves (keepCoopCharacter).
void barsForGuest(App& app, bool kept, const std::filesystem::path& folder)
{
    const auto cls = static_cast<nox::player::CharacterClass>(ownClass(app));
    if (auto bars = nox::coop::guestBars(app.characterFile, cls, kept, folder)) {
        const bool empty = *bars == nox::client::SpellBars{};
        app.bars = std::move(*bars);
        if (empty) {
            SDL_Log("coop: a character no solo game saved: its spell bars empty, as a new solo character's");
        } else {
            SDL_Log("coop: the campaign kept us: the spell bars of %s",
                    reinterpret_cast<const char*>(nox::coop::campaignCharacterFile(app.characterFile, folder).u8string().c_str()));
        }
    }
    app.guestBarsFile = nox::coop::campaignCharacterFile(app.characterFile, folder);
}

/// A guest's known abilities on its bars, those not on them yet (SpellBars::addKnownAbilities):
/// a new runner's bars start empty, and a load's or a rejoin's awards are quiet ones, which fill
/// no slot. At once, without a flight or a sound.
void abilitiesForGuest(App& app)
{
    if (!app.live || (ownClass(app) != 0 && !multiclass(app))) {
        return; // (abilities only on a warrior's bars, or multiclass ones: the book's test)
    }
    const auto& levels = app.live->client().world().self().abilityLevel;
    if (const int n = app.bars.addKnownAbilities(levels, true, multiclass(app) ? nox::client::kAbilityTag : 0u)) {
        SDL_Log("coop: %d known abilities onto the spell bars", n);
    }
}

class CoopUi final : public ModeUi {
public:
    explicit CoopUi(App& app) : app_(app) {}

    void bind(nox::modes::ClientMode* side) override
    {
        // (The game left: no longer a guest; a new game's, or none: what the last was reading forgotten.)
        if (!side && app_.cinematics) {
            app_.cinematics->setGuest(false);
        }
        coop_ = dynamic_cast<nox::coop::CoopClient*>(side);
        if (coop_) {
            // A guest's cinematics (the host's client's calls) and chapter screens onto our
            // Cinematics' queue as each comes, in their turn among the game's messages (a fade's
            // MSG_FADE_BEGIN before or after them) and each other, applied at its next update whether
            // or not the HUD draws (issue #14: a darkening call applied after the fade-in left the
            // screen black). (None is sent to the host.)
            const auto forUs = [this](const nox::coop::CoopClient* client) {
                return coop_ == client && app_.cinematics && !(client->hello() && !client->guest());
            };
            coop_->setCineSink([this, forUs, client = coop_](const nox::coop::coopmsg::Cine& c) {
                if (forUs(client)) {
                    app_.cinematics->call([c](nox::server::HostClient& h) { applyCine(h, c); });
                }
            });
            coop_->setChapterSink([this, forUs, client = coop_](const nox::coop::coopmsg::Chapter& ch) {
                if (!forUs(client)) {
                    return;
                }
                // As MSG_CHAPTER_END comes (Gui_Brief_ShowLockScreen): our own class's chapter, the
                // campaign's (every player's is). (A guest's death chapter screen fades back to the
                // game: the host is the one who loads.)
                app_.cinematics->setGuest(client->guest());
                app_.cinematics->message(std::vector<std::uint8_t>{0xD6, ch.chapter, static_cast<std::uint8_t>(ch.startup ? 1 : 0)});
                reading_ = Reading{true, false, {}};
            });
        }
        reading_ = {};
        dialogSeen_ = 0;
        barsLooked_ = false;
        campaignTold_ = false;
        worldSinceMs_ = 0;
        characterFolder_.clear();
        app_.guestBarsFile.clear(); // (saveBars ran before the game's let go)
        app_.partyDots = false;
        app_.partyNames = false;
        app_.partyCreatures.clear();
        creaturesSeen_ = 0;
        seenMap_.clear(); // (what the last game's view saw and hadn't told: let go with it)
        untoldWalls_.clear();
        untoldObelisks_.clear();
    }

    /// Each frame: the friend dot over the party's creatures while we're in the campaign
    /// (friend_dot.hpp, VD-39), the session's list copied as it changes; the names over the party's
    /// players (name_plate.hpp); a guest's minimap (seenWithHost); a guest's chapter screen's end
    /// told back (its screen went on our Cinematics' queue as it came: bind's sink).
    void update(App& app) override
    {
        app.partyDots = coop_ && coop_->inCampaign();
        app.partyNames = app.partyDots;
        if (coop_ && coop_->partyCreaturesVersion() != creaturesSeen_) {
            creaturesSeen_ = coop_->partyCreaturesVersion();
            app.partyCreatures = coop_->partyCreatures();
        }
        if (coop_ && coop_->guest()) {
            seenWithHost(app);
        }
        if (!coop_ || !app.cinematics) {
            return;
        }
        // (A guest's death chapter screen fades back to the game: the host is the one who loads.)
        app.cinematics->setGuest(coop_->guest());
        if (coop_->guest() && reading_.on) {
            if (app.cinematics->briefingShown()) {
                reading_.seenUp = true;
            } else if (reading_.seenUp) {
                // Ours over (clicked away, or its time ran out): the host told.
                coop_->chapterDone();
                reading_.on = false;
            }
        }
    }

    /// Each frame: the rules' notices shown (in the middle of the screen, and the console); who's
    /// still reading a chapter screen; the host's talk shown to the others.
    void drawHud(App& app, SDL_Renderer*, const HudFrame&) override
    {
        if (!coop_) {
            return;
        }
        if (coop_->guest()) {
            if (!barsLooked_) {
                barsLooked_ = true; // (a guest's spell bars looked at once it's in)
                characterFolder_ = coopCharacterFolder(coop_); // (once a game: Hello's campaign)
                barsForGuest(app, coop_->hello() && coop_->hello()->characterKept, characterFolder_);
                // (noxcpp) The campaign noted beside the character we joined with, and its co-op file
                // when that's one the character screen lists (character_play.hpp).
                if (coop_->hello()) {
                    characterPlayedCoop(app, coop_->hello()->campaignId, coop_->hello()->campaignTitle);
                }
                abilitiesSeen_ = app.quietAbilityAwards;
                abilitiesForGuest(app);
            } else if (app.quietAbilityAwards != abilitiesSeen_ && !app.flight) {
                abilitiesSeen_ = app.quietAbilityAwards; // (a shown award's flight lands first)
                abilitiesForGuest(app);
            }
            if (const auto kept = coop_->takeCharacter()) {
                keepCoopCharacter(app, *kept, characterFolder_);
                if (coop_->hello()) {
                    // (noxcpp) The co-op file noted too, when the character screen lists it (the game's own
                    // campaign's, beside ours).
                    characterPlayedCoop(app, coop_->hello()->campaignId, coop_->hello()->campaignTitle,
                                        nox::coop::campaignCharacterFile(app.characterFile, characterFolder_));
                }
            }
        }
        for (const auto& [kind, text] : coop_->takeNotices()) {
            (void)kind;
            centredMessage(app, text);
        }
        if (!app.cinematics) {
            return;
        }
        // A mod's campaign (Hello's): its name once, when the world has been up for 3 s with no
        // chapter screen over it (one that comes at the map's start read first), to the host and
        // the guests alike. The game's own: nothing.
        if (!campaignTold_ && coop_->hello() && !coop_->hello()->campaignTitle.empty()) {
            const std::uint64_t now = SDL_GetTicks();
            if (!inWorld(app) || app.cinematics->briefingShown()) {
                worldSinceMs_ = 0;
            } else if (worldSinceMs_ == 0) {
                worldSinceMs_ = now;
            }
        }
        if (!campaignTold_ && worldSinceMs_ != 0 && SDL_GetTicks() - worldSinceMs_ >= 3000) {
            campaignTold_ = true;
            const std::string line =
                nox::l10n::format(NOX_TR("menus.solo.campaign_line", "Campaign: {campaign}"), {{"campaign", coop_->hello()->campaignTitle}});
            app.centred.print(line, gameFrame(app), SDL_Color{240, 180, 42, 255}); // (the menus' gold)
            SDL_Log("coop: %s", line.c_str());
            if (app.console) {
                app.console->print(line, Yellow);
            }
        }
        // Who's still reading: told when it changes, while any is.
        const auto state = coop_->readyState();
        if (state != reading_.toldState) {
            reading_.toldState = state;
            if (state.first > 0 && !app.cinematics->briefingShown()) {
                centredMessage(app, nox::l10n::format(coop_->guest() ? NOX_TR("modes.coop.chapter.waiting", "Waiting for {n} of {of} to finish the chapter screen.")
                                                                     : NOX_TR("modes.coop.chapter.waiting_host",
                                                                              "Waiting for {n} of {of} to finish the chapter screen (type ?skip to go on)"),
                                                      {{"n", state.first}, {"of", state.second}}));
            }
        }
        // Another player's talk (the host's, or a guest's): the NPC's line in the console as each comes.
        if (coop_->dialogVersion() != dialogSeen_) {
            dialogSeen_ = coop_->dialogVersion();
            const auto& d = coop_->dialog();
            const bool ours = app.live && d.talker == app.live->client().netCode();
            if (d.open && !ours && !d.text.empty() && app.console) {
                app.console->print(resolveStringId(app, d.text), Yellow);
            }
        }
    }

    /// A guest's view on the scene's actor (the host) while a scene's bars are up and the actor is
    /// in sight.
    void cameraSet(App& app) override
    {
        if (!coop_ || !coop_->guest() || !coop_->sceneOn() || !app.live) {
            return;
        }
        const auto& ds = app.live->client().world().drawables();
        const auto it = ds.find(coop_->sceneActor());
        if (it == ds.end()) {
            return;
        }
        const auto pos = app.interpolator.position(it->first, it->second);
        if (std::hypot(pos.x - app.camera.x, pos.y - app.camera.y) > 640.0f) {
            return; // (out of sight: the world there isn't ours to see)
        }
        app.camera.x = std::round(pos.x * app.camera.zoom) / app.camera.zoom;
        app.camera.y = std::round(pos.y * app.camera.zoom) / app.camera.zoom;
    }

    /// A co-op campaign's join refused for the class (net::JoinError::ClassNotAllowed): the
    /// campaign's class named.
    std::optional<std::string> joinRefused(std::uint8_t joinError, const nox::lobby::ServerEntry* game) const override
    {
        // (A co-op campaign's alone: any other game's refusal reads as it did.)
        if (joinError != nox::net::JoinError::ClassNotAllowed || !game || !game->extended ||
            !(game->extended->features & nox::lobby::ServerFeature::CoopCampaign)) {
            return std::nullopt;
        }
        // Same class only: the class it doesn't refuse is the campaign's.
        const std::uint8_t refused = game->game.bannedClasses & 7u;
        for (std::uint8_t c = 0; c < 3; ++c) {
            if (refused == (7u & ~(1u << c))) {
                // (nox::coop::className's, as shown)
                const std::string& cls = c == 0   ? NOX_TR("modes.coop.class.warrior", "Warrior")
                                         : c == 1 ? NOX_TR("modes.coop.class.wizard", "Wizard")
                                                  : NOX_TR("modes.coop.class.conjurer", "Conjurer");
                return nox::l10n::format(NOX_TR("modes.coop.join.class_only", "This co-op campaign is for {class} characters only."), {{"class", cls}});
            }
        }
        return NOX_TR("modes.coop.join.class_refused", "This co-op campaign doesn't take characters of your class.");
    }

private:
    /// A guest's minimap, kept by the campaign (coop.hpp: a guest loads the map's own file, which
    /// remembers nothing): what the host kept of the map we're on put on our view once it's up (the
    /// view the client's last map load made), and the walls and obelisks our view sees for the
    /// first time told to the host about once a second (the last map's at once as the view moves on).
    void seenWithHost(App& app)
    {
        if (!app.live || !app.view || app.liveMapLoads != app.live->client().mapLoads()) {
            return; // (a view about to go: what it sees now goes with it)
        }
        const std::string map = std::filesystem::path(app.liveMap).stem().string();
        if (map != seenMap_) {
            tellSeen();
            seenMap_ = map;
        }
        if (const auto kept = coop_->takeSeen(map)) {
            app.view->markSeen(kept->walls);
            if (app.minimap) {
                app.minimap->markSeen(kept->obelisks);
            }
            SDL_Log("coop: the campaign's minimap of %s: %d walls, %d obelisks", map.c_str(), static_cast<int>(kept->walls.size()),
                    static_cast<int>(kept->obelisks.size()));
        }
        const auto walls = app.view->takeDrawnWalls();
        untoldWalls_.insert(untoldWalls_.end(), walls.begin(), walls.end());
        if (app.minimap) {
            const auto obelisks = app.minimap->takeSeenObelisks();
            untoldObelisks_.insert(untoldObelisks_.end(), obelisks.begin(), obelisks.end());
        }
        const std::uint64_t now = SDL_GetTicks();
        if ((!untoldWalls_.empty() || !untoldObelisks_.empty()) && now - seenToldMs_ >= 1000) {
            tellSeen();
            seenToldMs_ = now;
        }
    }
    /// What our view saw that the host hasn't been told, told (kSeenReport).
    void tellSeen()
    {
        if (coop_ && !seenMap_.empty() && (!untoldWalls_.empty() || !untoldObelisks_.empty())) {
            coop_->reportSeen(seenMap_, untoldWalls_, untoldObelisks_);
        }
        untoldWalls_.clear();
        untoldObelisks_.clear();
    }

    /// A guest's chapter screen under way: shown (its briefing seen up), then over (told back).
    struct Reading {
        bool on = false;
        bool seenUp = false;
        std::pair<std::uint8_t, std::uint8_t> toldState{0, 0};
    };

    App& app_;
    nox::coop::CoopClient* coop_ = nullptr;
    Reading reading_;
    std::uint32_t dialogSeen_ = 0;
    bool barsLooked_ = false;
    bool campaignTold_ = false; ///< (a mod's campaign named once a game)
    std::uint64_t worldSinceMs_ = 0; ///< (SDL_GetTicks since the world's been up, no chapter screen; 0: not)
    std::filesystem::path characterFolder_; ///< (coopCharacterFolder, as the guest was greeted)
    std::uint32_t abilitiesSeen_ = 0; ///< (App::quietAbilityAwards as the bars last took them)
    std::uint32_t creaturesSeen_ = 0; ///< (CoopClient::partyCreaturesVersion copied to App::partyCreatures)
    std::string seenMap_;                       ///< the map our view's walls and obelisks below are of
    std::vector<std::uint16_t> untoldWalls_;    ///< first drawn there, not yet told the host (cell keys)
    std::vector<std::uint16_t> untoldObelisks_; ///< first marked there, likewise (extents)
    std::uint64_t seenToldMs_ = 0;              ///< (SDL_GetTicks at the last report)
};

} // namespace

void registerModeUi_coop(std::vector<ModeUiEntry>& entries)
{
    SoloScreensStart start;
    start.saves = std::filesystem::path("Save") / "Coop";
    start.logTag = "co-op";
    start.what = [](std::uint8_t playerClass) { return "the " + std::string(nox::coop::className(playerClass)) + " campaign"; };
    entries.push_back({"coop", [](App& app) { return std::make_unique<CoopUi>(app); }, {}, std::move(start)});
}

} // namespace noxsdl
