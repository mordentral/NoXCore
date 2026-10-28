// SightRegion (sight.hpp): drawBlack's sweep, the polygon it leaves, and what drawWalls and the
// soft shadow edge take from it.
#include "noxcore/client/sight.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace nox::client {

namespace {

constexpr int kCell = 23;
constexpr int kTurn = 75000;     // the sweep's angle units in a turn
constexpr int kFar = 0x7FFFFFFF; // the screen's sides: behind everything
constexpr int kQuarterSin = 0x1922; // a quarter turn in the sine table's units (4096 a radian)
constexpr int kSinTurn = 0x6488;    // a full turn in them

// The arms of each wall direction (1 NE, 2 NW, 4 SE, 8 SW).
constexpr std::array<std::uint8_t, 11> kArms{0x09, 0x06, 0x0F, 0x0B, 0x07, 0x0D, 0x0E, 0x03, 0x05, 0x0C, 0x0A};
// The direction the arms seen make, by the arms (-1: the wall's own). SE + SW make direction 9.
constexpr std::array<std::int8_t, 16> kSeenDirection{-1, -1, -1, 7, -1, 8, 1, 4, -1, 0, 10, 3, 9, 5, 6, 2};
// A wall's base line per direction in its cell (x0, y0, x1, y1); directions 7-10 have their second
// half at dir + 4.
constexpr std::array<std::array<int, 4>, 15> kBase{{
    {0, 23, 23, 0}, {0, 0, 23, 23}, {0, 11, 23, 11}, {0, 23, 23, 0}, {0, 0, 23, 23},
    {0, 23, 23, 0}, {0, 0, 23, 23}, {0, 0, 10, 10}, {13, 10, 23, 0}, {0, 23, 10, 13},
    {0, 0, 10, 10}, {13, 10, 23, 0}, {13, 13, 23, 23}, {13, 13, 23, 23}, {0, 23, 10, 13},
}};

// The arctangent table: atan(i / 1000) in the sweep's units, rounded (i up to 99999).
const std::vector<std::int16_t>& atanTable()
{
    static const std::vector<std::int16_t> table = [] {
        std::vector<std::int16_t> t(100000);
        const double scale = std::bit_cast<double>(0x40C7504F69421993ull); // 75000 / 2 pi
        for (int i = 0; i < 100000; ++i) {
            t[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(static_cast<int>(std::atan(i * 0.001) * scale + 0.5));
        }
        return t;
    }();
    return table;
}

// The sine table: 4096 steps a turn, sin * 4096 truncated.
const std::array<int, 4096>& sinTable()
{
    static const std::array<int, 4096> table = [] {
        std::array<int, 4096> t{};
        const double step = static_cast<double>(std::bit_cast<float>(0x3AC90FDBu)); // 2 pi / 4096, as a float
        for (int i = 0; i < 4096; ++i) {
            t[static_cast<std::size_t>(i)] = static_cast<int>(std::sin(i * step) * 4096.0);
        }
        return t;
    }();
    return table;
}

// sin(a / 4096) * 4096 from the table (a in 4096ths of a radian, any sign).
int sinOf(int a)
{
    if (a < 0) {
        a += ((kSinTurn - 1 - a) / kSinTurn) * kSinTurn;
    }
    if (a > kSinTurn - 1) {
        a %= kSinTurn;
    }
    return sinTable()[static_cast<std::size_t>((a * 0x1000) / kSinTurn)];
}

// tan(a / 4096) * 4096 (the extremes where the cosine is 0).
int tanOf(int a)
{
    const int s = sinOf(a);
    const int c = sinOf(kQuarterSin - a);
    if (c == 0) {
        return s < 1 ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
    }
    return (s << 12) / c;
}

// The sweep's angle in the sine table's units.
int sinUnits(int a)
{
    return static_cast<int>((static_cast<long long>(a) * kSinTurn) / kTurn);
}

// half * t / 4096 in 32-bit arithmetic, rounded toward zero.
int scaled(int half, int t)
{
    const auto v = static_cast<std::int32_t>(static_cast<std::uint32_t>(half) * static_cast<std::uint32_t>(t));
    return (v + ((v >> 31) & 0xFFF)) >> 12;
}

int normalized(int a)
{
    while (a < 0) {
        a += kTurn;
    }
    while (a > kTurn - 1) {
        a -= kTurn;
    }
    return a;
}

// A ray's two points: the eye and one 4096 units along it.
struct Ray {
    float x0, y0, x1, y1;
};

// x rounded to the nearest whole (a fraction of exactly a half stays), as the wall hits do.
float roundedHit(double q)
{
    const auto f = static_cast<float>(q);
    double whole = 0;
    const auto frac = static_cast<float>(std::modf(static_cast<double>(f), &whole));
    auto x = static_cast<float>(whole);
    if (frac > 0.5) {
        x += 1.0f;
    } else if (frac < -0.5) {
        x -= 1.0f;
    }
    return x;
}

// Where the ray meets the cell's anti-diagonal (x + y constant), its x kept within [lo, hi] of
// the cell's.
void hitAntiDiagonal(int cellX, int cellY, const Ray& r, int lo, int hi, float& outX, float& outY)
{
    const int ox = cellX * kCell;
    const int oy = cellY * kCell;
    const auto loX = static_cast<float>(lo + ox);
    const auto hiX = static_cast<float>(ox + hi);
    const int c = static_cast<int>(hiX + static_cast<float>((oy - hi) + 0x16));
    if (r.x1 == r.x0) {
        outX = r.x1;
        outY = static_cast<float>(c) - outX;
        return;
    }
    if (r.y1 == r.y0) {
        outY = r.y1;
        outX = static_cast<float>(c) - outY;
        return;
    }
    double m = (static_cast<double>(r.y1) - r.y0) / (static_cast<double>(r.x1) - r.x0);
    if (m == -1.0) {
        m = static_cast<double>(std::bit_cast<float>(0xBF7D70A4u)); // -0.99
    }
    float x = roundedHit((c - (r.y0 - m * r.x0)) / (m + 1.0));
    if (x < loX) {
        x = loX;
    } else if (x > hiX) {
        x = hiX;
    }
    outX = x;
    outY = static_cast<float>(c) - x;
}

// Where the ray meets the cell's diagonal (y - x constant).
void hitDiagonal(int cellX, int cellY, const Ray& r, int lo, int hi, float& outX, float& outY)
{
    const int ox = cellX * kCell;
    const int oy = cellY * kCell;
    const auto loX = static_cast<float>(ox + lo);
    const auto hiX = static_cast<float>(ox + hi);
    const int c = static_cast<int>(static_cast<float>(oy + lo) - loX);
    if (r.x1 == r.x0) {
        outX = r.x1;
        outY = static_cast<float>(c) + r.x1;
        return;
    }
    if (r.y1 == r.y0) {
        outX = r.y1 - static_cast<float>(c);
        outY = r.y1;
        return;
    }
    double m = (static_cast<double>(r.y1) - r.y0) / (static_cast<double>(r.x1) - r.x0);
    if (m == 1.0) {
        m = static_cast<double>(std::bit_cast<float>(0x3F7D70A4u)); // 0.99
    }
    float x = roundedHit((c - (r.y0 - m * r.x0)) / (m - 1.0));
    if (x < loX) {
        x = loX;
    } else if (x > hiX) {
        x = hiX;
    }
    outX = x;
    outY = static_cast<float>(c) + x;
}

// The ray against a wall arm (or full diagonal); the point is left as it was for anything else.
void hitWall(int cellX, int cellY, std::uint8_t edge, const Ray& r, float& x, float& y)
{
    switch (edge) {
    case 1:
        hitAntiDiagonal(cellX, cellY, r, 11, 23, x, y);
        break;
    case 2:
        hitDiagonal(cellX, cellY, r, 0, 11, x, y);
        break;
    case 4:
        hitDiagonal(cellX, cellY, r, 11, 23, x, y);
        break;
    case 6:
        hitDiagonal(cellX, cellY, r, 0, 23, x, y);
        break;
    case 8:
        hitAntiDiagonal(cellX, cellY, r, 0, 11, x, y);
        break;
    case 9:
        hitAntiDiagonal(cellX, cellY, r, 0, 23, x, y);
        break;
    default:
        break;
    }
}

// The ray's line against the segment's, if their bounding boxes meet.
bool hitSegment(const Ray& r, float sx0, float sy0, float sx1, float sy1, float& x, float& y)
{
    if (std::min(sx0, sx1) > std::max(r.x0, r.x1) || std::min(r.x0, r.x1) > std::max(sx0, sx1) ||
        std::min(sy0, sy1) > std::max(r.y0, r.y1) || std::min(r.y0, r.y1) > std::max(sy0, sy1)) {
        return false;
    }
    const float d1x = r.x1 - r.x0;
    const float d1y = r.y1 - r.y0;
    const float d2x = sx1 - sx0;
    const float d2y = sy1 - sy0;
    const float wx = sx0 - r.x0;
    const float wy = sy0 - r.y0;
    const float num = wy * d1x - wx * d1y;
    const float den = d2x * d1y - d2y * d1x;
    if (den == 0.0f) {
        return false;
    }
    x = num * d2x / den + sx0;
    y = num * d2y / den + sy0;
    return true;
}

// Cohen-Sutherland against [x0, x1] x [y0, y1]; false when the segment misses it.
bool clipSegment(int& ax, int& ay, int& bx, int& by, int x0, int y0, int x1, int y1)
{
    const auto code = [&](int x, int y) {
        int c = x < x0 ? 8 : (x > x1 ? 4 : 0);
        if (y < y0) {
            c |= 2;
        } else if (y > y1) {
            c |= 1;
        }
        return c;
    };
    bool swapped = false;
    bool inside = false;
    for (;;) {
        int ca = code(ax, ay);
        const int cb = code(bx, by);
        if (ca & cb) {
            break;
        }
        if (ca == 0) {
            if (cb == 0) {
                inside = true;
                break;
            }
            std::swap(ax, bx);
            std::swap(ay, by);
            swapped = !swapped;
            ca = cb;
        }
        if (ca & 1) {
            ax = ((bx - ax) * (y1 - ay)) / (by - ay) + ax;
            ay = y1;
        } else if (ca & 2) {
            ax = ((bx - ax) * (y0 - ay)) / (by - ay) + ax;
            ay = y0;
        } else if (ca & 4) {
            ay = ((by - ay) * (x1 - ax)) / (bx - ax) + ay;
            ax = x1;
        } else if (ca & 8) {
            ay = ((x0 - ax) * (by - ay)) / (bx - ax) + ay;
            ax = x0;
        }
    }
    if (swapped) {
        std::swap(ax, bx);
        std::swap(ay, by);
    }
    return inside;
}

} // namespace

int SightRegion::angle(int dy, int dx)
{
    if (dx == 0) {
        return dy < 1 ? 56250 : 18750;
    }
    int quadrant = 1;
    int n = dy;
    if (dx < 0) {
        dx = -dx;
        if (dy < 0) {
            n = -dy;
            quadrant = 3;
        } else {
            quadrant = 2;
        }
    } else if (dy < 0) {
        quadrant = 4;
        n = -dy;
    }
    int r = (n * 1000) / dx;
    if (r > 99999) {
        r = 99999;
    }
    const int a = atanTable()[static_cast<std::size_t>(r)];
    if (quadrant == 2) {
        return 37500 - a;
    }
    if (quadrant == 3) {
        return a + 37500;
    }
    if (quadrant == 4) {
        return kTurn - a;
    }
    return a;
}

int SightRegion::seenDirection(std::uint8_t edges)
{
    return kSeenDirection[edges & 0x0F];
}

SightRegion::View SightRegion::wallView(const View& view)
{
    View w = view;
    w.left -= kWallReachColumns * kCell;
    w.width += 2 * kWallReachColumns * kCell;
    w.top -= kWallReachRows * kCell;
    w.height += 2 * kWallReachRows * kCell;
    return w;
}

const SightRegion::Interval* SightRegion::occluderAt(int a) const
{
    // The last interval starting at or before the angle, if it reaches it.
    int lo = 0;
    int hi = static_cast<int>(list_.size()) - 1;
    int found = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (list_[static_cast<std::size_t>(mid)].a0 <= a) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found < 0 || list_[static_cast<std::size_t>(found)].a1 < a) {
        return nullptr;
    }
    return &list_[static_cast<std::size_t>(found)];
}

