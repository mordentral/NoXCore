// HD water's ripples and splashes (water_ripples.hpp).
#include "water_ripples.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

namespace {

constexpr float kTouching = 4;      ///< world units above the surface: in it
constexpr float kFalling = 3;       ///< higher than this, then touching: it landed
constexpr float kMoving = 8;        ///< world units a second: it leaves a wake
constexpr double kWakeEvery = 0.22; ///< seconds between a mover's rings
constexpr double kStillEvery = 1.4; ///< between a creature's rings standing in the water

/// A 0-1 fraction from an id (staggers the standing rings).
double fraction(std::uint32_t id)
{
    id = (id ^ 61u) ^ (id >> 16);
    id *= 9u;
    id ^= id >> 4;
    id *= 0x27d4eb2du;
    id ^= id >> 15;
    return static_cast<double>(id & 0xFFFF) / 65536.0;
}

} // namespace

WaterRipples::Tracked* WaterRipples::find(std::uint32_t id)
{
    for (Tracked& t : tracked_) {
        if (t.used && t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

void WaterRipples::track(std::uint32_t id, float x, float y, float z, bool wet, Kind kind, bool dying, bool inner, double now)
{
    Tracked* t = find(id);
    const bool fresh = t == nullptr;
    if (fresh) {
        // A free slot, else the one seen longest ago.
        t = &tracked_[0];
        for (Tracked& c : tracked_) {
            if (!c.used) {
                t = &c;
                break;
            }
            if (c.lastSeen < t->lastSeen) {
                t = &c;
            }
        }
        *t = Tracked{};
        t->id = id;
        t->used = true;
        const double every = kind == kCreature ? kStillEvery : kWakeEvery;
        t->lastRing = now - every * fraction(id); // (not all at once as they come into view)
        if (started_ < 0) {
            started_ = now;
        }
        // An item come to be in the water well inside the view, once the view has settled: it was
        // dropped or thrown there (one scrolled into view comes in at the edge).
        if (kind == kItem && wet && inner && z <= kTouching && now - started_ > 1.0) {
            add(x, y, 0.55f, true, now);
        }
    } else {
        const double dt = now - t->lastSeen;
        if (dt > 1e-4) {
            const float moved = std::hypot(x - t->x, y - t->y);
            const float speed = static_cast<float>(moved / dt);
            t->speed += (speed - t->speed) * 0.35f;
        }
        if (wet && t->z > kFalling && z <= 1) {
            add(x, y, 0.9f, true, now); // landed in it
        }
        if (wet && dying && !t->dying && kind == kCreature) {
            add(x, y, 0.8f, true, now); // fell dying in it
        }
    }
    if (wet && z <= kTouching && kind != kMissile) {
        const bool moving = t->speed > kMoving;
        if ((moving || kind == kCreature) && now - t->lastRing >= (moving ? kWakeEvery : kStillEvery)) {
            add(x, y, moving ? std::clamp(t->speed / 120.0f, 0.3f, 0.7f) : 0.2f, false, now);
            t->lastRing = now;
        }
    }
    t->x = x;
    t->y = y;
    t->z = z;
    t->wet = wet;
    t->dying = dying;
    t->inner = inner;
    t->kind = kind;
    t->seen = true;
    t->lastSeen = now;
}

void WaterRipples::endFrame(double now)
{
    for (Tracked& t : tracked_) {
        if (!t.used) {
            continue;
        }
        if (t.seen) {
            t.seen = false;
            continue;
        }
        // Gone: a missile, or something in the air, that went while over the water well inside
        // the view ended in it.
        if (t.wet && t.inner && now - t.lastSeen < 0.5 && (t.kind == kMissile || t.z > kFalling)) {
            add(t.x, t.y, t.kind == kMissile ? 0.75f : 0.9f, true, now);
        }
        t.used = false;
    }
}

void WaterRipples::add(float x, float y, float strength, bool splash, double now)
{
    sources_[static_cast<std::size_t>(next_)] = Source{x, y, now, std::clamp(strength, 0.0f, 1.0f), splash};
    next_ = (next_ + 1) % kSources;
}

void WaterRipples::clear()
{
    sources_.fill(Source{});
    tracked_.fill(Tracked{});
    next_ = 0;
    started_ = -1;
}

int WaterRipples::pack(double now, std::span<float> out) const
{
    int n = 0;
    const int most = static_cast<int>(out.size() / 4);
    for (const Source& s : sources_) {
        const double age = now - s.born;
        if (n >= most || s.strength <= 0 || age < 0 || age >= kLife) {
            continue;
        }
        float* o = &out[static_cast<std::size_t>(n) * 4];
        o[0] = s.x;
        o[1] = s.y;
        o[2] = static_cast<float>(age);
        o[3] = s.splash ? -s.strength : s.strength;
        ++n;
    }
    return n;
}

} // namespace noxsdl
