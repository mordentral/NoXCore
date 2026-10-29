// The game's Options window (Options.wnd, with the advanced video options AdVidOpt.wnd in its
// middle; game_showOptions from the main menu, game_initOptionsInGame in
// a game), on nox.cfg's settings:
//
//   Screen Resolution     VideoMode: 640 x 480, 800 x 600, 1024 x 768, 1024 x 576 and noxsdl's
//                         ultra widescreen 1376 x 576, a drop-down in place of the game's radio
//                         buttons (five rows don't fit the panel's box). It sets how
//                         much of the world a game shows (App::viewWidth). All five can be
//                         chosen in a game too, applied at once (the game, game_initOptionsInGame, leaves only
//                         the one in use on).
//   Game Window Size      VideoSize: 65, 75, 85 or 100 percent (windowOptionsProc).
//   Colour depth          16-bit, as always here; 8-bit is off.
//   Input Config          closes this and opens the input configuration (InputCfg.wnd).
//   volume sliders        FXVolume, DialogVolume, MusicVolume (0-16384), live as they move; the
//                         three checkboxes switch sound effects, speech and music on and off.
//                         A slider at 0 switches its part off, raised it switches it back on;
//                         the effects slider plays a lever as a sample of the level.
//   advanced options      AdVidOpt.wnd's checkboxes on their nox.cfg settings (client_advVideoOptsLoad):
//                         Soft Shadow Edge, RenderGlow, Fade Objects, RenderBubbles and RenderGUI
//                         act here and are shown; the others are kept in the file but have
//                         nothing to switch, so they're left out of the panel.
//   noxsdl's switches     below those: the frame-rate counter, bloom, per-pixel lighting, smooth
//                         effects, HD spell effects, HD images, the kill feed, Windows' mouse
//                         acceleration and VSync (Extras), in the panel's own checkbox style; the mouse's speed, a
//                         slider; then two drop-downs: Smoothing (Off / Smooth Movement / Smooth
//                         Network / Lock to 30 FPS) and Display (Windowed / Fullscreen Windowed /
//                         Fullscreen); and on More, Weather Effects (On / Reduced / Off).
//   the panel's tabs      (noxsdl's) General and More along its top, in the game's tab look (the
//                         Server Settings' tabs, servopts.wnd): More has the rarely changed
//                         switches (Soft Shadow Edge, Fade Objects, Render GUI, Frame Rate) and
//                         HD Spell FX (General is full), General the rest. It opens on General
//                         each time.
//
// Changes apply at once (onChange); nox.cfg is written when the window closes.
#pragma once

#include "gui.hpp"

#include "noxcore/config/nox_config.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace noxsdl {

