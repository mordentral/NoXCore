// Keys and mouse buttons to the game's controls, through nox.cfg's bindings (noxcore
// config/nox_config.hpp): the keyboard's SDL scan codes become the DirectInput codes the file
// names keys by, the mouse buttons and wheel their 0x1000n codes.
//
// A binding fires its controls when its last key goes down with the others already held (a
// combination "LEFT_CTRL + A" fires on A with Ctrl held). Some controls act while held instead:
// moving (MoveForward), Action, the map's zoom and InvertSpellTarget; held() says whether any of
// their bindings is fully down.
//
// The game's own input layer (which fires which of its bindings on the press and which every
// frame) isn't traced; the split above is how the game plays.
#pragma once

#include "noxcore/config/nox_config.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

class InputBindings {
public:
    explicit InputBindings(nox::config::Config config) : config_(std::move(config)) { configChanged(); }

    const nox::config::Config& config() const { return config_; }
    void setConfig(nox::config::Config config)
    {
        config_ = std::move(config);
        down_.clear();
        configChanged();
    }
    /// Changes one setting (not the bindings), keeping what's held.
    void setSetting(std::string_view name, std::string value)
    {
        config_.set(name, std::move(value));
        configChanged();
    }
    /// TranslucentConsole: on unless the setting is there and reads 0 (the console's draw, each frame).
    bool translucentConsole() const { return translucentConsole_; }

    /// The game's code for a keyboard key, or nullopt for keys it has no name for.
    static std::optional<std::uint32_t> keyCode(SDL_Scancode scancode);
    /// The game's code for a mouse button, or nullopt.
    static std::optional<std::uint32_t> mouseCode(Uint8 button);

    /// A key or button went down: the controls whose bindings it completes, in file order.
    std::vector<std::string> press(std::uint32_t code);
    void release(std::uint32_t code);
    /// Forgets what's held (a menu took the keyboard; the window lost focus).
    void releaseAll() { down_.clear(); }
    bool isDown(std::uint32_t code) const { return down_.contains(code); }

    /// Whether any binding of `control` is fully held.
    bool held(std::string_view control) const;
    /// A control's place in the game's control table (nox::config::controlNames()), -1 for a name
    /// not in it: for held(int), looked up once by its user.
    static int control(std::string_view name);
    /// held() by the control's place in the table: its bindings found once a config.
    bool held(int control) const;

    /// The mouse button that picks items up (MousePickup), as a code.
    std::uint32_t pickupButton() const;

private:
    /// What's worked out from the config again whenever it changes: each control's bindings (by its
    /// place in the control table) and TranslucentConsole.
    void configChanged();

    nox::config::Config config_;
    std::set<std::uint32_t> down_;
    std::vector<std::vector<std::size_t>> bindingsByControl_; ///< indexes in config_.bindings
    bool translucentConsole_ = true;
};

} // namespace noxsdl
