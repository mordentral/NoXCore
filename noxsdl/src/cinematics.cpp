// The solo game's cinematics and screens (cinematics.hpp describes them): the fade, black screen
// and widescreen records, the chapter briefings and credits, the quest's screens, the in-game
// save / load menu with its message box, and the client's own .plr sections.
#include "cinematics.hpp"

#include "console.hpp"

#include "noxcore/client/movies.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/util/bytes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>

namespace noxsdl {

namespace {

constexpr int kFadeFrames = 25; ///< client_getIntroScreenDuration, and the server's fades
constexpr int kMusicBriefing = 50; ///< the briefing music's volume

// selchar.wnd as GUISave.c uses it (savegame_sub).
constexpr std::int32_t kSave = 501, kLoad = 502, kCancel = 503, kUp = 504, kDown = 505, kLists = 510, kNames = 511,
                       kInfo = 512, kLabel = 509;
// dlg.wnd
constexpr std::int32_t kDlgText = 4004, kDlgTitle = 4005, kDlgNo = 4006, kDlgYes = 4007, kDlgOk = 4001,
                       kDlgCancel = 4002;

constexpr const char* kClasses[3] = {"Warrior", "Wizard", "Conjurer"};

/// The briefing's music for a chapter (0-10) and class: 1; then 4 / 2 / 3; then the chapter + 3.
int chapterMusic(int chapter, int cls)
{
    if (chapter == 0) {
        return 1;
    }
    if (chapter == 1) {
        return cls == 0 ? 4 : (cls != 1) + 2;
    }
    return chapter + 3;
}

/// A string's %s / %S filled in order (the exe's swprintf on its own strings).
std::string format(const std::string& f, std::initializer_list<std::string> values)
{
    std::string out;
    auto v = values.begin();
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '%' && i + 1 < f.size() && (f[i + 1] == 's' || f[i + 1] == 'S') && v != values.end()) {
            out += *v++;
            ++i;
            continue;
        }
        out += f[i];
    }
    return out;
}

nox::gui::Color palette(int index)
{
    const SDL_Color c = paletteColor(static_cast<std::uint8_t>(index));
    return {c.r, c.g, c.b, false};
}

void enable(gui::Window* top, std::int32_t id, bool on)
{
    if (gui::Window* w = top ? top->find(id) : nullptr) {
        if (on) {
            w->status |= nox::gui::status::Enabled;
        } else {
            w->status &= ~nox::gui::status::Enabled;
        }
    }
}

} // namespace

// The host's own client: GAME.EXE's server's direct calls into its client.
class Cinematics::Link : public nox::server::HostClient {
public:
    explicit Link(Cinematics& c) : c_(c) {}

    void blind() override
    {
        SDL_Log("cinematics: Blind");
        c_.blind();
    }
    void unblind() override
    {
        // UnBlind: the map drawn, Draw_FadeOutScreen(25, 0, 0).
        SDL_Log("cinematics: UnBlind");
        c_.mapDrawn_ = true;
        c_.fadeFromBlack(kFadeFrames, false, {});
    }
    void fadeToBlack(int frames, bool overMenu, std::function<void()> done) override
    {
        SDL_Log("cinematics: fade to black, %d frames%s", frames, overMenu ? " over the GUI" : "");
        // `done` is the server's (LocalHost's bridge runs it on the server's thread): at once
        // with the map not drawn (Draw_FadeInScreen calls it there and then), else at the fade's
        // end, before the server's next tick.
        if (!c_.mapDrawn_) {
            if (done) {
                done();
            }
            return;
        }
        Cinematics& c = c_;
        c_.fadeToBlack(frames, overMenu, [&c, done = std::move(done)] {
            if (!done) {
                return;
            }
            if (c.hooks_.toHost) {
                c.hooks_.toHost(done);
            } else {
                done();
            }
        });
    }
    void setMapDrawn(bool drawn) override { c_.mapDrawn_ = drawn; }
    bool wideScreenMoving() const override { return c_.find(Fx::Bars) != nullptr; }
    void wideScreen(bool on) override
    {
        SDL_Log("cinematics: WideScreen %d", on ? 1 : 0);
        // (only when the bars got a record)
        if (on ? c_.barsIn() : c_.barsOut()) {
            c_.setHud(on);
        }
    }
    void playMapIntro(bool immediate) override
    {
        SDL_Log("cinematics: map intro%s", immediate ? " (immediate)" : "");
        c_.mapIntro(immediate);
    }
    void endGame(int kind) override
    {
        SDL_Log("cinematics: EndGame %d", kind);
        c_.endGame(kind);
    }
    void musicPush() override
    {
        if (c_.audio_) {
            c_.audio_->musicPush();
        }
    }
    void musicPop() override
    {
        if (c_.audio_) {
            c_.audio_->musicPop();
        }
    }
    void music(int track, int volume) override
    {
        if (c_.audio_) {
            c_.audio_->music(track, volume);
        }
    }
    void playVoice(std::string_view file, int volume) override
    {
        if (c_.audio_ && !file.empty()) {
            c_.audio_->dialog(std::string(file), volume);
        }
    }
    void stopAllFades() override
    {
        SDL_Log("cinematics: the fades stopped");
        c_.stopAllFades();
    }
    bool wideScreenOn() const override { return c_.wideScreen_; }
    bool briefingShown() const override { return c_.briefed_; }
    bool bookFlying() const override { return c_.hooks_.bookFlying && c_.hooks_.bookFlying(); }
    int inventoryRoom(std::uint16_t typeId, int count) const override
    {
        return c_.hooks_.inventoryRoom ? c_.hooks_.inventoryRoom(typeId, count) : kNoGrid;
    }
    InventoryWindow inventoryWindow() const override
    {
        return c_.hooks_.inventoryWindow ? c_.hooks_.inventoryWindow() : InventoryWindow{};
    }
    void clearInventoryWeapons() override
    {
        if (c_.hooks_.clearInventoryWeapons) {
            c_.hooks_.clearInventoryWeapons();
        }
    }

    bool wallDrawn(int x, int y) const override { return c_.hooks_.wallDrawn && c_.hooks_.wallDrawn(x, y); }
    bool minimapMarked(std::uint16_t netCode) const override
    {
        return c_.hooks_.minimapMarked && c_.hooks_.minimapMarked(netCode);
    }

    void saveStarting() override
    {
        if (c_.hooks_.cancelDrags) {
            c_.hooks_.cancelDrags();
        }
    }
    std::optional<bool> saveClientSections(const std::filesystem::path& plr, const ClientSave& info) override
    {
        return c_.saveClientSections(plr, info);
    }
    bool loadClientSections(const std::filesystem::path& plr, ClientLoad& out) override { return c_.loadClientSections(plr, out); }
    void gameLoaded() override
    {
        // The console cleared (its lines), Gui_ClearCenteredMessages (ours print to it).
        if (c_.hooks_.clearConsole) {
            c_.hooks_.clearConsole();
        }
    }

private:
    Cinematics& c_;
};

Cinematics::Cinematics(gui::Resources res, std::filesystem::path gameDir, Audio* audio, Hooks hooks)
    : res_(res), gameDir_(std::move(gameDir)), audio_(audio), hooks_(std::move(hooks)),
      link_(std::make_unique<Link>(*this)), gui_(std::move(res))
{
}

Cinematics::~Cinematics()
{
    if (hooks_.detach) {
        hooks_.detach();
    }
    if (brief_.shown && audio_) {
        audio_->endBriefingMusic();
    }
    if (hudHidden_ && hooks_.hideHud) {
        hooks_.hideHud(false); // (the game left in a widescreen: the HUD back for the next)
    }
}

nox::server::HostClient& Cinematics::hostClient()
{
    return *link_;
}

std::string Cinematics::text(const std::string& id) const
{
    return gui_.text(id);
}

// ------------------------------------------------------------------------------ the records

// A free record (flag 1 clear), or nullptr when all are in use.
Cinematics::Record* Cinematics::freeRecord()
{
    for (auto& r : records_) {
        if (!(r.flags & 1)) {
            return &r;
        }
    }
    return nullptr;
}

// Ends every running record of kind `fx`, calling their callbacks; whether there was one.
bool Cinematics::end(Fx fx)
{
    bool any = false;
    for (auto& r : records_) {
        if ((r.flags & 1) && r.fx == fx) {
            r.flags &= ~1u;
            auto done = std::move(r.done);
            r.done = {};
            if (done) {
                done();
            }
            any = true;
        }
    }
    return any;
}

const Cinematics::Record* Cinematics::find(Fx fx) const
{
    for (const auto& r : records_) {
        if ((r.flags & 1) && r.fx == fx) {
            return &r;
        }
    }
    return nullptr;
}