bool SightRegion::excused(float sx, int row, const ColumnsFor& self) const
{
    // The nearest occluder in the point's direction from the eye: the wall's own arm (its base
    // there lies behind its own front), or, for a see-over wall, a sight-blocking object.
    const int px = static_cast<int>(sx) + view_.left;
    const int py = row + view_.top;
    const Interval* o = occluderAt(normalized(angle(py - eyeY(), px - eyeX())));
    if (!o) {
        return false;
    }
    if (o->kind == kWall) {
        return o->cellX == self.cellX && o->cellY == self.cellY;
    }
    return self.seeOver && o->blocker >= 0;
}

std::array<float, 8> SightRegion::boxCorners(float width, float height)
{
    constexpr float kBoxHalf = 0.35355339f; // 1 / (2 sqrt 2)
    const float a = width * kBoxHalf;
    const float b = height * kBoxHalf;
    return {b - a, -a - b, -a - b, b - a, b + a, a - b, a - b, b + a};
}

SightRegion::WallState* SightRegion::wallAt(int x, int y)
{
    const int cx = x - cellX0_;
    const int cy = y - cellY0_;
    if (cx < 0 || cy < 0 || cx >= cellsW_ || cy >= cellsH_) {
        return nullptr;
    }
    return &walls_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellsW_) + static_cast<std::size_t>(cx)];
}

const SightRegion::WallState* SightRegion::wall(int x, int y) const
{
    return const_cast<SightRegion*>(this)->wallAt(x, y);
}

bool SightRegion::blockerSeen(std::size_t index) const
{
    return index < blockersSeen_.size() && blockersSeen_[index];
}

