// MapThumbnails: the map vote's top-down map pictures, drawn in software on a worker thread
// (map_thumbnail.hpp) and uploaded as textures on the render thread.
#include "map_thumbnail.hpp"

#include "thing_art.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <optional>
#include <set>

namespace noxsdl {

namespace {

// MapView's layout (map_view.cpp): 46-unit floor cells, the odd tile at (46x + 12, 46y - 11) and
// the even one at (46x - 11, 46y + 12); walls on the 23-unit grid.
constexpr int kCell = 46;
constexpr int kOddX = 12;
constexpr int kOddY = -11;
constexpr int kTile = 46;
constexpr int kWallCell = 23;

/// video.bag images, decoded once each (its few last segments kept, as SpriteCache does).
class Images {
public:
    explicit Images(const std::filesystem::path& gameDir) : bag_(gameDir / "video.bag", gameDir / "video.idx") {}

    // A reference's image index: its own, else looked up by name once (-1 if the bag hasn't it).
    std::int32_t indexOf(const nox::things::ImageRef& ref)
    {
        if (ref.index >= 0) {
            return ref.index;
        }
        auto [it, created] = named_.try_emplace(ref.name, -1);
        if (created) {
            it->second = static_cast<std::int32_t>(bag_.findImage(ref.name));
        }
        return it->second;
    }
    const nox::video::DecodedImage* get(const nox::things::ImageRef& ref) { return get(indexOf(ref)); }
    // Decoded on first use and kept; nullptr if out of range or undecodable (that is remembered too).
    const nox::video::DecodedImage* get(std::int32_t index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= bag_.images().size()) {
            return nullptr;
        }
        auto [it, created] = decoded_.try_emplace(index);
        if (created) {
            try {
                const auto& info = bag_.images()[static_cast<std::size_t>(index)];
                const auto& seg = segment(info.segment);
                if (static_cast<std::size_t>(info.offset) + info.size <= seg.size()) {
                    it->second = nox::video::decodeImage(std::span(seg).subspan(info.offset, info.size), info.type);
                }
            } catch (const std::exception&) {
                // Nothing drawn for it.
            }
        }
        return it->second ? &*it->second : nullptr;
    }

private:
    // The bag's segment `index`, most recently used first; only the last four stay in memory.
    const std::vector<std::uint8_t>& segment(std::uint32_t index)
    {
        for (auto it = segments_.begin(); it != segments_.end(); ++it) {
            if (it->first == index) {
                segments_.splice(segments_.begin(), segments_, it);
                return segments_.front().second;
            }
        }
        segments_.emplace_front(index, bag_.readSegment(index));
        if (segments_.size() > 4) {
            segments_.pop_back();
        }
        return segments_.front().second;
    }

    nox::video::VideoBag bag_;
    std::map<std::string, std::int32_t> named_;
    std::map<std::int32_t, std::optional<nox::video::DecodedImage>> decoded_;
    std::list<std::pair<std::uint32_t, std::vector<std::uint8_t>>> segments_;
};

/// The picture being drawn: premultiplied RGBA floats, one pixel per `k` x `k` world units from
/// world (x0, y0). Each source pixel adds its share (1 / k^2) to the picture pixel it falls in.
struct Canvas {
    int width = 0;
    int height = 0;
    int k = 1;
    int x0 = 0;
    int y0 = 0;
    std::vector<float> px; ///< width * height * 4

    /// The floor: tiles don't overlap, so their shares just add up (no seams where two diamonds
    /// meet inside one picture pixel).
    void add(const std::uint8_t* rgba, int w, int h, int wx, int wy)
    {
        const float share = 1.0f / static_cast<float>(k * k);
        for (int y = 0; y < h; ++y) {
            const int py = floorDiv(wy + y - y0);
            if (py < 0 || py >= height) {
                continue;
            }
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* s = rgba + (static_cast<std::size_t>(y) * w + x) * 4;
                if (s[3] == 0) {
                    continue;
                }
                const int pxx = floorDiv(wx + x - x0);
                if (pxx < 0 || pxx >= width) {
                    continue;
                }
                float* d = &px[(static_cast<std::size_t>(py) * width + pxx) * 4];
                const float a = s[3] / 255.0f * share;
                d[0] += s[0] * a;
                d[1] += s[1] * a;
                d[2] += s[2] * a;
                d[3] += a;
            }
        }
    }

