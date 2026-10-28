// SightMap (sight.hpp): the game's visible region for the camera's view, and the mask that blacks
// out the rest of the floor.
#include "sight.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

namespace {

constexpr float kOnEdge = 2; // a point this much nearer the eye than the region's edge still counts as seen

} // namespace

SightMap::~SightMap()
{
    if (mask_) {
        SDL_DestroyTexture(mask_);
    }
}

void SightMap::build(const Camera& camera, std::span<const nox::map::Wall> walls, const std::set<std::uint16_t>& windows,
                     const nox::things::ThingDb& things, std::span<const Blocker> blockers, int eyeShiftY)
{
    // The game's view: the world rectangle on screen in whole units (its viewport's World and
    // Size), the eye at its centre (shifted by the shake's swing).
    nox::client::SightRegion::View view;
    view.left = static_cast<int>(std::floor(camera.left()));
    view.top = static_cast<int>(std::floor(camera.top()));
    view.width = static_cast<int>(std::ceil(camera.right())) - view.left;
    view.height = static_cast<int>(std::ceil(camera.bottom())) - view.top;
    view.eyeShiftY = eyeShiftY;

    auto& cells = cells_;
    cells.clear();
    // The cells the walls' region takes (the view grown by the wall images' reach), and a cell
    // more for the divisions' rounding: SightRegion keeps only those in its own cells.
    const nox::client::SightRegion::View grown = nox::client::SightRegion::wallView(view);
    const int cx0 = grown.left / 23 - 1;
    const int cy0 = grown.top / 23 - 1;
    const int cx1 = (grown.left + grown.width) / 23 + 1;
    const int cy1 = (grown.top + grown.height) / 23 + 1;
    for (const auto& w : walls) {
        if (w.x < cx0 || w.x > cx1 || w.y < cy0 || w.y > cy1) {
            continue;
        }
        nox::client::SightRegion::Wall c;
        c.x = w.x;
        c.y = w.y;
        c.direction = static_cast<std::uint8_t>(w.direction & 0x7F);
        c.blocksSight = w.material < things.walls.size() && (things.walls[w.material].flags & 1);
        c.window = windows.contains(static_cast<std::uint16_t>((w.x << 8) | w.y));
        cells.push_back(c);
    }
    auto& objects = objects_;
    objects.clear();
    keys_.clear();
    for (const auto& b : blockers) {
        // (The client's positions are whole units.)
        nox::client::SightRegion::Blocker o{static_cast<int>(b.x0), static_cast<int>(b.y0), static_cast<int>(b.x1),
                                            static_cast<int>(b.y1), b.radius};
        if (b.radius <= 0 && b.boxWidth > 0) {
            o.box = true;
            o.corners = nox::client::SightRegion::boxCorners(b.boxWidth, b.boxHeight);
        }
        if (b.key != 0) {
            keys_[b.key] = objects.size();
        }
        objects.push_back(o);
    }
    region_.build(view, cells, objects);
    if (!classicWalls_) {
        // The improved wall culling (a deliberate deviation): the same sight over the grown view,
        // for the walls alone; the floor, the drawables and the blockers go by region_.
        wallRegion_.setImprovedWalls(true);
        wallRegion_.build(grown, cells, objects);
    }

    polygon_.clear();
    for (const auto& p : region_.polygon()) {
        polygon_.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
    }
}

std::optional<bool> SightMap::reached(std::uint32_t key) const
{
    const auto it = keys_.find(key);
    if (it == keys_.end()) {
        return std::nullopt;
    }
    return region_.blockerSeen(it->second);
}

