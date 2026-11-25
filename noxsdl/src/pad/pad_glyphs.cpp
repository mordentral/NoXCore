// (noxcpp) The pad's drawn shapes and button glyphs (pad_glyphs.hpp).
#include "pad/pad_glyphs.hpp"

#include "text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace noxsdl::pad {

namespace {

constexpr float kFeather = 1.0f; // (the soft edge, output pixels)

/// The drawing's vertices and indices, kept from one shape to the next (one render thread).
std::vector<SDL_Vertex>& vertices()
{
    static std::vector<SDL_Vertex> v;
    return v;
}
std::vector<int>& indices()
{
    static std::vector<int> i;
    return i;
}
std::vector<SDL_FPoint>& normals()
{
    static std::vector<SDL_FPoint> n;
    return n;
}

/// The outward normal at each point of a convex outline, its length the mitre (so an offset band
/// keeps its width along the edges), at most 2.
void vertexNormals(std::span<const SDL_FPoint> p, std::vector<SDL_FPoint>& out)
{
    const std::size_t n = p.size();
    out.assign(n, SDL_FPoint{0, 0});
    SDL_FPoint centre{0, 0};
    for (const auto& q : p) {
        centre.x += q.x;
        centre.y += q.y;
    }
    centre.x /= static_cast<float>(n);
    centre.y /= static_cast<float>(n);
    const auto edgeNormal = [&](std::size_t i) {
        const SDL_FPoint a = p[i], b = p[(i + 1) % n];
        SDL_FPoint e{b.y - a.y, a.x - b.x};
        const float len = std::hypot(e.x, e.y);
        if (len <= 1e-6f) {
            return SDL_FPoint{0, 0};
        }
        e = {e.x / len, e.y / len};
        const SDL_FPoint mid{(a.x + b.x) / 2 - centre.x, (a.y + b.y) / 2 - centre.y};
        if (e.x * mid.x + e.y * mid.y < 0) {
            e = {-e.x, -e.y};
        }
        return e;
    };
    for (std::size_t i = 0; i < n; ++i) {
        SDL_FPoint before = edgeNormal((i + n - 1) % n);
        SDL_FPoint after = edgeNormal(i);
        if (before.x == 0 && before.y == 0) {
            before = after;
        }
        if (after.x == 0 && after.y == 0) {
            after = before;
        }
        SDL_FPoint m{before.x + after.x, before.y + after.y};
        const float len = std::hypot(m.x, m.y);
        if (len <= 1e-6f) {
            out[i] = after;
            continue;
        }
        m = {m.x / len, m.y / len};
        const float cosine = std::max(0.5f, m.x * after.x + m.y * after.y);
        out[i] = {m.x / cosine, m.y / cosine};
    }
}

SDL_FColor clear(SDL_FColor c)
{
    c.a = 0;
    return c;
}

void push(std::vector<SDL_Vertex>& v, SDL_FPoint p, SDL_FColor c)
{
    v.push_back(SDL_Vertex{p, c, {0, 0}});
}

/// Two rings of points a band joins (a and b, i each, as quads), into the indices.
void band(std::vector<int>& idx, int a, int b, int n)
{
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        idx.insert(idx.end(), {a + i, a + j, b + j, a + i, b + j, b + i});
    }
}

void render(SDL_Renderer* r)
{
    auto& v = vertices();
    auto& idx = indices();
    if (v.empty() || idx.empty()) {
        return;
    }
    SDL_BlendMode mode = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r, &mode);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
    SDL_SetRenderDrawBlendMode(r, mode);
}

int textSize(float h, float share)
{
    return std::max(6, static_cast<int>(std::lround(h * share)));
}

SDL_Color byte(SDL_FColor c)
{
    const auto b = [](float f) { return static_cast<Uint8>(std::clamp(std::lround(f * 255.0f), 0L, 255L)); };
    return {b(c.r), b(c.g), b(c.b), b(c.a)};
}

/// Text centred on (cx, cy): its ink's middle there, whatever the face's line and ascent (the HD
/// fonts' and the original style's alike).
void centredText(Font* font, const std::string& text, float cx, float cy, int size, SDL_FColor c)
{
    if (!font || text.empty()) {
        return;
    }
    const float w = font->measure(text, size);
    const auto [top, bottom] = font->inkSpan(text, size);
    const float middle = bottom > top ? (top + bottom) / 2 : font->lineHeight(size) / 2;
    font->draw(text, std::round(cx - w / 2), std::round(cy - middle), size, byte(c));
}

float boxWidth(Font* font, const ButtonLabel& l, float h)
{
    const float text = font && !l.text.empty() ? font->measure(l.text, textSize(h, 0.62f)) : 0;
    return std::max(h, std::ceil(text + h * 0.55f));
}

} // namespace

