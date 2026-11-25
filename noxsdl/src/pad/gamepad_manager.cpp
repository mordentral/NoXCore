// (noxcpp) The game controllers (gamepad_manager.hpp).
#include "pad/gamepad_manager.hpp"

#include "noxcore/io/file.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace noxsdl::pad {

bool GamepadManager::init(const std::filesystem::path& mappingsFile)
{
    if (initialised_) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SDL_Log("game controllers: SDL's gamepad subsystem won't start: %s", SDL_GetError());
        return false;
    }
    initialised_ = true;
    std::error_code ec;
    if (!mappingsFile.empty() && std::filesystem::exists(mappingsFile, ec)) {
        const std::string path = nox::pathUtf8(mappingsFile);
        const int added = SDL_AddGamepadMappingsFromFile(path.c_str());
        if (added < 0) {
            SDL_Log("game controllers: %s not read: %s", path.c_str(), SDL_GetError());
        } else {
            SDL_Log("game controllers: %d mappings from %s", added, path.c_str());
        }
    }
    return true;
}

void GamepadManager::shutdown()
{
    for (Pad& p : pads_) {
        if (p.gamepad) {
            SDL_CloseGamepad(p.gamepad);
        }
    }
    pads_.clear();
    active_ = 0;
    frame_ = {};
    lastSampleNs_ = 0;
    if (initialised_) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        initialised_ = false;
    }
}

GamepadManager::Pad* GamepadManager::find(SDL_JoystickID id)
{
    const auto it = std::ranges::find(pads_, id, &Pad::id);
    return it == pads_.end() ? nullptr : &*it;
}

const GamepadManager::Pad* GamepadManager::find(SDL_JoystickID id) const
{
    const auto it = std::ranges::find(pads_, id, &Pad::id);
    return it == pads_.end() ? nullptr : &*it;
}

