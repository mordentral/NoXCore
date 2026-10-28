// A light's cone and its animation (light_cone.hpp gives what the game does).
#include "light_cone.hpp"

#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::lightcone {

namespace {

constexpr int kStep = 23; // world units between grid points
// colorLightAimAtTarget's radians to degrees: the float 57.2959 (0x42652EE0, 57.2958984375).
constexpr float kRadiansToDegrees = 57.2959f;

// The exe's float-to-int (__ftol, truncating).
std::int32_t ftol(double v)
{
    return nox::util::ftol(nox::util::X87::fromDouble(v));
}

// (The client's maths is cosmetic, so it rounds to the nearest. The exe's client figured these in
// its chop mode, which differs only on exact boundaries: a frame landing on a key, a halfway width,
// a degree of aim. The server's figuring is the one kept exact, util/x87.hpp.)

// initSinCosTables: sin(i * (2 pi / 4096 as a float)) * 4096, truncated, i = 0..4095.
const std::array<std::int32_t, 4096>& sineTable()
{
    static const std::array<std::int32_t, 4096> table = [] {
        std::array<std::int32_t, 4096> t{};
        const double step = static_cast<double>(0.0015339808f);
        for (int i = 0; i < 4096; ++i) {
            t[static_cast<std::size_t>(i)] = ftol(std::sin(i * step) * 4096.0);
        }
        return t;
    }();
    return table;
}

// The 16.16 multiply the light code uses (bits 16-47 of the product).
std::int32_t fixMul(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>((static_cast<std::int64_t>(a) * b) >> 16);
}

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// One edge's x per row (the exe's edge table: first row, row count, then an x per row).
struct Edge {
    int start = 0;
    int count = 0;
    std::vector<int> x;
};

// The edge from a to b (a above b), rows clipped to [0, rowEnd) and x to [0, colMax]. As the
// exe's: the row count includes both ends, the x step is the whole width over that count, and a
// start clipped at the top doesn't move x on for the rows cut off.
void edge(Point a, Point b, int rowEnd, int colMax, Edge& e)
{
    e.x.assign(static_cast<std::size_t>(std::max(rowEnd, 0)), 0);
    e.start = (a.y + 0x8000) >> 16;
    const int count = ((b.y + 0x8000) >> 16) - e.start + 1;
    e.count = count;
    if (count <= 0) {
        return;
    }
    int end = count + e.start;
    if (e.start < 0) {
        e.count = end;
        e.start = 0;
    }
    if (end > rowEnd) {
        e.count += rowEnd - end;
        end = rowEnd;
    }
    std::int32_t x = a.x + 0x8000;
    const std::int32_t dx = (b.x - a.x) / count;
    for (int r = e.start; r < end; ++r) {
        e.x[static_cast<std::size_t>(r)] = std::clamp(x >> 16, 0, colMax);
        x += dx;
    }
}

// The rows both edges cover (at least two of them, as the exe asks), each from the left edge's x
// up to (not including) the right one's.
void fill(const Edge& left, const Edge& right, std::vector<std::pair<int, int>>& out)
{
    const int s = std::max(left.start, right.start);
    const int e = std::min(left.start + left.count, right.start + right.count);
    if (e - s <= 1) {
        return;
    }
    for (int r = s; r < e; ++r) {
        for (int c = left.x[static_cast<std::size_t>(r)]; c < right.x[static_cast<std::size_t>(r)]; ++c) {
            out.emplace_back(c, r);
        }
    }
}

// The exe's triangle: corners turned (keeping their order) so the first is the top one, then the
// two edges from it filled, then the rest down to the lower of the other two.
void triangle(Point p1, Point p2, Point p3, int rowEnd, int colMax, std::vector<std::pair<int, int>>& out)
{
    Point top = p1, b = p2, c = p3;
    if (p2.y < p1.y) {
        if (p2.y <= p3.y) {
            top = p2, b = p3, c = p1;
        } else {
            top = p3, b = p1, c = p2;
        }
    } else if (p1.y > p3.y) {
        top = p3, b = p1, c = p2;
    }
    Edge eb, ec, rest;
    edge(top, b, rowEnd, colMax, eb);
    edge(top, c, rowEnd, colMax, ec);
    fill(eb, ec, out);
    if (c.y <= b.y) {
        if (c.y < b.y) {
            edge(c, b, rowEnd, colMax, rest);
            fill(eb, rest, out);
        }
        return;
    }
    edge(b, c, rowEnd, colMax, rest);
    fill(rest, ec, out);
}

float cross(const std::array<float, 2>& o, const std::array<float, 2>& a, float bx, float by)
{
    return (a[0] - o[0]) * (by - o[1]) - (a[1] - o[1]) * (bx - o[0]);
}

// Inside triangle (a, b, c) if it's wound the way the game fills (clockwise from a to b to c is
// negative here, y down).
bool insideFilled(const std::array<float, 2>& a, const std::array<float, 2>& b, const std::array<float, 2>& c, float px, float py)
{
    if (cross(a, b, c[0], c[1]) >= 0) {
        return false;
    }
    return cross(a, b, px, py) <= 0 && cross(b, c, px, py) <= 0 && cross(c, a, px, py) <= 0;
}

} // namespace

