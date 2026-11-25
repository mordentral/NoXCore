// (noxcpp) A game controller made up in SDL (its virtual joystick, a standard pad: every button,
// both sticks and triggers), for tests and --pad-script: SDL reports it as any pad, so it comes
// through SDL_EVENT_GAMEPAD_ADDED and the button and axis events into GamepadManager as a real one.
// SDL's gamepad subsystem must be up (GamepadManager::init).
#pragma once

#include "pad/pad_bindings.hpp"

#include <SDL3/SDL.h>

namespace noxsdl::pad {

class VirtualPad {
public:
    /// Attached (SDL_AttachVirtualJoystick); id() is 0 if SDL wouldn't.
    explicit VirtualPad(const char* name = "NoX Core test pad");
    ~VirtualPad();
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;

    SDL_JoystickID id() const { return id_; }
    explicit operator bool() const { return joystick_ != nullptr; }

    /// A button down or up (a trigger: all the way, or let go).
    void button(PadButton b, bool down);
    /// A stick at (x, y), -1 to 1 each (right: the right stick).
    void stick(bool right, float x, float y);
    /// A trigger at 0 to 1.
    void trigger(bool right, float value);

private:
    SDL_JoystickID id_ = 0;
    SDL_Joystick* joystick_ = nullptr;
};

} // namespace noxsdl::pad
