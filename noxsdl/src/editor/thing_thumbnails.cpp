// The content browser's thumbnails (thing_thumbnails.hpp): drawn on a worker, cached on disk,
// uploaded into atlas pages.
#include "editor/thing_thumbnails.hpp"
#include "editor/panel_textures.hpp"

#include "image_decoder.hpp"
#include "thing_art.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/mapedit/content.hpp"
#include "noxcore/video/image_pack.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <list>
#include <optional>

namespace fs = std::filesystem;

namespace noxsdl::editor {

/// The worker's images: the HD pack's when one is given and has the image, else video.bag's.
class ThumbSource {
public:
    ThumbSource(const fs::path& gameDir, const fs::path& pack)
        : bag_(gameDir / "video.bag", gameDir / "video.idx")
    {
        if (!pack.empty()) {
            try {
                pack_.emplace(pack);
            } catch (const std::exception& ex) {
                SDL_Log("map editor: thumbnails without the HD pack (%s)", ex.what());
            }
        }
    }

    struct Image {
        int width = 0; ///< pixels
        int height = 0;
        const std::uint8_t* rgba = nullptr;
        const std::uint8_t* material = nullptr; ///< one byte a pixel, or null (no material pixels)
        float offsetX = 0; ///< the draw offset, in original pixels
        float offsetY = 0;
        float scale = 1;   ///< pixels an original pixel (an upscaled image's)
    };