class OptionsWindow {
public:
    /// noxsdl's own switches, added below the advanced options (kept in noxcore.cfg: the game
    /// would reject nox.cfg with them in it).
    struct Extras {
        bool frameRate = true;     ///< the frame-rate counter
        bool bloom = true;         ///< bloom round bright effects and lights
        bool perPixelLight = true; ///< per-pixel lighting (off: the game's light grid)
        bool smoothEffects = true; ///< effects drawn smooth at the screen's resolution (sparks, rays, glows)
        /// HD Spell FX (the More page): the poison cloud and Force Field drawn as noxsdl's own shader
        /// effects instead of the game's sprites (the Reflective Shield's is off for now, to be
        /// reworked). Needs the GPU renderer's shaders. Off by default (by decision, 2026-10-29).
        bool hdSpellFx = false;
        /// Smoothing: 0 off (as vanilla), 1 Smooth Movement (everything eased to each new position
        /// over a server frame), 2 Smooth Network (everything drawn between server snapshots: the
        /// default), 3 Lock to 30 FPS (off, a frame drawn as each server update comes).
        int smoothing = 2;
        bool hdImages = false;     ///< images from the HD image pack (.nxp) instead of video.bag
        bool hdAvailable = false;  ///< whether there's a pack to use (the switch is greyed without)
        int gamma = 1;             ///< Insert / Delete's brightness, 1-10 (not on the panel)
        bool gpu = false;          ///< whether the GPU renderer's shaders are there (the last three need them)
        /// Win Mouse Accel: Windows' pointer speed and acceleration (Enhance Pointer Precision)
        /// on the mouse's raw movement. On by default; off it's the original's 1:1.
        bool mouseAccel = true;
        int mouseSpeed = 100;      ///< Mouse: the cursor's speed, percent (kMouseSpeedMin-Max)
        int mouseSpeed3D = 100;    ///< the 3D view's mouse look speed, percent (the same range)
        bool view3d = false;       ///< the 3D view is on: the Mouse slider sets mouseSpeed3D
        /// The speed the Mouse slider shows and sets.
        int& sliderSpeed() { return view3d ? mouseSpeed3D : mouseSpeed; }
        bool killFeed = true;      ///< Kill Feed: the game's kill feed at the top (off: the console only)
        /// Display: 0 a window, 1 fullscreen windowed (borderless, the desktop's size), 2 fullscreen
        /// (exclusive, the desktop's display mode).
        int display = 0;
        bool vsync = true;         ///< VSync: frames wait for the screen's refresh (off: as fast as it goes)
        /// Not shown here: a game we host's update rate (Server Settings' General page: 30-120).
        int serverUpdateRate = 60;
        bool serverMapVote = true; ///< Not shown here either: a game we host's map vote (the General page)
        /// Exact Original Maths (the More page): games we host figure their floats as the original's x87.
        bool exactMaths = true;
        /// Not shown here: a map editor session's pending edits in the game (0 Shaded, 1 As final,
        /// 2 Hidden: nox::mapedit::PendingView; the console's noxsdl pending).
        int pendingView = 0;
        /// Weather Effects (the More page): a mod's rain, snow, storm and wind drawn (0 On), with
        /// fewer particles and softer flashes (1 Reduced), or not at all (2 Off). Needs the GPU
        /// renderer's shaders.
        int weather = 0;
    };
    static constexpr int kMouseSpeedMin = 25, kMouseSpeedMax = 300;
    /// Sound effects, speech and music on (the checkboxes; not kept in nox.cfg, whose volumes say
    /// whether each starts on).
    using SoundOn = std::array<bool, 3>;
    struct Callbacks {
        std::function<void(const nox::config::Config&)> onChange; ///< a setting changed
        std::function<void(const nox::config::Config&)> onClose;  ///< closed (save it)
        std::function<void()> onInputConfig; ///< Input Config pressed (this has closed)
        std::function<void(const Extras&)> onExtras; ///< one of noxsdl's switches changed
        std::function<void(const SoundOn&)> onSound; ///< a sound checkbox changed
    };

    OptionsWindow(gui::Resources res, std::filesystem::path windowDir, Callbacks callbacks);

    /// Opens on `config`'s settings and noxsdl's; `inGame` locks the resolution and colour depth.
    void open(const nox::config::Config& config, bool inGame, const Extras& extras, const SoundOn& sound);
    void close();
    bool isOpen() const { return open_; }
    /// The tooltip for what's under the mouse: noxsdl's switches say what they do, with their
    /// upsides and downsides (the game's own have none).
    std::string tooltip() const;

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// Takes the input while open; Escape closes it.
    bool handleEvent(const SDL_Event& event, int width, int height);

private:
    bool onMessage(int message, gui::Window& from);
    void setting(std::string_view name, std::string value);
    /// One of noxsdl's switches usable or greyed (its light and its label).
    void setExtraEnabled(std::int32_t id, bool enabled);
    /// A drop-down (Smoothing, Display): its label, button, first choice and the Extras field it
    /// sets.
    struct DropDown {
        std::int32_t label;
        std::int32_t box;
        std::int32_t item;
        std::span<const char* const> names;
        int Extras::*value;
    };
    /// The drop-downs, Smoothing, Display, then Weather Effects (on the More page).
    static const std::array<DropDown, 3>& drops();
    /// A drop-down's choices shown under it, or not; its button's text.
    void showDrop(const DropDown& d, bool open);
    void setDropText(const DropDown& d);
    /// The resolution drop-down: the one in use (kResolutions' index, -1 none) on its button and lit
    /// in its list, the list shown or not.
    void showResolutions(int inUse, bool open);
    /// The mouse speed's label, with the speed ("Mouse x1.25").
    void setMouseText();
    /// The panel's page shown (0 General, 1 More): its tab lit, its rows shown, the other's hidden.
    void showPage(int page);

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    Callbacks callbacks_;
    gui::Window* window_ = nullptr;
    gui::Window* advanced_ = nullptr;
    nox::gui::Color extraText_; ///< noxsdl's switches' label colour while usable
    /// The panel's rows by the page they're on (the tabs' windows aren't in it).
    std::vector<std::pair<std::int32_t, int>> paged_;
    nox::config::Config config_;
    Extras extras_;
    SoundOn sound_{true, true, true};
    int resolution_ = -1; ///< the resolution drop-down's choice (kResolutions' index, -1 none)
    bool inGame_ = false;
    bool open_ = false;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