std::optional<KeySegment> keySegment(std::size_t n, std::uint16_t step, bool loop, std::uint32_t frame)
{
    if (n < 2 || step == 0) {
        return std::nullopt;
    }
    // x counts whole cycles; f is how many keys into the current one.
    const double x = static_cast<double>(frame) / static_cast<double>(static_cast<std::int32_t>(n * step));
    const std::int32_t cycle = ftol(x);
    const double f = (x - static_cast<double>(cycle)) * static_cast<double>(n);
    const auto i = static_cast<std::size_t>(ftol(f));
    KeySegment s;
    if (loop) {
        s.from = i;
        s.to = i + 1 >= n ? 0 : i + 1;
    } else if (cycle & 1) { // a non-looping one runs back down on odd cycles
        s.from = n - i - 1;
        s.to = s.from == 0 ? 0 : s.from - 1;
    } else {
        s.from = i;
        s.to = i + 1 >= n ? n - 1 : i + 1;
    }
    // Whole frames into the step, as the game truncates, kept to a byte (its AND 0xFF).
    s.frames = ftol((f - static_cast<double>(ftol(f))) * static_cast<double>(step)) & 0xFF;
    s.along = static_cast<float>(s.frames) / step;
    return s;
}

double keyValue(int from, int to, std::uint16_t step, int frames)
{
    // FILD (to - from); FDIV the step (a float); FMUL the frames; FIADD from.
    return static_cast<double>(to - from) / static_cast<double>(step) * static_cast<double>(frames) + static_cast<double>(from);
}

int keyWhole(int from, int to, std::uint16_t step, int frames)
{
    return ftol(keyValue(from, to, step, frames));
}

float keyFloat(int from, int to, std::uint16_t step, int frames)
{
    return static_cast<float>(keyValue(from, to, step, frames));
}

int radiusOf(float intensity)
{
    // lightRadiusFromIntensity: nothing at 1 or under; sqrt(((4 + min(I, 31)) / (4 + 1) + 1) *
    // (I * I / (961 * 0.001f))), made whole by Math_DoubleToInt (FISTP, truncating).
    if (!(intensity > 1.0f)) {
        return 0;
    }
    const double capped = intensity > 31.0f ? 31.0 : static_cast<double>(intensity);
    const double i = intensity;
    const double q = i * i / (961.0 * static_cast<double>(0.001f));
    const double u = (4.0 + capped) / 5.0 + 1.0;
    return ftol(std::sqrt(u * q));
}

int gridOriginIndex(float viewLeft)
{
    // The light grid's first point (the client's grid build): ((view + 11) / 46 - 1) * 46 - 11, an
    // even lattice point one or two before the view's edge (the lattice at 23 i - 11, i its index).
    // (Floored here, as a view left of 0 isn't the game's.)
    const int v = static_cast<int>(std::floor(viewLeft)) + 11;
    const int k = v >= 0 ? v / 46 : -((-v + 45) / 46);
    return 2 * (k - 1);
}

std::uint16_t angleFromDegrees(int degrees)
{
    // FILD; FMUL (1/360 as a float); FMUL 65536.0; FADD 0.5; __ftol.
    return static_cast<std::uint16_t>(ftol(degrees * static_cast<double>(1.0f / 360.0f) * 65536.0 + 0.5));
}

int degreesFromAngle(std::uint16_t angle)
{
    return ftol(angle * (1.0 / 65536.0) * 360.0);
}

std::int32_t sinFixed(std::uint16_t angle)
{
    return sineTable()[angle >> 4] << 4;
}

std::int32_t cosFixed(std::uint16_t angle)
{
    return sinFixed(static_cast<std::uint16_t>(angle + 0x4000));
}

Kite kite(float x, float y, float radius, Cone cone)
{
    const auto at = [&](std::uint16_t a) -> std::array<float, 2> {
        return {x + radius * static_cast<float>(cosFixed(a)) / 65536.0f, y + radius * static_cast<float>(sinFixed(a)) / 65536.0f};
    };
    return {std::array<float, 2>{x, y}, at(cone.direction), at(static_cast<std::uint16_t>(cone.direction + cone.width)),
            at(static_cast<std::uint16_t>(cone.direction - cone.width))};
}

bool contains(const Kite& k, float px, float py)
{
    return insideFilled(k[0], k[2], k[1], px, py) || insideFilled(k[0], k[1], k[3], px, py);
}

