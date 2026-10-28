// Line of sight: what the local player can see past walls (noxcore's SightRegion, the game's
// drawBlack), and its drawing.
//
// The region is worked out over the game's view of the world (the camera's rectangle, one unit a
// pixel) from the eye at its centre: the floor is drawn only inside it (a mask multiplied over the
// floor), walls only when the sight reaches them and as the arms it reaches make them (drawWalls,
// MapView), objects only where they're seen. With Soft Shadow Edge (nox.cfg SoftShadowEdge), the
// shadow edges fade into the seen floor as the game's ten black lines.
//
// Objects occlude too: every drawable flagged SHADOW (0x10000; the client keeps them on their own
// list) that isn't invisible. A door (class DOOR, unless its subclass is GATE) blocks along its
// leaf, from its position to the offset for its angle (table). Anything else blocks the angles its
// circle spans, or its box's face turned to the eye (the diagonal across when two are). Such an
// object is drawn when the sight reached it (reached()), not by where its position is; any other
// drawable by the game's drawable sight test (drawableSeen).
#pragma once

#include "camera.hpp"
#include "thing_art.hpp"

#include "noxcore/client/sight.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

namespace noxsdl {

class SightMap {
public:
    SightMap() = default;
    ~SightMap();
    SightMap(const SightMap&) = delete;
    SightMap& operator=(const SightMap&) = delete;

    /// An object in the way: a segment (a door's leaf), a circle, or a box.
    struct Blocker {
        float x0 = 0, y0 = 0; ///< the segment's ends, or the circle's or box's position
        float x1 = 0, y1 = 0;
        float radius = 0;     ///< > 0: a circle
        float boxWidth = 0;   ///< > 0: a box (EXTENT BOX width x height)
        float boxHeight = 0;
        std::uint32_t key = 0; ///< who it is, for reached() (0: nobody asks)
    };

    /// Rebuilds the visible region for the camera's view, from its centre. `walls` are the ones
    /// standing (MapView::walls: broken and open secret walls left out); `windows` holds the
    /// window walls (x << 8 | y). `eyeShiftY` is the screen shake as swung for this frame
    /// (SightRegion::View::eyeShiftY).
    void build(const Camera& camera, std::span<const nox::map::Wall> walls, const std::set<std::uint16_t>& windows,
               const nox::things::ThingDb& things, std::span<const Blocker> blockers = {}, int eyeShiftY = 0);

    /// Whether the sight reached the blocker with this key (its interval made it into the
    /// sweep): how the game decides a sight-blocking object is drawn. Nullopt when no blocker
    /// had the key this frame.
    std::optional<bool> reached(std::uint32_t key) const;
    /// Blockers' keys: a live drawable's by its net code, a map object's by its place in the map's list.
    static std::uint32_t liveKey(std::uint16_t netCode) { return 0x10000u | netCode; }
    static std::uint32_t mapKey(std::size_t index) { return 0x80000000u | static_cast<std::uint32_t>(index); }

    /// The drawable sight test (SightRegion::drawableSeen) as the scene's sort, its list draws and
    /// the cursor's hover make it for a drawable of type `art` at (x, y), `z` high: a sight
    /// blocker (SHADOW), or a door that isn't a gate, is seen when the sweep reached it (by `key`;
    /// one that isn't in this frame's blockers never is); a gate is tested at its leaf's middle
    /// (`doorAngle`); anything else by its line from the eye and, failing that, by its box on the
    /// screen (from `image`, the generic draw's last image of it). Our own unit is the caller's.
    bool drawableSeen(const TypeArt& art, std::uint32_t key, float x, float y, float z, std::uint8_t doorAngle,
                      const nox::client::DrawnImage& image) const;

    /// Whether the eye sees the point (one on the region's edge counts). A point off the view is
    /// seen when the place its line from the eye leaves the view is. (The floor's and effects'
    /// test; drawables go by drawableSeen.)
    bool visible(float x, float y) const;

    /// The part of the segment (x0, y0) -> (x1, y1) that is visible, as parameters 0..1 of its
    /// first and last visible sample, or nullopt if none of it is.
    std::optional<std::pair<float, float>> visibleRange(float x0, float y0, float x1, float y1) const;

    /// Soft Shadow Edge (nox.cfg SoftShadowEdge): the shadow edges faded into the seen floor.
    void setSoftEdges(bool on) { softEdges_ = on; }

    /// Blacks out everything outside the visible region, in `camera`'s space, by multiplying a
    /// mask over what's drawn (at the screen's resolution, its edge anti-aliased by drawing it at
    /// twice that and scaling down), with the soft shadow edges in it when they're on.
    void drawMask(SDL_Renderer* renderer, const Camera& camera);

    /// The region's outline in world units.
    const std::vector<SDL_FPoint>& polygon() const { return polygon_; }
    /// The region: the floor, the drawables, the blockers reached.
    const nox::client::SightRegion& region() const { return region_; }
    /// The walls the sight reaches and how they're drawn: with the improved culling (the default),
    /// a region worked out over the view grown by the wall images' reach (SightRegion::wallView)
    /// with SightRegion's improved column test; classic, the region itself (the game's drawWalls).
    const nox::client::SightRegion& wallRegion() const { return classicWalls_ ? region_ : wallRegion_; }
    /// The game's own wall culling (noxsdl walls classic on), for comparison.
    void setClassicWalls(bool on) { classicWalls_ = on; }
    bool classicWalls() const { return classicWalls_; }

private:
    nox::client::SightRegion region_;
    nox::client::SightRegion wallRegion_; ///< the improved culling's (unused when classic)
    bool classicWalls_ = false;
    std::vector<SDL_FPoint> polygon_;
    std::unordered_map<std::uint32_t, std::size_t> keys_; ///< a blocker's key: its index in the build
    /// build()'s wall cells and blockers as the region takes them (kept for their memory).
    std::vector<nox::client::SightRegion::Wall> cells_;
    std::vector<nox::client::SightRegion::Blocker> objects_;
    bool softEdges_ = true;
    SDL_Texture* mask_ = nullptr;
    int maskW_ = 0;
    int maskH_ = 0;
};

} // namespace noxsdl