bool SightMap::drawableSeen(const TypeArt& art, std::uint32_t key, float x, float y, float z, std::uint8_t doorAngle,
                            const nox::client::DrawnImage& image) const
{
    constexpr std::uint32_t kGate = 4; // the DOOR subclass GATE
    const bool door = (art.classes & nox::things::ObjectClass::Door) != 0;
    const bool gate = door && (art.subclasses & kGate);
    if (art.shadow || (door && !gate)) {
        // Its mark from the sweep, cleared every frame for every sight blocker and set when its
        // interval made it in: one left out (invisible, shapeless, outside the view) isn't seen.
        return reached(key).value_or(false);
    }
    // (The client's positions are whole units.)
    nox::client::SightRegion::Target t;
    t.x = static_cast<int>(std::floor(x));
    t.y = static_cast<int>(std::floor(y));
    if (gate) {
        const SDL_Point leaf = doorLeaf(doorAngle);
        t.x += leaf.x / 2;
        t.y += leaf.y / 2;
    }
    t.z = static_cast<int>(z);
    t.circle = !art.extentBox && art.extentRadius > 0;
    t.radius = t.circle ? art.extentRadius : 0;
    if (art.extentBox) {
        t.corners = nox::client::SightRegion::boxCorners(art.extentWidth, art.extentHeight);
    }
    t.zSizeMin = art.zSizeMin;
    t.zSizeMax = art.zSizeMax;
    t.image = image;
    return region_.drawableSeen(t);
}

bool SightMap::visible(float x, float y) const
{
    const auto& v = region_.view();
    if (v.width <= 0 || v.height <= 0) {
        return true;
    }
    const auto ex = static_cast<float>(region_.eyeX());
    const auto ey = static_cast<float>(region_.eyeY());
    // Off the view: the place its line from the eye leaves the view.
    const auto x0 = static_cast<float>(v.left);
    const auto y0 = static_cast<float>(v.top);
    const auto x1 = static_cast<float>(v.left + v.width - 1);
    const auto y1 = static_cast<float>(v.top + v.height - 1);
    float px = x;
    float py = y;
    if (px < x0 || px > x1 || py < y0 || py > y1) {
        const float dx = px - ex;
        const float dy = py - ey;
        float t = 1;
        if (px < x0) {
            t = std::min(t, (x0 - ex) / dx);
        } else if (px > x1) {
            t = std::min(t, (x1 - ex) / dx);
        }
        if (py < y0) {
            t = std::min(t, (y0 - ey) / dy);
        } else if (py > y1) {
            t = std::min(t, (y1 - ey) / dy);
        }
        px = ex + dx * t;
        py = ey + dy * t;
    }
    const auto inside = [&](float qx, float qy) {
        return region_.spanContains(static_cast<int>(std::floor(qx)), static_cast<int>(std::floor(qy)));
    };
    if (inside(px, py)) {
        return true;
    }
    // On the region's edge (a pillar's centre, a thing against a wall): a little nearer the eye.
    const float dx = ex - px;
    const float dy = ey - py;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 1) {
        return true;
    }
    return inside(px + dx / d * kOnEdge, py + dy / d * kOnEdge);
}

std::optional<std::pair<float, float>> SightMap::visibleRange(float x0, float y0, float x1, float y1) const
{
    constexpr int kSamples = 8;
    std::optional<std::pair<float, float>> range;
    // Nine samples; the first and last seen bound the range (a hidden gap between them counts as seen).
    for (int i = 0; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / kSamples;
        if (visible(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)) {
            if (!range) {
                range = std::pair{t, t};
            }
            range->second = t;
        }
    }
    return range;
}

