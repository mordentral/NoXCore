// A mode's own UI in noxsdl (noxcore/modes/mode.hpp's modes): its windows and HUD pieces, drawn
// over the game from what its client side (ClientMode) was told, its picks sent back through it.
// main.cpp knows only this: each mode built in has one ModeUi for the program's life (made with
// the game windows), bound to its client side for each live game.
#pragma once

#include "noxcore/modes/mode.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby {
struct ServerEntry;
}
struct HostSettings; // (local_host.hpp)

namespace noxsdl {

struct App;

/// Where the HUD is this frame (the windows over it: at its scale, with the mouse).
struct HudFrame {
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    SDL_FPoint mouse{};
    std::uint64_t nowMs = 0;
    bool renderGui = true; ///< (off: update only, nothing drawn)
};

/// The words of the quest's game-over window, when a mode has its own.
struct GameOverWords {
    std::string title;
    std::string question;
    bool tallies = true; ///< (false: the generators' and secrets' lines left empty)
};

class ModeUi {
public:
    virtual ~ModeUi() = default;
    /// Its client side for the live game now (null: none, the game gone).
    virtual void bind(nox::modes::ClientMode* side) = 0;
    /// An event over the game (the menus closed): whether the mode's windows took it.
    virtual bool handleEvent(App& app, const SDL_Event& event, int viewWidth, int viewHeight, float hudScale)
    {
        (void)app;
        (void)event;
        (void)viewWidth;
        (void)viewHeight;
        (void)hudScale;
        return false;
    }
    /// Every mouse button's release, first (after the bindings' release, before any window): true
    /// when it's the end of a press the mode's windows took (nothing else saw that press), and so
    /// goes no further.
    virtual bool release(App& app, const SDL_Event& event)
    {
        (void)app;
        (void)event;
        return false;
    }
    /// The window's focus lost (its releases go to another program): a press it holds let go.
    virtual void inputLost(App& app) { (void)app; }
    /// Each frame with the game's windows' updates (before anything's drawn).
    virtual void update(App& app) { (void)app; }
    /// Each frame in the 2D view, on the floor: after the floor and the hover ring, under every
    /// thing and creature (a script mode's ground decals).
    virtual void drawFloor(App& app, SDL_Renderer* renderer)
    {
        (void)app;
        (void)renderer;
    }
    /// Each frame in the 2D view, over the world and what's drawn over it (names, bars, bubbles),
    /// under the HUD: a mode's labels over things.
    virtual void drawWorld(App& app, SDL_Renderer* renderer)
    {
        (void)app;
        (void)renderer;
    }
    /// Each frame in a game, right over the HUD's bars and under the game's windows, before
    /// drawHud: its HUD pieces (its windows updated here when it has any).
    virtual void drawUnderWindows(App& app, SDL_Renderer* renderer, const HudFrame& hud)
    {
        (void)app;
        (void)renderer;
        (void)hud;
    }
    /// Each frame, over the HUD: its windows updated, and drawn when the GUI is.
    virtual void drawHud(App& app, SDL_Renderer* renderer, const HudFrame& hud)
    {
        (void)app;
        (void)renderer;
        (void)hud;
    }
    /// Each frame the GUI is drawn, over everything but the pointer: a tooltip of its own.
    virtual void drawOver(App& app, SDL_Renderer* renderer, const HudFrame& hud)
    {
        (void)app;
        (void)renderer;
        (void)hud;
    }
    /// Each frame once the camera's set from our own player (it may move it: a scene's view).
    virtual void cameraSet(App& app) { (void)app; }
    /// A join of `game` refused (`joinError`, net::JoinError's): what the game list says, when
    /// it's the mode's to say; none: the game's own words.
    virtual std::optional<std::string> joinRefused(std::uint8_t joinError, const nox::lobby::ServerEntry* game) const
    {
        (void)joinError;
        (void)game;
        return std::nullopt;
    }
    /// The quest's game over in the mode's words (none: the game's own).
    virtual std::optional<GameOverWords> gameOver() const { return std::nullopt; }
    /// A --fake-* test screen of its own (`flag` as typed): shown, true; not its, false.
    virtual bool fakeScreen(App& app, std::string_view flag)
    {
        (void)app;
        (void)flag;
        return false;
    }
};

/// A mode started through the solo game's screens (the class screen choosing its class, the
/// character screen its saved game, on saves of its own) rather than an options panel: a campaign
/// mode's. It's hosted with the class as its option "class".
struct SoloScreensStart {
    /// Its saves' folder under the solo saves' root ("Save/Example": a slot there each, as solo
    /// games are; never the solo game's).
    std::filesystem::path saves;
    /// Its hosting's log tag ("example") and what's hosted, by the class chosen ("the Wizard
    /// campaign").
    std::string logTag;
    std::function<std::string(std::uint8_t playerClass)> what;
};

struct ModeUiEntry {
    std::string modeId; ///< (ModeInfo::id: its client side's mode)
    std::function<std::unique_ptr<ModeUi>(App& app)> make;
    std::vector<std::string> fakeFlags; ///< its --fake-* flags
    std::optional<SoloScreensStart> soloScreens;
    /// Its client side when it isn't a registered mode's (the script modes' UI): made for each
    /// live game's client (HeadlessClient::adoptModeSide). None: the registry's mode's.
    std::function<std::unique_ptr<nox::modes::ClientMode>(nox::client::HeadlessClient&)> makeSide;
};

/// The UIs of the modes built in (registerBuiltinModeUis fills it at start-up).
std::vector<ModeUiEntry>& modeUiEntries();
/// `modeId`'s UI entry; null: none.
const ModeUiEntry* modeUiEntry(std::string_view modeId);

/// A solo-screens mode's saves' root under the solo one (made as it's asked for: a new game's
/// character is written under its Save\ at once); the solo root itself for any other.
std::filesystem::path modeSaveRoot(const std::filesystem::path& soloRoot, std::string_view modeId);
/// About to host solo-screens mode `modeId` for class `playerClass`: the app's save root the
/// mode's (the in-game save and load menus on its saves), the solo one kept; `settings` hosting it.
void beginSoloScreensHost(App& app, std::string_view modeId, std::uint8_t playerClass, HostSettings& settings);
/// The game left, or its hosting failed: the solo saves' root back.
void endSoloScreensHost(App& app);
/// The game we host is a solo-screens mode's (a campaign mode hosted for guests, the co-op
/// campaign): its name as the mode list shows it ("Co-op Campaign"). None: we host no such game.
/// Its host's Server Settings take only what applies to a campaign (server_options.hpp).
std::optional<std::string> hostedCampaign(const App& app);
/// (builtin_mode_uis.cpp, made from the built-in modes: once; again does nothing.)
void registerBuiltinModeUis();
/// The script modes' UI (script_mode_ui.cpp): a server's UI script's windows, for any script mode.
void registerScriptModeUi(std::vector<ModeUiEntry>& entries);

/// Each mode's UI made (the game windows' set-up), and bound to each live game's client sides
/// (HeadlessClient::modeSide; let go before the game goes: unbindModeClients).
void makeModeUis(App& app);
void bindModeClients(App& app);
void unbindModeClients(App& app);
/// Whether `flag` is a mode's --fake-* flag.
bool isModeFakeFlag(std::string_view flag);

} // namespace noxsdl