namespace shape {

void fill(SDL_Renderer* r, std::span<const SDL_FPoint> p, SDL_FColor c)
{
    if (p.size() < 3) {
        return;
    }
    auto& v = vertices();
    auto& idx = indices();
    auto& nrm = normals();
    v.clear();
    idx.clear();
    vertexNormals(p, nrm);
    const int n = static_cast<int>(p.size());
    for (int i = 0; i < n; ++i) {
        push(v, p[static_cast<std::size_t>(i)], c);
    }
    for (int i = 0; i < n; ++i) {
        const auto& q = p[static_cast<std::size_t>(i)];
        const auto& m = nrm[static_cast<std::size_t>(i)];
        push(v, {q.x + m.x * kFeather, q.y + m.y * kFeather}, clear(c));
    }
    for (int i = 1; i + 1 < n; ++i) {
        idx.insert(idx.end(), {0, i, i + 1});
    }
    band(idx, 0, n, n);
    render(r);
}

void stroke(SDL_Renderer* r, std::span<const SDL_FPoint> p, float width, SDL_FColor c)
{
    if (p.size() < 3) {
        return;
    }
    auto& v = vertices();
    auto& idx = indices();
    auto& nrm = normals();
    v.clear();
    idx.clear();
    vertexNormals(p, nrm);
    const int n = static_cast<int>(p.size());
    const float half = std::max(0.5f, width / 2 - kFeather / 2);
    for (const float d : {-(half + kFeather), -half, half, half + kFeather}) {
        const bool edge = std::abs(d) > half;
        for (int i = 0; i < n; ++i) {
            const auto& q = p[static_cast<std::size_t>(i)];
            const auto& m = nrm[static_cast<std::size_t>(i)];
            push(v, {q.x + m.x * d, q.y + m.y * d}, edge ? clear(c) : c);
        }
    }
    band(idx, 0, n, n);
    band(idx, n, 2 * n, n);
    band(idx, 2 * n, 3 * n, n);
    render(r);
}

void circlePoints(std::vector<SDL_FPoint>& out, float cx, float cy, float radius)
{
    ellipsePoints(out, cx, cy, radius, radius, 0);
}

void ellipsePoints(std::vector<SDL_FPoint>& out, float cx, float cy, float rx, float ry, float turn)
{
    out.clear();
    const float around = 2 * std::numbers::pi_v<float> * std::max(rx, ry);
    const int n = std::clamp(static_cast<int>(around / 3.0f), 12, 128);
    const float c = std::cos(turn), s = std::sin(turn);
    for (int i = 0; i < n; ++i) {
        const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n);
        const float x = std::cos(a) * rx, y = std::sin(a) * ry;
        out.push_back({cx + x * c - y * s, cy + x * s + y * c});
    }
}

void roundBoxPoints(std::vector<SDL_FPoint>& out, float x, float y, float w, float h, float radius)
{
    out.clear();
    radius = std::clamp(radius, 0.0f, std::min(w, h) / 2);
    const int steps = std::clamp(static_cast<int>(radius * 0.6f), 2, 24);
    const std::array<SDL_FPoint, 4> centres{{{x + w - radius, y + radius}, {x + w - radius, y + h - radius},
                                             {x + radius, y + h - radius}, {x + radius, y + radius}}};
    for (int corner = 0; corner < 4; ++corner) {
        const float from = (static_cast<float>(corner) - 1) * std::numbers::pi_v<float> / 2; // (-90: the top right's start)
        for (int i = 0; i <= steps; ++i) {
            const float a = from + static_cast<float>(i) / static_cast<float>(steps) * std::numbers::pi_v<float> / 2;
            out.push_back({centres[static_cast<std::size_t>(corner)].x + std::cos(a) * radius,
                           centres[static_cast<std::size_t>(corner)].y + std::sin(a) * radius});
        }
    }
}

void line(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, float width, SDL_FColor c)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::hypot(dx, dy);
    if (len <= 1e-3f) {
        return;
    }
    const float half = std::max(0.5f, width / 2);
    const float nx = -dy / len * half, ny = dx / len * half;
    const std::array<SDL_FPoint, 4> q{{{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}}};
    fill(r, q, c);
}

} // namespace shape

float glyphWidth(Font* font, const ButtonLabel& label, float h)
{
    return label.glyph == Glyph::Box || label.glyph == Glyph::Stick ? boxWidth(font, label, h) : h;
}