    std::optional<Image> get(const nox::things::ImageRef& ref)
    {
        const std::int32_t index = ref.index >= 0 ? ref.index : static_cast<std::int32_t>(bag_.findImage(ref.name));
        if (index < 0 || static_cast<std::size_t>(index) >= bag_.images().size()) return std::nullopt;
        if (pack_ && static_cast<std::uint32_t>(index) < pack_->size()) {
            try {
                if (auto p = pack_->read(static_cast<std::size_t>(index))) {
                    if (p->pixelWidth > 0 && p->pixelHeight > 0 && p->rgba.size() == static_cast<std::size_t>(p->pixelWidth) * p->pixelHeight * 4) {
                        last_ = std::move(*p);
                        const std::size_t n = static_cast<std::size_t>(last_.pixelWidth) * last_.pixelHeight;
                        return Image{last_.pixelWidth, last_.pixelHeight, last_.rgba.data(),
                                     last_.material.size() == n ? last_.material.data() : nullptr,
                                     static_cast<float>(last_.offsetX), static_cast<float>(last_.offsetY),
                                     last_.width > 0 ? static_cast<float>(last_.pixelWidth) / static_cast<float>(last_.width) : 1.0f};
                    }
                }
            } catch (const std::exception&) {
                // (video.bag's then)
            }
        }
        try {
            const auto& info = bag_.images()[static_cast<std::size_t>(index)];
            const auto& seg = segment(info.segment);
            if (static_cast<std::size_t>(info.offset) + info.size > seg.size()) return std::nullopt;
            decoded_ = nox::video::decodeImage(std::span(seg).subspan(info.offset, info.size), info.type);
            if (decoded_.width <= 0 || decoded_.height <= 0) return std::nullopt;
            const std::size_t n = static_cast<std::size_t>(decoded_.width) * decoded_.height;
            return Image{decoded_.width, decoded_.height, decoded_.rgba.data(), decoded_.material.size() == n ? decoded_.material.data() : nullptr,
                         static_cast<float>(decoded_.offsetX), static_cast<float>(decoded_.offsetY), 1.0f};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    ThumbSource(const nox::things::ThingDb& things, const fs::path& gameDir, const fs::path& pack)
        : ThumbSource(gameDir, pack)
    {
        art_.emplace(things);
    }
    ThingArt& thingArt() { return *art_; }

private:
    const std::vector<std::uint8_t>& segment(std::uint32_t index)
    {
        for (auto it = segments_.begin(); it != segments_.end(); ++it) {
            if (it->first == index) {
                segments_.splice(segments_.begin(), segments_, it);
                return segments_.front().second;
            }
        }
        segments_.emplace_front(index, bag_.readSegment(index));
        if (segments_.size() > 4) segments_.pop_back();
        return segments_.front().second;
    }

    nox::video::VideoBag bag_;
    std::optional<nox::video::ImagePackReader> pack_;
    std::optional<ThingArt> art_;
    nox::video::PackImage last_;
    nox::video::DecodedImage decoded_;
    std::list<std::pair<std::uint32_t, std::vector<std::uint8_t>>> segments_;
};

namespace {

/// The image box-filtered (area-averaged, premultiplied) to fit kSize, centred in the square.
ThingThumbnails::Pixels fit(const ThumbSource::Image& img)
{
    constexpr int S = ThingThumbnails::kSize;
    // The opaque part only (sprites carry empty margins), so small things fill the cell.
    int x0 = img.width, y0 = img.height, x1 = -1, y1 = -1;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            if (img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3] > 8) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    ThingThumbnails::Pixels out;
    if (x1 < x0) return out;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    // Down to fit; small ones up by whole steps (crisp), at most 2x.
    float k = std::min(static_cast<float>(S) / static_cast<float>(w), static_cast<float>(S) / static_cast<float>(h));
    if (k > 1) k = std::min(2.0f, std::floor(k));
    const int ow = std::max(1, static_cast<int>(static_cast<float>(w) * k));
    const int oh = std::max(1, static_cast<int>(static_cast<float>(h) * k));
    const int ox = (S - ow) / 2, oy = (S - oh) / 2;
    std::vector<float> acc(static_cast<std::size_t>(S) * S * 4, 0.0f);
    std::vector<float> area(static_cast<std::size_t>(S) * S, 0.0f);
    if (k >= 1) {
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                const int sx = x0 + std::min(w - 1, static_cast<int>(static_cast<float>(x) / k));
                const int sy = y0 + std::min(h - 1, static_cast<int>(static_cast<float>(y) / k));
                const std::uint8_t* s = img.rgba + (static_cast<std::size_t>(sy) * img.width + sx) * 4;
                const std::size_t d = (static_cast<std::size_t>(oy + y) * S + ox + x);
                const float a = s[3] / 255.0f;
                acc[d * 4 + 0] = s[0] * a;
                acc[d * 4 + 1] = s[1] * a;
                acc[d * 4 + 2] = s[2] * a;
                acc[d * 4 + 3] = a;
                area[d] = 1;
            }
        }
    } else {
        for (int y = 0; y < h; ++y) {
            const int dy = std::min(oh - 1, static_cast<int>(static_cast<float>(y) * k));
            for (int x = 0; x < w; ++x) {
                const int dx = std::min(ow - 1, static_cast<int>(static_cast<float>(x) * k));
                const std::uint8_t* s = img.rgba + (static_cast<std::size_t>(y0 + y) * img.width + x0 + x) * 4;
                const std::size_t d = (static_cast<std::size_t>(oy + dy) * S + ox + dx);
                const float a = s[3] / 255.0f;
                acc[d * 4 + 0] += s[0] * a;
                acc[d * 4 + 1] += s[1] * a;
                acc[d * 4 + 2] += s[2] * a;
                acc[d * 4 + 3] += a;
                area[d] += 1;
            }
        }
    }
    out.rgba.assign(static_cast<std::size_t>(S) * S * 4, 0);
    for (std::size_t i = 0; i < area.size(); ++i) {
        if (area[i] <= 0 || acc[i * 4 + 3] <= 0) continue;
        const float a = acc[i * 4 + 3];
        for (int c = 0; c < 3; ++c) out.rgba[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(acc[i * 4 + c] / a, 0.0f, 255.0f));
        out.rgba[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(a / area[i] * 255.0f, 0.0f, 255.0f));
    }
    return out;
}

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t n)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::uint64_t fileStamp(std::uint64_t h, const fs::path& p)
{
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    const auto when = fs::last_write_time(p, ec).time_since_epoch().count();
    const std::string s = p.filename().string();
    h = fnv(h, s.data(), s.size());
    h = fnv(h, &size, sizeof size);
    return fnv(h, &when, sizeof when);
}

} // namespace