void SightRegion::pushInterval(Interval i)
{
    // Spanning more than half a turn: it crosses angle 0, split into its two sides of it.
    if (i.a1 - i.a0 > 37499) {
        Interval low = i;
        low.a0 = 0;
        low.a1 = i.a0;
        i.a0 = i.a1;
        i.a1 = kTurn - 1;
        input_.push_back(low);
    }
    input_.push_back(i);
}

void SightRegion::addWallEdge(int x, int y, std::uint8_t edge)
{
    // A wall arm (or full diagonal): its angles from the eye, keyed by the distance to its middle
    // (the centre 23x + 11, 23y + 11, the arms' middles 5 off it: 11.5 and 5.75 chopped).
    const int ex = eyeX();
    const int ey = eyeY();
    const int ox = x * kCell;
    const int oy = y * kCell;
    const int cx = ox + 11;
    const int cy = oy + 11;
    constexpr int kHalf = 5;
    int mx = 0;
    int my = 0;
    int sx = cx;
    int sy = cy;
    int tx = 0;
    int ty = 0;
    switch (edge) {
    case 1:
        mx = cx - ex + kHalf;
        my = cy - ey - kHalf;
        tx = ox + kCell;
        ty = oy;
        break;
    case 2:
        mx = cx - ex - kHalf;
        my = cy - ey - kHalf;
        tx = ox;
        ty = oy;
        break;
    case 4:
        mx = cx - ex + kHalf;
        my = cy - ey + kHalf;
        tx = ox + kCell;
        ty = oy + kCell;
        break;
    case 6:
        mx = cx - ex;
        my = cy - ey;
        sx = ox;
        sy = oy;
        tx = ox + kCell;
        ty = oy + kCell;
        break;
    case 8:
        mx = cx - ex - kHalf;
        my = cy - ey + kHalf;
        tx = ox;
        ty = oy + kCell;
        break;
    case 9:
        mx = cx - ex;
        my = cy - ey;
        sx = ox + kCell;
        sy = oy;
        tx = ox;
        ty = oy + kCell;
        break;
    default:
        return;
    }
    Interval i;
    i.kind = kWall;
    i.edges = edge;
    i.cellX = x;
    i.cellY = y;
    i.distance = my * my + mx * mx;
    i.a0 = normalized(angle(sy - ey, sx - ex));
    i.a1 = normalized(angle(ty - ey, tx - ex));
    if (i.a1 < i.a0) {
        std::swap(i.a0, i.a1);
    }
    pushInterval(i);
}

