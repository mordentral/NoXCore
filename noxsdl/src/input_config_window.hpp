// The game's input configuration window (InputCfg.wnd, GuiInput.c: setup its buttons
// the list the key capture), from Options' Input Config button.
//
//   list       every control but ToggleQuitMenu in the game's order: its title
//              (bindevent:<name>) and the first keys of its first two bindings (keybind:<key>),
//              in three columns that scroll together (the wheel).
//   a key      clicking a key cell opens the "press a key" box over it (980, modal): the next
//              key, mouse button or wheel step goes in the cell and is taken out of every other
//              cell; Escape leaves it as it was.
//   buttons    Reset: nox.cfg's bindings again; Defaults: default.cfg's; Back: the bindings
//              rebuilt from the cells, a key each (each control's second key, then its first),
//              with Esc = ToggleQuitMenu, written to nox.cfg; the radios:
//              the mouse button that picks items up (MousePickup).
// Rebuilt this way a control keeps at most two single-key bindings, as in the game.
#pragma once

#include "gui.hpp"

#include "noxcore/config/nox_config.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class InputConfigWindow {
public:
    struct Callbacks {
        /// Back: the bindings as the cells have them (written to nox.cfg by the caller).
        std::function<void(const nox::config::Config&)> onSave;
        /// The window went (after Back or Escape): Options comes back.
        std::function<void()> onClose;
    };

    InputConfigWindow(gui::Resources res, std::filesystem::path gameDir, Callbacks callbacks);

    bool isOpen() const { return open_; }
    /// Opens it on `config`'s bindings.
    void open(const nox::config::Config& config);
    /// Shuts it; with `save`, the bindings go to onSave first.
    void close(bool save);

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// Mouse and keys while it's open.
    void handleEvent(const SDL_Event& event, int width, int height);

private:
    struct Row {
        std::string control;
        std::optional<std::uint32_t> keys[2];
    };

    bool onMessage(int message, gui::Window& from);
    /// The rows from a configuration's bindings.
    void fill(const nox::config::Config& config);
    /// The lists' lines from the rows.
    void showRows();
    /// The cell being set takes `code`, which leaves any other cell.
    void assign(std::uint32_t code);
    void cancelCapture();
    void scroll(int by);
    std::string keyTitle(std::optional<std::uint32_t> code) const;
    /// Reads a file's bindings (nox.cfg / default.cfg in the game folder), if it reads.
    std::optional<nox::config::Config> readFile(const char* name) const;

    gui::Gui gui_;
    std::filesystem::path gameDir_;
    Callbacks callbacks_;
    bool open_ = false;
    nox::config::Config config_;
    std::vector<Row> rows_;
    gui::Window* window_ = nullptr;
    gui::Window* popup_ = nullptr;
    /// The cell waiting for a key: its column (0 first key, 1 second) and row.
    std::optional<std::pair<int, int>> capture_;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
