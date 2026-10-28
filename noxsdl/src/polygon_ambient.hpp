// The view's ambient light from the map's polygons and AmbientData, as the game's polygonDrawColor
// works it out once a frame (no SDL here, so the tests check it).
//
// - The polygons are kept as the game keeps them for its tests: every one in list order (its
//   number is its place + 1), its corners truncated to whole units, their box.
// - Each frame, when the view's centre (whole units) has moved since the last frame, or nothing is
//   known yet: the polygon it's in (polygonIsPlayerInPolygon: the one it was in tested first, then
//   the rest in list order, so of overlapping polygons the one entered first keeps it). Outside
//   them all the map's ambient; in one, that polygon's ambient on entering it. It switches at once
//   (no fade), and stays put while the centre stands still.
// - The minimap's group (the player's currentPolygonByte130) is set with it: 1 outside every
//   polygon (or with none at all), the polygon's group on entering one; it stays with the polygon
//   the view is in, not the last one it left.
// - With no polygons at all, the map's ambient every frame.
// - The map's ambient is its AmbientData; a map without one keeps the 25, 25, 25 the level switch
//   (mapSwitchLevel; a client's world reset) sets before every map load.
#pragma once

#include "noxcore/map/polygon_hit.hpp"
#include "noxcore/map/sections.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace noxsdl {

class PolygonAmbient {
public:
    using Colour = std::array<std::uint8_t, 3>;
    static constexpr Colour kNoAmbientData{25, 25, 25};

    /// The map's AmbientData (null: it has none) and Polygons; what the view is in forgotten.
    void load(const nox::map::AmbientData* ambient, const nox::map::Polygons* polygons)
    {
        mapAmbient_ = ambient ? Colour{clamp8(ambient->r), clamp8(ambient->g), clamp8(ambient->b)} : kNoAmbientData;
        areas_.clear();
        if (polygons) {
            // Corners by id (server_mapRWPolygons), truncated; a missing id skipped.
            std::map<std::uint32_t, std::pair<int, int>> corners;
            for (const auto& p : polygons->points) {
                corners[p.id] = {static_cast<int>(p.x), static_cast<int>(p.y)};
            }
            for (const auto& poly : polygons->polygons) {
                Area a;
                a.ambient = poly.ambientLight;
                a.minimapGroup = poly.minimapGroup;
                for (const auto id : poly.points) {
                    if (const auto it = corners.find(id); it != corners.end()) {
                        a.points.push_back(it->second);
                    }
                }
                if (!a.points.empty()) {
                    a.minX = a.maxX = a.points[0].first;
                    a.minY = a.maxY = a.points[0].second;
                    for (const auto& [px, py] : a.points) {
                        a.minX = std::min(a.minX, px);
                        a.maxX = std::max(a.maxX, px);
                        a.minY = std::min(a.minY, py);
                        a.maxY = std::max(a.maxY, py);
                    }
                }
                areas_.push_back(std::move(a));
            }
        }
        state_ = {};
    }

    /// This frame's ambient with the view's centre at (x, y) (call once a frame).
    Colour at(float fx, float fy)
    {
        const int x = static_cast<int>(fx);
        const int y = static_cast<int>(fy);
        if (areas_.empty()) {
            state_.area = 0;
            state_.ambient = mapAmbient_;
            state_.group = 1;
        } else if (state_.area == kUnknown || x != state_.x || y != state_.y) {
            const std::uint32_t found = areaAt(x, y, state_.area);
            if (found == 0) {
                state_.area = 0;
                state_.ambient = mapAmbient_;
                state_.group = 1;
            } else if (found != state_.area) {
                state_.area = found;
                state_.ambient = areas_[found - 1].ambient;
                state_.group = areas_[found - 1].minimapGroup;
            }
        }
        state_.x = x;
        state_.y = y;
        return state_.ambient;
    }

    /// The polygon (place + 1) whose ambient at() last gave; 0 the map's (or nothing yet).
    std::uint32_t polygon() const { return state_.area == kUnknown ? 0 : state_.area; }
    /// The minimap's group as at() last set it: 1 outside every polygon, else the group of the
    /// polygon it's in (1 before any frame).
    std::uint8_t group() const { return state_.group; }
    Colour mapAmbient() const { return mapAmbient_; }

    /// The minimap group of the first polygon (list order) containing (x, y), or nullopt.
    std::optional<std::uint8_t> groupAt(float x, float y)
    {
        if (const std::uint32_t a = areaAt(static_cast<int>(x), static_cast<int>(y), 0)) {
            return areas_[a - 1].minimapGroup;
        }
        return std::nullopt;
    }

    /// The polygon (place + 1) whole-unit (x, y) is in, `current` tested first, or 0
    /// (polygonIsPlayerInPolygon: the box, then the ray's crossings).
    std::uint32_t areaAt(int x, int y, std::uint32_t current)
    {
        const auto inside = [&](const Area& a) {
            return a.minX <= x && x <= a.maxX && a.minY <= y && y <= a.maxY && !a.points.empty() &&
                   nox::map::polygonHit(a.points, x, y, ray_);
        };
        if (current != 0 && current != kUnknown && current <= areas_.size() && inside(areas_[current - 1])) {
            return current;
        }
        for (std::uint32_t i = 1; i <= areas_.size(); ++i) {
            if (i != current && inside(areas_[i - 1])) {
                return i;
            }
        }
        return 0;
    }

private:
    static constexpr std::uint32_t kUnknown = 0xDEADFACEu; ///< nothing tested yet
    static std::uint8_t clamp8(std::uint32_t v) { return static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 255)); }

    struct Area {
        std::vector<std::pair<int, int>> points;
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        Colour ambient{};
        std::uint8_t minimapGroup = 0;
    };
    struct State {
        std::uint32_t area = kUnknown; ///< the polygon the centre is in (0: none)
        int x = 0;                     ///< where the centre was last frame
        int y = 0;
        Colour ambient = kNoAmbientData;
        std::uint8_t group = 1; ///< the minimap's group (currentPolygonByte130)
    };
    Colour mapAmbient_ = kNoAmbientData;
    std::vector<Area> areas_;
    State state_;
    std::uint32_t ray_ = 0; ///< the polygon test's corner counter (map/polygon_hit.hpp)
};

} // namespace noxsdl
