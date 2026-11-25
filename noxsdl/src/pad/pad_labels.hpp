// (noxcpp) What a pad button is called where the game shows it: the HUD's slot and potion keys, A's
// prompt, the Controller screen. The names follow the pad in use (an Xbox pad's A / B / X / Y and
// LB / LT, a PlayStation pad's Cross / Circle / Square / Triangle and L1 / L2, a Switch pad's B / A /
// Y / X by place and L / ZL), or the family chosen in noxcore_controller.cfg (Prompts); with no pad
// to go by, plain names by place (the face buttons as four dots, the one meant filled).
//
// A label is a glyph and its text: the face buttons a letter in a circle or the shape printed on
// them, the D-pad a cross with an arm filled, a stick's click its top, the rest their text in a
// rounded box. Drawing them is pad_glyphs.hpp's. No SDL calls here (its enums only): the
// families' face labels are SDL's tables for those pads (by place: a Switch pad's South is B).
#pragma once

#include "pad/pad_bindings.hpp"

#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace noxsdl::pad {

/// Whose names: none to go by (by place), or a pad family's.
enum class LabelFamily : std::uint8_t { Generic, Xbox, PlayStation, Switch };

/// The family the names come from: the one `style` picks, else the connected pad's (`type`: SDL's
/// for it), else Generic (`connected` false, or a pad SDL doesn't know).
LabelFamily labelFamily(PromptStyle style, SDL_GamepadType type, bool connected);
/// A pad type standing for a family (whose face labels SDL gives for it).
SDL_GamepadType familyType(LabelFamily family);

enum class Glyph : std::uint8_t {
    Box,      ///< the text in a rounded box (bumpers, triggers, View, Start, the rest)
    Round,    ///< a letter in a circle (A, B, X, Y)
    Cross,    ///< the shapes printed on some pads' face buttons, in a circle
    Circle,
    Square,
    Triangle,
    Place,    ///< the four face buttons as dots, the one meant filled (no pad to name them by)
    Dpad,     ///< the D-pad's cross, the arm meant filled
    Stick,    ///< a stick's top, its text in it (the stick's click)
};

struct ButtonLabel {
    PadButton button = PadButton::South;
    Glyph glyph = Glyph::Box;
    std::string text; ///< what's printed ("A", "LB", "L1", "ZL", "+"); empty for the shapes, dots and D-pad

    bool operator==(const ButtonLabel&) const = default;
};

/// `b`'s label in `family`; `face` is what SDL says is printed on it for the face buttons (the pad's
/// own SDL_GetGamepadButtonLabel, or the family's: familyFaceLabel), UNKNOWN for the rest (and
/// for a face button whose label SDL doesn't know: then by place).
ButtonLabel buttonLabel(PadButton b, LabelFamily family, SDL_GamepadButtonLabel face);
/// What SDL says a family's pad prints on face button `b` (UNKNOWN for other buttons, Generic's).
SDL_GamepadButtonLabel familyFaceLabel(LabelFamily family, PadButton b);
/// A label in words, for a line of text ("A", "Cross", "Bottom button", "D-pad Up", "LB").
std::string labelWords(const ButtonLabel& label);

/// What an action's prompt shows: the button it's on, with the modifier's before it when that's
/// on the modifier's layer. No button: nothing to show.
struct PromptLabels {
    std::optional<ButtonLabel> modifier;
    std::optional<ButtonLabel> button;

    bool operator==(const PromptLabels&) const = default;
};

/// The HUD's pad prompts: the five slots' (InvokeSlot1-5) and the potions' (QuickHealth,
/// QuickMana, QuickCurePoison), made again only when the bindings or the pad change (`key`).
struct HudPrompts {
    std::array<PromptLabels, 5> slots;
    std::array<PromptLabels, 3> potions;
};
struct PromptCache {
    HudPrompts hud;
    std::uint64_t key = ~0ull;    ///< what `hud` was made for
    std::uint32_t generation = 0; ///< counts the configs put in use (applyPadConfig)
};

} // namespace noxsdl::pad
