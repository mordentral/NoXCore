// The game's point-in-polygon test for map polygons, shared by the server (which polygon a player
// is in: its enter and leave scripts) and the client (which polygon's ambient light the view takes:
// polygonDrawColor). Whole units throughout: the corners truncated, the point the caller's.
//
// The test (polygonIsPlayerInPolygon -> the polygon test): the point must lie in the polygon's box
// (pointInRect, the caller's), then a ray from it to one of the world's corners (0 or 0x1700 each
// way) is tested against every edge, the ends inclusive; an odd count is inside. Which corner turns
// with a counter shared by every test (its bit 1 before and after a step), so a ray that grazes a
// corner on one test takes another way on the next.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace nox::map {

/// Whether segment a (x1, y1, x2, y2) crosses segment b, both inclusive, as the game tests it.
inline bool segmentsCross(const int a[4], const int b[4])
{
    const int ax = a[0], ay = a[1], ex = a[2], ey = a[3];
    const int bx1 = b[0], by1 = b[1], bx2 = b[2], by2 = b[3];
    const int adx = ex - ax;
    const int bdx = bx1 - bx2;
    int aHi = ex, aLo = ax;
    if (adx < 0) {
        aHi = ax;
        aLo = ex;
    }
    int bHi = bx2, bLo = bx1;
    if (0 < bdx) {
        bHi = bx1;
        bLo = bx2;
    }
    // Their x spans must overlap, then their y spans; then the meeting's parameters (t along b,
    // u along a, both scaled by den) must each lie between 0 and den.
    if (!(bLo <= aHi && aLo <= bHi)) {
        return false;
    }
    const int bdy = by1 - by2;
    const int ady = ey - ay;
    int aHiY = ey, aLoY = ay;
    if (ady < 0) {
        aHiY = ay;
        aLoY = ey;
    }
    if (bdy < 1) {
        if (aHiY < by1 || by2 < aLoY) {
            return false;
        }
    } else if (aHiY < by2 || by1 < aLoY) {
        return false;
    }
    const int t = (ax - bx1) * bdy - (ay - by1) * bdx;
    const int den = ady * bdx - bdy * adx;
    if (den < 1) {
        if (0 < t || t < den) {
            return false;
        }
    } else if (t < 0 || den < t) {
        return false;
    }
    const int u = (ay - by1) * adx - (ax - bx1) * ady;
    if (den < 1) {
        return u < 1 && den <= u;
    }
    return -1 < u && u <= den;
}

/// Whether (x, y) is inside the polygon with these corners (non-empty; its box already passed),
/// by the crossings of a ray to the world corner `ray` picks; `ray` steps once.
inline bool polygonHit(std::span<const std::pair<int, int>> points, int x, int y, std::uint32_t& ray)
{
    const int ex = (ray & 2u) ? 0x1700 : 0;
    ++ray;
    const int ey = (ray & 2u) ? 0x1700 : 0;
    const int r[4] = {x, y, ex, ey};
    int edge[4] = {points[0].first, points[0].second, 0, 0};
    unsigned crossings = 0;
    const std::size_t n = points.size();
    for (std::size_t i = 1; i <= n; ++i) {
        const auto& [px, py] = points[i % n];
        // Each edge runs from the previous corner to this one: the two ends are replaced in turn.
        if ((i & 1) == 0) {
            edge[0] = px;
            edge[1] = py;
        } else {
            edge[2] = px;
            edge[3] = py;
        }
        if (segmentsCross(r, edge)) {
            ++crossings;
        }
    }
    return (crossings & 1) != 0;
}

} // namespace nox::map