namespace {

/// The look's layers drawn over each other as the scene draws them (each at its own draw offset
/// from the object's origin), each layer's material pixels shaded by its colours first, then
/// fitted (fit). Empty if none of the images could be read.
ThingThumbnails::Pixels composite(const std::vector<LookLayer>& layers, ThumbSource& source)
{
    struct Layer {
        int w = 0, h = 0;
        float ox = 0, oy = 0, scale = 1;
        std::vector<std::uint8_t> rgba;
    };
    std::vector<Layer> ls;
    for (const auto& layer : layers) {
        const auto img = source.get(layer.image);
        if (!img) continue;
        Layer l{img->width, img->height, img->offsetX, img->offsetY, img->scale > 0 ? img->scale : 1.0f, {}};
        const std::size_t n = static_cast<std::size_t>(l.w) * l.h;
        l.rgba.assign(img->rgba, img->rgba + n * 4);
        if (layer.colours && img->material) shadeMaterials(l.rgba.data(), img->material, n, *layer.colours);
        ls.push_back(std::move(l));
    }
    if (ls.empty()) return {};
    if (ls.size() == 1) return fit({ls[0].w, ls[0].h, ls[0].rgba.data()});
    // On a canvas at the finest layer's scale, each layer "over" the ones before it.
    float s = 1, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& l : ls) {
        s = std::max(s, l.scale);
        x0 = std::min(x0, l.ox);
        y0 = std::min(y0, l.oy);
        x1 = std::max(x1, l.ox + static_cast<float>(l.w) / l.scale);
        y1 = std::max(y1, l.oy + static_cast<float>(l.h) / l.scale);
    }
    const int cw = std::max(1, static_cast<int>(std::ceil((x1 - x0) * s)));
    const int ch = std::max(1, static_cast<int>(std::ceil((y1 - y0) * s)));
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(cw) * ch * 4, 0);
    for (const auto& l : ls) {
        for (int cy = 0; cy < ch; ++cy) {
            const int py = static_cast<int>(std::floor(((static_cast<float>(cy) + 0.5f) / s + y0 - l.oy) * l.scale));
            if (py < 0 || py >= l.h) continue;
            for (int cx = 0; cx < cw; ++cx) {
                const int px = static_cast<int>(std::floor(((static_cast<float>(cx) + 0.5f) / s + x0 - l.ox) * l.scale));
                if (px < 0 || px >= l.w) continue;
                const std::uint8_t* src = &l.rgba[(static_cast<std::size_t>(py) * l.w + px) * 4];
                if (src[3] == 0) continue;
                std::uint8_t* dst = &canvas[(static_cast<std::size_t>(cy) * cw + cx) * 4];
                const float sa = src[3] / 255.0f, da = dst[3] / 255.0f;
                const float oa = sa + da * (1 - sa);
                for (int c = 0; c < 3; ++c) {
                    dst[c] = static_cast<std::uint8_t>(std::clamp((src[c] * sa + dst[c] * da * (1 - sa)) / oa, 0.0f, 255.0f));
                }
                dst[3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f));
            }
        }
    }
    return fit({cw, ch, canvas.data()});
}

} // namespace