void GamepadManager::open(SDL_JoystickID id)
{
    if (find(id)) {
        return;
    }
    SDL_Gamepad* g = SDL_OpenGamepad(id);
    if (!g) {
        SDL_Log("game controller %u: not opened: %s", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    pads_.push_back(Pad{id, g, {}, false, false});
    if (active_ == 0) {
        active_ = id;
    }
    const char* name = SDL_GetGamepadName(g);
    const char* kind = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
    SDL_Log("game controller %u: %s (%s)", static_cast<unsigned>(id), name ? name : "?", kind ? kind : "unknown");
}

void GamepadManager::close(SDL_JoystickID id)
{
    const auto it = std::ranges::find(pads_, id, &Pad::id);
    if (it == pads_.end()) {
        return;
    }
    SDL_CloseGamepad(it->gamepad);
    pads_.erase(it);
    SDL_Log("game controller %u: gone", static_cast<unsigned>(id));
    if (active_ == id) {
        active_ = pads_.empty() ? 0 : pads_.front().id;
    }
    if (pads_.empty()) {
        frame_ = {};
        lastSampleNs_ = 0;
        setDevice(InputDevice::MouseKeyboard);
    }
}

void GamepadManager::touched(SDL_JoystickID id)
{
    active_ = id;
    setDevice(InputDevice::Pad);
}

void GamepadManager::setDevice(InputDevice d)
{
    mouseTravel_ = 0;
    if (d != device_) {
        device_ = d;
        ++deviceChanges_;
    }
}

SDL_Gamepad* GamepadManager::active() const
{
    if (const Pad* p = find(active_)) {
        return p->gamepad;
    }
    return pads_.empty() ? nullptr : pads_.front().gamepad;
}

std::string GamepadManager::activeName() const
{
    SDL_Gamepad* g = active();
    const char* name = g ? SDL_GetGamepadName(g) : nullptr;
    return name ? name : "";
}

SDL_GamepadType GamepadManager::activeType() const
{
    SDL_Gamepad* g = active();
    return g ? SDL_GetGamepadType(g) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

SDL_GamepadButtonLabel GamepadManager::faceLabel(PadButton b) const
{
    const SDL_GamepadButton sdl = sdlButton(b);
    if (sdl == SDL_GAMEPAD_BUTTON_INVALID) {
        return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
    if (SDL_Gamepad* g = active()) {
        return SDL_GetGamepadButtonLabel(g, sdl);
    }
    return SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_STANDARD, sdl);
}

bool GamepadManager::isDown(PadButton b) const
{
    const Pad* p = find(active_);
    const auto i = static_cast<std::size_t>(b);
    return p && i < kPadButtonCount && p->down.test(i);
}

SDL_GamepadButton GamepadManager::sdlButton(PadButton b)
{
    const auto i = static_cast<std::size_t>(b);
    return i < kSdlButtonCount ? static_cast<SDL_GamepadButton>(i) : SDL_GAMEPAD_BUTTON_INVALID;
}

std::optional<PadButton> GamepadManager::fromSdlButton(Uint8 button)
{
    return button < kSdlButtonCount ? std::optional<PadButton>(static_cast<PadButton>(button)) : std::nullopt;
}

bool GamepadManager::isPadEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
    case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
    case SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BALL_MOTION:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
    case SDL_EVENT_JOYSTICK_UPDATE_COMPLETE:
        return true;
    default:
        return false;
    }
}

std::optional<PadEdge> GamepadManager::trigger(Pad& p, SDL_GamepadAxis axis, Sint16 value)
{
    const bool left = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    bool& held = left ? p.leftTriggerHeld : p.rightTriggerHeld;
    const bool now = triggerHeld(held, triggerUnit(value), settings_.triggerThreshold);
    if (now == held) {
        return std::nullopt;
    }
    held = now;
    const PadButton b = left ? PadButton::LeftTrigger : PadButton::RightTrigger;
    p.down.set(static_cast<std::size_t>(b), now);
    if (now) {
        touched(p.id);
    }
    return PadEdge{b, now, p.id};
}

std::optional<PadEdge> GamepadManager::handleEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        open(event.gdevice.which);
        return std::nullopt;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close(event.gdevice.which);
        return std::nullopt;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        Pad* p = find(event.gbutton.which);
        const auto b = fromSdlButton(event.gbutton.button);
        if (!p || !b) {
            return std::nullopt;
        }
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        p->down.set(static_cast<std::size_t>(*b), down);
        if (down) {
            touched(p->id);
        }
        return PadEdge{*b, down, p->id};
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        Pad* p = find(event.gaxis.which);
        if (!p) {
            return std::nullopt;
        }
        const auto axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            return trigger(*p, axis, event.gaxis.value);
        }
        // A stick past its dead zone touches the pad (its other axis as it stands).
        const bool leftStick = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY;
        const SDL_GamepadAxis xAxis = leftStick ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
        const SDL_GamepadAxis yAxis = leftStick ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
        const float x = axisUnit(axis == xAxis ? event.gaxis.value : SDL_GetGamepadAxis(p->gamepad, xAxis));
        const float y = axisUnit(axis == yAxis ? event.gaxis.value : SDL_GetGamepadAxis(p->gamepad, yAxis));
        if (std::hypot(x, y) > settings_.deadZone) {
            touched(p->id);
        }
        return std::nullopt;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (device_ == InputDevice::Pad) {
            mouseTravel_ += std::abs(event.motion.xrel) + std::abs(event.motion.yrel);
            if (mouseTravel_ > kMouseHandBack) {
                setDevice(InputDevice::MouseKeyboard);
            }
        }
        return std::nullopt;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        setDevice(InputDevice::MouseKeyboard);
        return std::nullopt;
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat) {
            setDevice(InputDevice::MouseKeyboard);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

void GamepadManager::sample(std::uint64_t nowNs)
{
    const Pad* p = find(active_);
    if (!p) {
        return; // (frame_ was cleared when the last pad went)
    }
    frame_.seconds = lastSampleNs_ != 0 && nowNs > lastSampleNs_
                         ? std::min(static_cast<float>(static_cast<double>(nowNs - lastSampleNs_) / 1e9), 0.1f)
                         : 0.0f;
    lastSampleNs_ = nowNs;
    SDL_Gamepad* g = p->gamepad;
    const auto axis = [g](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(g, a); };
    frame_.left = radialDeadZone({axisUnit(axis(SDL_GAMEPAD_AXIS_LEFTX)), axisUnit(axis(SDL_GAMEPAD_AXIS_LEFTY))},
                                 settings_.deadZone, settings_.outerDeadZone);
    frame_.right = radialDeadZone({axisUnit(axis(SDL_GAMEPAD_AXIS_RIGHTX)), axisUnit(axis(SDL_GAMEPAD_AXIS_RIGHTY))},
                                  settings_.deadZone, settings_.outerDeadZone);
    if (settings_.invertY) {
        frame_.right.y = -frame_.right.y;
    }
    frame_.leftTrigger = triggerUnit(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    frame_.rightTrigger = triggerUnit(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
}

} // namespace noxsdl::pad
