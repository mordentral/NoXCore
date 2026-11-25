// (noxcpp) The Controller screen's picture of a pad, as geometry (no SDL): a plain generic pad (a
// body with two grips, two bumpers and two triggers on top, two sticks, a D-pad, four face buttons,
// View and Start, a middle button), where each button sits, and the callouts naming what each does:
// a label column down each side, each label as near its button's height as the others let it, a
// line from it to the button.
//
// The pad is drawn in units of its width (kPadAspect tall); layoutPicture fits it into a box of the
// screen's layout with the label columns beside it.
#pragma once

#include "pad/pad_bindings.hpp"

#include <optional>
#include <span>
#include <vector>

namespace noxsdl::pad {

/// The pad's height for a width of 1.
inline constexpr float kPadAspect = 0.68f;

struct PicturePoint {
    float x = 0;
    float y = 0;
};

/// A button's place on the pad, in pad units: its middle and half sizes (a circle when `round`).
struct PadSpot {
    PadButton button = PadButton::South;
    PicturePoint at;
    float rx = 0;
    float ry = 0;
    bool round = true;
    bool callout = true; ///< named by a callout (the middle button isn't: the system's)
};
/// Every drawn button's place.
std::span<const PadSpot> padSpots();

/// The body's parts, in pad units: a box with round corners and two grips (ellipses).
struct PadBody {
    float x = 0.07f, y = 0.10f, w = 0.86f, h = 0.40f, radius = 0.13f;
    struct Grip {
        PicturePoint at;
        float rx = 0, ry = 0, turn = 0; ///< turn: radians
    };
    Grip left{{0.21f, 0.47f}, 0.14f, 0.21f, 0.35f};
    Grip right{{0.79f, 0.47f}, 0.14f, 0.21f, -0.35f};
};

struct Callout {
    PadButton button = PadButton::South;
    bool left = true;     ///< in the left column (else the right)
    float y = 0;          ///< its row's middle
    PicturePoint anchor;  ///< the button's middle
};

/// The picture laid out in a box (layout units).
struct PictureLayout {
    float x = 0, y = 0, w = 0, h = 0; ///< the box
    float padX = 0, padY = 0, padW = 0; ///< the pad's top left and width (kPadAspect * padW tall)
    float labelW = 0; ///< each label column's width, at the box's two edges
    float rowH = 0;   ///< a label row's height
    std::vector<Callout> callouts;

    PicturePoint toBox(PicturePoint unit) const { return {padX + unit.x * padW, padY + unit.y * padW}; }
    float scale() const { return padW; }
    /// A label's row's left edge.
    float labelX(const Callout& c) const { return c.left ? x : x + w - labelW; }
};

/// The picture in the box at (x, y), w by h, with labels `labelW` wide and rows `rowH` tall.
PictureLayout layoutPicture(float x, float y, float w, float h, float labelW, float rowH);
/// The button whose place or label is at (x, y), if any.
std::optional<PadButton> pictureButtonAt(const PictureLayout& layout, float x, float y);

} // namespace noxsdl::pad