    /// A sprite over what's there: gathered on its own first, then laid over (source over).
    void over(const nox::video::DecodedImage& img, int wx, int wy)
    {
        // The picture pixels the sprite covers; it's gathered into `scratch` at that size first.
        const int pl = std::max(0, floorDiv(wx - x0));
        const int pt = std::max(0, floorDiv(wy - y0));
        const int pr = std::min(width - 1, floorDiv(wx + img.width - 1 - x0));
        const int pb = std::min(height - 1, floorDiv(wy + img.height - 1 - y0));
        if (pr < pl || pb < pt) {
            return;
        }
        const int sw = pr - pl + 1;
        const int sh = pb - pt + 1;
        scratch.assign(static_cast<std::size_t>(sw) * sh * 4, 0.0f);
        const float share = 1.0f / static_cast<float>(k * k);
        for (int y = 0; y < img.height; ++y) {
            const int py = floorDiv(wy + y - y0) - pt;
            if (py < 0 || py >= sh) {
                continue;
            }
            for (int x = 0; x < img.width; ++x) {
                const std::uint8_t* s = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
                if (s[3] == 0) {
                    continue;
                }
                const int pxx = floorDiv(wx + x - x0) - pl;
                if (pxx < 0 || pxx >= sw) {
                    continue;
                }
                float* d = &scratch[(static_cast<std::size_t>(py) * sw + pxx) * 4];
                const float a = s[3] / 255.0f * share;
                d[0] += s[0] * a;
                d[1] += s[1] * a;
                d[2] += s[2] * a;
                d[3] += a;
            }
        }
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                const float* s = &scratch[(static_cast<std::size_t>(y) * sw + x) * 4];
                if (s[3] <= 0) {
                    continue;
                }
                float* d = &px[(static_cast<std::size_t>(pt + y) * width + pl + x) * 4];
                // Source over, premultiplied: what's under shows through by 1 less the gathered alpha.
                const float keep = 1 - std::min(1.0f, s[3]);
                for (int c = 0; c < 4; ++c) {
                    d[c] = s[c] + d[c] * keep;
                }
            }
        }
    }

    // Division rounding toward minus infinity, for points left of or above the canvas's origin.
    int floorDiv(int v) const { return v >= 0 ? v / k : -((-v + k - 1) / k); }

    std::vector<float> scratch;
};

// As MapView has it: a header without details is enabled.
std::uint32_t objectFlags(const nox::map::ObjectHeader& header)
{
    return header.details ? header.details->flags : kObjectEnabled;
}

} // namespace

