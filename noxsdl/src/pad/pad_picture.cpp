// (noxcpp) The Controller screen's picture of a pad, as geometry (pad_picture.hpp).
#include "pad/pad_picture.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace noxsdl::pad {

namespace {

using B = PadButton;

constexpr std::array<PadSpot, 17> kSpots{{
    {B::LeftTrigger, {0.22f, 0.0375f}, 0.09f, 0.0375f, false},
    {B::RightTrigger, {0.78f, 0.0375f}, 0.09f, 0.0375f, false},
    {B::LeftShoulder, {0.22f, 0.09f}, 0.11f, 0.025f, false},
    {B::RightShoulder, {0.78f, 0.09f}, 0.11f, 0.025f, false},
    {B::LeftStick, {0.27f, 0.25f}, 0.075f, 0.075f, true},
    {B::RightStick, {0.63f, 0.43f}, 0.075f, 0.075f, true},
    {B::DpadUp, {0.37f, 0.38f}, 0.022f, 0.038f, false},
    {B::DpadDown, {0.37f, 0.50f}, 0.022f, 0.038f, false},
    {B::DpadLeft, {0.31f, 0.44f}, 0.038f, 0.022f, false},
    {B::DpadRight, {0.43f, 0.44f}, 0.038f, 0.022f, false},
    {B::North, {0.73f, 0.188f}, 0.03f, 0.03f, true},
    {B::South, {0.73f, 0.312f}, 0.03f, 0.03f, true},
    {B::West, {0.668f, 0.25f}, 0.03f, 0.03f, true},
    {B::East, {0.792f, 0.25f}, 0.03f, 0.03f, true},
    {B::View, {0.435f, 0.24f}, 0.03f, 0.016f, false},
    {B::Start, {0.565f, 0.24f}, 0.03f, 0.016f, false},
    {B::Guide, {0.5f, 0.33f}, 0.026f, 0.026f, true, false},
}};

/// The left column's buttons and the right's.
constexpr std::array<B, 8> kLeft{B::LeftTrigger, B::LeftShoulder, B::View, B::LeftStick, B::DpadUp, B::DpadLeft, B::DpadRight, B::DpadDown};
constexpr std::array<B, 8> kRight{B::RightTrigger, B::RightShoulder, B::North, B::Start, B::West, B::East, B::South, B::RightStick};

const PadSpot* spotOf(B b)
{
    const auto it = std::ranges::find(kSpots, b, &PadSpot::button);
    return it == kSpots.end() ? nullptr : &*it;
}

/// Rows as near their wanted heights as `gap` apart allows, within [top, bottom] (wanted sorted).
void spread(std::vector<float>& ys, float gap, float top, float bottom)
{
    const std::size_t n = ys.size();
    if (n == 0) {
        return;
    }
    // Down from the top: each at least a gap under the one before; then up from the bottom.
    ys[0] = std::max(ys[0], top);
    for (std::size_t i = 1; i < n; ++i) {
        ys[i] = std::max(ys[i], ys[i - 1] + gap);
    }
    ys[n - 1] = std::min(ys[n - 1], bottom);
    for (std::size_t i = n - 1; i-- > 0;) {
        ys[i] = std::min(ys[i], ys[i + 1] - gap);
    }
    // (Too many for the room: from the top, a gap apart, past the bottom.)
    if (ys[0] < top) {
        for (std::size_t i = 0; i < n; ++i) {
            ys[i] = top + gap * static_cast<float>(i);
        }
    }
}

} // namespace

std::span<const PadSpot> padSpots()
{
    return kSpots;
}

PictureLayout layoutPicture(float x, float y, float w, float h, float labelW, float rowH)
{
    PictureLayout l;
    l.x = x;
    l.y = y;
    l.w = w;
    l.h = h;
    l.labelW = labelW;
    l.rowH = rowH;
    const float gap = std::max(4.0f, w * 0.015f);
    l.padW = std::max(10.0f, std::min(w - 2 * (labelW + gap), h / kPadAspect));
    l.padX = x + (w - l.padW) / 2;
    l.padY = y + (h - l.padW * kPadAspect) / 2;
    for (const bool left : {true, false}) {
        const auto& order = left ? kLeft : kRight;
        std::vector<Callout> side;
        for (const B b : order) {
            if (const PadSpot* s = spotOf(b)) {
                side.push_back(Callout{b, left, 0, l.toBox(s->at)});
            }
        }
        std::ranges::stable_sort(side, {}, [](const Callout& c) { return c.anchor.y; });
        std::vector<float> ys;
        for (const auto& c : side) {
            ys.push_back(c.anchor.y);
        }
        spread(ys, rowH, y + rowH / 2, y + h - rowH / 2);
        for (std::size_t i = 0; i < side.size(); ++i) {
            side[i].y = ys[i];
            l.callouts.push_back(side[i]);
        }
    }
    return l;
}

std::optional<PadButton> pictureButtonAt(const PictureLayout& l, float px, float py)
{
    for (const auto& c : l.callouts) {
        const float lx = l.labelX(c);
        if (px >= lx && px < lx + l.labelW && py >= c.y - l.rowH / 2 && py < c.y + l.rowH / 2) {
            return c.button;
        }
    }
    // The places, a little larger than drawn (small buttons are easy to miss); where two larger
    // places meet, the one nearer its middle.
    std::optional<PadButton> best;
    float bestDistance = 1.0f;
    for (const PadSpot& s : kSpots) {
        if (!s.callout) {
            continue;
        }
        const PicturePoint at = l.toBox(s.at);
        const float rx = s.rx * l.padW + 2, ry = s.ry * l.padW + 2;
        const float dx = (px - at.x) / rx, dy = (py - at.y) / ry;
        const float d = s.round ? std::sqrt(dx * dx + dy * dy) : std::max(std::abs(dx), std::abs(dy));
        if (d <= bestDistance) {
            bestDistance = d;
            best = s.button;
        }
    }
    return best;
}

} // namespace noxsdl::pad
