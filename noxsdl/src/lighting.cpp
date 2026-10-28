// The light map (lighting.hpp gives the game's formula): the grid built each frame, its bilinear
// sample and floor overlay, and the per-pixel path's light target.
#include "lighting.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

namespace {

constexpr float kStep = 23;
constexpr float kOffset = -11;          // lattice point (i, j) is at world (23i - 11, 23j - 11)
constexpr float kMaxIntensity = 63;     // Draw_LightSetIntensity clamp
constexpr float kIntensityCap = 31;     // the exe's 31.0
constexpr float kThreshold = 4;         // the exe's 4.0
constexpr float kFalloff = 961.0f * 66.0f / 65536.0f; // the exe's 961, times 66, over 16.16
constexpr float kMargin = 100;          // lights this far outside the view still count

// The lattice index at or before a world coordinate (left of it, or above it).
int latticeFloor(float world)
{
    return static_cast<int>(std::floor((world - kOffset) / kStep));
}

} // namespace

float lightRadius(float intensity)
{
    // lightRadiusFromIntensity with its constants: min 1.0, 0.001, 961, 4, 1.0, cap 31.
    if (intensity <= 1) {
        return 0;
    }
    const float capped = std::min(intensity, kIntensityCap);
    return std::sqrt(((4 + capped) / (4 + 1.0f) + 1.0f) * (intensity * intensity / (961.0f * 0.001f)));
}

LightMap::~LightMap()
{
    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
    if (target_) {
        SDL_DestroyTexture(target_);
    }
}

// The grid point at lattice (i, j); nullptr outside this frame's grid.
const LightMap::Accum* LightMap::at(int i, int j) const
{
    const int x = i - originI_;
    const int y = j - originJ_;
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return nullptr;
    }
    return &grid_[static_cast<std::size_t>(y) * width_ + x];
}

