// (noxcpp) Pad buttons' names on the screen (pad_labels.hpp).
#include "pad/pad_labels.hpp"

#include "noxcore/strings/l10n.hpp"

namespace noxsdl::pad {

namespace {

using B = PadButton;

/// The non-face buttons' text by family (Generic, Xbox, PlayStation, Switch).
std::string_view otherText(PadButton b, LabelFamily f)
{
    const auto pick = [f](std::string_view generic, std::string_view xbox, std::string_view ps, std::string_view sw) {
        switch (f) {
        case LabelFamily::Xbox: return xbox;
        case LabelFamily::PlayStation: return ps;
        case LabelFamily::Switch: return sw;
        default: return generic;
        }
    };
    switch (b) {
    case B::LeftShoulder: return pick("LB", "LB", "L1", "L");
    case B::RightShoulder: return pick("RB", "RB", "R1", "R");
    case B::LeftTrigger: return pick("LT", "LT", "L2", "ZL");
    case B::RightTrigger: return pick("RT", "RT", "R2", "ZR");
    case B::LeftStick: return pick("L3", "LS", "L3", "LS");
    case B::RightStick: return pick("R3", "RS", "R3", "RS");
    case B::View: return pick("View", "View", "Share", "-");
    case B::Start: return pick("Start", "Menu", "Options", "+");
    case B::Guide: return pick("Guide", "Guide", "Guide", "Home");
    case B::Misc: return pick("Misc", "Share", "Mic", "Capture");
    case B::RightPaddle1: return "P1";
    case B::LeftPaddle1: return "P3";
    case B::RightPaddle2: return "P2";
    case B::LeftPaddle2: return "P4";
    case B::Touchpad: return pick("Pad", "Pad", "Touchpad", "Pad");
    default: return padButtonName(b);
    }
}

bool isFace(PadButton b)
{
    return b == B::South || b == B::East || b == B::West || b == B::North;
}

bool isDpad(PadButton b)
{
    return b == B::DpadUp || b == B::DpadDown || b == B::DpadLeft || b == B::DpadRight;
}

} // namespace

LabelFamily labelFamily(PromptStyle style, SDL_GamepadType type, bool connected)
{
    switch (style) {
    case PromptStyle::Xbox: return LabelFamily::Xbox;
    case PromptStyle::PlayStation: return LabelFamily::PlayStation;
    case PromptStyle::Switch: return LabelFamily::Switch;
    default: break;
    }
    if (!connected) {
        return LabelFamily::Generic;
    }
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return LabelFamily::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return LabelFamily::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return LabelFamily::Switch;
    case SDL_GAMEPAD_TYPE_STANDARD: return LabelFamily::Xbox; // (SDL's standard pad: A B X Y, LB LT)
    default: return LabelFamily::Generic;
    }
}

SDL_GamepadType familyType(LabelFamily family)
{
    switch (family) {
    case LabelFamily::Xbox: return SDL_GAMEPAD_TYPE_XBOXONE;
    case LabelFamily::PlayStation: return SDL_GAMEPAD_TYPE_PS5;
    case LabelFamily::Switch: return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    default: return SDL_GAMEPAD_TYPE_UNKNOWN;
    }
}

SDL_GamepadButtonLabel familyFaceLabel(LabelFamily family, PadButton b)
{
    if (!isFace(b)) {
        return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
    const auto at = [b](SDL_GamepadButtonLabel south, SDL_GamepadButtonLabel east, SDL_GamepadButtonLabel west,
                        SDL_GamepadButtonLabel north) {
        return b == B::South ? south : b == B::East ? east : b == B::West ? west : north;
    };
    switch (family) {
    case LabelFamily::Xbox:
        return at(SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y);
    case LabelFamily::PlayStation:
        return at(SDL_GAMEPAD_BUTTON_LABEL_CROSS, SDL_GAMEPAD_BUTTON_LABEL_CIRCLE, SDL_GAMEPAD_BUTTON_LABEL_SQUARE,
                  SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE);
    case LabelFamily::Switch:
        // (by place: the bottom button is B, the right one A)
        return at(SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_Y, SDL_GAMEPAD_BUTTON_LABEL_X);
    default: return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
}

ButtonLabel buttonLabel(PadButton b, LabelFamily family, SDL_GamepadButtonLabel face)
{
    ButtonLabel l;
    l.button = b;
    if (isFace(b)) {
        switch (face) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: l.glyph = Glyph::Round; l.text = "A"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_B: l.glyph = Glyph::Round; l.text = "B"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_X: l.glyph = Glyph::Round; l.text = "X"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_Y: l.glyph = Glyph::Round; l.text = "Y"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: l.glyph = Glyph::Cross; break;
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: l.glyph = Glyph::Circle; break;
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: l.glyph = Glyph::Square; break;
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: l.glyph = Glyph::Triangle; break;
        default: l.glyph = Glyph::Place; break;
        }
        return l;
    }
    if (isDpad(b)) {
        l.glyph = Glyph::Dpad;
        return l;
    }
    l.glyph = b == B::LeftStick || b == B::RightStick ? Glyph::Stick : Glyph::Box;
    l.text = std::string(otherText(b, family));
    return l;
}

std::string labelWords(const ButtonLabel& label)
{
    switch (label.glyph) {
    case Glyph::Cross: return NOX_TR("game.pad.button.cross", "Cross");
    case Glyph::Circle: return NOX_TR("game.pad.button.circle", "Circle");
    case Glyph::Square: return NOX_TR("game.pad.button.square", "Square");
    case Glyph::Triangle: return NOX_TR("game.pad.button.triangle", "Triangle");
    case Glyph::Place:
        switch (label.button) {
        case B::South: return NOX_TR("game.pad.button.bottom", "Bottom button");
        case B::East: return NOX_TR("game.pad.button.right", "Right button");
        case B::West: return NOX_TR("game.pad.button.left", "Left button");
        default: return NOX_TR("game.pad.button.top", "Top button");
        }
    case Glyph::Dpad:
        switch (label.button) {
        case B::DpadUp: return NOX_TR("game.pad.button.dpad_up", "D-pad Up");
        case B::DpadDown: return NOX_TR("game.pad.button.dpad_down", "D-pad Down");
        case B::DpadLeft: return NOX_TR("game.pad.button.dpad_left", "D-pad Left");
        default: return NOX_TR("game.pad.button.dpad_right", "D-pad Right");
        }
    default: return label.text;
    }
}

} // namespace noxsdl::pad
