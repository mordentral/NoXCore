// (noxcpp) The game controllers (SDL's gamepad API: XInput, DirectInput, HIDAPI's PlayStation,
// Switch and other pads, Linux's evdev, Steam Input and the rest behind one API).
//
// GamepadManager opens each pad as SDL reports it (SDL_EVENT_GAMEPAD_ADDED) and closes it as it
// goes; the pad last touched is the one played with (one local player). A gamecontrollerdb.txt
// beside the program adds mappings for unusual pads. Every SDL event goes through handleEvent
// first: it keeps the pads, which buttons are held and which device was touched last, and turns a
// pad's button, and a trigger crossing its threshold, into a PadEdge. The sticks and triggers are
// read once a frame (sample) through the dead zones, with the pad layer's own frame time.
//
// The device in use (activeDevice) is the input mode the player chose (setMode): the pad, or the
// mouse and keyboard. Nothing the player does hands it from one to the other (a mouse nudged on
// the desk never takes the game from the pad): only setMode does, and the last pad going (back to
// the mouse by itself). Which events each mode ignores is the program's (pad/pad_mode.cpp): the
// pad's in the mouse's mode, the mouse's in the pad's (the keys always type).
//
// Nothing here costs a frame while no pad is connected.
#pragma once

#include "pad/pad_bindings.hpp"
#include "pad/pad_labels.hpp"
#include "pad/pad_math.hpp"

#include <SDL3/SDL.h>

#include <bitset>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::pad {

/// A pad button (or trigger) going down or up.
struct PadEdge {
    PadButton button = PadButton::South;
    bool down = false;
    SDL_JoystickID pad = 0; ///< the pad it came from
};

/// The device the player last touched.
enum class InputDevice : std::uint8_t { MouseKeyboard, Pad };

/// The active pad's sticks and triggers this frame, through the dead zones (PadSettings).
struct PadFrame {
    StickVec left;      ///< after the radial dead zone, 0 to 1 long
    StickVec right;     ///< the same (y turned over with InvertY)
    float leftTrigger = 0;  ///< 0 to 1
    float rightTrigger = 0;
    float seconds = 0;  ///< since the last sample (at most 0.1 s; 0 the first time)
};

class GamepadManager {
public:
    GamepadManager() = default;
    GamepadManager(const GamepadManager&) = delete;
    GamepadManager& operator=(const GamepadManager&) = delete;
    ~GamepadManager() { shutdown(); }

    /// SDL's gamepad subsystem up (beside the video's), and `mappingsFile`'s mappings when it's
    /// there (gamecontrollerdb.txt). Pads connected already come as SDL_EVENT_GAMEPAD_ADDED.
    /// False (logged) if SDL won't: then there are just no pads.
    bool init(const std::filesystem::path& mappingsFile = {});
    /// Closes the pads and the subsystem (once; the destructor does it too).
    void shutdown();

    /// Every SDL event, first: the pads opened and closed, the held buttons, the device in use;
    /// a pad button's or trigger's press or release returned.
    std::optional<PadEdge> handleEvent(const SDL_Event& event);
    /// Whether an event is the pads' (buttons, axes, devices, touchpads, sensors).
    static bool isPadEvent(const SDL_Event& event);

    /// The dead zones and the trigger threshold (noxcore_controller.cfg's).
    void setSettings(const PadSettings& s) { settings_ = s; }
    const PadSettings& settings() const { return settings_; }

    /// Whether any pad is open.
    bool connected() const { return !pads_.empty(); }
    std::size_t count() const { return pads_.size(); }
    /// The pad played with (the last touched; the first opened before any is), or null.
    SDL_Gamepad* active() const;
    SDL_JoystickID activeId() const { return active_; }
    /// Its name and kind (SDL's: Xbox 360 / One, PS3 / 4 / 5, Switch Pro, Joy-Con...), for the
    /// prompts and the Controller screen's picture; SDL_GAMEPAD_TYPE_UNKNOWN with none.
    std::string activeName() const;
    SDL_GamepadType activeType() const;
    /// What's printed on a face button of the active pad (A / Cross / B on a Switch pad...): SDL's
    /// label, by the pad's type when there's no pad open (SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN for a
    /// button that isn't a face button).
    SDL_GamepadButtonLabel faceLabel(PadButton b) const;
    /// What `b` is called on the screen (pad_labels.hpp): `style`'s family, or (Auto) the active
    /// pad's own labels; by place with no pad.
    ButtonLabel label(PadButton b, PromptStyle style) const;

    /// The device in use: the mode chosen (the pad's only while one is connected).
    InputDevice activeDevice() const { return device_; }
    bool padInUse() const { return device_ == InputDevice::Pad; }
    /// The input mode: the pad's takes effect only with a pad connected (none: the mouse's).
    void setMode(InputDevice d);
    /// Whether the last pad's going put the mode back to the mouse's (till setMode).
    bool fellBack() const { return fellBack_; }
    /// How many times the device in use has changed (a reader that keeps the last value sees a change).
    std::uint32_t deviceChanges() const { return deviceChanges_; }

    /// Whether the active pad holds `b` (as its events said; a trigger past its threshold).
    bool isDown(PadButton b) const;

    /// The active pad's sticks and triggers read now; `nowNs` SDL_GetTicksNS's.
    void sample(std::uint64_t nowNs);
    const PadFrame& frame() const { return frame_; }

    /// SDL_GamepadButton for a PadButton (SDL_GAMEPAD_BUTTON_INVALID for the triggers), and back.
    static SDL_GamepadButton sdlButton(PadButton b);
    static std::optional<PadButton> fromSdlButton(Uint8 button);

    /// The mouse id of the pad's made-up clicks (a mode's window, the panels: pad/pad_panels.cpp):
    /// the pad's own, so the pad's mode lets them through.
    static constexpr SDL_MouseID kPadMouse = 0x50AD0001u;
    /// The keyboard id of the on-screen keyboard's made-up keys (pad/osk_game.cpp: Backspace, Enter,
    /// Escape): the pad's own too.
    static constexpr SDL_KeyboardID kPadKeys = 0x50AD0002u;

private:
    struct Pad {
        SDL_JoystickID id = 0;
        SDL_Gamepad* gamepad = nullptr;
        std::bitset<kPadButtonCount> down;
        bool leftTriggerHeld = false;
        bool rightTriggerHeld = false;
    };
    Pad* find(SDL_JoystickID id);
    const Pad* find(SDL_JoystickID id) const;
    void open(SDL_JoystickID id);
    void close(SDL_JoystickID id);
    void setDevice(InputDevice d);
    std::optional<PadEdge> trigger(Pad& p, SDL_GamepadAxis axis, Sint16 value);

    bool initialised_ = false;
    std::vector<Pad> pads_;
    SDL_JoystickID active_ = 0;
    PadSettings settings_;
    InputDevice mode_ = InputDevice::MouseKeyboard;   ///< the mode chosen
    InputDevice device_ = InputDevice::MouseKeyboard; ///< in effect (the mouse's with no pad)
    bool fellBack_ = false;
    std::uint32_t deviceChanges_ = 0;
    PadFrame frame_;
    std::uint64_t lastSampleNs_ = 0;
};

} // namespace noxsdl::pad