void SightRegion::build(const View& view, std::span<const Wall> walls, std::span<const Blocker> blockers)
{
    view_ = view;
    input_.clear();
    list_.clear();
    points_.clear();
    const int ex = eyeX();
    const int ey = eyeY();

    // The wall cells in view.
    cellX0_ = view.left / kCell;
    cellY0_ = view.top / kCell;
    cellsW_ = (view.left + view.width) / kCell - cellX0_ + 1;
    cellsH_ = (view.top + view.height) / kCell - cellY0_ + 1;
    walls_.assign(static_cast<std::size_t>(std::max(cellsW_, 0)) * static_cast<std::size_t>(std::max(cellsH_, 0)), {});
    std::vector<int> cellWall(walls_.size(), -1);
    for (std::size_t i = 0; i < walls.size(); ++i) {
        const int cx = walls[i].x - cellX0_;
        const int cy = walls[i].y - cellY0_;
        if (cx >= 0 && cy >= 0 && cx < cellsW_ && cy < cellsH_) {
            cellWall[static_cast<std::size_t>(cy) * static_cast<std::size_t>(cellsW_) + static_cast<std::size_t>(cx)] = static_cast<int>(i);
        }
    }
    blockersSeen_.assign(blockers.size(), false);

    // The screen's sides, behind everything: top, right, bottom, left.
    {
        const int x0 = view.left;
        const int y0 = view.top;
        const int x1 = view.left + view.width;
        const int y1 = view.top + view.height;
        const int tl = angle(y0 - ey, x0 - ex);
        const int tr = angle(y0 - ey, x1 - ex);
        const int br = angle(y1 - ey, x1 - ex);
        const int bl = angle(y1 - ey, x0 - ex);
        const auto side = [&](int a, int b, Kind kind) {
            Interval i;
            i.kind = kind;
            i.distance = kFar;
            i.a0 = normalized(std::min(a, b));
            i.a1 = normalized(std::max(a, b));
            pushInterval(i);
        };
        side(tl, tr, kTop);
        side(tr, br, kRight);
        side(br, bl, kBottom);
        side(bl, tl, kLeft);
    }

    // The sight-blocking objects near the view.
    for (std::size_t n = 0; n < blockers.size(); ++n) {
        const Blocker& b = blockers[n];
        Interval i;
        i.blocker = static_cast<int>(n);
        if (b.radius > 0) {
            // A circle: the angles it spans round the angle to its centre, keyed by the distance
            // to the centre.
            if (!(static_cast<float>(b.x0) - b.radius < static_cast<float>(view.left + view.width) &&
                  static_cast<float>(view.left) < static_cast<float>(b.x0) + b.radius &&
                  static_cast<float>(b.y0) - b.radius < static_cast<float>(view.top + view.height) &&
                  static_cast<float>(view.top) < static_cast<float>(b.y0) + b.radius)) {
                continue;
            }
            i.kind = kCircle;
            const auto fx = static_cast<float>(b.x0 - ex);
            const auto fy = static_cast<float>(b.y0 - ey);
            i.distance = static_cast<int>(fx * fx + fy * fy);
            const int dist = static_cast<int>(std::sqrt(static_cast<double>(i.distance)));
            const int centre = angle(static_cast<int>(fy), static_cast<int>(fx));
            const int half = angle(static_cast<int>(b.radius), dist);
            i.a0 = normalized(half + centre);
            i.a1 = normalized(centre - half);
        } else if (b.box) {
            // A box: its corners (top, left, right, bottom) on its position, in view when the
            // box they bound is.
            const auto px = static_cast<float>(b.x0);
            const auto py = static_cast<float>(b.y0);
            std::array<float, 8> c{};
            for (std::size_t k = 0; k < 8; k += 2) {
                c[k] = px + b.corners[k];
                c[k + 1] = py + b.corners[k + 1];
            }
            if (!(c[2] < static_cast<float>(view.left + view.width) && static_cast<float>(view.left) < c[4] &&
                  c[1] < static_cast<float>(view.top + view.height) && static_cast<float>(view.top) < c[7])) {
                continue;
            }
            int a[4];
            for (std::size_t k = 0; k < 4; ++k) {
                a[k] = angle(static_cast<int>(c[2 * k + 1]) - ey, static_cast<int>(c[2 * k]) - ex);
            }
            // Which of its faces the eye is past: 1 top-right, 2 bottom-left, 4 top-left, 8
            // bottom-right (8 and 4 never both).
            const auto fx = static_cast<float>(ex);
            const auto fy = static_cast<float>(ey);
            const float k = std::bit_cast<float>(0x3F350481u); // 1 / sqrt 2
            int side = 0;
            if ((((c[1] - c[0]) + fx) - fy) * k <= 0.0f) {
                if ((((c[3] - c[2]) + fx) - fy) * k < 0.0f) {
                    side = 2;
                }
            } else {
                side = 1;
            }
            if ((((c[5] + c[4]) - fx) - fy) * k < 0.0f) {
                side |= 8;
            } else if ((((c[1] + c[0]) - fx) - fy) * k > 0.0f) {
                side |= 4;
            }
            // One face: the face, keyed by the distance to its middle; two: the diagonal across,
            // keyed by the distance to the box's position. Inside it: nothing.
            const auto toMiddle = [&](std::size_t p, std::size_t q) {
                const float mx = (c[2 * p] + c[2 * q]) * 0.5f - fx;
                const float my = (c[2 * p + 1] + c[2 * q + 1]) * 0.5f - fy;
                return static_cast<int>(mx * mx + my * my);
            };
            const auto toPosition = [&] {
                const auto mx = static_cast<float>(b.x0 - ex);
                const auto my = static_cast<float>(b.y0 - ey);
                return static_cast<int>(mx * mx + my * my);
            };
            int from = 0;
            int to = 0;
            switch (side) {
            case 1:
                i.kind = kBoxTopRight;
                i.distance = toMiddle(2, 0);
                from = a[0];
                to = a[2];
                break;
            case 2:
                i.kind = kBoxBottomLeft;
                i.distance = toMiddle(3, 1);
                from = a[3];
                to = a[1];
                break;
            case 4:
                i.kind = kBoxTopLeft;
                i.distance = toMiddle(1, 0);
                from = a[0];
                to = a[1];
                break;
            case 8:
                i.kind = kBoxBottomRight;
                i.distance = toMiddle(3, 2);
                from = a[3];
                to = a[2];
                break;
            case 5:
            case 10:
                i.kind = kBoxAcross;
                i.distance = toPosition();
                from = a[1];
                to = a[2];
                break;
            case 6:
            case 9:
                i.kind = kBoxUpright;
                i.distance = toPosition();
                from = a[0];
                to = a[3];
                break;
            default:
                continue;
            }
            // (Ordered, then each brought into the turn, as the screen's sides are.)
            i.a0 = normalized(std::min(from, to));
            i.a1 = normalized(std::max(from, to));
            pushInterval(i);
            continue;
        } else {
            // A door: its leaf, keyed by the distance to the leaf's middle.
            constexpr float kReach = 32;
            if (!(static_cast<float>(b.x0) - kReach < static_cast<float>(view.left + view.width) &&
                  static_cast<float>(view.left) < static_cast<float>(b.x0) + kReach &&
                  static_cast<float>(b.y0) - kReach < static_cast<float>(view.top + view.height) &&
                  static_cast<float>(view.top) < static_cast<float>(b.y0) + kReach)) {
                continue;
            }
            i.kind = kDoor;
            const int lx = b.x1 - b.x0;
            const int ly = b.y1 - b.y0;
            const auto fx = static_cast<float>((ex - lx / 2) - b.x0);
            const auto fy = static_cast<float>((ey - ly / 2) - b.y0);
            i.distance = static_cast<int>(fx * fx + fy * fy);
            i.a0 = normalized(angle(b.y0 - ey, b.x0 - ex));
            i.a1 = normalized(angle(ly - ey + b.y0, lx - ex + b.x0));
        }
        if (i.a1 < i.a0) {
            std::swap(i.a0, i.a1);
        }
        pushInterval(i);
    }

    // The walls, row by row over the even cells of the grid.
    for (int y = cellY0_; y < cellY0_ + cellsH_; ++y) {
        for (int x = cellX0_ + ((cellX0_ + y) & 1); x < cellX0_ + cellsW_; x += 2) {
            if (x < 0 || y < 0 || x >= 256 || y >= 256) {
                continue;
            }
            const int index = cellWall[static_cast<std::size_t>(y - cellY0_) * static_cast<std::size_t>(cellsW_) +
                                       static_cast<std::size_t>(x - cellX0_)];
            if (index < 0) {
                continue;
            }
            const Wall& w = walls[static_cast<std::size_t>(index)];
            WallState& state = *wallAt(x, y);
            if (!w.blocksSight) {
                // Seen wherever it is in view; a front wall when its row is below the eye.
                state.seen = true;
                state.front = ey < y * kCell;
                state.seeOver = true;
                continue;
            }
            if (w.window) {
                // See-through from within about 60 units of its cell.
                const int dx = (ex - x * kCell) - 11;
                const int dy = (ey - y * kCell) - 11;
                if (dy * dy + dx * dx <= 0xE0F) {
                    state.seen = true;
                    continue;
                }
            }
            const int dir = w.direction & 0x7F;
            if (dir == 0) {
                addWallEdge(x, y, 9);
            } else if (dir == 1) {
                addWallEdge(x, y, 6);
            } else if (dir < static_cast<int>(kArms.size())) {
                const std::uint8_t arms = kArms[static_cast<std::size_t>(dir)];
                for (const std::uint8_t arm : {std::uint8_t{2}, std::uint8_t{1}, std::uint8_t{8}, std::uint8_t{4}}) {
                    if (arms & arm) {
                        addWallEdge(x, y, arm);
                    }
                }
            }
        }
    }

    sweep();
    emitPoints(blockers);
    markShadowEdges();
    mergePoints();
    scanConvert();
}