ThingThumbnails::Pixels ThingThumbnails::draw(const nox::things::ThingDb& things, std::size_t thing, ThumbSource& source, const RecordLook* look,
                                              const nox::things::ModifierDb* modifiers)
{
    if (thing >= things.things.size()) return {};
    const auto& def = things.things[thing];
    const TypeArt* art = source.thingArt().get(def.name);
    std::optional<nox::things::ImageRef> ref;
    // Items: their menu icon (in its colours); creatures and scenery: their still.
    const bool item = art && art->itemColours != ItemColours::None;
    if (art && art->still && !item) ref = art->still;
    if (!ref && def.menuIcon && (def.menuIcon->index >= 0 || !def.menuIcon->name.empty())) ref = def.menuIcon;
    if (!ref && art && art->still) ref = art->still;
    if (art && look && modifiers && look->kind != RecordLook::Kind::Plain) {
        // In the colours the game draws a new one in: an NPC's layers (facing south, standing), a
        // maiden's still, an item's icon (as the inventory draws it).
        const auto image = look->kind == RecordLook::Kind::Maiden && art->still ? art->still : ref;
        if (auto p = composite(lookLayers(*art, *look, image, source.thingArt(), *modifiers), source); !p.rgba.empty()) return p;
    }
    if (!ref) return {};
    const auto img = source.get(*ref);
    if (!img) return {};
    return fit(*img);
}

ThingThumbnails::ThingThumbnails(const nox::things::ThingDb& things, fs::path gameDir, fs::path imagePack, fs::path cacheDir,
                                 const nox::things::ModifierDb* modifiers, std::map<std::size_t, RecordLook> looks)
    : things_(things)
    , gameDir_(std::move(gameDir))
    , imagePack_(std::move(imagePack))
    , cacheDir_(std::move(cacheDir))
    , modifiers_(modifiers)
    , looks_(modifiers ? std::move(looks) : std::map<std::size_t, RecordLook>{})
{
    // Every placeable type at the back of the line: drawn once the shown ones are.
    for (std::size_t i = 0; i < things_.things.size(); ++i) {
        if (nox::mapedit::placeableType(things_.things[i])) {
            queue_.push_back(i);
            queued_.insert(i);
        }
    }
    worker_ = std::thread([this] { run(); });
}

ThingThumbnails::~ThingThumbnails()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (SDL_Texture* t : pages_) {
        if (t) {
            panelTextureForget(t);
            SDL_DestroyTexture(t);
        }
    }
}

std::uint64_t ThingThumbnails::cacheKey() const
{
    std::uint64_t h = 1469598103934665603ull;
    const std::uint32_t v[2] = {kCacheVersion, static_cast<std::uint32_t>(kSize)};
    h = fnv(h, v, sizeof v);
    h = fileStamp(h, gameDir_ / "thing.bin");
    h = fileStamp(h, gameDir_ / "video.idx");
    if (!imagePack_.empty()) h = fileStamp(h, imagePack_);
    // The installed content packs' thing.patch files change the types as loaded (none: the key as it was).
    for (const fs::path& dir : nox::content::contentPackDirs()) {
        if (const fs::path patch = nox::findNoCase(dir, "thing.patch"); fs::exists(patch)) {
            h = fileStamp(h, patch);
        }
    }
    return h;
}

std::uint32_t ThingThumbnails::lookHash(std::size_t thing) const
{
    const auto it = looks_.find(thing);
    return it == looks_.end() ? 0u : it->second.hash();
}

fs::path ThingThumbnails::cacheFile() const
{
    char name[40];
    std::snprintf(name, sizeof name, "thumbs-%016llx.bin", static_cast<unsigned long long>(cacheKey()));
    return cacheDir_ / "thumbs" / name;
}