MapThumbnails::Pixels MapThumbnails::draw(const std::filesystem::path& path, const nox::things::ThingDb& things,
                                          const std::filesystem::path& gameDir, int maxSide)
{
    namespace oc = nox::things::ObjectClass;
    Pixels out;
    nox::map::MapFile map;
    nox::map::FloorMap floor;
    std::vector<nox::map::Wall> walls;
    try {
        map = nox::map::loadMapFile(path);
        floor = nox::map::decodeSection<nox::map::FloorMap>(map);
        walls = nox::map::decodeSection<nox::map::WallMap>(map).walls;
    } catch (const std::exception& e) {
        SDL_Log("map picture %s: %s", path.string().c_str(), e.what());
        return out;
    }
    // Window walls, and secret walls that start open, by cell (x << 8 | y, as MapView::wallKey).
    std::set<std::uint16_t> windows;
    std::set<std::uint16_t> openSecret;
    const auto key = [](int x, int y) { return static_cast<std::uint16_t>((x << 8) | (y & 0xFF)); };
    try {
        const auto win = nox::map::decodeSection<nox::map::WindowWalls>(map);
        for (const auto& p : win.walls) {
            windows.insert(key(p.x, p.y));
        }
        // A version 1 section also resets those walls' variant (as MapView does).
        if (win.version < 2) {
            for (auto& w : walls) {
                if (windows.contains(key(w.x, w.y))) {
                    w.variant = 0;
                }
            }
        }
    } catch (const std::exception&) {
        // No windows.
    }
    try {
        for (const auto& s : nox::map::decodeSection<nox::map::SecretWalls>(map).walls) {
            if (s.flags & 8) {
                openSecret.insert(key(s.pos.x, s.pos.y)); // starts open: not there
            }
        }
    } catch (const std::exception&) {
        // None.
    }

    // The area: the floor's tiles, and the walls' cells.
    int left = std::numeric_limits<int>::max();
    int top = left;
    int right = std::numeric_limits<int>::min();
    int bottom = right;
    const auto grow = [&](int x0, int y0, int x1, int y1) {
        left = std::min(left, x0);
        top = std::min(top, y0);
        right = std::max(right, x1);
        bottom = std::max(bottom, y1);
    };
    // Both of a cell's tiles: from (46x - 11, 46y - 11) to 46 past (46x + 12, 46y + 12).
    for (const auto& p : floor.tiles) {
        grow(p.x * kCell + kOddY, p.y * kCell + kOddY, p.x * kCell + kOddX + kTile, p.y * kCell + kOddX + kTile);
    }
    // Walls' cells, with 40 units above them for their height.
    for (const auto& w : walls) {
        grow(w.x * kWallCell, w.y * kWallCell - 40, w.x * kWallCell + kWallCell, w.y * kWallCell + kWallCell);
    }
    if (right <= left || bottom <= top) {
        return out;
    }
    // A margin, and square, so every map sits the same way in its card.
    const int side = std::max(right - left, bottom - top) + 2 * kCell;
    const int cx = (left + right) / 2;
    const int cy = (top + bottom) / 2;
    Canvas canvas;
    // k world units a picture pixel: the smallest whole number that fits `side` into maxSide.
    canvas.k = std::max(1, (side + maxSide - 1) / maxSide);
    canvas.width = canvas.height = std::max(1, side / canvas.k);
    canvas.x0 = cx - canvas.width * canvas.k / 2;
    canvas.y0 = cy - canvas.height * canvas.k / 2;
    canvas.px.assign(static_cast<std::size_t>(canvas.width) * canvas.height * 4, 0.0f);

    Images images(gameDir);
    // A floor variant's image, only when it's a whole 46 x 46 tile.
    const auto floorImage = [&](std::uint8_t f, std::int16_t variant) -> const nox::video::DecodedImage* {
        if (f >= things.floors.size() || variant < 0 || static_cast<std::size_t>(variant) >= things.floors[f].images.size()) {
            return nullptr;
        }
        const auto* img = images.get(things.floors[f].images[static_cast<std::size_t>(variant)]);
        return img && img->width == kTile && img->height == kTile && img->rgba.size() == kTile * kTile * 4 ? img : nullptr;
    };

    // The floor, each tile with its edges composited over it (MapView::edgedTile).
    std::vector<std::uint8_t> tile(static_cast<std::size_t>(kTile) * kTile * 4);
    const auto addTile = [&](int x, int y, const nox::map::FloorTile& t) {
        const auto* base = floorImage(t.image, t.variant);
        if (!base) {
            return;
        }
        if (t.edges.empty()) {
            canvas.add(base->rgba.data(), kTile, kTile, x, y);
            return;
        }
        tile = base->rgba;
        for (const auto& e : t.edges) {
            if (e.edge >= things.edges.size() || e.direction >= things.edges[e.edge].images.size()) {
                continue;
            }
            const auto* mask = images.get(things.edges[e.edge].images[e.direction]);
            if (!mask || mask->edgeMask.size() != static_cast<std::size_t>(kTile) * kTile) {
                continue;
            }
            const auto* blend = floorImage(e.floor, e.variant);
            // Edge mask 1: the edge image's own pixel; 2: the neighbouring floor's; else the tile's stays.
            for (std::size_t i = 0; i < mask->edgeMask.size(); ++i) {
                const std::uint8_t* src = mask->edgeMask[i] == 1 ? &mask->rgba[i * 4]
                                          : mask->edgeMask[i] == 2 && blend ? &blend->rgba[i * 4]
                                                                            : nullptr;
                if (src) {
                    std::copy(src, src + 4, tile.begin() + static_cast<std::ptrdiff_t>(i * 4));
                }
            }
        }
        canvas.add(tile.data(), kTile, kTile, x, y);
    };
    for (const auto& p : floor.readPairs()) { // (as the game reads a pair stored twice)
        if (p.odd) {
            addTile(p.x * kCell + kOddX, p.y * kCell + kOddY, *p.odd);
        }
        if (p.even) {
            addTile(p.x * kCell + kOddY, p.y * kCell + kOddX, *p.even);
        }
    }

    // Walls and objects, back to front by y (no eye: every wall its back image, solid).
    struct Item {
        float sortY = 0;
        const nox::video::DecodedImage* image = nullptr;
        int x = 0; ///< the image's top-left, world units
        int y = 0;
    };
    std::vector<Item> items;
    for (const auto& w : walls) {
        if (openSecret.contains(key(w.x, w.y)) || w.material >= things.walls.size()) {
            continue;
        }
        const auto& dirs = things.walls[w.material].directions;
        std::size_t dir = w.direction & 0x7F;
        std::size_t variant = w.variant;
        if (windows.contains(key(w.x, w.y)) && dir <= 1) {
            dir += 11;  // directions 11 and 12: the windowed pieces of 0 and 1
            if (dir < dirs.size() && variant >= dirs[dir].size()) {
                variant = 0;
            }
        }
        if (dir >= dirs.size() || variant >= dirs[dir].size()) {
            continue;
        }
        const auto& image = dirs[dir][variant][0];
        if (const auto* img = images.get(image.image)) {
            // Sorted at the cell's centre; placed as MapView places walls, plus the image's own offset.
            items.push_back({static_cast<float>(w.y * kWallCell + 11), img,
                             w.x * kWallCell - 50 + image.x + img->offsetX, w.y * kWallCell - 72 - image.y + img->offsetY});
        }
    }
    try {
        ThingArt art(things);
        const auto toc = nox::map::decodeSection<nox::map::ObjectTOC>(map);
        std::map<std::uint16_t, std::string_view> typeNames;
        for (const auto& e : toc.entries) {
            typeNames[e.id] = e.name;
        }
        const auto records = nox::map::decodeSection<nox::map::ObjectData>(map).records;
        std::vector<nox::map::ObjectXfer> xfers;
        xfers.reserve(records.size());
        for (const auto& r : records) {
            xfers.push_back(nox::map::ObjectXfer::parse(r.data));
        }
        // Only what the client loads from the map itself (CLIENT_PERSIST or IMMOBILE); not pressure plates.
        constexpr std::uint32_t kClientLoaded = oc::ClientPersist | oc::Immobile;
        for (const auto& root : nox::map::buildObjectTree(xfers)) {
            const auto name = typeNames.find(records[root.record].tocId);
            if (name == typeNames.end()) {
                continue;
            }
            const TypeArt* a = art.get(name->second);
            if (!a || !a->still || a->editorOnly || !(a->classes & kClientLoaded) || a->drawFn == DrawFn::PressurePlate) {
                continue;
            }
            const auto& x = xfers[root.record];
            if ((a->classes & oc::Fire) && !(objectFlags(x.header) & kObjectEnabled)) {
                continue; // (an unlit flame)
            }
            nox::things::ImageRef ref = *a->still;
            if (a->xfer == "DoorXfer" && !a->draw.images.empty()) {
                try {
                    const auto data = nox::map::xfer::decode(a->xfer, x.xferVersion, x.extra, {a->classes, a->subclasses},
                                                             x.extraOffset());
                    if (const auto* door = std::get_if<nox::map::xfer::Door>(&data)) {
                        ref = a->draw.images[(door->currentDirection & 31) % a->draw.images.size()];  // its starting angle (0-31)
                    }
                } catch (const std::exception&) {
                    // Its still.
                }
            }
            const auto* img = images.get(ref);
            if (!img) {
                continue;
            }
            // As MapView sorts them: wall hangings first of all, BELOW things under the rest.
            const float sortY = a->hanging ? std::numeric_limits<float>::lowest() : a->below ? x.header.y - 100000.0f : x.header.y;
            items.push_back({sortY, img, static_cast<int>(std::lround(x.header.x - a->halfWidth)) + img->offsetX,
                             static_cast<int>(std::lround(x.header.y - a->halfHeight - a->z)) + img->offsetY});
        }
    } catch (const std::exception&) {
        // The floor and walls alone.
    }
    // Back to front; the stable sort keeps walls before objects at the same y.
    std::ranges::stable_sort(items, {}, &Item::sortY);
    for (const auto& item : items) {
        if (item.image->rgba.size() == static_cast<std::size_t>(item.image->width) * item.image->height * 4) {
            canvas.over(*item.image, item.x, item.y);
        }
    }

    // Straight alpha, on black where nothing is (the map's void).
    out.width = canvas.width;
    out.height = canvas.height;
    out.worldWidth = static_cast<float>(canvas.width * canvas.k);
    out.worldHeight = static_cast<float>(canvas.height * canvas.k);
    out.rgba.resize(canvas.px.size());
    // Premultiplied colour over black is the colour itself; every pixel opaque.
    for (std::size_t i = 0; i < canvas.px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            out.rgba[i + c] = static_cast<std::uint8_t>(std::clamp(canvas.px[i + c] + 0.5f, 0.0f, 255.0f));
        }
        out.rgba[i + 3] = 255;
    }
    return out;
}

