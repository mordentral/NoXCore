// noxcore.exe's side of the co-op campaign (noxcore/coop), as its ModeUi (mode_ui.hpp): the live
// game's co-op client (MSG_NOXCPP_COOP: the greeting, the party, the rules' notices, a guest's
// cinematics and chapter screens), a guest's view on the host's scenes, and what the game list
// says when a co-op game refuses a character of another class. Its hosting goes through the solo
// screens (SoloScreensStart: the class screen picks the campaign, its saves under Save\Coop).
#include "app.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/solo_saves.hpp"
#include "noxcore/coop/coop.hpp"
#include "noxcore/coop/coop_client.hpp"
#include "noxcore/lobby/browser.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/net/server_transport.hpp"
#include "noxcore/player/player_file.hpp"

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
void applyCine(App& app, const nox::coop::coopmsg::Cine& c)
{
    using Call = nox::coop::coopmsg::CineCall;
    nox::server::HostClient& h = app.cinematics->hostClient();
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

/// Our campaign character as the host's save left it, kept beside the one we joined with as
/// "<name> (Co-op).plr" (never over it): the server's sections as a quest's SAVEDATA is kept, then
/// our own sections as a network character's, so the list of network characters shows it.
void keepCoopCharacter(App& app, std::span<const std::uint8_t> data)
{
    if (app.characterFile.empty() || !app.cinematics || !app.live) {
        SDL_Log("coop: no character file to keep the campaign's character beside");
        return;
    }
    std::u16string stem = app.characterFile.stem().u16string();
    const std::u16string mark = u" (Co-op)";
    if (!stem.ends_with(mark)) {
        stem += mark;
    }
    const std::filesystem::path plr = app.characterFile.parent_path() / std::filesystem::path(stem + u".plr");
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

/// A guest who came with a character no solo game saved (a network character: no GameData of
/// its own; the server gives it a new solo character's empty book): its spell bars emptied too, as
/// a new solo character's are (the class screen's), so no spell it doesn't know sits on them. One
/// it keeps beside its own ("(Co-op).plr", the campaign's save of it) keeps its bars.
void barsForGuest(App& app)
{
    bool savedBySolo = false; // (no file at all: a network game's starting set, emptied too)
    try {
        if (!app.characterFile.empty()) {
            const auto file = nox::player::loadPlayerFile(app.characterFile);
            const auto* data = file.find(nox::player::SectionId::GameData);
            savedBySolo = data && !data->data.empty();
        }
    } catch (const std::exception&) {
    }
    if (!savedBySolo) {
        app.bars = nox::client::SpellBars{};
        SDL_Log("coop: a character no solo game saved: its spell bars empty, as a new solo character's");
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
        reading_ = {};
        dialogSeen_ = 0;
        barsLooked_ = false;
    }

    /// Each frame: the rules' notices shown (in the middle of the screen, and the console); a
    /// guest's cinematics made on its own Cinematics (the host's client's calls it was told), its
    /// chapter screens shown and their end told back; who's still reading; the host's talk shown to
    /// the others.
    void drawHud(App& app, SDL_Renderer*, const HudFrame&) override
    {
        if (!coop_) {
            return;
        }
        if (coop_->guest()) {
            if (!barsLooked_) {
                barsLooked_ = true; // (a guest's spell bars looked at once it's in)
                barsForGuest(app);
            }
            if (const auto kept = coop_->takeCharacter()) {
                keepCoopCharacter(app, *kept);
            }
        }
        for (const auto& [kind, text] : coop_->takeNotices()) {
            (void)kind;
            centredMessage(app, text);
        }
        if (!app.cinematics) {
            return;
        }
        // (A guest's death chapter screen fades back to the game: the host is the one who loads.)
        app.cinematics->setGuest(coop_->guest());
        if (coop_->guest()) {
            for (const auto& c : coop_->takeCines()) {
                applyCine(app, c);
            }
            if (const auto ch = coop_->takeChapter()) {
                // As MSG_CHAPTER_END comes (Gui_Brief_ShowLockScreen): our own class's chapter, the
                // campaign's (every player's is).
                app.cinematics->message(std::vector<std::uint8_t>{0xD6, ch->chapter, static_cast<std::uint8_t>(ch->startup ? 1 : 0)});
                reading_ = Reading{true, false, {}};
            }
            if (reading_.on) {
                if (app.cinematics->briefingShown()) {
                    reading_.seenUp = true;
                } else if (reading_.seenUp) {
                    // Ours over (clicked away, or its time ran out): the host told.
                    coop_->chapterDone();
                    reading_.on = false;
                }
            }
        }
        // Who's still reading: told when it changes, while any is.
        const auto state = coop_->readyState();
        if (state != reading_.toldState) {
            reading_.toldState = state;
            if (state.first > 0 && !app.cinematics->briefingShown()) {
                centredMessage(app, "Waiting for " + std::to_string(state.first) + " of " + std::to_string(state.second) + " to finish the chapter screen" +
                                        (coop_->guest() ? std::string(".") : std::string(" (type ?skip to go on)")));
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
                return "This co-op campaign is for " + std::string(nox::coop::className(c)) + " characters only.";
            }
        }
        return std::string("This co-op campaign doesn't take characters of your class.");
    }

private:
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