void LightMap::build(const Camera& camera, Rgb ambient, std::span<const LightSource> lights, std::uint32_t frame)
{
    ambient_ = ambient;
    // The grid: from the game's first point (an even lattice point one or two before the view: the
    // client's grid build) to two past its far edge, so every sample in view has its four points.
    // (Where it starts moves no all-round light's points; a cone's filler clips at the grid's top
    // and left as the game's does.)
    originI_ = lightcone::gridOriginIndex(camera.left());
    originJ_ = lightcone::gridOriginIndex(camera.top());
    width_ = latticeFloor(camera.right()) + 2 - originI_ + 1;
    height_ = latticeFloor(camera.bottom()) + 2 - originJ_ + 1;
    grid_.assign(static_cast<std::size_t>(width_) * height_, Accum{float(ambient[0]), float(ambient[1]), float(ambient[2])});

    // Lights within kMargin of the view count; each is kept resolved (resolved_) for sampleExact and
    // the per-pixel path.
    const float left = camera.left() - kMargin;
    const float right = camera.right() + kMargin;
    const float top = camera.top() - kMargin;
    const float bottom = camera.bottom() + kMargin;
    resolved_.clear();
    for (const auto& light : lights) {
        if (light.x < left || light.x > right || light.y < top || light.y > bottom) {
            continue;
        }
        float intensity = std::min(light.intensity, kMaxIntensity);
        if (light.flicker) {
            // Math_RandIntVisual(0, I / 4) whole steps, a new pick every game frame.
            // A hash of the frame and the light's seed stands in for the exe's random pick.
            const std::uint32_t h = (frame * 2654435761u) ^ (light.seed * 40503u + 0x9E3779B9u);
            const int steps = static_cast<int>(intensity / 4);
            intensity += static_cast<float>(steps > 0 ? static_cast<int>(h % static_cast<std::uint32_t>(steps + 1)) : 0);
        }
        if (intensity <= 1) {
            continue;
        }
        // The formula's parts: the radius it reaches, floor(I) for the falloff, and min(I, 31) + 4 over it.
        const float radius = lightRadius(intensity);
        const float whole = std::floor(intensity);
        const float numerator = std::min(intensity, kIntensityCap) + kThreshold;
        Resolved& resolved = resolved_.emplace_back(Resolved{light.x, light.y, radius, whole, numerator, light.color, light.anti});
        // A cone's kite reaches the drawable's whole radius (LightRadius: truncated).
        const int coneRadius = light.cone ? lightcone::radiusOf(intensity) : 0;
        if (light.cone) {
            // A cone never darkens (cliLight16's cone path passes "not anti").
            resolved.anti = false;
            resolved.kite = lightcone::kite(light.x, light.y, static_cast<float>(coneRadius), *light.cone);
        }
        if (perPixel()) {
            continue; // worked out per pixel and per sprite instead of on the grid
        }
        if (light.cone) {
            // The grid points inside its kite, filled as the game's scanlines fill them, each lit by the
            // formula with no radius test. (The light's own whole position and radius, as the game's.)
            // The filler clips to the game's grid, 57 x 45 from its first point (its last row and
            // the spans' last column never lit), or past our grid's far edges when the view is
            // bigger than any the game draws (so no cone stops short inside the view).
            lightcone::gridPoints(light.x, light.y, coneRadius, *light.cone, originI_, originJ_, width_, height_, conePoints_);
            for (const auto& [gx, gy] : conePoints_) {
                const float dx = light.x - (kStep * static_cast<float>(originI_ + gx) + kOffset);
                const float dy = light.y - (kStep * static_cast<float>(originJ_ + gy) + kOffset);
                add(grid_[static_cast<std::size_t>(gy) * width_ + gx], resolved, dx * dx + dy * dy);
            }
            continue;
        }
        // Every lattice point in the light's bounding square; those outside its circle are skipped.
        for (int j = latticeFloor(light.y - radius); j <= latticeFloor(light.y + radius) + 1; ++j) {
            for (int i = latticeFloor(light.x - radius); i <= latticeFloor(light.x + radius) + 1; ++i) {
                const int gx = i - originI_;
                const int gy = j - originJ_;
                if (gx < 0 || gy < 0 || gx >= width_ || gy >= height_) {
                    continue;
                }
                const float dx = light.x - (kStep * i + kOffset);
                const float dy = light.y - (kStep * j + kOffset);
                const float d2 = dx * dx + dy * dy;
                if (d2 > radius * radius) {
                    continue;
                }
                add(grid_[static_cast<std::size_t>(gy) * width_ + gx], resolved, d2);
            }
        }
    }
}

void LightMap::add(Accum& p, const Resolved& l, float d2)
{
    // The exe's falloff; only what's over the threshold counts, a 32nd of it times the colour.
    const float v = l.numerator / (1 + kFalloff * d2 / (l.whole * l.whole));
    if (v <= kThreshold) {
        return;
    }
    const float amount = (l.anti ? -1.0f : 1.0f) * (v - kThreshold) / 32;
    p.r = std::clamp(p.r + l.color[0] * amount, 0.0f, 255.0f);
    p.g = std::clamp(p.g + l.color[1] * amount, 0.0f, 255.0f);
    p.b = std::clamp(p.b + l.color[2] * amount, 0.0f, 255.0f);
}

Rgb LightMap::sampleExact(float x, float y) const
{
    // The grid's sum at one point: ambient, then each light added and clamped in turn.
    Accum p{float(ambient_[0]), float(ambient_[1]), float(ambient_[2])};
    for (const Resolved& l : resolved_) {
        const float dx = l.x - x;
        const float dy = l.y - y;
        const float d2 = dx * dx + dy * dy;
        // A cone: inside its kite (no radius test); all round: within the radius.
        if (l.kite ? !lightcone::contains(*l.kite, x, y) : d2 > l.radius * l.radius) {
            continue;
        }
        add(p, l, d2);
    }
    return {static_cast<std::uint8_t>(p.r), static_cast<std::uint8_t>(p.g), static_cast<std::uint8_t>(p.b)};
}