MapThumbnails::MapThumbnails(std::filesystem::path gameDir, std::filesystem::path mapCache,
                             const nox::things::ThingDb& things, int maxSide)
    : gameDir_(std::move(gameDir)), mapCache_(std::move(mapCache)), things_(things), maxSide_(maxSide),
      worker_([this] { run(); })
{
}

MapThumbnails::~MapThumbnails()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    worker_.join();
    clear();
}

std::filesystem::path MapThumbnails::find(const std::string& name) const
{
    // A name is a server's: a folder's name, never a path.
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
        return {};
    }
    std::error_code ec;
    for (const auto& dir : {gameDir_ / "maps", mapCache_}) {
        const auto path = dir / name / (name + ".map");
        if (!dir.empty() && std::filesystem::exists(nox::resolveNoCase(path), ec)) { // (in any case)
            return path;
        }
    }
    return {};
}

// The worker: draws the queued names one at a time and leaves the pixels for get() to upload.
void MapThumbnails::run()
{
    for (;;) {
        std::string name;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            name = std::move(queue_.front());
            queue_.pop_front();
        }
        Pixels pixels;
        if (const auto path = find(name); !path.empty()) {
            const std::uint64_t start = SDL_GetTicks();
            try {
                pixels = draw(path, things_, gameDir_, maxSide_);
            } catch (const std::exception& e) {
                // (video.bag's reader, or anything else draw doesn't catch: none drawn, the game
                // runs on; out of the thread it would end the program)
                SDL_Log("map picture %s: %s", name.c_str(), e.what());
                pixels = {};
            }
            SDL_Log("map picture %s: %dx%d in %llu ms", name.c_str(), pixels.width, pixels.height,
                    static_cast<unsigned long long>(SDL_GetTicks() - start));
        }
        std::lock_guard lock(mutex_);
        // Kept for get() only if still asked for: one whose ask a clear() dropped while it was being
        // drawn is let go here (nothing would ever take it from drawn_).
        if (asked_.erase(name)) {
            drawn_[name] = std::move(pixels);
        }
    }
}