void SightMap::drawMask(SDL_Renderer* renderer, const Camera& camera)
{
    if (polygon_.size() < 3) {
        return;
    }
    SDL_Texture* target = SDL_GetRenderTarget(renderer);
    int w = 0;
    int h = 0;
    if (target) {
        float fw = 0;
        float fh = 0;
        SDL_GetTextureSize(target, &fw, &fh);
        w = static_cast<int>(fw);
        h = static_cast<int>(fh);
    } else {
        SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
    }
    // Drawn at twice the target's resolution and scaled down with linear filtering: each target
    // pixel averages 2 x 2 samples, so the edge is anti-aliased (the region itself is the game's).
    constexpr int kSupersample = 2;
    if (!mask_ || maskW_ != w * kSupersample || maskH_ != h * kSupersample) {
        if (mask_) {
            SDL_DestroyTexture(mask_);
        }
        maskW_ = w * kSupersample;
        maskH_ = h * kSupersample;
        mask_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, maskW_, maskH_);
        if (!mask_) {
            return;
        }
        SDL_SetTextureBlendMode(mask_, SDL_BLENDMODE_MUL);  // multiplied in: black hides, white leaves what's there
        SDL_SetTextureScaleMode(mask_, SDL_SCALEMODE_LINEAR);
    }

    // The polygon filled as the game's spans fill it, by the even-odd rule: a fan of triangles from
    // the eye, each turning what it covers from black to white or back (1 - what's there). The
    // polygon isn't always star-shaped round the eye: where a point made whole lands a little past
    // the next occluder's ray, the edge between them turns back, and a plain white fan would cover
    // the sliver past that shadow edge, leaving it lit between the soft edge and the black.
    std::vector<SDL_Vertex> verts;
    verts.reserve(polygon_.size() * 3);
    const SDL_FColor white{1, 1, 1, 1};
    const float scale = camera.zoom * kSupersample;
    const auto screen = [&](float x, float y) {
        return SDL_FPoint{(x - camera.left()) * scale, (y - camera.top()) * scale};
    };
    const SDL_FPoint eye = screen(static_cast<float>(region_.eyeX()), static_cast<float>(region_.eyeY()));
    for (std::size_t i = 0; i < polygon_.size(); ++i) {
        const SDL_FPoint a = screen(polygon_[i].x, polygon_[i].y);
        const SDL_FPoint b = screen(polygon_[(i + 1) % polygon_.size()].x, polygon_[(i + 1) % polygon_.size()].y);
        verts.push_back({eye, white, {0, 0}});
        verts.push_back({a, white, {0, 0}});
        verts.push_back({b, white, {0, 0}});
    }
    SDL_SetRenderTarget(renderer, mask_);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_BlendMode previousMode = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &previousMode);
    static const SDL_BlendMode kToggle =
        SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_BLENDFACTOR_ZERO, SDL_BLENDOPERATION_ADD,
                                   SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    if (!SDL_SetRenderDrawBlendMode(renderer, kToggle)) {
        // (A renderer without custom blending: the plain fan, right wherever the polygon is
        // star-shaped round the eye.)
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }
    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()), nullptr, 0);
    SDL_SetRenderDrawBlendMode(renderer, previousMode);

    if (softEdges_) {
        // Soft Shadow Edge: along each shadow edge, ten one-unit black lines, each a unit further
        // into the seen side and fainter (alpha 0xD0 down by 0x14), blended over what's there.
        verts.clear();
        for (const auto& e : region_.softEdges()) {
            const auto sx = static_cast<float>(e.stepX);
            const auto sy = static_cast<float>(e.stepY);
            for (std::size_t k = 0; k < nox::client::SightRegion::kSoftEdgeAlpha.size(); ++k) {
                const SDL_FColor black{0, 0, 0, nox::client::SightRegion::kSoftEdgeAlpha[k] / 255.0f};
                const auto near = static_cast<float>(k);
                const float far = near + 1;
                const SDL_FPoint a0 = screen(static_cast<float>(e.x0) + sx * near, static_cast<float>(e.y0) + sy * near);
                const SDL_FPoint b0 = screen(static_cast<float>(e.x1) + sx * near, static_cast<float>(e.y1) + sy * near);
                const SDL_FPoint a1 = screen(static_cast<float>(e.x0) + sx * far, static_cast<float>(e.y0) + sy * far);
                const SDL_FPoint b1 = screen(static_cast<float>(e.x1) + sx * far, static_cast<float>(e.y1) + sy * far);
                verts.push_back({a0, black, {0, 0}});
                verts.push_back({b0, black, {0, 0}});
                verts.push_back({b1, black, {0, 0}});
                verts.push_back({a0, black, {0, 0}});
                verts.push_back({b1, black, {0, 0}});
                verts.push_back({a1, black, {0, 0}});
            }
        }
        if (!verts.empty()) {
            SDL_BlendMode mode = SDL_BLENDMODE_NONE;
            SDL_GetRenderDrawBlendMode(renderer, &mode);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()), nullptr, 0);
            SDL_SetRenderDrawBlendMode(renderer, mode);
        }
    }
    SDL_SetRenderTarget(renderer, target);
    SDL_RenderTexture(renderer, mask_, nullptr, nullptr);
}

} // namespace noxsdl