void LightMap::drawPerPixel(SDL_Renderer* renderer, const Camera& camera)
{
    const int w = camera.viewWidth;
    const int h = camera.viewHeight;
    if (!perPixel() || w <= 0 || h <= 0) {
        return;
    }
    if (!target_ || targetWidth_ != w || targetHeight_ != h) {
        if (target_) {
            SDL_DestroyTexture(target_);
        }
        // 8-bit unsigned normalised, as the game's light values are: each light's blend clamps to
        // 0-1 as the grid clamps each light's sum to 0-255, so a light never brightens the floor
        // past its own colours. (Not a float target, which doesn't clamp: overlapping lights, a
        // cleansing flame's ring, lit the floor brighter than its art. SDL's GPU renderer has no
        // 16-bit unorm format, and gives half floats for RGBA64.)
        target_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
        targetWidth_ = w;
        targetHeight_ = h;
        if (!target_) {
            return;
        }
        SDL_SetTextureBlendMode(target_, SDL_BLENDMODE_MUL);
    }
    // Each light's quad adds its colour into the target (or takes it away for an anti-light); alpha is left alone.
    static const SDL_BlendMode add = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    static const SDL_BlendMode subtract = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT, SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);

    SDL_Texture* previous = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, target_);
    SDL_SetRenderDrawColorFloat(renderer, ambient_[0] / 255.0f, ambient_[1] / 255.0f, ambient_[2] / 255.0f, 1.0f);
    SDL_RenderClear(renderer);
    SDL_SetGPURenderState(renderer, perPixel_);
    for (const Resolved& l : resolved_) {
        if (l.whole <= 0) {
            continue;
        }
        // A quad over the light's circle; alpha carries the formula's numerator over 64 to the shader.
        const float cx = (l.x - camera.left()) * camera.zoom;
        const float cy = (l.y - camera.top()) * camera.zoom;
        const float r = l.radius * camera.zoom;
        const float k = 0.5f * l.radius / l.whole; // uv * 2 - 1 = offset / floor(I)
        const SDL_FColor c{l.color[0] / 255.0f, l.color[1] / 255.0f, l.color[2] / 255.0f, l.numerator / 64.0f};
        SDL_SetTextureBlendMode(white_, l.anti ? subtract : add);
        if (l.kite) {
            // A cone: its kite's two triangles (P0, P2, P1) and (P0, P1, P3) where the game fills them,
            // the texture coordinates mapped as the quad's (uv * 2 - 1 = offset / floor(I)).
            SDL_Vertex v[4];
            for (std::size_t i = 0; i < 4; ++i) {
                const auto& p = (*l.kite)[i];
                v[i] = {{(p[0] - camera.left()) * camera.zoom, (p[1] - camera.top()) * camera.zoom}, c,
                        {0.5f + 0.5f * (p[0] - l.x) / l.whole, 0.5f + 0.5f * (p[1] - l.y) / l.whole}};
            }
            int index[6];
            int n = 0;
            const auto tri = [&](int a, int b, int d) {
                const auto& q = *l.kite;
                // (wound as the game fills: negative, y down; the other way it lights nothing)
                if ((q[b][0] - q[a][0]) * (q[d][1] - q[a][1]) - (q[b][1] - q[a][1]) * (q[d][0] - q[a][0]) < 0) {
                    index[n++] = a, index[n++] = b, index[n++] = d;
                }
            };
            tri(0, 2, 1);
            tri(0, 1, 3);
            if (n > 0) {
                SDL_RenderGeometry(renderer, white_, v, 4, index, n);
            }
            continue;
        }
        const SDL_Vertex v[4] = {{{cx - r, cy - r}, c, {0.5f - k, 0.5f - k}},
                                 {{cx + r, cy - r}, c, {0.5f + k, 0.5f - k}},
                                 {{cx + r, cy + r}, c, {0.5f + k, 0.5f + k}},
                                 {{cx - r, cy + r}, c, {0.5f - k, 0.5f + k}}};
        const int index[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(renderer, white_, v, 4, index, 6);
    }
    SDL_SetGPURenderState(renderer, nullptr);
    SDL_SetRenderTarget(renderer, previous);
    const SDL_FRect whole{0, 0, static_cast<float>(w), static_cast<float>(h)};
    SDL_RenderTexture(renderer, target_, &whole, &whole);
}