float drawGlyph(SDL_Renderer* r, Font* font, const ButtonLabel& l, float x, float cy, float h, const GlyphInk& ink)
{
    static std::vector<SDL_FPoint> pts;
    const float edge = std::max(1.0f, h * 0.08f);
    const float w = glyphWidth(font, l, h);
    const float cx = x + w / 2;
    const auto disc = [&] {
        shape::circlePoints(pts, cx, cy, h / 2 - edge / 2);
        shape::fill(r, pts, ink.fill);
        shape::stroke(r, pts, edge, ink.ink);
    };
    switch (l.glyph) {
    case Glyph::Round:
        disc();
        centredText(font, l.text, cx, cy, textSize(h, 0.62f), ink.ink);
        break;
    case Glyph::Cross: {
        disc();
        const float a = h * 0.2f;
        shape::line(r, {cx - a, cy - a}, {cx + a, cy + a}, edge * 1.2f, ink.ink);
        shape::line(r, {cx - a, cy + a}, {cx + a, cy - a}, edge * 1.2f, ink.ink);
        break;
    }
    case Glyph::Circle:
        disc();
        shape::circlePoints(pts, cx, cy, h * 0.22f);
        shape::stroke(r, pts, edge * 1.2f, ink.ink);
        break;
    case Glyph::Square:
        disc();
        shape::roundBoxPoints(pts, cx - h * 0.2f, cy - h * 0.2f, h * 0.4f, h * 0.4f, 0);
        shape::stroke(r, pts, edge * 1.2f, ink.ink);
        break;
    case Glyph::Triangle: {
        disc();
        const float t = h * 0.25f;
        const std::array<SDL_FPoint, 3> tri{{{cx, cy - t}, {cx + t * 0.866f, cy + t * 0.5f}, {cx - t * 0.866f, cy + t * 0.5f}}};
        shape::stroke(r, tri, edge * 1.2f, ink.ink);
        break;
    }
    case Glyph::Place: {
        const float off = h * 0.3f, dot = h * 0.17f;
        const std::array<std::pair<PadButton, SDL_FPoint>, 4> dots{{{PadButton::North, {cx, cy - off}},
                                                                    {PadButton::South, {cx, cy + off}},
                                                                    {PadButton::West, {cx - off, cy}},
                                                                    {PadButton::East, {cx + off, cy}}}};
        for (const auto& [b, at] : dots) {
            shape::circlePoints(pts, at.x, at.y, dot);
            if (b == l.button) {
                shape::fill(r, pts, ink.ink);
            } else {
                shape::stroke(r, pts, std::max(1.0f, h * 0.06f), ink.dim);
            }
        }
        break;
    }
    case Glyph::Dpad: {
        // The cross on a disc as the face buttons' (so it reads as a button on any art, never as
        // a lone blob), the arm meant in the ink.
        disc();
        const float half = h * 0.36f, a = h * 0.26f;
        shape::roundBoxPoints(pts, cx - a / 2, cy - half, a, 2 * half, a * 0.2f);
        shape::fill(r, pts, ink.dim);
        shape::roundBoxPoints(pts, cx - half, cy - a / 2, 2 * half, a, a * 0.2f);
        shape::fill(r, pts, ink.dim);
        switch (l.button) {
        case PadButton::DpadUp: shape::roundBoxPoints(pts, cx - a / 2, cy - half, a, half - a / 2, a * 0.2f); break;
        case PadButton::DpadDown: shape::roundBoxPoints(pts, cx - a / 2, cy + a / 2, a, half - a / 2, a * 0.2f); break;
        case PadButton::DpadLeft: shape::roundBoxPoints(pts, cx - half, cy - a / 2, half - a / 2, a, a * 0.2f); break;
        default: shape::roundBoxPoints(pts, cx + a / 2, cy - a / 2, half - a / 2, a, a * 0.2f); break;
        }
        shape::fill(r, pts, ink.ink);
        break;
    }
    case Glyph::Box:
    case Glyph::Stick: {
        const float radius = l.glyph == Glyph::Stick ? h / 2 : h * 0.28f;
        shape::roundBoxPoints(pts, x + edge / 2, cy - h / 2 + edge / 2, w - edge, h - edge, radius);
        shape::fill(r, pts, ink.fill);
        shape::stroke(r, pts, edge, ink.ink);
        centredText(font, l.text, cx, cy, textSize(h, 0.62f), ink.ink);
        break;
    }
    }
    return w;
}

float promptHeight(const PromptLabels& prompt, float h)
{
    return prompt.modifier ? std::max(h, kMinPairHeight) : h;
}

float promptWidth(Font* font, const PromptLabels& prompt, float h)
{
    if (!prompt.button) {
        return 0;
    }
    h = promptHeight(prompt, h);
    float w = glyphWidth(font, *prompt.button, h);
    if (prompt.modifier) {
        w += glyphWidth(font, *prompt.modifier, h) + h * 0.5f;
    }
    return w;
}

float drawPrompt(SDL_Renderer* r, Font* font, const PromptLabels& prompt, float x, float cy, float h, const GlyphInk& ink)
{
    if (!prompt.button) {
        return 0;
    }
    h = promptHeight(prompt, h);
    float at = x;
    if (prompt.modifier) {
        at += drawGlyph(r, font, *prompt.modifier, at, cy, h, ink);
        // The "+" drawn, not typed (a font's at this size is a dot): two bars on a dark edge.
        const float px = at + h * 0.25f, arm = h * 0.2f, width = std::max(1.5f, h * 0.12f);
        for (const auto& [w, c] : {std::pair{width + 2.0f, ink.fill}, std::pair{width, ink.ink}}) {
            shape::line(r, {px - arm - (w - width) / 2, cy}, {px + arm + (w - width) / 2, cy}, w, c);
            shape::line(r, {px, cy - arm - (w - width) / 2}, {px, cy + arm + (w - width) / 2}, w, c);
        }
        at += h * 0.5f;
    }
    at += drawGlyph(r, font, *prompt.button, at, cy, h, ink);
    return at - x;
}

} // namespace noxsdl::pad
