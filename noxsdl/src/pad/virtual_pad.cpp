// (noxcpp) A made-up game controller (virtual_pad.hpp).
#include "pad/virtual_pad.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::pad {

namespace {

Sint16 axisValue(float v)
{
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<Sint16>(std::lround(v >= 0 ? v * 32767.0f : v * 32768.0f));
}

} // namespace

VirtualPad::VirtualPad(const char* name)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT; // (in SDL_GamepadAxis's order: SDL maps them so)
    desc.nbuttons = static_cast<Uint16>(kSdlButtonCount); // (in SDL_GamepadButton's order, to the touchpad)
    desc.name = name;
    id_ = SDL_AttachVirtualJoystick(&desc);
    if (id_ == 0) {
        SDL_Log("virtual pad: not attached: %s", SDL_GetError());
        return;
    }
    joystick_ = SDL_OpenJoystick(id_);
    if (!joystick_) {
        SDL_Log("virtual pad: not opened: %s", SDL_GetError());
        SDL_DetachVirtualJoystick(id_);
        id_ = 0;
    }
}

VirtualPad::~VirtualPad()
{
    if (joystick_) {
        SDL_CloseJoystick(joystick_);
    }
    if (id_ != 0) {
        SDL_DetachVirtualJoystick(id_);
    }
}

void VirtualPad::button(PadButton b, bool down)
{
    if (!joystick_) {
        return;
    }
    if (b == PadButton::LeftTrigger || b == PadButton::RightTrigger) {
        trigger(b == PadButton::RightTrigger, down ? 1.0f : 0.0f);
        return;
    }
    SDL_SetJoystickVirtualButton(joystick_, static_cast<int>(b), down);
}

void VirtualPad::stick(bool right, float x, float y)
{
    if (!joystick_) {
        return;
    }
    SDL_SetJoystickVirtualAxis(joystick_, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX, axisValue(x));
    SDL_SetJoystickVirtualAxis(joystick_, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY, axisValue(y));
}

void VirtualPad::trigger(bool right, float value)
{
    if (!joystick_) {
        return;
    }
    // (A virtual pad's trigger axis rests at the joystick's minimum: SDL reads -32768 to 32767 as
    // the trigger's 0 to 32767.)
    const auto v = static_cast<Sint16>(std::lround(-32768.0 + std::clamp(value, 0.0f, 1.0f) * 65535.0));
    SDL_SetJoystickVirtualAxis(joystick_, right ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER, v);
}

} // namespace noxsdl::pad
