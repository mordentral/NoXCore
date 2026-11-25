// (noxcpp) The game controller's numbers: a stick's dead zones, a trigger read as a button, the
// stick's tilt as the walk's or run's cursor point. Pure functions (no SDL), so the tests check them as they are.
//
// A stick is read as one point, not two axes: inside the inner dead zone it's at rest; past it the
// tilt is scaled again from 0 there to 1 at the outer dead zone (so a small push past the dead zone
// is a small move, not a jump to the zone's edge), and past the outer one it's full. The direction
// is kept as pushed. A trigger counts as pressed at its threshold and as let go a little under it,
// so a finger resting on the threshold doesn't press it over and over.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace noxsdl::pad {

/// A stick's point, -1 to 1 each way: x right, y down (as SDL reads it, and as the screen goes).
struct StickVec {
    float x = 0;
    float y = 0;

    bool operator==(const StickVec&) const = default;
    float length() const { return std::hypot(x, y); }
};

/// The defaults (noxcore_controller.cfg's DeadZone, OuterDeadZone, TriggerThreshold).
inline constexpr float kDefaultDeadZone = 0.18f;
inline constexpr float kDefaultOuterDeadZone = 0.95f;
inline constexpr float kDefaultTriggerThreshold = 0.35f;
/// How far under its threshold a pressed trigger must come back to count as let go.
inline constexpr float kTriggerHysteresis = 0.1f;

/// An SDL axis value (-32768 to 32767) as -1 to 1.
inline float axisUnit(std::int16_t v)
{
    return v >= 0 ? static_cast<float>(v) / 32767.0f : static_cast<float>(v) / 32768.0f;
}

/// A trigger's SDL axis value (0 to 32767; some drivers dip below 0 at rest) as 0 to 1.
inline float triggerUnit(std::int16_t v)
{
    return std::clamp(static_cast<float>(v) / 32767.0f, 0.0f, 1.0f);
}

/// The radial dead zone with rescale: `raw` at rest inside `inner`, full past `outer`, scaled
/// between, its direction kept. (outer is kept above inner.)
inline StickVec radialDeadZone(StickVec raw, float inner, float outer)
{
    inner = std::clamp(inner, 0.0f, 0.95f);
    outer = std::clamp(outer, inner + 0.01f, 1.0f);
    const float m = raw.length();
    if (m <= inner || m <= 0.0f) {
        return {};
    }
    const float scaled = std::clamp((m - inner) / (outer - inner), 0.0f, 1.0f);
    return {raw.x / m * scaled, raw.y / m * scaled};
}

/// A trigger as a button: pressed at `threshold`, let go under it less kTriggerHysteresis.
inline bool triggerHeld(bool wasHeld, float value, float threshold)
{
    threshold = std::clamp(threshold, 0.05f, 1.0f);
    if (wasHeld) {
        return value > std::max(threshold - kTriggerHysteresis, 0.02f);
    }
    return value >= threshold;
}

/// Walking with the stick: the game walks while the cursor point is under 100 units from the
/// player and runs past that (and, once running, till the player stops), so the stick puts the
/// point out from the player by its tilt, as the mouse's distance does: from kNearReach at the
/// dead zone's edge out to the run distance at the run threshold (noxcore_controller.cfg's
/// RunThreshold), then on to kRunReach at full tilt, a straight line each side.
inline constexpr float kNearReach = 24;
inline constexpr float kRunDistance = 100;
inline constexpr float kRunReach = 160;
/// The point at rest, while the pad is in use: a walk's length the way the stick last leaned.
inline constexpr float kWalkReach = 60;
inline constexpr float kDefaultRunThreshold = 0.65f;

/// The cursor point's distance from the player for a tilt (0 to 1, after the dead zone; 0 at
/// its edge) and the run threshold (kept within 0.3 to 0.95).
inline float stickReach(float tilt, float runThreshold = kDefaultRunThreshold)
{
    const float t = std::clamp(tilt, 0.0f, 1.0f);
    const float th = std::clamp(runThreshold, 0.3f, 0.95f);
    if (t <= th) {
        return kNearReach + (kRunDistance - kNearReach) * (t / th);
    }
    return kRunDistance + (kRunReach - kRunDistance) * ((t - th) / (1.0f - th));
}

} // namespace noxsdl::pad