void rasterize(int relX, int relY, int radius, Cone cone, int cols, int rows, std::vector<std::pair<int, int>>& out)
{
    out.clear();
    if (cols < 2 || rows < 1) {
        return;
    }
    // In 16.16 grid units: the light, and its radius.
    const Point p0{static_cast<std::int32_t>((static_cast<std::int64_t>(relX) << 16) / kStep),
                   static_cast<std::int32_t>((static_cast<std::int64_t>(relY) << 16) / kStep)};
    const auto r = static_cast<std::int32_t>((static_cast<std::int64_t>(radius) << 16) / kStep);
    const auto at = [&](std::uint16_t a) { return Point{fixMul(r, cosFixed(a)) + p0.x, fixMul(r, sinFixed(a)) + p0.y}; };
    const Point p1 = at(cone.direction);
    const Point p2 = at(static_cast<std::uint16_t>(cone.direction + cone.width));
    const Point p3 = at(static_cast<std::uint16_t>(cone.direction - cone.width));
    triangle(p0, p2, p1, rows - 1, cols - 2, out);
    triangle(p0, p1, p3, rows - 1, cols - 2, out);
}

void gridPoints(float x, float y, int radius, Cone cone, int originI, int originJ, int cols, int rows,
                std::vector<std::pair<int, int>>& out)
{
    // The light's whole position from the grid's first point (PosVec less the grid's origin).
    const int relX = static_cast<int>(x) - (kStep * originI - 11);
    const int relY = static_cast<int>(y) - (kStep * originJ - 11);
    // Clipped as the game's 57 x 45 grid clips, or past our grid's far edges when it's bigger.
    rasterize(relX, relY, radius, cone, std::max(kGridCols, cols + 2), std::max(kGridRows, rows + 1), out);
    std::erase_if(out, [&](const std::pair<int, int>& p) { return p.first >= cols || p.second >= rows; });
}

ConeAnim coneAnim(const nox::map::xfer::InvisibleLight& l)
{
    ConeAnim a;
    a.omni = l.omni;
    a.cone = {l.direction, l.cone};
    a.flags = l.animFlags;
    std::size_t n = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const auto& c = l.colorKeys[i];
        a.updates = a.updates || (c[0] | c[1] | c[2]) != 0 || l.intensityKeys[i] != 0 || l.coneKeys[i] != 0;
        n += l.coneKeys[i] != 0 ? 1 : 0;
    }
    a.keys.assign(l.coneKeys.begin(), l.coneKeys.begin() + static_cast<std::ptrdiff_t>(n));
    a.step = l.coneStep;
    a.rotStart = static_cast<std::uint16_t>(degreesFromAngle(l.direction));
    a.rotSpeed = static_cast<std::int16_t>(l.rotationSpeed);
    a.rotMax = l.rotationMax;
    a.target = l.target;
    return a;
}

std::optional<Cone> coneAt(const ConeAnim& a, float x, float y, std::uint32_t frame, int fps)
{
    std::uint32_t omni = a.omni;
    Cone cone = a.cone;
    if (a.updates) {
        // Aiming sets the direction, which clears LightOmni (from the light's first update on).
        const bool aims = (a.flags & 0x40) != 0 && a.targetAt.has_value();
        if (aims) {
            omni = 0;
        }
        // colorLightAlterRadius: the cone keys, whole degrees.
        if (omni == 0) {
            if (const auto s = keySegment(a.keys.size(), a.step, (a.flags & 0x10) != 0, frame)) {
                cone.width = angleFromDegrees(keyWhole(a.keys[s->from], a.keys[s->to], a.step, s->frames));
            }
        }
        // colorLightRotate: a turn (or the sweep's span) every |span / speed| seconds.
        if (omni == 0 && (a.flags & 0x80) != 0 && a.rotSpeed != 0) {
            const double speed = a.rotSpeed;
            const double span = (a.flags & 0x100) != 0 ? static_cast<double>(static_cast<int>(a.rotMax) - static_cast<int>(a.rotStart)) : 360.0;
            const double period = span / speed * fps; // frames
            const double t = static_cast<double>(frame) / period;
            const double phase = (t - static_cast<double>(ftol(t))) * period;
            int degrees = ftol(phase * (speed / fps) + a.rotStart);
            if (degrees < 0) {
                degrees += 360;
            } else if (degrees >= 360) {
                degrees -= 360;
            }
            cone.direction = angleFromDegrees(degrees);
        }
        // colorLightAimAtTarget: acos of dx over the distance, made a float, times the float 57.2959
        // (0x42652EE0), 360 less that when the target is above, truncated.
        if (aims) {
            const int dx = static_cast<int>((*a.targetAt)[0]) - static_cast<int>(x);
            const int dy = static_cast<int>((*a.targetAt)[1]) - static_cast<int>(y);
            const double len = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            const float radians = static_cast<float>(std::acos(static_cast<double>(dx) / len));
            double deg = static_cast<double>(radians) * static_cast<double>(kRadiansToDegrees);
            if (dy < 0) {
                deg = 360.0 - deg;
            }
            cone.direction = angleFromDegrees(ftol(deg));
        }
    }
    if (omni == kOmni) {
        return std::nullopt;
    }
    return cone;
}

} // namespace noxsdl::lightcone
