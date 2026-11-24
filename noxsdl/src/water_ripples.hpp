// HD water's ripples and splashes (HD Spell FX): a small fixed pool of ring sources the water
// shader (shaders/fx_water.frag.hlsl) draws, fed each frame from the drawables in view over the
// water. Visual only, and only from what the client already knows of each drawable (where it is,
// how high, what class, whether it dies); no SDL here (the tests take it as it is).
//
//   - something standing in the water rings it now and then; moving through it, a ring every few
//     tenths of a second (a wake);
//   - something landing in it (its height falling to the surface), a missile ending over it (gone
//     while flying over it), an item come to be in it (dropped or thrown there), or a creature
//     dying in it splashes: a strong ring and a burst of droplets.
//
// Fixed sizes, no allocation after construction: kSources rings in flight (the oldest replaced),
// kTracked drawables followed: those over the water, and once followed, while they're seen (none
// let go for a newcomer: past kTracked, a new one isn't followed).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace noxsdl {

class WaterRipples {
public:
    WaterRipples();

    static constexpr int kSources = 24;
    static constexpr int kTracked = 96;
    static constexpr float kLife = 2.5f; ///< seconds a ring lasts

    struct Source {
        float x = 0;
        float y = 0;
        double born = -1e9; ///< seconds
        float strength = 0; ///< 0-1
        bool splash = false;
    };

    /// What a drawable in view is, for track().
    enum Kind : std::uint8_t {
        kOther = 0,
        kCreature = 1, ///< a player or monster
        kMissile = 2,
        kItem = 3,
    };

    /// A drawable near the view this frame, at its ground point (x, y), `z` above it, at `now`
    /// seconds; `wet`: (x, y) is over the water; `dying`: a creature in its death; `inner`: well
    /// inside the view (one gone from there ended there, not off the screen's edge). One not
    /// followed yet and not over the water is passed over.
    void track(std::uint32_t id, float x, float y, float z, bool wet, Kind kind, bool dying, bool inner, double now);
    /// After the frame's track() calls: the drawables not seen this frame are let go, a missile or
    /// a falling thing gone over the water splashing where it was.
    void endFrame(double now);
    /// A ring (or a splash) at (x, y) now.
    void add(float x, float y, float strength, bool splash, double now);

    /// The rings in flight at `now`, packed for the shader: x, y, age (seconds), strength (negative
    /// for a splash). Returns how many it wrote (at most out.size() / 4).
    int pack(double now, std::span<float> out) const;
    /// How many drawables are followed now (the tests').
    int followed() const { return kTracked - freeCount_; }

private:
    struct Tracked {
        std::uint32_t id = 0;
        bool used = false;
        bool seen = false;    ///< this frame
        bool wet = false;
        bool dying = false;
        bool inner = false;
        Kind kind = kOther;
        float x = 0;
        float y = 0;
        float z = 0;
        float speed = 0;      ///< world units a second, smoothed
        double lastSeen = 0;
        double lastRing = -1e9;
    };
    /// The tracked by id: an open-addressed table of tracked_ slots (-1 empty), twice and more as
    /// big as kTracked.
    static constexpr std::size_t kIndexSize = 256;
    static_assert(kIndexSize >= 2 * kTracked && (kIndexSize & (kIndexSize - 1)) == 0);
    static std::size_t home(std::uint32_t id);
    Tracked* find(std::uint32_t id);
    void release(Tracked& t);
    void resetFree();
    std::array<Source, kSources> sources_{};
    std::array<Tracked, kTracked> tracked_{};
    std::array<std::int16_t, kIndexSize> index_{};
    std::array<std::uint8_t, kTracked> free_{}; ///< the free slots, the next taken last
    int freeCount_ = 0;
    int next_ = 0; ///< the source replaced next (the oldest: they're added in order)
    double started_ = -1; ///< the first track()'s time
};

} // namespace noxsdl