const MapThumbnails::Picture& MapThumbnails::get(SDL_Renderer* renderer, const std::string& name)
{
    auto [it, created] = pictures_.try_emplace(name);
    Picture& picture = it->second;
    // The first ask queues it; later asks upload it once the worker has drawn it.
    if (created) {
        std::lock_guard lock(mutex_);
        if (asked_.insert(name).second) { // (else asked for already: once is enough)
            queue_.push_back(name);
            wake_.notify_one();
        }
        return picture;
    }
    if (picture.done) {
        return picture;
    }
    std::optional<Pixels> pixels;
    {
        std::lock_guard lock(mutex_);
        if (const auto d = drawn_.find(name); d != drawn_.end()) {
            pixels = std::move(d->second);
            drawn_.erase(d);
        }
    }
    if (pixels) {
        picture.done = true;
        if (pixels->width > 0) {
            picture.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pixels->width,
                                                pixels->height);
            if (picture.texture) {
                SDL_UpdateTexture(picture.texture, nullptr, pixels->rgba.data(), pixels->width * 4);
                SDL_SetTextureScaleMode(picture.texture, SDL_SCALEMODE_LINEAR);
            }
            picture.worldWidth = pixels->worldWidth;
            picture.worldHeight = pixels->worldHeight;
        }
    }
    return picture;
}

void MapThumbnails::clear()
{
    for (auto& [name, picture] : pictures_) {
        if (picture.texture) {
            SDL_DestroyTexture(picture.texture);
        }
    }
    pictures_.clear();
    std::lock_guard lock(mutex_);
    queue_.clear();
    asked_.clear(); // (one being drawn now is let go as it's done, unless asked for again by then)
    drawn_.clear();
}

} // namespace noxsdl
