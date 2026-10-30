// What the mouse is on in the world and which cursor that gives (cursor_pick.hpp).
#include "cursor_pick.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::pick {

namespace {

/// A float to whole units as the game converts them: cut toward zero.
int chop(float v)
{
    return static_cast<int>(v);
}

/// A box's corners from its centre: w / (2 sqrt 2) and h / (2 sqrt 2), as the shape keeps them.
constexpr float kHalfDiagonal = 0.35355339f;
/// The point-in-box test's scale (1 / sqrt 2): the sides' distances, signed.
constexpr float kSideScale = 0.70710677f;

} // namespace

bool under(const Shape& shape, int x, int y, int z, int cx, int cy)
{
    // The two levels: the footprint at the bottom (y - ZSIZE bottom - z) and at the top
    // (y - ZSIZE top - z).
    const auto fz = static_cast<float>(z);
    const int top = chop((static_cast<float>(y) - shape.zMax) - fz);
    const int bottom = chop((static_cast<float>(y) - shape.zMin) - fz);
    if (!shape.box) {
        const int r = chop(shape.radius);
        const std::int64_t r2 = chop(shape.radius * shape.radius);
        // Between the two levels: within the radius sideways.
        if (cy <= bottom && top <= cy) {
            return x - r < cx && cx < x + r;
        }
        // Below the bottom or above the top: inside the circle at that level.
        const std::int64_t dx = cx - x;
        const std::int64_t dy = cy - (cy <= bottom ? top : bottom);
        return dx * dx + dy * dy < r2;
    }
    // A box turned 45 degrees: its top, left and right corners from the centre.
    const float f1 = shape.width * kHalfDiagonal;
    const float f2 = shape.height * kHalfDiagonal;
    const float topX = f2 - f1, topY = -f1 - f2;
    const float leftX = -f1 - f2, leftY = f2 - f1;
    const float rightX = f2 + f1, rightY = f1 - f2;
    const auto qx = static_cast<float>(cx);
    const auto qy = static_cast<float>(cy);
    // Inside its four sides with the centre at (x, level).
    const auto inBox = [&](int level) {
        const auto px = static_cast<float>(x);
        const auto py = static_cast<float>(level);
        const float a = (((topY + py) - (topX + px)) + qx) - qy;
        const float b = (((leftY + py) - (leftX + px)) + qx) - qy;
        const float d = ((((rightY + py) + rightX) + px) - qx) - qy;
        const float e = (((topY + py) + (topX + px)) - qx) - qy;
        return a * kSideScale < 0 && 0 < b * kSideScale && 0 < d * kSideScale && e * kSideScale < 0;
    };
    if (inBox(bottom) || inBox(top)) {
        return true;
    }
    // Between the levels: the left half from the left corner to the centre, the band between the
    // levels moved by that corner's height; the right half likewise from the centre to the right
    // corner.
    const int lx = chop(leftX);
    const int ly = chop(leftY);
    if (x + lx < cx && cx < x && top + ly < cy && cy < bottom + ly) {
        return true;
    }
    const int rx = chop(rightX);
    const int ry = chop(rightY);
    return x <= cx && cx < x + rx && top + ry < cy && cy < bottom + ry;
}

bool nearCursor(int x, int y, int cx, int cy)
{
    // The drawables' grid: a square for every 128 units (the division cut toward zero).
    constexpr int kCell = 128;
    constexpr int kReach = 96;
    const auto within = [](int v, int c) {
        const int lo = std::max(0, (c - kReach) / kCell);
        const int hi = (c + kReach) / kCell;
        const int at = v / kCell;
        return lo <= at && at <= hi;
    };
    return within(x, cx) && within(y, cy);
}

int depth(const Shape& shape, int y, int z)
{
    return chop((static_cast<float>(z) + static_cast<float>(y)) + shape.zMin);
}

WorldCursor worldCursor(const Target& t, const Situation& s)
{
    namespace oc = nox::things::ObjectClass;
    constexpr std::uint32_t kShopkeeper = 0x8; // a monster's SUBCLASS SHOPKEEPER
    constexpr std::uint32_t kUseable = 0x80;   // an immobile thing's SUBCLASS USEABLE
    constexpr std::uint32_t kTalks = 0x10;     // X_STATUS: something to say
    constexpr std::int64_t kTalkReach = 0x3D08; // 125 squared, less one (15624)
    constexpr std::int64_t kReach = 0x15F8;     // 75 squared, less one (5624)
    if (s.wideScreen) {
        return WorldCursor::None; // (cursorUpdate: no world cursor while the widescreen's on)
    }
    const bool monster = (t.classes & oc::Monster) != 0;
    const bool talker = monster && (t.xStatus & kTalks);
    const bool shopkeeper = monster && (t.subclasses & kShopkeeper);
    if (!(t.classes & (oc::Immobile | oc::Monster | oc::Player)) || talker || shopkeeper) {
        if (!(t.classes & oc::Pickup) || s.dragging) {
            // Within 125, not in a dialog or a shop: the Talk hand, else a shopkeeper's Trade.
            if (t.dist2 > kTalkReach || s.busy) {
                return WorldCursor::None;
            }
            return talker ? WorldCursor::Talk : shopkeeper ? WorldCursor::Trade : WorldCursor::None;
        }
        if (t.dist2 > kReach) {
            return WorldCursor::PickupFar;
        }
        return s.canUse ? WorldCursor::Pickup : WorldCursor::Caution;
    }
    // A useable immobile thing (levers, switches, buttons), or a Mimic still a chest: the Use hand
    // within 75, enabled, X_STATUS bits 4 and 8 clear.
    const bool blocked = (t.xStatus & 0xC) != 0;
    const bool useable = (t.classes & oc::Immobile) && (t.subclasses & kUseable) && !blocked;
    if ((useable || t.mimicAsleep) && t.dist2 <= kReach && t.enabled && !blocked) {
        return WorldCursor::Use;
    }
    return WorldCursor::None;
}

std::uint8_t facing(float fromX, float fromY, float cursorX, float cursorY, std::optional<int> frontY)
{
    // (The game's x87: the angle from FPATAN, the turn, the units a turn and the half its float
    // constants; the sum cut toward zero.)
    constexpr float kTurn = 6.2831855f;     // 2 pi
    constexpr float kPerTurn = 40.743664f;  // 256 / (2 pi)
    const double dx = static_cast<double>(cursorX) - fromX;
    const double dy = (frontY ? static_cast<double>(*frontY) : static_cast<double>(cursorY)) - fromY;
    int f = static_cast<int>((std::atan2(dy, dx) + kTurn) * kPerTurn + 0.5f);
    if (f < 0) {
        f += static_cast<int>((0xFFu - static_cast<unsigned>(f)) & 0xFFFFFF00u);
    }
    if (f > 0xFF) {
        f -= static_cast<int>((static_cast<unsigned>(f) >> 8) << 8);
    }
    return static_cast<std::uint8_t>(f);
}

} // namespace noxsdl::pick