void ThingThumbnails::loadCache()
{
    if (cacheDir_.empty()) return;
    std::ifstream in(cacheFile(), std::ios::binary);
    if (!in) return;
    const auto u32 = [&] {
        std::uint32_t v = 0;
        in.read(reinterpret_cast<char*>(&v), 4);
        return v;
    };
    if (u32() != 0x4854584Eu /* NXTH */ || u32() != kCacheVersion || u32() != static_cast<std::uint32_t>(kSize)) return;
    const std::uint32_t count = u32();
    std::map<std::size_t, Pixels> read;
    constexpr std::size_t n = static_cast<std::size_t>(kSize) * kSize;
    std::set<std::size_t> stale; // (drawn in other colours than the type's look now: drawn again)
    for (std::uint32_t i = 0; i < count && in; ++i) {
        const std::uint32_t thing = u32();
        if (u32() != lookHash(thing)) stale.insert(thing);
        const std::uint32_t runs = u32();
        Pixels p;
        if (runs > 0) {
            // Runs: u16 transparent pixels, u16 opaque pixels, then the opaque ones' RGBA.
            p.rgba.assign(n * 4, 0);
            std::size_t at = 0;
            for (std::uint32_t r = 0; r < runs && in; ++r) {
                std::uint16_t skip = 0, take = 0;
                in.read(reinterpret_cast<char*>(&skip), 2);
                in.read(reinterpret_cast<char*>(&take), 2);
                at += skip;
                if (at + take > n) {
                    in.setstate(std::ios::failbit);
                    break;
                }
                in.read(reinterpret_cast<char*>(&p.rgba[at * 4]), static_cast<std::streamsize>(take) * 4);
                at += take;
            }
        }
        read[thing] = std::move(p);
    }
    if (!in) return;
    std::lock_guard lock(mutex_);
    for (auto& [thing, p] : read) {
        if (thing >= things_.things.size() || stale.contains(thing)) continue;
        if (queued_.erase(thing)) std::erase(queue_, thing);
        all_[thing] = std::move(p);
    }
}

void ThingThumbnails::saveCache()
{
    if (cacheDir_.empty()) return;
    std::map<std::size_t, Pixels> all;
    {
        std::lock_guard lock(mutex_);
        if (!dirty_) return;
        dirty_ = false;
        all = all_;
    }
    std::error_code ec;
    fs::create_directories(cacheDir_ / "thumbs", ec);
    const fs::path file = cacheFile();
    // (Older caches, made for other files, go.)
    for (const auto& e : fs::directory_iterator(cacheDir_ / "thumbs", ec)) {
        if (e.path().filename().string().starts_with("thumbs-") && e.path() != file) fs::remove(e.path(), ec);
    }
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        u32(0x4854584Eu);
        u32(kCacheVersion);
        u32(static_cast<std::uint32_t>(kSize));
        u32(static_cast<std::uint32_t>(all.size()));
        constexpr std::size_t n = static_cast<std::size_t>(kSize) * kSize;
        for (const auto& [thing, p] : all) {
            u32(static_cast<std::uint32_t>(thing));
            u32(lookHash(thing)); // (the colours it was drawn in)
            std::vector<std::pair<std::uint16_t, std::uint16_t>> runs;
            if (!p.rgba.empty()) {
                std::size_t i = 0;
                while (i < n) {
                    std::size_t skip = 0;
                    while (i < n && p.rgba[i * 4 + 3] == 0) ++i, ++skip;
                    std::size_t take = 0;
                    while (i + take < n && p.rgba[(i + take) * 4 + 3] != 0) ++take;
                    if (skip == 0 && take == 0) break;
                    runs.emplace_back(static_cast<std::uint16_t>(skip), static_cast<std::uint16_t>(take));
                    i += take;
                }
            }
            u32(static_cast<std::uint32_t>(runs.size()));
            std::size_t at = 0;
            for (const auto& [skip, take] : runs) {
                out.write(reinterpret_cast<const char*>(&skip), 2);
                out.write(reinterpret_cast<const char*>(&take), 2);
                at += skip;
                out.write(reinterpret_cast<const char*>(&p.rgba[at * 4]), static_cast<std::streamsize>(take) * 4);
                at += take;
            }
        }
        if (!out) return;
    }
    fs::rename(tmp, file, ec);
}