void Cinematics::fadeToBlack(int frames, bool menu, std::function<void()> done)
{
    // Draw_FadeInScreen.
    if (!mapDrawn_) {
        if (done) {
            done();
        }
        return;
    }
    end(Fx::ToBlack);
    Record* r = freeRecord();
    if (!r || frames <= 0) {
        return;
    }
    r->flags = menu ? 5 : 1;  // 1 running, 4 over the GUI (Record::flags)
    r->acc = 0;
    r->frames = frames;
    r->fx = Fx::ToBlack;
    r->step = 0xFF0000 / frames;  // 255 in 16.16, over `frames`
    r->done = std::move(done);
}

void Cinematics::fadeFromBlack(int frames, bool menu, std::function<void()> done)
{
    // Draw_FadeOutScreen: the map intro's black ended after.
    end(Fx::FromBlack);
    Record* r = freeRecord();
    if (!r || frames <= 0) {
        return;
    }
    r->flags = menu ? 5 : 1;
    r->frames = frames;
    r->acc = 0;
    r->fx = Fx::FromBlack;
    r->step = 0xFF0000 / frames;
    r->done = std::move(done);
    end(Fx::Black);
}

bool Cinematics::barsIn()
{
    // The bars' slide in (0x10, 10).
    Record* r = freeRecord();
    if (!r) {
        return false;
    }
    // Each bar grows to 16% of the back buffer's height (h is 16x it, over 100), in 16.16, over
    // 10 frames.
    const int h = backbufferHeight_ * 16;
    r->frames = 10;
    r->flags = 3;  // running and lasting: held at full height
    r->acc = 0;
    r->done = {};
    r->fx = Fx::Bars;
    r->step = ((h / 100) * 0x10000) / 10;
    return true;
}

bool Cinematics::barsOut()
{
    // The bars' slide out (0x10, 0x1E).
    Record* r = freeRecord();
    if (!r) {
        return false;
    }
    const int h = backbufferHeight_ * 16;
    r->frames = 30;
    r->acc = (h / 100) * 0x10000;
    r->flags = 1;
    r->once = 0;
    r->done = {};
    r->fx = Fx::BarsOut;
    r->step = r->acc / 30;
    return true;
}

void Cinematics::mapIntro(bool immediate)
{
    // cliPlayMapIntro: the fades ended, the map
    // off, then.
    end(Fx::FromBlack);
    end(Fx::ToBlack);
    mapDrawn_ = false;
    blackScreen(immediate);
}

void Cinematics::blackScreen(bool overAll)
{
    // unless a fade from black runs, the black screen (over everything when
    // `overAll`), lasting.
    if (find(Fx::FromBlack)) {
        return;
    }
    Record* r = freeRecord();
    if (!r) {
        return;
    }
    r->flags = overAll ? 7 : 3;  // running and lasting; 4: over the GUI too
    r->frames = 0;
    r->acc = 0;
    r->step = 0;
    r->done = {};
    r->fx = Fx::Black;
}

void Cinematics::stopAllFades()
{
    // video_stopAllFades: (their callbacks run), the
    // black ended, the map drawn. (Game_SetPaused(0): the server's builtin.)
    end(Fx::FromBlack);
    end(Fx::ToBlack);
    end(Fx::Black);
    mapDrawn_ = true;
}

void Cinematics::clearScreen()
{
    stopAllFades();
    end(Fx::Bars);
    end(Fx::BarsOut);
    under_.clear();
    over_.clear();
    if (wideScreen_ || hudHidden_) {
        setHud(false);
    }
}

void Cinematics::blind()
{
    // Blind: Draw_FadeInScreen(25, 0): at black the map off, unless a
    // fade back has begun.
    fadeToBlack(kFadeFrames, false, [this] {
        if (!find(Fx::FromBlack)) {
            mapDrawn_ = false;
        }
    });
}

void Cinematics::setHud(bool hidden)
{
    // The windows (book, inventory, map, console, menus) hidden and the HUD off,
    // or those open before back. Each call does it (no guard: hidden twice, what the second
    // found open is all that comes back; back twice, it comes back twice).
    // (The windows themselves at the next update: the host may call this from its own thread
    // while the main one is parked for a solo save or load (LocalHost), and closing the console or
    // a menu stops SDL's text input, the main thread's.)
    wideScreen_ = hidden;
    if (hooks_.hideHud) {
        deferred_.push_back([this, hidden] { hooks_.hideHud(hidden); });
    }
    hudHidden_ = hidden;
}

void Cinematics::pauseHost(int how, nox::server::PauseReason why)
{
    if (hooks_.hostPause) {
        hooks_.hostPause(how, why);
    }
}

void Cinematics::endGame(int kind)
{
    // the ending's class, TrackData's bit for it; the black over everything
    // (cliPlayMapIntro(1)), unpaused, the HUD back; then the game left
    // (client_quit) and at the menu the class's movie
    // (movies\<Class>.vqa), then the credits over the menu. noxsdl plays the movie
    // from the game and stays in it for the credits, leaving it at their end
    // (a deliberate deviation).
    endKind_ = kind;
    if (hooks_.trackData && hooks_.setTrackData) { // (|= 1 << (kind & 0x1F))
        hooks_.setTrackData(static_cast<int>(static_cast<unsigned>(hooks_.trackData()) | (1u << (static_cast<unsigned>(kind) & 0x1Fu))));
    }
    mapIntro(true);
    pauseHost(-1, nox::server::PauseReason::Other);
    setHud(false);
    // (The game's end clears the records, before the menu's credits.)
    for (auto& r : records_) {
        r.flags = 0;
        r.done = {};
    }
    under_.clear();
    over_.clear();
    // The movie (its texture made) or the credits at the next update, out of the host's call
    // (which may come from its own thread while the main one is parked: LocalHost).
    deferred_.push_back([this, kind] {
        const std::string movie = nox::client::movies::endingMovie(kind);
        if (!movie.empty() && hooks_.playMovie) {
            hooks_.playMovie(movie, [this, alive = std::weak_ptr<int>(alive_)] {
                if (!alive.expired()) {
                    showBriefing(0xFF, 1);
                }
            });
            return;
        }
        showBriefing(0xFF, 1);
    });
}

void Cinematics::drawRecord(Record& r, std::vector<Drawn>& out)
{
    switch (r.fx) {
    case Fx::ToBlack:
        out.push_back({Fx::ToBlack, (r.acc >> 16) & 0xFF});
        r.acc += r.step;
        break;
    case Fx::FromBlack: // (char)(-1 - (char)(acc >> 16))
        out.push_back({Fx::ToBlack, 0xFF - ((r.acc >> 16) & 0xFF)});
        r.acc += r.step;
        break;
    case Fx::Bars:
        if (r.frames != 0) {
            r.acc += r.step;
        }
        out.push_back({Fx::Bars, r.acc >> 16});
        break;
    case Fx::BarsOut:
        if (r.once == 0) {
            end(Fx::Bars);
            r.once = 1;
        }
        r.acc -= r.step;
        out.push_back({Fx::Bars, r.acc >> 16});
        break;
    case Fx::Black:
        out.push_back({Fx::Black, 0});
        break;
    case Fx::None:
        break;
    }
}

void Cinematics::run(int pass)
{
    // A pass's records (0: under the GUI; 1: over everything) drawn and counted down.
    const std::uint32_t want = pass ? 5u : 1u;
    auto& out = pass ? over_ : under_;
    out.clear();
    for (auto& r : records_) {
        if ((r.flags & 5u) != want) {
            continue;
        }
        drawRecord(r, out);
        if (r.frames < 1) {
            if (!(r.flags & 2u)) {
                r.flags &= ~want;
                auto done = std::move(r.done);
                r.done = {};
                if (done) {
                    done();
                }
            }
        } else {
            --r.frames;
        }
    }
}