Rgb LightMap::sample(float x, float y) const
{
    if (perPixel()) {
        return sampleExact(x, y);
    }
    // Lattice coordinates, then bilinear between the four points around them.
    const float fx = (x - kOffset) / kStep;
    const float fy = (y - kOffset) / kStep;
    const int i = static_cast<int>(std::floor(fx));
    const int j = static_cast<int>(std::floor(fy));
    const Accum* p00 = at(i, j);
    const Accum* p10 = at(i + 1, j);
    const Accum* p01 = at(i, j + 1);
    const Accum* p11 = at(i + 1, j + 1);
    if (!p00 || !p10 || !p01 || !p11) {
        return ambient_;
    }
    const float tx = fx - static_cast<float>(i);
    const float ty = fy - static_cast<float>(j);
    const auto mix = [&](float a, float b, float c, float d) {
        const float top = a + (b - a) * tx;
        const float bottom = c + (d - c) * tx;
        return static_cast<std::uint8_t>(std::clamp(top + (bottom - top) * ty, 0.0f, 255.0f));
    };
    return {mix(p00->r, p10->r, p01->r, p11->r), mix(p00->g, p10->g, p01->g, p11->g),
            mix(p00->b, p10->b, p01->b, p11->b)};
}

void LightMap::drawOverlay(SDL_Renderer* renderer, const Camera& camera)
{
    if (width_ <= 0 || height_ <= 0) {
        return;
    }
    if (!texture_ || textureWidth_ < width_ || textureHeight_ < height_) {
        if (texture_) {
            SDL_DestroyTexture(texture_);
        }
        textureWidth_ = std::max(width_, 64);
        textureHeight_ = std::max(height_, 64);
        texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, textureWidth_,
                                     textureHeight_);
        if (!texture_) {
            return;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_MUL);
        SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(texture_, nullptr, &pixels, &pitch)) {
        return;
    }
    // One texel per lattice point; the MUL blend multiplies the floor by it.
    for (int y = 0; y < height_; ++y) {
        auto* row = static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch;
        for (int x = 0; x < width_; ++x) {
            const auto& p = grid_[static_cast<std::size_t>(y) * width_ + x];
            row[x * 4 + 0] = static_cast<std::uint8_t>(p.r);
            row[x * 4 + 1] = static_cast<std::uint8_t>(p.g);
            row[x * 4 + 2] = static_cast<std::uint8_t>(p.b);
            row[x * 4 + 3] = 255;
        }
    }
    SDL_UnlockTexture(texture_);

    // Texel (x, y)'s centre is lattice point (originI + x, originJ + y); linear filtering then
    // interpolates between points exactly as the game's bilinear sample. The outer half texel
    // is clamped, which only touches the margin beyond the view.
    const SDL_FRect src{0, 0, static_cast<float>(width_), static_cast<float>(height_)};
    const float x0 = kStep * static_cast<float>(originI_) + kOffset - kStep / 2;
    const float y0 = kStep * static_cast<float>(originJ_) + kOffset - kStep / 2;
    const SDL_FRect dst{(x0 - camera.left()) * camera.zoom, (y0 - camera.top()) * camera.zoom,
                        kStep * static_cast<float>(width_) * camera.zoom, kStep * static_cast<float>(height_) * camera.zoom};
    SDL_RenderTexture(renderer, texture_, &src, &dst);
}

} // namespace noxsdl
