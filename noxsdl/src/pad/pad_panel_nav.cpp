// (noxcpp) The snap cursor's pure parts (pad_panel_nav.hpp).
#include "pad/pad_panel_nav.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace noxsdl::pad {

namespace {

/// How far `to`'s middle lies the way `dir` goes from `from`'s, and across it (signed along, the
/// offset across as a distance); whether their extents across the way overlap (the beam).
struct Along {
    float along = 0;
    float across = 0;
    bool beam = false;
};

Along along(const NavBox& from, const NavBox& to, NavDir dir)
{
    const float dx = to.cx() - from.cx();
    const float dy = to.cy() - from.cy();
    Along a;
    switch (dir) {
    case NavDir::Up: a.along = -dy; a.across = std::fabs(dx); break;
    case NavDir::Down: a.along = dy; a.across = std::fabs(dx); break;
    case NavDir::Left: a.along = -dx; a.across = std::fabs(dy); break;
    case NavDir::Right: a.along = dx; a.across = std::fabs(dy); break;
    }
    if (dir == NavDir::Up || dir == NavDir::Down) {
        a.beam = to.x < from.x + from.w && from.x < to.x + to.w;
    } else {
        a.beam = to.y < from.y + from.h && from.y < to.y + to.h;
    }
    return a;
}

} // namespace

std::optional<std::size_t> navPick(std::span<const NavBox> spots, const NavBox& from, NavDir dir)
{
    // (A middle under a pixel that way isn't that way: side by side spots of one row.)
    constexpr float kMin = 1.0f;
    std::optional<std::size_t> best;
    float bestScore = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (spots[i] == from) {
            continue;
        }
        const Along a = along(from, spots[i], dir);
        if (!a.beam || a.along < kMin) {
            continue;
        }
        const float score = a.along + a.across / 2;
        if (score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    if (best) {
        return best;
    }
    bestScore = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (spots[i] == from) {
            continue;
        }
        const Along a = along(from, spots[i], dir);
        if (a.along < kMin || a.along * 2 < a.across) {
            continue; // (more across than that way: not that way)
        }
        const float score = a.along + 2 * a.across;
        if (score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

std::optional<std::size_t> navJump(std::span<const NavBox> spots, std::span<const int> groups, const NavBox& from, int fromGroup,
                                   NavDir dir)
{
    std::vector<NavBox> outside;
    std::vector<std::size_t> index;
    for (std::size_t i = 0; i < spots.size() && i < groups.size(); ++i) {
        if (groups[i] != fromGroup) {
            outside.push_back(spots[i]);
            index.push_back(i);
        }
    }
    const auto pick = navPick(outside, from, dir);
    return pick ? std::optional<std::size_t>(index[*pick]) : std::nullopt;
}

std::optional<std::size_t> navNearest(std::span<const NavBox> spots, const NavBox& to)
{
    std::optional<std::size_t> best;
    float bestD = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < spots.size(); ++i) {
        const float d = std::hypot(spots[i].cx() - to.cx(), spots[i].cy() - to.cy());
        if (d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

std::optional<TrayStep> trayEdgeStep(int row, const TrayView& view, NavDir dir)
{
    const int last = view.firstRow + view.shown - 1;
    if (dir == NavDir::Up && row <= view.firstRow && row > 0) {
        return TrayStep{row - 1, -1};
    }
    if (dir == NavDir::Down && row >= last && row < view.lastRow) {
        return TrayStep{row + 1, 1};
    }
    return std::nullopt;
}

int trayLastRow(int lastFilledRow, bool holding, int last)
{
    if (holding) {
        return last;
    }
    return std::clamp(lastFilledRow, 0, last);
}

std::optional<NavDir> NavRepeat::frame(std::optional<NavDir> heldNow, std::uint64_t nowMs)
{
    if (heldNow != held_) {
        held_ = heldNow;
        if (!heldNow) {
            return std::nullopt;
        }
        nextMs_ = nowMs + kFirstMs;
        return heldNow;
    }
    if (held_ && nowMs >= nextMs_) {
        nextMs_ = nowMs + kEveryMs;
        return held_;
    }
    return std::nullopt;
}

std::optional<NavDir> stickDir(StickVec v, std::optional<NavDir> held)
{
    const float tilt = v.length();
    if (tilt < (held ? kStickLetGo : kStickPress)) {
        return std::nullopt;
    }
    if (std::fabs(v.x) > std::fabs(v.y)) {
        return v.x > 0 ? NavDir::Right : NavDir::Left;
    }
    return v.y > 0 ? NavDir::Down : NavDir::Up;
}

std::optional<NavDir> dpadDir(PadButton b)
{
    switch (b) {
    case PadButton::DpadUp: return NavDir::Up;
    case PadButton::DpadDown: return NavDir::Down;
    case PadButton::DpadLeft: return NavDir::Left;
    case PadButton::DpadRight: return NavDir::Right;
    default: return std::nullopt;
    }
}

PanelPress panelPress(PadButton b)
{
    switch (b) {
    case PadButton::South: return PanelPress::Accept;
    case PadButton::East: return PanelPress::Back;
    case PadButton::West: return PanelPress::Drop;
    case PadButton::North: return PanelPress::Move;
    case PadButton::RightShoulder: return PanelPress::Secondary;
    case PadButton::LeftShoulder: return PanelPress::PreviousPage;
    default: return PanelPress::None;
    }
}

bool modifierLayerPress(PadButton b, std::optional<PadButton> modifier, bool modifierHeld)
{
    return modifier && (b == *modifier || modifierHeld);
}

std::optional<DragEnd> padDragEnd(PanelPress p)
{
    switch (p) {
    case PanelPress::Accept: return DragEnd::QuickClick;
    case PanelPress::Drop: return DragEnd::Outside;
    case PanelPress::Secondary: return DragEnd::SecondaryBox;
    default: return std::nullopt;
    }
}

std::optional<int> slotOfButton(const PadConfig& config, PadButton b)
{
    static constexpr std::string_view kSlots[5] = {"InvokeSlot1", "InvokeSlot2", "InvokeSlot3", "InvokeSlot4", "InvokeSlot5"};
    for (const auto& binding : config.bindings) {
        if (binding.button != b || binding.withModifier) {
            continue;
        }
        for (int i = 0; i < 5; ++i) {
            if (binding.action == kSlots[i]) {
                return i;
            }
        }
    }
    return std::nullopt;
}

} // namespace noxsdl::pad
