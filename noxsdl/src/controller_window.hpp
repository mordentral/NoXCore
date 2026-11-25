// (noxcpp) The Controller screen, from Options' Controller button (as Input Config is from its own):
// the game controller's buttons and settings (noxcore_controller.cfg), over the menus or a game.
//
//   the picture   the left half: a plain pad drawn in vectors (crisp at any size), a callout per
//                 button naming what it does (the modifier's layer shown while the modifier is held
//                 on the pad, ticked below the picture, or the list's LT cell pointed at). Its
//                 labels are the connected pad's (Prompts: or a family's), by place with none. A
//                 button pointed at (the mouse, or pressed on the pad) lights its rows; a row pointed
//                 at lights its button. Clicking a button brings its row into the list.
//   Buttons tab   every action (pad_remap.hpp's rows) with its button and its LT-layer button (the
//                 menus' actions: their first and second). A cell clicked, or the pad's A on it (the
//                 list's left / right pick the cell), waits for the next pad button ("press a
//                 button", modal): it goes in the cell and leaves the cells it was in. The menus'
//                 Back buttons bind on a short press; held they cancel; so does Escape. A right click
//                 empties a cell.
//   Settings tab  the left and right sticks' dead zones, the run's tilt, the aim's and free cursor's
//                 speed (sliders); aim assist, the lock, the buttons' names (drop-downs); invert the
//                 right stick, rumble (check boxes).
//   buttons       Defaults: the default layout and settings; Reset: what the file has; Back (or
//                 Escape, or the pad's B): kept and written. Every change is in use at once.
//
// The pad moves over it as over any window (gui_nav.hpp): the tabs on LB / RB, the list's lines on
// up / down, its cells on left / right, the sliders stepped. While it waits for a button it has the
// pad's buttons as they are (padEventFirst gives them to it before the menus' mapping).
#pragma once

#include "gui.hpp"
#include "pad/gamepad_manager.hpp"
#include "pad/pad_bindings.hpp"
#include "pad/pad_picture.hpp"
#include "pad/pad_remap.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class ControllerWindow {
public:
    struct Callbacks {
        std::function<void(const pad::PadConfig&)> onApply; ///< a change: in use at once
        std::function<void(const pad::PadConfig&)> onSave;  ///< Back: in use, and written
        std::function<pad::PadConfig()> saved;              ///< the file's (Reset)
        std::function<void()> onClose;                      ///< gone: Options comes back
    };

    ControllerWindow(gui::Resources res, std::filesystem::path windowDir, const pad::GamepadManager& pads, Callbacks callbacks);

    bool isOpen() const { return open_; }
    void open(const pad::PadConfig& config);
    /// Gone; with `save`, the config to onSave first.
    void close(bool save);
    /// Waiting for a button: the pad's buttons come here as they are.
    bool capturing() const { return wait_.active(); }

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// Every event while it's open; `padEdge` a pad button's (padEventFirst's).
    void handleEvent(const SDL_Event& event, const std::optional<pad::PadEdge>& padEdge, int width, int height);
    /// What's under the mouse says what it does (the settings').
    std::string tooltip() const;

private:
    /// A list line: an action's row, or the menus' heading.
    struct Line {
        std::string action; ///< empty: the heading
        std::string title;
    };
    struct DropDown {
        std::int32_t box;
        std::int32_t item;
        int count;
    };

    bool onMessage(int message, gui::Window& from);
    void build();
    void fillLines();
    void showPage(int page);
    void apply();
    void setConfig(const pad::PadConfig& c);
    void syncSettings();
    void showDrop(const DropDown& d, bool open);
    void shutDrops();
    int dropValue(int which) const;
    void setDropValue(int which, int value);
    std::string dropName(int which, int value) const;
    void startCapture(int line, int cell, std::uint64_t nowMs);
    void endCapture();
    void finishCapture(pad::PadButton b);
    void bringRowIntoView(pad::PadButton b);
    pad::ButtonLabel label(pad::PadButton b) const;
    std::string actionTitle(std::string_view action) const;
    /// The button the picture lights now, and whether the modifier's layer shows.
    std::optional<pad::PadButton> litButton() const;
    bool layerShown() const;
    /// The list line and cell pointed at (the mouse, or the pad's focus on the list).
    std::optional<std::pair<int, int>> pointedCell() const;

    void drawPanel(gui::Window& self, gui::Painter& p);
    void drawPicture(gui::Window& self, gui::Painter& p);
    void drawList(gui::Window& self, gui::Painter& p);
    void drawSettings(gui::Window& self, gui::Painter& p);
    void drawPopup(gui::Window& self, gui::Painter& p);

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    const pad::GamepadManager& pads_;
    Callbacks callbacks_;
    bool open_ = false;
    pad::PadConfig config_;
    std::vector<Line> lines_;
    int page_ = 0;
    bool showLayer_ = false;
    gui::Window* panel_ = nullptr;
    gui::Window* list_ = nullptr;
    gui::Window* popup_ = nullptr;
    pad::PictureLayout picture_;
    /// The mouse in layout units (its last motion), and the list line / cell under it.
    SDL_FPoint mouse_{-1, -1};
    int navCell_ = 0; ///< the cell the pad's left / right picked on the list
    /// A pad button lately pressed (lit a moment), and when.
    std::optional<pad::PadButton> pressed_;
    std::uint64_t pressedMs_ = 0;
    std::uint64_t nowMs_ = 0;
    /// The wait for a button: its line and cell.
    pad::CaptureWait wait_;
    int captureLine_ = -1;
    int captureCell_ = 0;
    std::string notice_; ///< the last change's word (what lost the button)
    std::uint64_t noticeUntilMs_ = 0;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