void Cinematics::drawList(SDL_Renderer* renderer, int width, int height, const std::vector<Drawn>& list) const
{
    // dest - src on the colour, alpha kept: a fade takes its level off every channel.
    static const SDL_BlendMode subtract =
        SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT,
                                   SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (const auto& d : list) {
        switch (d.fx) {
        case Fx::ToBlack: {
            // Each colour less the level, kept at 0.
            if (d.value <= 0) {
                break;
            }
            SDL_SetRenderDrawBlendMode(renderer, subtract);
            const auto v = static_cast<std::uint8_t>(std::min(d.value, 255));
            SDL_SetRenderDrawColor(renderer, v, v, v, 255);
            const SDL_FRect all{0, 0, w, h};
            SDL_RenderFillRect(renderer, &all);
            break;
        }
        case Fx::Bars: {
            if (d.value <= 0 || backbufferHeight_ <= 0) {
                break;
            }
            const float bar = std::round(static_cast<float>(d.value) * h / static_cast<float>(backbufferHeight_));
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            const SDL_FRect bars[2] = {{0, 0, w, bar}, {0, h - bar, w, bar}};
            SDL_RenderFillRects(renderer, bars, 2);
            break;
        }
        case Fx::Black: {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            const SDL_FRect all{0, 0, w, h};
            SDL_RenderFillRect(renderer, &all);
            break;
        }
        default:
            break;
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}

void Cinematics::drawUnderGui(SDL_Renderer* renderer, int width, int height) const
{
    if (mapDrawn_) {
        drawList(renderer, width, height, under_);
    }
}

void Cinematics::drawOverAll(SDL_Renderer* renderer, int width, int height) const
{
    drawList(renderer, width, height, over_);
}

// ---------------------------------------------------------------------------- the frames

void Cinematics::message(std::span<const std::uint8_t> m)
{
    if (!m.empty() && m[0] == 0x2B) {
        // MSG_USE_MAP: the client's map change (gameChangeMap, as the map's
        // loaded and the client's ready): the map off and the black screen over everything, here and now (before
        // our host's map start can fade, StartupScreen's Draw_FadeInScreen then ending at once).
        mapDrawn_ = false;
        blackScreen(true);
        return;
    }
    // MSG_FADE_BEGIN (0xE4) and MSG_CHAPTER_END (0xD6): 3 bytes, handled at the next update.
    if (m.size() >= 3 && (m[0] == 0xE4 || m[0] == 0xD6)) {
        messages_.emplace_back(m.begin(), m.begin() + 3);
    }
    // MSG_GAUNTLET's screens (CliNet_OnPacket: 0x0C 90 bytes, 0x0D / 0x0E 69, 0x1C 3).
    if (m.size() >= 3 && m[0] == 0xF0 && m[1] == 0x1C) {
        messages_.emplace_back(m.begin(), m.begin() + 3);
    } else if (m.size() >= 0x5A && m[0] == 0xF0 && m[1] == 0x0C) {
        messages_.emplace_back(m.begin(), m.begin() + 0x5A);
    } else if (m.size() >= 0x45 && m[0] == 0xF0 && (m[1] == 0x0D || m[1] == 0x0E)) {
        messages_.emplace_back(m.begin(), m.begin() + 0x45);
    }
}

void Cinematics::flushMessages()
{
    while (!messages_.empty()) {
        const auto m = std::move(messages_.front());
        messages_.pop_front();
        handle(m);
    }
}

// One queued message, handled on the main thread (from update).
void Cinematics::handle(const std::vector<std::uint8_t>& m)
{
    SDL_Log("cinematics: message 0x%02X {%u, %u}", m[0], m[1], m[2]);
    if (m[0] == 0xE4) {
        // CliNet_OnPacket's MSG_FADE_BEGIN.
        const bool menu = m[2] == 1;
        if (m[1] == 1) {
            mapDrawn_ = true;
            pauseHost(1, nox::server::PauseReason::Fade);
            fadeFromBlack(kFadeFrames, menu, [this] {
                // unpaused unless a fade to black runs.
                if (!find(Fx::ToBlack)) {
                    pauseHost(0, nox::server::PauseReason::Fade);
                }
            });
        } else {
            fadeToBlack(kFadeFrames, menu, [this] {
                if (!find(Fx::FromBlack)) {
                    mapDrawn_ = false;
                }
            });
        }
    } else if (m[0] == 0xD6) {
        showBriefing(m[1], m[2]); // MSG_CHAPTER_END
    } else if (m[0] == 0xF0 && m[1] == 0x1C) {
        noSkip_ = m[2]; // Cli_SetQuestBriefingNoSkip
    } else if (m[0] == 0xF0 && m[1] == 0x0C) {
        questLevelWon(m);
    } else if (m[0] == 0xF0) {
        questBriefing(m);
    }
}

void Cinematics::update(std::uint64_t nowMs, bool inGame, int backbufferHeight, int playerClass)
{
    nowMs_ = nowMs;
    backbufferHeight_ = backbufferHeight > 0 ? backbufferHeight : 768;
    playerClass_ = std::clamp(playerClass, 0, 2);
    flushMessages();
    gui_.update(nowMs);
    gui::runQueued(deferred_);
    // The client's frames, 30 a second (the exe's main loop's in a solo game).
    const std::uint64_t frame = nowMs * 30 / 1000;
    if (lastFrame_ == 0 || frame < lastFrame_) {
        lastFrame_ = frame;
    }
    // At most 4 frames caught up after a stall.
    const std::uint64_t due = std::min<std::uint64_t>(frame - lastFrame_, 4);
    lastFrame_ = frame;
    for (std::uint64_t i = 0; i < due; ++i) {
        clientFrame(inGame);
    }
}

void Cinematics::clientFrame(bool inGame)
{
    // The world runs the records under the GUI at its end while the map is drawn;
    // then the windows (the briefing's own drawing); then those over everything (mainloop).
    if (inGame && mapDrawn_) {
        run(0);
    } else {
        under_.clear();
    }
    ++clientFrames_;
    briefingFrame();
    run(1);
}

bool Cinematics::screenUp() const
{
    return brief_.shown || loadMenu_ != nullptr || dialog_ != nullptr;
}

bool Cinematics::refuses(std::string_view control) const
{
    // The widescreen (guiCursor) refuses the casts, potions, map, inventory, book, weapon
    // swap, quick save / load and Escape (consoleEsc: a fade too, video_inFadeTransition).
    if (control == "ToggleQuitMenu") {
        return wideScreen_ || find(Fx::FromBlack) || find(Fx::ToBlack) || !mapDrawn_;
    }
    if (!wideScreen_) {
        return false;
    }
    return control.starts_with("InvokeSlot") || control == "ToggleBook" || control == "QuickHealth" ||
           control == "QuickMana" || control == "QuickCurePoison" || control == "ToggleInventory" ||
           control == "ToggleMap" || control == "SwapWeapons" || control == "AutoSave" || control == "AutoLoad" ||
           control == "PlaceTrapBomber";
}

// ---------------------------------------------------------------------------- the briefing

std::string Cinematics::endgameVoice() const
{
    // GetEndgameDialog: by the class finished, what's left to play.
    const int done = hooks_.trackData ? hooks_.trackData() : 0;
    switch (endKind_) {
    case 0:
        if (!(done & 2)) {
            return "gmcap14e.wav";
        }
        if (!(done & 4)) {
            return "gmcap13e.wav";
        }
        break;
    case 1:
        if (!(done & 1)) {
            return "gmcap17e.wav";
        }
        if (!(done & 4)) {
            return "gmcap18e.wav";
        }
        break;
    case 2:
        if (!(done & 2)) {
            return "gmcap16e.wav";
        }
        if (!(done & 1)) {
            return "gmcap15e.wav";
        }
        break;
    default:
        break;
    }
    return {};
}

void Cinematics::showBriefing(int chapter, int variant, int questMode)
{
    // Gui_Brief_ShowLockScreen. First the stage = the chapter + 1 (the
    // one a save's File Info and GameData keep).
    if (hooks_.setStage) {
        hooks_.setStage(static_cast<std::uint8_t>(chapter + 1));
    }
    // Chapters 0-10 are the campaign's; 0xFE marks the quest's screens and 0xFF the end credits.
    if (chapter != 0xFF && chapter != 0xFE && (chapter < 0 || chapter > 10)) {
        return; // (the exe reads past its table)
    }
    briefed_ = true;
    brief_.voiced = true;
    brief_.finished = false;
    brief_.loadOpened = false;
    brief_.shown = true;
    brief_.proc = true;
    brief_.textDrawn = true;
    brief_.chapter = chapter;
    brief_.variant = variant;
    brief_.questMode = 0;
    int music = 0;
    if (chapter == 0xFE) {
        // The quest's: no voice, music 0; 1 the results (MenuSystemBG), 4 the start
        // (GauntletInstructionBackground), else 2 the stage's picture and text.
        brief_.voice.clear();
        brief_.text.clear();
        if (questMode & 1) {
            brief_.questMode = 1;
            brief_.image = "MenuSystemBG";
        } else if (questMode & 4) {
            brief_.questMode = 4;
            brief_.image = "GauntletInstructionBackground";
        } else {
            brief_.questMode = 2;
            brief_.image = quest_.image;
            brief_.text = quest_.text;
        }
        if (hooks_.hideBook) {
            hooks_.hideBook();
        }
    } else if (chapter == 0xFF) {
        brief_.image.clear();
        brief_.text = text("Nox:Credits");
        brief_.voice = endgameVoice();
        brief_.variant = 0xFF;
        music = 0x18;
    } else {
        // The briefing's table: "Briefing:<Class>ChapterBegin<n>" (variant 1) or "...Loss<n>"
        // (0): the picture by that name, the text and its voice.
        char label[64];
        std::snprintf(label, sizeof label, variant == 0 ? "Briefing:%sChapterLoss%d" : "Briefing:%sChapterBegin%d",
                      kClasses[playerClass_], chapter + 1);
        brief_.image = std::string(label).substr(9);
        brief_.text = text(label);
        brief_.voice.clear();
        if (res_.strings) {
            if (const auto* e = res_.strings->find(label); e && !e->variants.empty()) {
                brief_.voice = e->variants.front().extra;
            }
        }
        music = chapterMusic(chapter, playerClass_);
    }
    // The text's window 640 wide and as tall as the text at 480; centred up and down, or the
    // credits from 500 down rolling up till 20 above the top (a step of 1.0 a frame for them,
    // 0 for the others: briefingFrame).
    gui::Painter p(nullptr, res_, 640, 480);
    const int fh = p.fontHeight(true);
    brief_.height = p.textHeight(brief_.text, true, 480);
    if (chapter == 0xFF) {
        brief_.stop = -20 - brief_.height;
        brief_.y = 500;
    } else {
        brief_.stop = ((480 - fh) - brief_.height) / 2;
        brief_.y = static_cast<float>(brief_.stop);
    }
    brief_.waiting = false;
    if (chapter == 0xFE) {
        // (the quest's: nothing to wait for)
    } else if (brief_.variant == 0) {  // variant 0: a death (chapter loss) screen
        // A death screen seen once in this game goes straight to the load menu.
        if (guest_ || !seen_[static_cast<std::size_t>(chapter)]) { // (noxcpp: a mode guest's always shown)
            brief_.waiting = true;
            seen_[static_cast<std::size_t>(chapter)] = true;
        } else {
            brief_.loadOpened = true;
            openLoadMenu();
            brief_.proc = false;
        }
    } else {
        brief_.waiting = true;
    }
    if (audio_) {
        audio_->briefingMusic(music, kMusicBriefing);
    }
    brief_.since = nowMs_;
    brief_.input = false;
    // Fade in; the voice starts once the fade is over.
    fadeFromBlack(kFadeFrames, true, [this] { briefingVoice(); });
    if (brief_.waiting) {
        brief_.voiced = false;
    }
}

void Cinematics::briefingVoice()
{
    // playGMCAPsmth: the fade's over; the voice, if it waits.
    SDL_Log("cinematics: briefing %d / %d in, voice %s", brief_.chapter, brief_.variant, brief_.voice.c_str());
    brief_.input = true;
    if (brief_.waiting) {
        if (audio_ && !brief_.voice.empty()) {
            audio_->dialog(brief_.voice, 100);
        }
        brief_.voiced = true;
    }
}

void Cinematics::stopBriefing()
{
    // The briefing stopped.
    if (audio_) {
        audio_->stopDialog();
    }
    brief_.finished = true;
    brief_.waiting = false;
    brief_.since = 0;
}

void Cinematics::fadeOutOfBriefing()
{
    // (the voice stopped), the map drawn a moment so the fade runs,
    // then off again at black.
    stopBriefing();
    mapDrawn_ = true;
    fadeToBlack(kFadeFrames, true, [this] { briefingOver(); });
    mapDrawn_ = false;
}

void Cinematics::briefingFrame()
{
    // The briefing's frame: the text moved; once in place with its voice done and 15 s gone (or
    // the screen skipped), a death screen opens the load menu, the others fade out.
    if (!brief_.shown || !brief_.textDrawn) {
        return;
    }
    brief_.y -= brief_.chapter == 0xFF ? 1.0f : 0.0f;
    if (brief_.questMode != 0) {
        // The quest's draw functions: the start's puts the book away each frame;
        // each blinks its line.
        if (brief_.questMode == 4 && hooks_.hideBook) {
            hooks_.hideBook();
        }
        questBlink();
    }
    if (static_cast<int>(brief_.y) > brief_.stop || !brief_.voiced) {
        return;
    }
    if (audio_ && audio_->dialogBusy()) {
        return;
    }
    if (brief_.since != 0 && nowMs_ - brief_.since <= 15000) {
        return; // (a start of 0: the exe's clock is long past it)
    }
    if (!brief_.finished && (brief_.questMode & 5) != 0) {
        return; // the quest's start and results wait for their click
    }
    if (brief_.questMode == 2 && quest_.bossSound) {
        res_.sound(0x246);
    }
    stopBriefing();
    if (brief_.variant == 0 && !guest_) { // (noxcpp: a mode guest's fades back: the host loads)
        if (!brief_.loadOpened) {
            brief_.loadOpened = true;
            openLoadMenu();
            brief_.proc = false;
        }
    } else {
        fadeOutOfBriefing();
    }
    brief_.textDrawn = false;
}

void Cinematics::briefingInput()
{
    // client_wndQuestBriefProc: once faded in, any key or click, unless our quest save
    // is on its way.
    if (!brief_.proc || !brief_.input || noSkip_ == 1) {
        return;
    }
    stopBriefing();
    if (brief_.variant == 0 && !guest_) {
        brief_.loadOpened = true;
        openLoadMenu();
    } else if (brief_.variant == 0xFF || (brief_.variant == 0 && guest_)) {
        fadeOutOfBriefing();
    }
    brief_.proc = false;
}

void Cinematics::briefingOver()
{
    // the screen gone, its music's end (the music before it back), the map drawn; the credits
    // reset the ending's kind and end the game (in the exe they're at the menu, which comes back:
    // the briefing window destroyed, the main menu and its background shown again; the main
    // menu's own credits, with no quitGame hook, just stop drawing over it); then from black over
    // it, unpaused at the end.
    brief_.shown = false;
    brief_.proc = false;
    stopBriefing();
    if (audio_) {
        audio_->endBriefingMusic();
    }
    mapDrawn_ = true;
    if (brief_.variant == 0xFF) {
        endKind_ = -1;
        if (hooks_.quitGame) {
            deferred_.push_back(hooks_.quitGame);
        }
    } else if (brief_.questMode & 5) {
        showBriefing(0xFE, 1, 2); // the quest's start or results: the stage's screen next
        return;
    }
    fadeFromBlack(kFadeFrames, true, [this] {
        // The briefing stopped, its flag cleared, Game_SetPaused(0).
        stopBriefing();
        briefed_ = false;
        pauseHost(0, nox::server::PauseReason::Chapter);
    });
}

void Cinematics::drawBriefing(SDL_Renderer* renderer, int width, int height)
{
    // The full-screen black window, Briefing.wnd's 640 x 480 in it: the picture, and the text
    // (inside the picture's rectangle; wndStaticDrawNoImage: centred, each line on its own,
    // wrapped at 640, a pixel between lines, white, the large font).
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    const SDL_FRect all{0, 0, static_cast<float>(width), static_cast<float>(height)};
    SDL_RenderFillRect(renderer, &all);
    gui::Painter p(renderer, res_, width, height);
    if (!brief_.image.empty()) {
        p.image(brief_.image, 0, 0);
    }
    // The quest's three screens draw their own text over the picture.
    if (brief_.questMode != 0) {
        if (brief_.textDrawn) {
            if (brief_.questMode == 1) {
                drawQuestResults(p);
            } else if (brief_.questMode == 2) {
                drawQuestStage(p);
            } else {
                drawQuestStart(p);
            }
        }
        return;
    }
    if (!brief_.textDrawn || brief_.text.empty()) {
        return;
    }
    // The text is clipped to the 640 x 480 layout.
    const SDL_FRect area = p.toOutput(0, 0, 640, 480);
    const SDL_Rect clip{static_cast<int>(area.x), static_cast<int>(area.y), static_cast<int>(area.w),
                        static_cast<int>(area.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    // The lines: words while they fit in 640, a newline ending one.
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    const auto flushWord = [&] {
        if (word.empty()) {
            return;
        }
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && p.textWidth(tried, true) > 640) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
        word.clear();
    };
    // Words split at spaces and lines at newlines; carriage returns are dropped.
    for (const char ch : brief_.text) {
        if (ch == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else if (ch == ' ') {
            flushWord();
        } else if (ch != '\r') {
            word += ch;
        }
    }
    flushWord();
    if (!line.empty()) {
        lines.push_back(line);
    }
    // The block centred on the text window's middle; brief_.y is that window's top (it rolls up for the credits).
    const int fh = p.fontHeight(true);
    const int total = static_cast<int>(lines.size()) * fh;
    int y = static_cast<int>(brief_.y) + brief_.height / 2 - total / 2;
    const nox::gui::Color white{255, 255, 255, false};
    for (const auto& l : lines) {
        const int lw = p.textWidth(l, true);
        p.text(l, (640 - lw) / 2, y, true, white);
        y += fh + 1;
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

// ------------------------------------------------------------------------- the quest's screens

namespace {

// The exe's own named colours that these screens draw in.
const nox::gui::Color kQuestWhite{0xFF, 0xFF, 0xFF, false};  // its white
const nox::gui::Color kQuestBlack{0, 0, 0, false};           // its black
const nox::gui::Color kQuestOrange{0xF0, 0xB4, 0x2A, false}; // its orange
const nox::gui::Color kQuestGreen{0x80, 0xFF, 0x80, false};  // its green
const nox::gui::Color kQuestBlue{0, 0xA0, 0xFF, false};      // its blue

/// The results' six places (x, y each).
constexpr std::array<SDL_Point, 6> kQuestSlots{{{120, 55}, {290, 55}, {460, 55}, {120, 235}, {290, 235}, {460, 235}}};

using nox::util::u16At;

/// A C string from `at` to its 0 or the message's end.
std::string questString(const std::vector<std::uint8_t>& m, std::size_t at)
{
    std::string s;
    for (std::size_t i = at; i < m.size() && m[i] != 0; ++i) {
        s += static_cast<char>(m[i]);
    }
    return s;
}

/// The string's first %d given the number (the exe's swprintf).
std::string questFormat(std::string fmt, long long value)
{
    if (const auto at = fmt.find("%d"); at != std::string::npos) {
        fmt.replace(at, 2, std::to_string(value));
    }
    return fmt;
}

/// The characters of a UTF-8 string (wcslen).
std::size_t utf8Length(const std::string& s)
{
    return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (c & 0xC0) != 0x80; }));
}

} // namespace

void Cinematics::questBriefing(const std::vector<std::uint8_t>& m)
{
    // Gui_Brief_ShowQuestStart (0x0E) / Gui_Brief_ShowQuestNextLevel (0x0D):
    // the picture named at 5 (WarriorChapterBegin8 when thing.bin hasn't it), the
    // text by the label at 37 (none: an empty one), the stage at 2. 0x0D keeps its flag 2 (the
    // stage's screen sounds 0x246 at its end) and shows at once with its flag 1; 0x0E always.
    // (closes the server's message of the day: noxsdl shows none.)
    quest_.bossSound = false;
    if (hooks_.hideBook) {
        hooks_.hideBook();
    }
    const std::string image = questString(m, 5);
    quest_.image = gui::imageExists(res_.things, image) ? image : "WarriorChapterBegin8";
    const std::string label = questString(m, 37);
    quest_.text = label.empty() ? std::string() : gui_.text(label, "GUIBrief.c");
    quest_.stage = u16At(m, 2);
    SDL_Log("cinematics: quest 0x%02X stage %d, %s, %s", m[1], quest_.stage, image.c_str(), label.c_str());
    if (m[1] == 0x0E) {
        showBriefing(0xFE, 1, 4);
        return;
    }
    if (m[4] & 2) {
        quest_.bossSound = true;
    }
    if (m[4] & 1) {
        showBriefing(0xFE, 1, 2);
    }
}

void Cinematics::questLevelWon(const std::vector<std::uint8_t>& m)
{
    // Gui_Brief_ShowQuestLevelWon: the map's secrets (2) and stage (4); six 14-byte
    // records from 6 {u16 player, obelisks, secrets, secrets credited, kills, u32 score}, each
    // with a player kept in its own place; the first as many as there were sorted by score, most
    // first (by qsort: MSVC's shortsort below 9); the labels' width measured once
    // (at most 85); the results' screen.
    quest_.won = {};
    quest_.secretsTotal = u16At(m, 2);
    quest_.wonStage = u16At(m, 4);
    int count = 0;
    for (std::size_t i = 0; i < quest_.won.size(); ++i) {
        const std::size_t at = 6 + i * 14;
        const std::uint16_t netCode = u16At(m, at);
        if (netCode == 0) {  // an unused record stays blank in its place
            continue;
        }
        auto& e = quest_.won[i];
        const auto name = hooks_.playerName ? hooks_.playerName(netCode) : std::nullopt;
        e.player = name.has_value();
        e.name = name.value_or(std::string());
        e.netCode = netCode;
        e.generators = u16At(m, at + 2);
        e.secrets = u16At(m, at + 4);
        e.credited = u16At(m, at + 6);
        e.kills = u16At(m, at + 8);
        e.score = static_cast<std::uint32_t>(u16At(m, at + 10) | (u16At(m, at + 12) << 16));  // the u32 score in two u16 halves, low first
        ++count;
    }
    // qsort's sense: above 0 when `a` sorts after `b` (a lower score).
    const auto compare = [](const Quest::Entry& a, const Quest::Entry& b) {
        return a.score == b.score ? 0 : (a.score < b.score ? 1 : -1);
    };
    // A selection sort from the end, the way that shortsort orders so few records, so equal scores
    // come out in the exe's order.
    for (int hi = count - 1; hi > 0; --hi) {
        int most = 0;
        for (int i = 1; i <= hi; ++i) {
            if (compare(quest_.won[static_cast<std::size_t>(i)], quest_.won[static_cast<std::size_t>(most)]) > 0) {
                most = i;
            }
        }
        std::swap(quest_.won[static_cast<std::size_t>(most)], quest_.won[static_cast<std::size_t>(hi)]);
    }
    if (quest_.labelWidth == 0) {
        gui::Painter p(nullptr, res_, 640, 480);
        for (const char* label : {"GUIBrief.c:GeneratorsDestroyed", "GUIBrief.c:Kills", "GUIBrief.c:numSecretsFound",
                                  "GUIBrief.c:TotalScore"}) {
            quest_.labelWidth = std::max(quest_.labelWidth, p.textWidth(text(label), true));
        }
        quest_.labelWidth = std::min(quest_.labelWidth, 85);
    }
    showBriefing(0xFE, 1, 1);
}

void Cinematics::questBlink()
{
    // Each draw function's own flag (1 at first): at every 30th frame, on, it goes off and the
    // line isn't drawn, off, it comes on; between, the line drawn while it's on.
    bool& on = quest_.blink[brief_.questMode == 4 ? 0 : brief_.questMode == 2 ? 1 : 2];
    quest_.blinkShown = false;
    if (clientFrames_ % 30 == 0) {
        if (on) {
            on = false;
            return;
        }
        on = true;
    } else if (!on) {
        return;
    }
    quest_.blinkShown = true;
}

void Cinematics::drawThing(gui::Painter& p, const char* type, int x, int y)
{
    // The type's drawables (enabled), put at the screen point and drawn by
    // their draw functions.
    const TypeArt* a = art_.get(type);
    if (!a) {
        return;
    }
    Pose pose;
    pose.time = static_cast<double>(clientFrames_);
    const nox::things::ImageRef* ref = pickImage(*a, pose);
    if (!ref && a->still) {
        ref = &*a->still;
    }
    if (ref) {
        p.sprite(*ref, x - static_cast<int>(a->halfWidth), y - static_cast<int>(a->halfHeight) - static_cast<int>(a->z));
    }
}

void Cinematics::drawQuestStart(gui::Painter& p)
{
    // At the 640 x 480's corner: "HOW TO PLAY" (the window's font) outlined in
    // black, then the things a quest has with their words in the default font, the orange part
    // and then the white 4 pixels on.
    const std::string title = text("GeneralPrint:QuestSplash1");
    const int tx = 320 - p.textWidth(title, true) / 2;
    // The title outlined: black at the four diagonal offsets, then orange on top.
    for (const SDL_Point d : {SDL_Point{-1, -1}, SDL_Point{1, -1}, SDL_Point{-1, 1}, SDL_Point{1, 1}}) {
        p.text(title, tx + d.x, 20 + d.y, true, kQuestBlack);
    }
    p.text(title, tx, 20, true, kQuestOrange);
    // From (x, y), the white wrapped at `right`.
    const auto words = [&](const char* a, const char* b, int x, int y, int right) {
        const std::string sa = text(a);
        p.text(sa, x, y, false, kQuestOrange);
        x += 4 + p.textWidth(sa, false);
        p.text(text(b), x, y, false, kQuestWhite, right - x);
    };
    // Ending at `right` on one line when both are under `fits` wide, else from x, wrapped.
    const auto rightWords = [&](const char* a, const char* b, int right, int fits, int x, int y) {
        const std::string sa = text(a);
        const std::string sb = text(b);
        const int wa = p.textWidth(sa, false);
        const int wb = p.textWidth(sb, false);
        if (wb + wa < fits) {
            p.text(sa, right - wb - wa - 4, y, false, kQuestOrange);
            p.text(sb, right - wb, y, false, kQuestWhite);
        } else {
            p.text(sa, x, y, false, kQuestOrange);
            const int bx = x + 4 + wa;
            p.text(sb, bx, y, false, kQuestWhite, right - bx);
        }
    };
    const auto centredWords = [&](const char* a, const char* b, int y) {
        const std::string sa = text(a);
        const std::string sb = text(b);
        const int wa = p.textWidth(sa, false);
        const int x = 320 - (wa + p.textWidth(sb, false)) / 2;
        p.text(sa, x, y, false, kQuestOrange);
        p.text(sb, x + 4 + wa, y, false, kQuestWhite);
    };
    // Each thing a quest has, drawn at a fixed layout point beside its two-part caption.
    drawThing(p, "BeholderGenerator", 73, 123);
    words("GeneralPrint:QuestSplash2a", "GeneralPrint:QuestSplash2b", 109, 76, 520);
    drawThing(p, "GauntletExitB", 565, 117);
    rightWords("GeneralPrint:QuestSplash3a", "GeneralPrint:QuestSplash3b", 520, 391, 199, 115);
    drawThing(p, "SoulGate", 133, 192);
    // (its wrap at the screen's top corner less its left, + 630: an original bug)
    words("GeneralPrint:QuestSplash4a", "GeneralPrint:QuestSplash4b", 157, 156, 630);
    drawThing(p, "Ankh", 525, 222);
    rightWords("GeneralPrint:QuestSplash7a", "GeneralPrint:QuestSplash7b", 500, 216, 250, 198);
    drawThing(p, "WarHammer", 182, 262);
    drawThing(p, "ConjurerSpellBook", 201, 251);
    drawThing(p, "HastePotion", 185, 234);
    words("GeneralPrint:QuestSplash5a", "GeneralPrint:QuestSplash5b", 221, 240, 470);
    drawThing(p, "QuestGoldChest", 484, 278);
    drawThing(p, "QuestGoldPile", 503, 303);
    rightWords("GeneralPrint:QuestSplash6a", "GeneralPrint:QuestSplash6b", 462, 351, 113, 286);
    drawThing(p, "GoldKey", 186, 333);
    drawThing(p, "SilverKey", 219, 345);
    drawThing(p, "DunMirChest4", 220, 322);
    words("GeneralPrint:QuestSplash8a", "GeneralPrint:QuestSplash8b", 241, 330, 550);
    centredWords("GeneralPrint:QuestSplash9a", "GeneralPrint:QuestSplash9b", 370);
    centredWords("GeneralPrint:QuestSplash10a", "GeneralPrint:QuestSplash10b", 395);
    centredWords("GeneralPrint:QuestSplash11a", "GeneralPrint:QuestSplash11b", 420);
    if (quest_.blinkShown) {
        // (measured in the default font and drawn in the window's, as the exe does)
        const std::string click = text("GeneralPrint:QuestSplash12");
        p.text(click, 320 - p.textWidth(click, false) / 2, 450, true, kQuestWhite);
    }
}

void Cinematics::drawQuestStage(gui::Painter& p)
{
    // White in the window's font over the stage's picture: "Stage: n" centred
    // 3 lines' height down, the text 3 lines' height up from the bottom, the blinking line at 462.
    const int fh = p.fontHeight(true);
    const std::string stage = text("Noxworld.c:Stage") + " " + std::to_string(quest_.stage);
    p.text(stage, 320 - p.textWidth(stage, true) / 2, 240 + (fh - 80) * 3, true, kQuestWhite);
    if (!quest_.text.empty()) {
        // Split at newlines; the block is centred on its widest line.
        std::vector<std::string> lines;
        std::size_t from = 0;
        for (std::size_t at; (at = quest_.text.find('\n', from)) != std::string::npos; from = at + 1) {
            lines.push_back(quest_.text.substr(from, at - from));
        }
        lines.push_back(quest_.text.substr(from));
        int w = 0;
        for (const auto& l : lines) {
            w = std::max(w, p.textWidth(l, true));
        }
        const int h = static_cast<int>(lines.size()) * fh;
        int y = 240 + (80 - h) * 3;
        for (const auto& l : lines) {
            p.text(l, 320 - w / 2, y, true, kQuestWhite);
            y += fh;
        }
    }
    if (quest_.blinkShown) {
        const std::string click = text("GeneralPrint:QuestSplash12");
        p.text(click, 320 - p.textWidth(click, true) / 2, 462, true, kQuestWhite);
    }
}

void Cinematics::drawQuestResults(gui::Painter& p)
{
    // In the window's font over MenuSystemBG: "Game Results - Stage: n", then each
    // player in its place: "<place>) <name>" in orange (shortened to under 154 wide), and its
    // obelisks, secrets, kills (green) and score (blue) by their labels a line and a half apart
    // (half a line more under the name); then the map's secrets, ours and our friends', and the
    // blinking line.
    const int fh = p.fontHeight(true);
    const std::string title = text("GUIBrief.c:GauntletStatTitle") + " - " + text("Noxworld.c:Stage") + " " +
                              std::to_string(quest_.wonStage);
    p.text(title, 320 - p.textWidth(title, true) / 2, fh, true, kQuestWhite);
    // (- the second place's x less the first's y, where the name's limit
    // takes the first's x)
    const int column = 290 - 55;
    const int step = static_cast<int>(static_cast<float>(fh) * 1.5f);
    const int labelW = quest_.labelWidth;
    const std::uint16_t own = hooks_.ownNetCode ? hooks_.ownNetCode() : 0;
    int players = 0;
    std::uint32_t ours = 0;
    std::uint32_t friends = 0;
    // Each player record in its fixed place (kQuestSlots), adding up whose secrets were credited.
    for (std::size_t i = 0; i < quest_.won.size(); ++i) {
        const auto& e = quest_.won[i];
        if (!e.player) {
            continue;
        }
        int x = kQuestSlots[i].x;
        int y = kQuestSlots[i].y;
        ++players;
        if (e.netCode == own) {
            ours = e.credited;
        } else {
            friends += e.credited;
        }
        std::string name = std::to_string(i + 1) + ") " + e.name;
        const int limit = (290 - 120) - 16 + x;
        // Shortened a character at a time while too wide, keeping at least 6.
        while (limit <= p.textWidth(name, true) + x && utf8Length(name) >= 6) {
            // (the last character off)
            while ((static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) {
                name.pop_back();
            }
            name.pop_back();
        }
        p.text(p.firstLine(name, true, column - 8), x, y, true, kQuestOrange);
        y += step / 2 + step;
        // A label in white, then its value in the value's colour.
        const auto row = [&](const char* label, long long value, const nox::gui::Color& c) {
            p.text(p.firstLine(text(label), true, labelW), x, y, true, kQuestWhite);
            p.text(p.firstLine(" " + std::to_string(value), true, column - labelW - 8), x + labelW, y, true, c);
        };
        row("GUIBrief.c:GeneratorsDestroyed", e.generators, kQuestGreen);
        y += step;
        row("GUIBrief.c:numSecretsFound", e.secrets, kQuestGreen);
        y += step;
        row("GUIBrief.c:Kills", e.kills, kQuestGreen);
        y += step;
        row("GUIBrief.c:TotalScore", static_cast<std::int32_t>(e.score), kQuestBlue);
    }
    // The map's secret count, then ours and (with two or more players) our friends'.
    const std::string total = questFormat(text("GeneralPrint:SecretsTotal"), quest_.secretsTotal);
    p.text(total, 320 - p.textWidth(total, true) / 2, (150 - fh) * 3, true, kQuestOrange);
    std::string found = ours == 0 ? text("GeneralPrint:SecretsNoneFound") : questFormat(text("GeneralPrint:SecretsFound"), ours);
    if (players >= 2) {
        found += " - " + (friends == 0 ? text("GeneralPrint:SecretsNoneFoundByFriends")
                                       : questFormat(text("GeneralPrint:SecretsFoundByFriends"), friends));
    }
    p.text(found, 320 - p.textWidth(found, true) / 2, (225 - fh) * 2, true, kQuestOrange);
    if (quest_.blinkShown) {
        const std::string click = text("GeneralPrint:QuestSplash12");
        p.text(click, 320 - p.textWidth(click, true) / 2, 450, true, kQuestWhite);
    }
}

void Cinematics::drawScreens(SDL_Renderer* renderer, int width, int height)
{
    if (brief_.shown) {
        drawBriefing(renderer, width, height);
    }
    if (loadMenu_ || dialog_) {
        gui_.draw(renderer, width, height);
    }
}

bool Cinematics::handleEvent(const SDL_Event& event, int width, int height)
{
    if (event.type == SDL_EVENT_QUIT) {
        return false;
    }
    if (loadMenu_ || dialog_) {
        gui_.handleEvent(event, width, height);
        return true;
    }
    if (brief_.shown) {
        // A modal window over everything; its procedure (while set) takes all but the pointer's
        // coming and going (0x11 / 0x12).
        if ((event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            briefingInput();
        }
        return event.type != SDL_EVENT_KEY_UP && event.type != SDL_EVENT_MOUSE_BUTTON_UP;
    }
    return false;
}

// ----------------------------------------------------------------------- the save / load menu

bool Cinematics::dyingOrDead() const
{
    // playerAnimCheck: no drawable, or its state 1, 2 or 0x33.
    const int s = hooks_.animState ? hooks_.animState() : -1;
    return s < 0 || s == 1 || s == 2 || s == 0x33;
}

int Cinematics::selected() const
{
    const gui::Window* names = loadMenu_ ? loadMenu_->find(kNames) : nullptr;
    return names ? names->selectedLine : -1;
}

void Cinematics::openLoadMenu()
{
    // savegame_sub (not in a quest): the window (savegame_sub: selchar.wnd,
    // at the top, centred; the arrows hidden; Save and Load named) filled with the saved games,
    // shown modal with the input; Save on unless a briefing's up that isn't a startup screen's
    // (g_briefVariant 0: a death's) or our player is dying or dead (animation state 2, 1 or 0x33); Cancel
    // "Quit" and the label "Load Game" while a briefing's up, else "Cancel" and "Save/Load Game";
    // paused.
    if (!loadMenu_) {
        loadMenu_ = gui_.load(gameDir_ / "window" / "selchar.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return message == gui::Clicked && onLoadMenu(from);
        });
        if (!loadMenu_) {
            return;
        }
        if (gui::Window* lists = loadMenu_->find(kLists)) {
            lists->proc = loadMenu_->proc;
        }
        for (const auto id : {kUp, kDown}) {
            if (gui::Window* w = loadMenu_->find(id)) {
                w->show(false);
            }
        }
        if (gui::Window* w = loadMenu_->find(kSave)) {
            w->text = gui_.text("SaveGUISave", "GUISave.c");
        }
        if (gui::Window* w = loadMenu_->find(kLoad)) {
            w->text = gui_.text("SaveGUILoad", "GUISave.c");
        }
    }
    fillLoadMenu();
    gui_.setModal(dialog_ ? dialog_ : loadMenu_);
    const int state = hooks_.animState ? hooks_.animState() : -1;
    enable(loadMenu_, kSave, (!briefed_ || brief_.variant != 0) && state != 2 && state != 1 && state != 0x33);
    if (gui::Window* w = loadMenu_->find(kCancel)) {
        w->text = gui_.text(briefed_ ? "SaveGUIQuit" : "SaveGUICancel", "GUISave.c");
    }
    if (gui::Window* w = loadMenu_->find(kLabel)) {
        w->text = gui_.text(briefed_ ? "LoadLabel" : "SaveLoadLabel", "GUISave.c");
    }
    pauseHost(1, nox::server::PauseReason::Menu);
}

bool Cinematics::closeLoadMenu()
{
    // The load menu hidden, its input let go, unpaused (when it was up).
    if (!loadMenu_) {
        return false;
    }
    gui_.destroy(loadMenu_);
    loadMenu_ = nullptr;
    slots_.clear();
    gui_.setModal(dialog_);
    pauseHost(0, nox::server::PauseReason::Menu);
    return true;
}

void Cinematics::fillLoadMenu()
{
    // savegame_sub: the three lists emptied; each of the 14 slots read;
    // a line in each list per slot, in its colour: 0x1FE " ", 0x1FF the date and
    // time, 0x200 "<name> the <class>" with " - " and the place its stage names (AUTOSAVE: " "
    // and "Auto Save"); an empty one " " / EmptySlot / EmptySlot, grey (AUTOSAVE's " " all
    // three). Cancel and Load on with a saved game, both off without (Save as it was); the newest
    // selected in the three.
    gui::Window* lists = loadMenu_->find(kLists);
    gui::Window* dates = loadMenu_->find(kNames);
    gui::Window* names = loadMenu_->find(kInfo);
    if (!lists || !dates || !names) {
        return;
    }
    lists->lines.clear();
    dates->lines.clear();
    names->lines.clear();
    lists->selectedLine = dates->selectedLine = names->selectedLine = -1;
    slots_ = nox::client::solo_saves::list(saveRoot_.empty() ? gameDir_ : saveRoot_);
    int saved = 0;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        const auto& slot = slots_[static_cast<std::size_t>(i)];
        const nox::gui::Color c = palette(nox::client::solo_saves::lineColor(slot));
        if (!slot.exists) {
            const std::string empty = i == 0 ? " " : gui_.text("EmptySlot", "GUISave.c");
            lists->lines.push_back({" ", c});
            dates->lines.push_back({empty, c});
            names->lines.push_back({empty, c});
            continue;
        }
        ++saved;
        const int cls = static_cast<int>(slot.info.characterClass);
        std::string line = format(gui_.text("the", "GUISave.c"),
                                  {nox::strings::toUtf8(slot.info.name),
                                   text("nox.str:" + std::string(nox::client::solo_saves::className(cls)))});
        if (slot.name == "AUTOSAVE") {
            line += " " + gui_.text("AutoSaveLabel", "GUISave.c");
        } else {
            line += " - " + text(std::string(nox::client::solo_saves::stageLabel(slot.info.questStage, cls)));
        }
        lists->lines.push_back({" ", c});
        dates->lines.push_back({nox::client::solo_saves::whenText(slot.info.time), c});
        names->lines.push_back({line, c});
    }
    enable(loadMenu_, kCancel, saved > 0);
    enable(loadMenu_, kLoad, saved > 0);
    const int newest = nox::client::solo_saves::latest(slots_);
    if (newest != -1) {
        lists->selectedLine = dates->selectedLine = names->selectedLine = newest;
    }
}

bool Cinematics::onLoadMenu(gui::Window& from)
{
    // savegame_sub: ButtonPress for any button, then by its id.
    gui::Window* lists = loadMenu_->find(kLists);
    gui::Window* dates = loadMenu_->find(kNames);
    gui::Window* names = loadMenu_->find(kInfo);
    switch (from.id) {
    case kLists:
    case kNames:
    case kInfo:
        // (0x4010: the choice told to the three lists, 0x4013)
        for (gui::Window* l : {lists, dates, names}) {
            if (l) {
                l->selectedLine = from.selectedLine;
            }
        }
        return true;
    case kSave: {
        res_.sound(0x2FE);
        const int i = selected();
        if (i < 0 || i >= static_cast<int>(slots_.size())) {
            return true;
        }
        if (hooks_.deadFlag && hooks_.deadFlag()) {
            enable(loadMenu_, kSave, false); // (our drawable flagged DEAD: Save off, the menu stays)
            return true;
        }
        const auto& slot = slots_[static_cast<std::size_t>(i)];
        if (slot.exists) {
            // A saved game there: asked first (Yes: No), the menu
            // letting go of the input meanwhile.
            overwrite_ = slot.name; // (the folder's name from its path)
            dialog(DialogParent::SaveMenu, gui_.text("GUISave.c:OverwriteSaveTitle"), gui_.text("GUISave.c:OverwriteSaveMessage"),
                   0x38,
                   [this] {
                       if (hooks_.requestSave) {
                           hooks_.requestSave(overwrite_);
                       }
                       closeLoadMenu();
                   },
                   {});
            return true;
        }
        // An empty slot: AUTOSAVE or SAVE%04d asked for at once, the menu shut.
        const std::string name = nox::client::solo_saves::slotName(i);
        deferred_.push_back([this, name] {
            if (hooks_.requestSave) {
                hooks_.requestSave(name);
            }
            closeLoadMenu();
        });
        return true;
    }
    case kLoad: {
        res_.sound(0x2FE);
        const int i = selected();
        if (i < 0 || i >= static_cast<int>(slots_.size()) || !slots_[static_cast<std::size_t>(i)].exists) {
            res_.sound(0x39D); // NoCanDo
            return true;
        }
        // Dying or dead: loaded at once, without asking.
        if (dyingOrDead()) {
            deferred_.push_back([this] { loadSlot(); });
            return true;
        }
        // Alive: asked first (Yes / No, not closed by itself: savegame_sub closes it,
        // Gui_Dialog_Close the No).
        dialog(DialogParent::None, gui_.text("SelChar.c:LoadLabel"), gui_.text("GUIQuit.c:ReallyLoadMessage"), 0x18,
               [this] { loadSlot(); }, [this] { dialogClose(); });
        return true;
    }
    case kCancel:
        res_.sound(0x2FE);
        deferred_.push_back([this] {
            if (briefed_) {
                // Quit: the briefing's voice and music over, the game left (Game_ExitMainLoop),
                // the menu shut.
                stopBriefing();
                if (audio_) {
                    audio_->endBriefingMusic();
                }
                brief_.shown = false;
                closeLoadMenu();
                if (hooks_.quitGame) {
                    hooks_.quitGame();
                }
            } else {
                closeLoadMenu();
            }
        });
        return true;
    default:
        res_.sound(0x2FE); // (a dialog's buttons heard too: windowDlgProc forwards them here)
        return false;
    }
}

void Cinematics::loadSlot()
{
    // savegame_sub: the selected slot (by its path's folder) loaded on
    // our host (its load plays the map intro through its host client); failing, an error
    // box, which the Gui_Dialog_Close after closes at once (an original bug); after a briefing
    // the fade out of it; the menu shut.
    const int i = selected();
    if (i < 0 || i >= static_cast<int>(slots_.size()) || !slots_[static_cast<std::size_t>(i)].exists) {
        return;
    }
    const std::string name = slots_[static_cast<std::size_t>(i)].name;
    const bool ok = hooks_.loadGame && hooks_.loadGame(name);
    if (!ok) {
        dialog(DialogParent::None, gui_.text("SaveErrorTitle", "GUISave.c"), gui_.text("SaveErrorTitle", "GUISave.c"), 0x21, {}, {});
    }
    if (briefed_ && brief_.shown) {
        fadeOutOfBriefing();
    }
    dialogClose();
    closeLoadMenu();
}

void Cinematics::autoLoadNow()
{
    // The autosave loaded at once (an error box when it fails).
    pauseHost(0, nox::server::PauseReason::Menu);
    const bool ok = hooks_.loadGame && hooks_.loadGame("AUTOSAVE");
    if (!ok) {
        dialog(DialogParent::None, gui_.text("GUISave.c:SaveErrorTitle"), gui_.text("GUISave.c:SaveErrorTitle"), 0x21, {}, {});
    }
}

void Cinematics::autoLoadFromMenu()
{
    // menuGameOnButton's 0x2329, after the menu's closed.
    pauseHost(1, nox::server::PauseReason::Menu);
    if (!dyingOrDead()) {
        dialog(DialogParent::None, gui_.text("SelChar.c:LoadLabel"), gui_.text("GUIQuit.c:ReallyLoadMessage"), 0x38,
               [this] { autoLoadNow(); }, [this] { pauseHost(0, nox::server::PauseReason::Menu); });
        return;
    }
    autoLoadNow();
}

void Cinematics::autoLoadKey()
{
    // Cli_ProcessControlEvents' 0x37.
    res_.sound(0x399);
    pauseHost(1, nox::server::PauseReason::Menu);
    dialog(DialogParent::None, gui_.text("SelChar.c:LoadLabel"), gui_.text("GUIQuit.c:ReallyLoadMessage"), 0x38,
           [this] {
               pauseHost(0, nox::server::PauseReason::Menu); // the game load of "AUTOSAVE", no box
               if (hooks_.loadGame) {
                   hooks_.loadGame("AUTOSAVE");
               }
           },
           [this] { pauseHost(0, nox::server::PauseReason::Menu); });
}

// ----------------------------------------------------------------------------- the message box

void Cinematics::dialog(DialogParent parent, const std::string& title, const std::string& message, std::uint32_t flags,
                        std::function<void()> onOk, std::function<void()> onCancel)
{
    // Gui_Dialog_Create: one already up has its cancel run (its window kept, its flags, title and
    // text changed); else dlg.wnd made, centred, modal.
    if (dialog_ && dialogState_.onCancel) {
        auto old = dialogState_.onCancel;
        old();
    }
    dialogState_.parent = parent;
    dialogState_.onOk = std::move(onOk);
    dialogState_.onCancel = std::move(onCancel);
    dialogState_.flags = flags;
    if (!dialog_) {
        dialog_ = gui_.load(gameDir_ / "window" / "dlg.wnd", [this](gui::Window&, int msg, gui::Window& from) {
            if (msg != gui::Clicked) {
                return false;
            }
            const bool ok = from.id == kDlgOk || from.id == kDlgYes;
            if (!ok && from.id != kDlgNo && from.id != kDlgCancel) {
                return false;
            }
            deferred_.push_back([this, ok] { dialogAnswer(ok); });
            return true;
        });
        if (!dialog_) {
            return;
        }
        gui::centreOnScreen(*dialog_);
        dialog_->status |= nox::gui::status::Above;
    }
    if (gui::Window* t = dialog_->find(kDlgTitle)) {
        t->text = title;
    }
    if (gui::Window* t = dialog_->find(kDlgText)) {
        t->text = message;
    }
    // The buttons by the flags.
    const std::pair<std::uint32_t, std::int32_t> buttons[] = {{1, kDlgOk}, {2, kDlgCancel}, {8, kDlgYes}, {0x10, kDlgNo}};
    for (const auto& [bit, id] : buttons) {
        if (gui::Window* b = dialog_->find(id)) {
            b->show((flags & bit) != 0);
        }
    }
    gui_.setModal(dialog_);
}

void Cinematics::dialogAnswer(bool ok)
{
    // windowDlgProc: the click told to the parent, the answer's callback, then
    // closed when the flags (as they are now) say so.
    if (!dialog_) {
        return;
    }
    if (dialogState_.parent == DialogParent::SaveMenu && loadMenu_) {
        res_.sound(0x2FE); // (savegame_sub's ButtonPress)
    }
    auto then = ok ? dialogState_.onOk : dialogState_.onCancel;
    if (then) {
        then();
    }
    if (dialogState_.flags & 0x20u) {  // 0x20: closed after an answer
        dialogClose();
    }
}

void Cinematics::dialogClose()
{
    // Gui_Dialog_Close: the window gone, the input back to what had it.
    if (!dialog_) {
        return;
    }
    gui_.destroy(dialog_);
    dialog_ = nullptr;
    dialogState_.parent = DialogParent::None;
    gui_.setModal(loadMenu_);
}

// ------------------------------------------------------------------- the client's .plr sections

bool Cinematics::saveClientSections(const std::filesystem::path& plr, const nox::server::HostClient::ClientSave& info,
                                    std::uint32_t gameFlags)
{
    // An award still flying lands now; the file opened to append
    // (a missing one fails); the GUI section (the bars, the set and trap set chosen, the host
    // Player's summon order), the File Info (parseFileInfoData's write) and the
    // music (plr_rwMusic: out of the network, 1, the current track and this level's
    // saved ones), then the 0 id.
    std::error_code ec;
    if (!std::filesystem::exists(plr, ec)) {
        return false;
    }
    if (hooks_.bookFlying && hooks_.bookFlying() && hooks_.landFlight) {
        hooks_.landFlight();
    }
    nox::player::Gui gui = hooks_.bars ? hooks_.bars() : nox::player::Gui{};
    gui.summonOrder = info.summonOrder;
    nox::player::FileInfo held = hooks_.fileInfo ? hooks_.fileInfo() : nox::player::FileInfo{};
    const bool network = (gameFlags & 0x2000u) != 0;
    if (!network) {
        held.flags &= ~8u; // (never set)
    }
    const auto written = nox::client::solo_saves::saveFileInfo(held, plr.string(), nox::client::solo_saves::localTime(),
                                                               info.level, info.mapName, info.stage, gameFlags);
    if (hooks_.setFileInfo) {
        auto kept = written; // (as the write leaves it)
        hooks_.setFileInfo(kept);
    }
    nox::player::Music music;
    music.version = 11;
    music.solo = network ? 0 : 1; // (plr_rwMusic: in the network its byte 0, no more)
    if (audio_ && !network) {
        const auto c = audio_->musicCurrent();
        music.current = {c.extra, c.position, c.track, c.volume};
        for (int i = 0; i < audio_->musicStackSize(); ++i) {
            const auto e = audio_->musicStackEntry(i);
            music.stack.push_back({e.extra, e.position, e.track, e.volume});
        }
    }
    return nox::client::solo_saves::appendClientSections(plr, gui, written, music);
}

bool Cinematics::loadClientSections(const std::filesystem::path& plr, nox::server::HostClient::ClientLoad& out)
{
    // Plr_LoadClientPlayerData: the file opened (else 0); the bars cleared; its
    // sections walked: GUI (the bars, the set chosen, the trap set; the summon order the
    // server's), File Info, Music (wanted at once, or held past a briefing; the
    // stack's size), the others skipped.
    nox::player::PlayerFile file;
    try {
        file = nox::player::loadPlayerFile(plr);
    } catch (const std::exception&) {
        return false;
    }
    if (hooks_.clearBars) {
        hooks_.clearBars();
    }
    try {
        for (const auto& s : file.sections) {
            switch (static_cast<nox::player::SectionId>(s.id)) {
            case nox::player::SectionId::Gui: {
                const auto g = nox::player::Gui::parse(s.data);
                if (hooks_.setBars) {
                    hooks_.setBars(g);
                }
                if (g.version > 2) {
                    out.summonOrder = g.summonOrder;
                }
                break;
            }
            case nox::player::SectionId::FileInfo:
                if (hooks_.setFileInfo) {
                    hooks_.setFileInfo(nox::player::FileInfo::parse(s.data));
                }
                break;
            case nox::player::SectionId::Music: {
                const auto m = nox::player::Music::parse(s.data);
                if (m.solo != 0 && audio_) {
                    for (std::size_t i = 0; i < m.stack.size() && i < 6; ++i) {
                        const auto& e = m.stack[i];
                        audio_->setMusicStackEntry(static_cast<int>(i), {e[2], e[3], e[1], e[0]});
                    }
                    audio_->restoreMusic({m.current[2], m.current[3], m.current[1], m.current[0]},
                                         static_cast<int>(m.stack.size()));
                }
                break;
            }
            default:
                break;
            }
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

} // namespace noxsdl