void SightRegion::sweep()
{
    // The intervals taken off the stack one at a time and merged into the sorted list.
    while (!input_.empty()) {
        Interval n = input_.back();
        input_.pop_back();
        // From the last one ending before it starts.
        int lo = 0;
        int hi = static_cast<int>(list_.size()) - 1;
        while (lo <= hi) {
            const int mid = (hi + lo) / 2;
            if (list_[static_cast<std::size_t>(mid)].a1 < n.a0) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        bool gone = false;
        for (int i = hi >= 0 ? hi : 0; i < static_cast<int>(list_.size()); ++i) {
            Interval& e = list_[static_cast<std::size_t>(i)];
            if (n.a1 < e.a0) {
                break;
            }
            // Where they overlap, the one whose middle is nearer keeps the angles.
            if (n.a0 <= e.a1 && e.a0 <= n.a1) {
                if (e.a0 < n.a0) {
                    if (e.a1 <= n.a1) {
                        if (n.distance < e.distance) {
                            e.a1 = n.a0 - 1;
                        } else {
                            n.a0 = e.a1 + 1;
                            if (n.a1 < n.a0) {
                                n.alive = false;
                            }
                        }
                    } else if (n.distance < e.distance) {
                        // In front of the middle of e: e split round it, its far side back on the stack.
                        Interval far = e;
                        far.a0 = n.a1 + 1;
                        if (far.a1 < far.a0) {
                            far.alive = false;
                        }
                        e.a1 = n.a0 - 1;
                        if (e.a1 < e.a0) {
                            e.alive = false;
                        }
                        input_.push_back(far);
                    } else {
                        n.alive = false;
                    }
                } else if (e.a1 <= n.a1) {
                    if (e.distance <= n.distance) {
                        // Behind e, round it: n split, its far side back on the stack.
                        Interval far = n;
                        far.a0 = e.a1 + 1;
                        if (far.a1 < far.a0) {
                            far.alive = false;
                        }
                        n.a1 = e.a0 - 1;
                        if (n.a1 < n.a0) {
                            n.alive = false;
                        }
                        input_.push_back(far);
                    } else {
                        e.alive = false;
                    }
                } else if (n.distance < e.distance) {
                    e.a0 = n.a1 + 1;
                    if (e.a1 < e.a0) {
                        e.alive = false;
                    }
                } else {
                    n.a1 = e.a0 - 1;
                    if (n.a1 < n.a0) {
                        n.alive = false;
                    }
                }
            }
            if (!n.alive || n.a1 - n.a0 < 0) {
                gone = true;
                break;
            }
            if (!e.alive || e.a1 - e.a0 < 0) {
                list_.erase(list_.begin() + i);
                --i;
            }
        }
        if (gone) {
            continue;
        }
        // In by its start angle.
        lo = 0;
        hi = static_cast<int>(list_.size()) - 1;
        while (lo <= hi) {
            const int mid = (hi + lo) / 2;
            if (list_[static_cast<std::size_t>(mid)].a0 < n.a0) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        list_.insert(list_.begin() + lo, n);
    }
}

void SightRegion::emitPoints(std::span<const Blocker> blockers)
{
    const int ex = eyeX();
    const int ey = eyeY();
    const auto fex = static_cast<float>(ex);
    const auto fey = static_cast<float>(ey);
    const int halfW = view_.width / 2;
    const int halfH = view_.height / 2;
    // The ray at a sweep angle: the eye and a point 4096 along it.
    const auto ray = [&](int a) {
        const int s = sinUnits(a);
        return Ray{fex, fey, static_cast<float>(sinOf(kQuarterSin - s) + ex), static_cast<float>(sinOf(s) + ey)};
    };
    float p0x = 0;
    float p0y = 0;
    float p1x = 0;
    float p1y = 0;
    for (const Interval& s : list_) {
        int id = 0;
        switch (s.kind) {
        case kWall: {
            hitWall(s.cellX, s.cellY, s.edges, ray(s.a0), p0x, p0y);
            hitWall(s.cellX, s.cellY, s.edges, ray(s.a1), p1x, p1y);
            if (WallState* w = wallAt(s.cellX, s.cellY)) {
                w->seen = true;
                if (p1x < p0x) {
                    w->front = true; // its seen part runs right to left: facing the eye from below
                }
                w->edges |= s.edges;
            }
            id = s.cellY * 0x100 + s.cellX;
            break;
        }
        case kTop:
            p0x = static_cast<float>(scaled(halfH, tanOf(sinUnits(s.a0) - 0x4B66)) + ex);
            p0y = static_cast<float>(view_.top);
            p1x = static_cast<float>(scaled(halfH, tanOf(sinUnits(s.a1) - 0x4B66)) + ex);
            p1y = static_cast<float>(view_.top);
            break;
        case kBottom:
            p0x = static_cast<float>(ex - scaled(halfH, tanOf(sinUnits(s.a0) - kQuarterSin)));
            p0y = static_cast<float>(view_.top - 1 + view_.height);
            p1x = static_cast<float>(ex - scaled(halfH, tanOf(sinUnits(s.a1) - kQuarterSin)));
            p1y = p0y;
            break;
        case kLeft:
            p0x = static_cast<float>(view_.left);
            p0y = static_cast<float>(ey - scaled(halfW, tanOf(sinUnits(s.a0) - 0x3244)));
            p1x = p0x;
            p1y = static_cast<float>(ey - scaled(halfW, tanOf(sinUnits(s.a1) - 0x3244)));
            break;
        case kRight:
            p0x = static_cast<float>(view_.left - 1 + view_.width);
            p0y = static_cast<float>(scaled(halfW, tanOf(sinUnits(s.a0))) + ey);
            p1x = p0x;
            p1y = static_cast<float>(scaled(halfW, tanOf(sinUnits(s.a1))) + ey);
            break;
        case kDoor:
        case kCircle:
        case kBoxTopLeft:
        case kBoxTopRight:
        case kBoxBottomLeft:
        case kBoxBottomRight:
        case kBoxUpright:
        case kBoxAcross: {
            const Blocker& b = blockers[static_cast<std::size_t>(s.blocker)];
            float sx0 = 0;
            float sy0 = 0;
            float sx1 = 0;
            float sy1 = 0;
            if (s.kind >= kBoxTopLeft) {
                // A box's face or diagonal, corner to corner (top 0, left 1, right 2, bottom 3).
                std::size_t p = 0;
                std::size_t q = 0;
                switch (s.kind) {
                case kBoxTopLeft:
                    p = 0;
                    q = 1;
                    break;
                case kBoxTopRight:
                    p = 0;
                    q = 2;
                    break;
                case kBoxBottomLeft:
                    p = 3;
                    q = 1;
                    break;
                case kBoxBottomRight:
                    p = 3;
                    q = 2;
                    break;
                case kBoxUpright:
                    p = 0;
                    q = 3;
                    break;
                default:
                    p = 1;
                    q = 2;
                    break;
                }
                sx0 = static_cast<float>(b.x0) + b.corners[2 * p];
                sy0 = static_cast<float>(b.y0) + b.corners[2 * p + 1];
                sx1 = static_cast<float>(b.x0) + b.corners[2 * q];
                sy1 = static_cast<float>(b.y0) + b.corners[2 * q + 1];
            } else if (s.kind == kDoor) {
                sx0 = static_cast<float>(b.x0);
                sy0 = static_cast<float>(b.y0);
                sx1 = static_cast<float>(b.x1);
                sy1 = static_cast<float>(b.y1);
            } else {
                // The circle as the line through its centre square to the eye.
                const int dx = ex - b.x0;
                const int dy = ey - b.y0;
                sx0 = static_cast<float>(b.x0 - dy);
                sy0 = static_cast<float>(b.y0 + dx);
                sx1 = static_cast<float>(b.x0 + dy);
                sy1 = static_cast<float>(b.y0 - dx);
            }
            if (!hitSegment(ray(s.a0), sx0, sy0, sx1, sy1, p0x, p0y)) {
                p0x = sx0;
                p0y = sy0;
            }
            if (!hitSegment(ray(s.a1), sx0, sy0, sx1, sy1, p1x, p1y)) {
                p1x = sx1;
                p1y = sy1;
            }
            blockersSeen_[static_cast<std::size_t>(s.blocker)] = true;
            id = s.blocker + 1;
            break;
        }
        default:
            break;
        }
        int ax = static_cast<int>(p0x) - view_.left;
        int ay = static_cast<int>(p0y) - view_.top;
        int bx = static_cast<int>(p1x) - view_.left;
        int by = static_cast<int>(p1y) - view_.top;
        if (clipSegment(ax, ay, bx, by, 0, 0, view_.width - 1, view_.height - 1)) {
            points_.push_back({ax, ay, s.kind, id});
            points_.push_back({bx, by, s.kind, id});
        }
    }
}

void SightRegion::markShadowEdges()
{
    // An edge from one occluder to another (more than 4 apart, or onto a door) is a shadow edge.
    // The last point is dropped (it closes on the first).
    const int n = static_cast<int>(points_.size());
    if (n < 2) {
        points_.clear();
        return;
    }
    const auto far = [&](int a, int b) {
        return std::abs(points_[static_cast<std::size_t>(b)].x - points_[static_cast<std::size_t>(a)].x) > 4 ||
               std::abs(points_[static_cast<std::size_t>(b)].y - points_[static_cast<std::size_t>(a)].y) > 4;
    };
    int prev = n - 2;
    for (int cur = 0; cur < n - 1; prev = cur, ++cur) {
        ScreenPoint& p = points_[static_cast<std::size_t>(prev)];
        const ScreenPoint& c = points_[static_cast<std::size_t>(cur)];
        bool mark = false;
        if (p.kind == kWall && c.kind == kWall) {
            mark = p.id != c.id && far(prev, cur);
        } else if (p.kind == kWall || c.kind == kWall) {
            mark = !((p.kind == kDoor || c.kind == kDoor) && !far(prev, cur));
        } else if (p.kind == c.kind) {
            mark = c.id != p.id && far(prev, cur);
        } else {
            mark = true;
        }
        if (mark) {
            p.kind = kShadowEdge;
        }
    }
    points_.pop_back();
}

void SightRegion::mergePoints()
{
    // A point in line with its neighbours on the same kind of edge dropped.
    const int n = static_cast<int>(points_.size());
    if (n <= 2) {
        return;
    }
    int kept = 1;
    int px = points_[0].x;
    int py = points_[0].y;
    int kx = points_[1].x;
    int ky = points_[1].y;
    for (int j = 2; j < n; ++j) {
        const ScreenPoint c = points_[static_cast<std::size_t>(j)];
        if ((c.y - ky) * (px - kx) - (c.x - kx) * (py - ky) != 0 || c.kind != points_[static_cast<std::size_t>(kept)].kind) {
            ++kept;
            px = kx;
            py = ky;
        }
        points_[static_cast<std::size_t>(kept)] = c;
        kx = c.x;
        ky = c.y;
    }
    points_.resize(static_cast<std::size_t>(kept + 1));
}

void SightRegion::scanConvert()
{
    // Each edge adds its x at the top of every row it crosses (the bottom row left to the next),
    // at most 32 a row.
    rows_.assign(static_cast<std::size_t>(std::max(view_.height, 0)), {});
    const auto add = [&](int x, int row) {
        if (row < 0 || row >= static_cast<int>(rows_.size())) {
            return;
        }
        auto& r = rows_[static_cast<std::size_t>(row)];
        if (r.size() >= 32) {
            return;
        }
        r.insert(std::upper_bound(r.begin(), r.end(), x), x);
    };
    const int n = static_cast<int>(points_.size());
    for (int i = 0; i < n; ++i) {
        const ScreenPoint& a = points_[static_cast<std::size_t>(i)];
        const ScreenPoint& b = points_[static_cast<std::size_t>(i == n - 1 ? 0 : i + 1)];
        const int x0 = a.x;
        const int y0 = a.y;
        const int x1 = b.x;
        const int y1 = b.y;
        if (y0 == y1) {
            continue;
        }
        if (x0 == x1) {
            for (int y = std::min(y0, y1); y < std::max(y0, y1); ++y) {
                add(x0, y);
            }
            continue;
        }
        int step = 0;
        int row = 0;
        int end = 0;
        int x = 0;
        if (y1 < y0) {
            step = x1 <= x0 ? 1 : -1;
            row = y1;
            x = x1;
            end = y0;
        } else {
            step = x1 <= x0 ? -1 : 1;
            row = y0;
            x = x0;
            end = y1;
        }
        const int dx = std::abs(x1 - x0);
        const int dy = end - row;
        int err = 0;
        while (row < end) {
            add(x, row);
            ++row;
            for (err += dx; dy <= err; err -= dy) {
                x += step;
            }
        }
    }
}

bool SightRegion::spanContains(int x, int y) const
{
    const int row = y - view_.top;
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return false;
    }
    const int sx = x - view_.left;
    const auto& r = rows_[static_cast<std::size_t>(row)];
    for (std::size_t i = 0; i + 1 < r.size(); i += 2) {
        if (r[i] <= sx && sx <= r[i + 1]) {
            return true;
        }
    }
    return false;
}

bool SightRegion::segmentsMeet(const std::array<int, 4>& a, const std::array<int, 4>& b)
{
    // Their bounding boxes first (x, then y), then where each line crosses the other, as
    // fractions num / den of the segments kept in whole numbers: both within 0..1 (ends included).
    // (64-bit: the game's 32-bit products never overflow over a map's units.)
    const std::int64_t ax = a[0];
    const std::int64_t ay = a[1];
    const std::int64_t bx = a[2];
    const std::int64_t by = a[3];
    const std::int64_t cx = b[0];
    const std::int64_t cy = b[1];
    const std::int64_t dx = b[2];
    const std::int64_t dy = b[3];
    const std::int64_t adx = bx - ax;
    const std::int64_t cdx = cx - dx;
    if (std::min(cx, dx) > std::max(ax, bx) || std::min(ax, bx) > std::max(cx, dx)) {
        return false;
    }
    const std::int64_t cdy = cy - dy;
    const std::int64_t ady = by - ay;
    const std::int64_t aMaxY = std::max(ay, by);
    const std::int64_t aMinY = std::min(ay, by);
    if (cdy < 1) {
        if (aMaxY < cy || dy < aMinY) {
            return false;
        }
    } else if (aMaxY < dy || cy < aMinY) {
        return false;
    }
    const std::int64_t num = (ax - cx) * cdy - (ay - cy) * cdx;
    const std::int64_t den = ady * cdx - cdy * adx;
    if (den < 1) {
        if (num > 0 || num < den) {
            return false;
        }
    } else if (num < 0 || den < num) {
        return false;
    }
    const std::int64_t other = (ay - cy) * adx - (ax - cx) * ady;
    if (den < 1) {
        return other < 1 && den <= other;
    }
    return other >= 0 && other <= den;
}

bool SightRegion::lineClear(int ax, int ay, int bx, int by) const
{
    // Against every edge, the last point's to the first included.
    if (points_.empty()) {
        return true;
    }
    const std::array<int, 4> line{ax, ay, bx, by};
    const ScreenPoint* prev = &points_.back();
    for (const ScreenPoint& p : points_) {
        if (segmentsMeet(line, {prev->x, prev->y, p.x, p.y})) {
            return false;
        }
        prev = &p;
    }
    return true;
}

bool SightRegion::insidePolygon(int sx, int sy) const
{
    // Each edge from the previous point (the last's first) to this one.
    bool inside = false;
    if (points_.empty()) {
        return inside;
    }
    const ScreenPoint* prev = &points_.back();
    for (const ScreenPoint& cur : points_) {
        const bool straddles = cur.y > sy ? prev->y <= sy : prev->y > sy;
        if (straddles) {
            const std::int64_t cross =
                static_cast<std::int64_t>(prev->x - cur.x) * (sy - cur.y) / (prev->y - cur.y) + cur.x;
            if (sx < cross) {
                inside = !inside;
            }
        }
        prev = &cur;
    }
    return inside;
}

SightRegion::ScreenBox SightRegion::screenBox(const Target& t, int sx, int sy)
{
    // Each float made whole by itself, chopped (the FPU's mode); the image's width halved
    // unsigned. Its z is added (not taken off) at the bottom, as the game has it.
    const auto whole = [](float f) { return static_cast<int>(f); };
    const int w = t.image.width;
    const int h = t.image.height;
    ScreenBox b;
    if (t.circle) {
        const int r = whole(t.radius);
        if (t.image.drawn) {
            b.x0 = sx - (w >> 1);
            b.y0 = sy + t.z - h + r - whole(t.zSizeMin);
            b.x1 = b.x0 + w;
            b.y1 = b.y0 + h;
        } else {
            b.x0 = sx - r;
            b.y0 = sy - r - whole(t.zSizeMax);
            b.x1 = sx + r;
            b.y1 = sy + t.z + r - whole(t.zSizeMin);
        }
    } else if (t.image.drawn) {
        // Up from the bottom corner.
        b.x0 = sx - (w >> 1);
        b.y1 = sy + t.z + whole(t.corners[7]);
        b.y0 = b.y1 - h;
        b.x1 = b.x0 + w;
    } else {
        // The left and right corners' x, the top corner's y less ZSIZE's top, the bottom corner's y.
        b.x0 = sx + whole(t.corners[2]);
        b.y0 = sy - whole(t.zSizeMax) + whole(t.corners[1]);
        b.x1 = sx + whole(t.corners[4]);
        b.y1 = sy + t.z + whole(t.corners[7]);
    }
    return b;
}

bool SightRegion::drawableSeen(const Target& t) const
{
    // In the screen's units (view-relative), the eye as the polygon's.
    const int ex = view_.width / 2;
    const int ey = view_.height / 2 + view_.eyeShiftY;
    const int sx = t.x - view_.left;
    const int sy = t.y - view_.top;
    if (lineClear(ex, ey, sx, sy)) {
        return true;
    }
    // Its line crosses an edge: by its box against the screen, each of its four sides on or off.
    const ScreenBox b = screenBox(t, sx, sy);
    const int right = view_.width - 1;
    const int bottom = view_.height - 1;
    const bool leftOff = b.x0 < 0 || b.x0 > right;
    const bool topOff = b.y0 < 0 || b.y0 > bottom;
    const bool rightOff = b.x1 < 0 || b.x1 > right;
    const bool bottomOff = b.y1 < 0 || b.y1 > bottom;
    const bool anyOn = !leftOff || !topOff || !rightOff || !bottomOff;
    const bool anyOff = leftOff || topOff || rightOff || bottomOff;
    if (!anyOn || !anyOff) {
        return false; // wholly off the screen, or wholly on it (its middle hidden)
    }
    // Partly on: two points inside it on its side towards the screen; a box over the top or bottom
    // goes by that over the left or right.
    int ix0 = b.x0;
    int ix1 = b.x1;
    int iy0 = b.y0;
    int iy1 = b.y1;
    if (t.image.drawn) {
        if (b.x1 - b.x0 > 6) {
            ix0 += 3;
            ix1 -= 3;
        }
        if (b.y1 - b.y0 > 21) {
            iy0 += 11;
            iy1 -= 11;
        }
    }
    // (A box turned inside out, its sides off but none past the screen's sides the way it faces,
    // leaves both points at its position: the game's second point is left unset then.)
    int p0x = sx;
    int p0y = sy;
    int p1x = sx;
    int p1y = sy;
    if (b.x0 < 0) {
        p0x = p1x = ix1;
        p0y = iy0;
        p1y = iy1;
    } else if (b.x1 > right) {
        p0x = p1x = ix0;
        p0y = iy0;
        p1y = iy1;
    }
    if (b.y0 < 0) {
        p0x = ix0;
        p1x = ix1;
        p0y = p1y = iy1;
    } else if (b.y1 > bottom) {
        p0x = ix0;
        p1x = ix1;
        p0y = p1y = iy0;
    }
    return lineClear(ex, ey, p0x, p0y) && lineClear(ex, ey, p1x, p1y);
}

std::vector<SightRegion::Point> SightRegion::polygon() const
{
    std::vector<Point> out;
    out.reserve(points_.size());
    for (const auto& p : points_) {
        out.push_back({p.x + view_.left, p.y + view_.top, p.kind, p.id});
    }
    return out;
}

std::vector<SightRegion::SoftEdge> SightRegion::softEdges() const
{
    // Each shadow edge, its lines stepping across it on the side the polygon lies (the edge is
    // walked clockwise): along y for a flat edge, along x for a steep one.
    std::vector<SoftEdge> out;
    const int n = static_cast<int>(points_.size());
    for (int cur = 0, prev = n - 1; cur < n; prev = cur, ++cur) {
        const ScreenPoint& a = points_[static_cast<std::size_t>(prev)];
        if (a.kind != kShadowEdge) {
            continue;
        }
        const ScreenPoint& b = points_[static_cast<std::size_t>(cur)];
        const int dx = a.x - b.x;
        const int dy = a.y - b.y;
        SoftEdge e{a.x + view_.left, a.y + view_.top, b.x + view_.left, b.y + view_.top, 0, 0};
        if (std::abs(dy) < std::abs(dx)) {
            e.stepY = dx < 0 ? 1 : -1;
        } else {
            e.stepX = dy > 0 ? 1 : -1;
        }
        out.push_back(e);
    }
    return out;
}

bool SightRegion::columns(int ax, int ay, int bx, int by, int& left, int& right, const ColumnsFor& self) const
{
    // The screen columns of the line (ax, ay) - (bx, by) that run inside the spans: `left` and
    // `right` narrowed to them; false when none does. Walks it a row at a time from its left end
    // (a crossing counts 2 further right).
    const int width = view_.width;
    const int height = view_.height;
    const auto rowAt = [&](int r) -> const std::vector<int>* {
        return r >= 0 && r < static_cast<int>(rows_.size()) ? &rows_[static_cast<std::size_t>(r)] : nullptr;
    };
    const auto inSpan = [&](int r, float x) {
        const auto* row = rowAt(r);
        if (!row) {
            return false;
        }
        for (std::size_t i = 0; i + 1 < row->size(); i += 2) {
            if (static_cast<float>((*row)[i]) <= x && x <= static_cast<float>((*row)[i + 1] + 2)) {
                return true;
            }
        }
        return false;
    };
    // Improved culling: a point out of the spans still counts where only the wall's own arms (or,
    // a see-over wall, only a sight-blocking object) hide it.
    const bool excuses = improved_ && self.cellX >= 0;
    const auto seen = [&](int r, float x) {
        return inSpan(r, x) || (excuses && r >= 0 && r < height && excused(x, r, self));
    };
    if (ay == by && excuses) {
        // Its columns walked one by one: from the first seen to the last of that run.
        if (ay < 0 || ay >= height) {
            return false;
        }
        const int l = std::max(std::min(ax, bx), 0);
        const int r = std::min(std::max(ax, bx), width - 1);
        int first = -1;
        int last = -1;
        for (int x = l; x <= r; ++x) {
            if (seen(ay, static_cast<float>(x))) {
                if (first < 0) {
                    first = x;
                }
                last = x;
            } else if (first >= 0) {
                break;
            }
        }
        if (first < 0) {
            return false;
        }
        if (first > l) {
            left = std::max(left, first);
        }
        if (last < r) {
            right = std::min(right, last + 1);
        }
        return true;
    }
    if (ay == by) {
        const auto* row0 = rowAt(ay);
        if (!row0 || row0->empty()) {
            return false;
        }
        auto l = static_cast<float>(std::min(ax, bx));
        const auto r = static_cast<float>(std::max(ax, bx));
        int rr = ay;
        if (rr >= height) {
            rr = height - 1;
        }
        if (l < 0.0f) {
            l = 0.0f;
        }
        if (static_cast<float>(width) < l) {
            l = static_cast<float>(width);
        }
        const auto& row = rows_[static_cast<std::size_t>(rr)];
        for (std::size_t i = 0; i + 1 < row.size(); i += 2) {
            const int s0 = row[i];
            const int s1 = row[i + 1];
            if (l < static_cast<float>(s0)) {
                left = std::max(left, s0);
                if (r <= static_cast<float>(s1) || right <= s1) {
                    return true;
                }
                right = s1;
                return true;
            }
            if (l <= static_cast<float>(s1) && static_cast<float>(s1) < r) {
                right = std::min(right, s1);
                return true;
            }
            if (r <= static_cast<float>(s1)) {
                return true;
            }
        }
        right = 0;
        return true;
    }
    int lx = ax;
    int ly = ay;
    int rx = bx;
    int ry = by;
    if (ax > bx) {
        lx = bx;
        ly = by;
        rx = ax;
        ry = ay;
    }
    auto rightX = static_cast<float>(rx);
    auto x = static_cast<float>(lx);
    int rows = ry - ly;
    int dir = 1;
    if (rows < 0) {
        rows = -rows;
        dir = -1;
    }
    const float step = (rightX - x) / static_cast<float>(rows);
    int r = ly;
    if (r < 0) {
        x = x - static_cast<float>(r) * step;
        r = 0;
    } else if (height <= r) {
        if (height <= ry) {
            return false;
        }
        const int over = r - height;
        r = height - 1;
        x = static_cast<float>(over + 1) * step + x;
        left = std::max(left, static_cast<int>(x));
    }
    if (ry < 0) {
        rows += ry;
    } else if (height <= ry) {
        rows = (height - ry) - 1 + rows;
        rightX = static_cast<float>(rows) * step + x;
        right = std::min(right, static_cast<int>(rightX));
    }
    if (x < 0.0f) {
        x = 0.0f;
    }
    if (static_cast<float>(width) < rightX) {
        rows -= static_cast<int>(rightX - static_cast<float>(width));
    }
    const bool startSeen = seen(r, x);
    x += step;
    r += dir;
    int remaining = rows - 1;
    if (!startSeen) {
        bool found = false;
        while (remaining > 0) {
            if (seen(r, x)) {
                left = std::max(left, static_cast<int>(x) - 1);
                found = true;
                break;
            }
            x += step;
            r += dir;
            --remaining;
        }
        if (!found) {
            return remaining != 0;
        }
    }
    if (remaining != 0 && remaining >= 0) {
        do {
            if (!seen(r, x)) {
                right = std::min(right, static_cast<int>(x));
                return true;
            }
            x += step;
            --remaining;
            r += dir;
        } while (remaining > 0);
    }
    return true;
}

std::optional<SightRegion::WallDraw> SightRegion::wallDraw(int x, int y, std::uint8_t direction, bool window) const
{
    const WallState* s = wall(x, y);
    if (!s || !s->seen) {
        return std::nullopt;
    }
    // Drawn as the direction its seen arms make (two or more), else its own.
    int dir = seenDirection(s->edges);
    if (dir < 0) {
        dir = direction & 0x7F;
    }
    if (dir >= static_cast<int>(kBase.size())) {
        return std::nullopt;
    }
    WallDraw d;
    d.direction = dir;
    d.image = dir;
    if (window && dir == 0) {
        d.image = 11;
    } else if (window && dir == 1) {
        d.image = 12;
    }
    d.front = s->front;
    // Only the columns where the drawn direction's base line is in sight.
    const ColumnsFor self{x, y, s->seeOver};
    const int sx = x * kCell - view_.left;
    const int sy = y * kCell - view_.top;
    int left = 0;
    int right = view_.width;
    const auto& b = kBase[static_cast<std::size_t>(dir)];
    if (dir == 7 || dir == 9) {
        // Two halves: either one seen draws the wall, across both.
        const bool first = columns(sx + b[0], sy + b[1], sx + b[2], sy + b[3], left, right, self);
        if (!first) {
            left = sx + b[2];
        }
        const auto& c = kBase[static_cast<std::size_t>(dir + 4)];
        if (!columns(sx + c[0], sy + c[1], sx + c[2], sy + c[3], left, right, self)) {
            if (!first) {
                return std::nullopt;
            }
            right = std::min(right, sx + c[0]);
        }
    } else if (dir == 8 || dir == 10) {
        // Two halves, each seen when more than 2 columns of it are: only the second, its half
        // image (13 / 14); only the first, the straight wall it lies along (0 / 1).
        int l1 = sx + b[0];
        int r1 = sx + b[2];
        const bool firstSeen = columns(sx + b[0], sy + b[1], sx + b[2], sy + b[3], l1, r1, self) && r1 - l1 > 2;
        const auto& c = kBase[static_cast<std::size_t>(dir + 4)];
        const int c0 = sx + c[0];
        const int c1 = sx + c[2];
        left = c0;
        right = c1;
        const bool second = columns(c0, sy + c[1], c1, sy + c[3], left, right, self);
        const bool secondSeen = second && right - left > 2;
        if (!firstSeen) {
            if (!secondSeen) {
                return std::nullopt;
            }
            d.image = dir == 8 ? 13 : 14;
            if (right == c1) {
                right = view_.width;
            }
            if (left == c0) {
                left = 0;
            }
        } else if (secondSeen) {
            left = std::min(left, l1);
            if (left <= c0) {
                left = 0;
            }
            right = std::max(right, r1);
            if (c1 <= right) {
                right = view_.width;
            }
        } else {
            left = l1;
            right = r1;
            if (dir == 8) {
                d.image = 0;
                if (r1 == c1) {
                    right = view_.width;
                }
            } else {
                d.image = 1;
                if (left == c0) {
                    left = 0;
                }
            }
        }
    } else if (!columns(sx + b[0], sy + b[1], sx + b[2], sy + b[3], left, right, self)) {
        return std::nullopt;
    }
    if (right <= left) {
        return std::nullopt;
    }
    if (left > 0) {
        d.clipLeft = left + view_.left;
    }
    if (right < view_.width) {
        d.clipRight = right + view_.left;
    }
    return d;
}

} // namespace nox::client