void ThingThumbnails::run()
{
    loadCache();
    {
        std::lock_guard lock(mutex_);
        loaded_ = true;
    }
    std::optional<ThumbSource> source;
    try {
        source.emplace(things_, gameDir_, imagePack_);
    } catch (const std::exception& ex) {
        SDL_Log("map editor: no thumbnails (%s)", ex.what());
        return;
    }
    for (;;) {
        std::size_t thing = 0;
        {
            std::unique_lock lock(mutex_);
            if (queue_.empty() && dirty_) {
                lock.unlock();
                saveCache(); // (idle: the cache written once everything asked for is drawn)
                lock.lock();
            }
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            thing = queue_.front();
            queue_.pop_front();
            queued_.erase(thing);
        }
        Pixels p;
        try {
            const auto look = looks_.find(thing);
            p = draw(things_, thing, *source, look == looks_.end() ? nullptr : &look->second, modifiers_);
        } catch (const std::exception&) {
        }
        std::lock_guard lock(mutex_);
        all_[thing] = std::move(p); // (up when the browser next shows it)
        dirty_ = true;
    }
    saveCache();
}

ThingThumbnails::Cell ThingThumbnails::get(std::size_t thing)
{
    Cell c;
    const auto it = slots_.find(thing);
    if (it != slots_.end()) {
        if (it->second.none) {
            c.none = true;
            return c;
        }
        constexpr int perRow = kPage / kSize;
        const int col = it->second.index % perRow, row = it->second.index / perRow;
        c.texture = pages_[static_cast<std::size_t>(it->second.page)];
        const float k = 1.0f / static_cast<float>(kPage);
        c.u0 = static_cast<float>(col * kSize) * k;
        c.v0 = static_cast<float>(row * kSize) * k;
        c.u1 = static_cast<float>(col * kSize + kSize) * k;
        c.v1 = static_cast<float>(row * kSize + kSize) * k;
        return c;
    }
    // Not up yet: made already (drawn or from the disk cache), it goes up next; else it's asked
    // for first (what the browser shows goes before the rest).
    std::lock_guard lock(mutex_);
    if (drawn_.contains(thing)) return c;
    if (const auto a = all_.find(thing); a != all_.end()) {
        drawn_[thing] = a->second;
        return c;
    }
    if (queue_.empty() || queue_.front() != thing) {
        if (queued_.contains(thing)) std::erase(queue_, thing);
        queue_.push_front(thing);
        queued_.insert(thing);
        wake_.notify_one();
    }
    return c;
}

void ThingThumbnails::pump(SDL_Renderer* renderer, int budget)
{
    std::vector<std::pair<std::size_t, Pixels>> ready;
    {
        std::lock_guard lock(mutex_);
        while (!drawn_.empty() && static_cast<int>(ready.size()) < budget) {
            auto node = drawn_.extract(drawn_.begin());
            ready.emplace_back(node.key(), std::move(node.mapped()));
        }
    }
    constexpr int perPage = (kPage / kSize) * (kPage / kSize);
    for (auto& [thing, p] : ready) {
        if (slots_.contains(thing)) continue;
        if (p.rgba.empty()) {
            slots_[thing] = {-1, 0, true};
            continue;
        }
        const int page = used_ / perPage, index = used_ % perPage;
        if (page >= static_cast<int>(pages_.size())) {
            SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kPage, kPage);
            if (!t) return;
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            std::vector<std::uint8_t> clear(static_cast<std::size_t>(kPage) * kPage * 4, 0);
            SDL_UpdateTexture(t, nullptr, clear.data(), kPage * 4);
            panelTextureSet(t, clear.data(), kPage, kPage, kPage * 4); // (kept for a floating panel's copy)
            pages_.push_back(t);
        }
        constexpr int perRow = kPage / kSize;
        const SDL_Rect r{(index % perRow) * kSize, (index / perRow) * kSize, kSize, kSize};
        SDL_UpdateTexture(pages_[static_cast<std::size_t>(page)], &r, p.rgba.data(), kSize * 4);
        panelTextureUpdate(pages_[static_cast<std::size_t>(page)], r, p.rgba.data(), kSize * 4);
        slots_[thing] = {page, index, false};
        ++used_;
    }
}

std::size_t ThingThumbnails::pending()
{
    std::lock_guard lock(mutex_);
    return queue_.size() + (loaded_ ? 0 : 1);
}

} // namespace noxsdl::editor
