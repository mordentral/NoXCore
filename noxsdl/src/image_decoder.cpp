// ImageDecoder (image_decoder.hpp): the workers, their two queues, and the CPU side of an HD
// image's textures (its levels, tinted or in one colour).
#include "image_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <memory>
#include <utility>

namespace noxsdl {

namespace {

constexpr int kMaxMips = 4;    // down to 1/16: a 4x image drawn at a quarter of its logical size
constexpr int kMinMipSide = 8; // no level smaller than this
// Finished decodes the drawing thread hasn't taken yet: past this the workers leave the preload
// list alone (what's asked for while drawing still goes ahead), so a map's preload can't pile up
// its whole size in memory waiting for the uploads.
constexpr std::size_t kMaxWaitingBytes = std::size_t{96} << 20;

/// Half the size, each pixel the average of its 2x2 block with alpha weighting (premultiplied),
/// so transparent pixels don't darken the edges.
std::vector<std::uint8_t> halve(const std::vector<std::uint8_t>& rgba, int w, int h, int& ow, int& oh)
{
    ow = (w + 1) / 2;
    oh = (h + 1) / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh) * 4);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            unsigned sum[3] = {0, 0, 0};
            unsigned alpha = 0;
            unsigned count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = 2 * x + dx;
                    const int sy = 2 * y + dy;
                    if (sx >= w || sy >= h) {
                        continue;
                    }
                    const auto* p = &rgba[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) +
                                           static_cast<std::size_t>(sx)) * 4];
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += p[c] * unsigned{p[3]};
                    }
                    alpha += p[3];
                    ++count;
                }
            }
            auto* o = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)) * 4];
            for (int c = 0; c < 3; ++c) {
                o[c] = alpha ? static_cast<std::uint8_t>((sum[c] + alpha / 2) / alpha) : 0;
            }
            o[3] = static_cast<std::uint8_t>((alpha + count / 2) / count);
        }
    }
    return out;
}

/// Half the size of a material plane: its slots are labels, not intensities, so each block takes
/// its first pixel's.
std::vector<std::uint8_t> halveLabels(const std::vector<std::uint8_t>& labels, int w, int h)
{
    const int ow = (w + 1) / 2;
    const int oh = (h + 1) / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh));
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)] =
                labels[static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(2 * x)];
        }
    }
    return out;
}

TextureLevel rgbaLevel(std::vector<std::uint8_t> bytes, int w, int h)
{
    return TextureLevel{SDL_PIXELFORMAT_RGBA32, w, h, w * 4, std::move(bytes)};
}

std::size_t resultBytes(const DecodeResult& r)
{
    std::size_t n = 0;
    for (const auto& l : r.levels) {
        n += l.bytes.size();
    }
    for (const auto& l : r.labels) {
        n += l.size();
    }
    if (r.pixels) {
        n += r.pixels->rgba.size() + r.pixels->material.size() + r.pixels->edgeMask.size();
    }
    return n;
}

} // namespace

void shadeMaterials(std::uint8_t* rgba, const std::uint8_t* material, std::size_t pixels, const MaterialColours& colours)
{
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t m = material[i];
        if (m == 0) {
            continue;
        }
        const auto& c = colours[std::min<std::size_t>(m - 1u, colours.size() - 1)];
        for (std::size_t ch = 0; ch < 3; ++ch) {
            auto& px = rgba[i * 4 + ch];
            px = static_cast<std::uint8_t>((px * c[ch]) >> 8);
        }
    }
}

std::vector<std::uint8_t> besideMaterial(const std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& material,
                                         int w, int h)
{
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(w) * 2 * static_cast<std::size_t>(h) * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t from = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            const std::size_t left = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 2 + static_cast<std::size_t>(x)) * 4;
            const std::size_t right = left + static_cast<std::size_t>(w) * 4;
            std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(from * 4), 4, packed.begin() + static_cast<std::ptrdiff_t>(left));
            packed[right] = material[from];
            packed[right + 3] = 255;
        }
    }
    return packed;
}

DecodeResult buildDecodeResult(const DecodeJob& job, std::optional<nox::video::PackImage> image)
{
    DecodeResult r;
    r.job = job;
    if (!image) {
        return r;
    }
    nox::video::PackImage& p = *image;
    r.width = static_cast<float>(p.width);
    r.height = static_cast<float>(p.height);
    r.offsetX = static_cast<float>(p.offsetX);
    r.offsetY = static_cast<float>(p.offsetY);
    r.name = p.name;
    if (job.kind == DecodeKind::Tile) {
        p.bc7.clear(); // (a BC7 tile's level 0 decoded is what the floor composites from)
        r.pixels = std::move(p);
        r.ok = true;
        return r;
    }
    int w = p.pixelWidth;
    int h = p.pixelHeight;
    if (!p.bc7.empty() && p.rgba.empty()) {
        // A kCodecBc7 image read for its blocks: its levels as the pack has them (as many as the
        // RGBA path below makes: the pack's levels are halved the same way), and for the material
        // shader its material bytes, halved for each level as the RGBA path halves them.
        if (w <= 0 || h <= 0 || (job.kind != DecodeKind::Plain && job.kind != DecodeKind::Material)) {
            return r;
        }
        const bool material = job.kind == DecodeKind::Material;
        std::vector<std::uint8_t> labels = std::move(p.material);
        if (material && labels.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
            return r; // (no material pixels: the plain image serves)
        }
        const auto levels = nox::video::bc7Levels(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                                  static_cast<std::uint32_t>(p.bc7.size()));
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto& l = levels[i];
            // (The RGBA path's rule: up to kMaxMips more levels while the next is at least kMinMipSide.)
            if (i > 0 && (static_cast<int>(i) > kMaxMips || static_cast<int>(levels[i - 1].width) / 2 < kMinMipSide ||
                          static_cast<int>(levels[i - 1].height) / 2 < kMinMipSide)) {
                break;
            }
            TextureLevel t;
            t.width = static_cast<int>(l.width);
            t.height = static_cast<int>(l.height);
            t.pitch = static_cast<int>(l.paddedWidth / 4 * 16);
            t.bytes = std::move(p.bc7[i]);
            t.bc7 = true;
            r.levels.push_back(std::move(t));
            if (material) {
                if (i > 0) {
                    labels = halveLabels(labels, static_cast<int>(levels[i - 1].width), static_cast<int>(levels[i - 1].height));
                }
                r.labels.push_back(labels);
            }
        }
        r.ok = !r.levels.empty();
        return r;
    }
    if (w <= 0 || h <= 0 || p.rgba.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4) {
        return r;
    }
    std::vector<std::uint8_t> rgba = std::move(p.rgba);
    std::vector<std::uint8_t> labels = std::move(p.material);
    const bool hasMaterial = labels.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (job.kind == DecodeKind::Mono) {
        // Brightness as 0.30 R + 0.59 G + 0.11 B in 8-bit fixed point (0x4C, 0x96, 0x1C of 256),
        // times the colour.
        const unsigned cr = (job.colour >> 16) & 0xFF;
        const unsigned cg = (job.colour >> 8) & 0xFF;
        const unsigned cb = job.colour & 0xFF;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            const unsigned lum = (rgba[i] * 0x4Cu + rgba[i + 1] * 0x96u + rgba[i + 2] * 0x1Cu) >> 8;
            rgba[i] = static_cast<std::uint8_t>(lum * cr >> 8);
            rgba[i + 1] = static_cast<std::uint8_t>(lum * cg >> 8);
            rgba[i + 2] = static_cast<std::uint8_t>(lum * cb >> 8);
        }
    } else if (job.kind == DecodeKind::Tint) {
        if (!hasMaterial) {
            return r; // (no material pixels: the plain image serves)
        }
        shadeMaterials(rgba.data(), labels.data(), labels.size(), job.colours);
    }
    const bool material = job.kind == DecodeKind::Material;
    if (material && !hasMaterial) {
        return r;
    }
    r.levels.push_back(material ? rgbaLevel(besideMaterial(rgba, labels, w, h), w * 2, h) : rgbaLevel(rgba, w, h));
    // Each level half the one before, at most kMaxMips of them, none under kMinMipSide.
    while (static_cast<int>(r.levels.size()) <= kMaxMips && w / 2 >= kMinMipSide && h / 2 >= kMinMipSide) {
        int nw = 0;
        int nh = 0;
        rgba = halve(rgba, w, h, nw, nh);
        if (material) {
            labels = halveLabels(labels, w, h);
        }
        w = nw;
        h = nh;
        r.levels.push_back(material ? rgbaLevel(besideMaterial(rgba, labels, w, h), w * 2, h) : rgbaLevel(rgba, w, h));
    }
    r.ok = true;
    return r;
}

ImageDecoder::ImageDecoder(std::filesystem::path pack, int threads)
    : path_(std::move(pack))
{
    for (int i = 0; i < std::max(1, threads); ++i) {
        workers_.emplace_back([this] { run(); });
    }
}

ImageDecoder::~ImageDecoder()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    for (auto& t : workers_) {
        t.join();
    }
}

int ImageDecoder::defaultThreads()
{
    const unsigned cores = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(cores / 2), 1, 4);
}

void ImageDecoder::request(const DecodeJob& job, std::uint64_t nowMs)
{
    const auto key = job.key();
    {
        std::lock_guard lock(mutex_);
        if (busy_.contains(key)) {
            return;
        }
        auto it = wanted_.find(key);
        if (it == wanted_.end()) {
            it = wanted_.emplace(key, Want{job, 0, 0, preloadKeys_.erase(key) > 0}).first;
        } else {
            order_.erase(it->second.seq);
        }
        it->second.seq = ++seq_;
        it->second.askedMs = nowMs;
        order_.emplace(it->second.seq, key);
    }
    wake_.notify_one();
}

void ImageDecoder::preload(std::vector<DecodeJob> jobs)
{
    {
        std::lock_guard lock(mutex_);
        preload_.clear();
        preloadKeys_.clear();
        for (auto& j : jobs) {
            if (preloadKeys_.insert(j.key()).second) {
                preload_.push_back(std::move(j));
            }
        }
    }
    wake_.notify_all();
}

void ImageDecoder::cancelPreload(const DecodeJob::Key& key)
{
    std::lock_guard lock(mutex_);
    preloadKeys_.erase(key);
}

void ImageDecoder::dropRequests()
{
    std::lock_guard lock(mutex_);
    wanted_.clear();
    order_.clear();
}

void ImageDecoder::dropStale(std::uint64_t nowMs, std::uint64_t maxIdleMs)
{
    bool requeued = false;
    {
        std::lock_guard lock(mutex_);
        for (auto it = wanted_.begin(); it != wanted_.end();) {
            if (nowMs - it->second.askedMs <= maxIdleMs) {
                ++it;
                continue;
            }
            if (it->second.mapImage && preloadKeys_.insert(it->first).second) {
                preload_.push_back(it->second.job); // (still one of the map's: back in its queue)
                requeued = true;
            }
            order_.erase(it->second.seq);
            it = wanted_.erase(it);
        }
    }
    if (requeued) {
        wake_.notify_all();
    }
}

bool ImageDecoder::takeWait(DecodeResult& out, std::uint64_t maxMs)
{
    {
        // (The workers take their next jobs at normal priority while a frame waits on them.)
        ++waiting_;
        struct Done {
            std::atomic<int>& n;
            ~Done() { --n; }
        } done{waiting_};
        std::unique_lock lock(mutex_);
        if (!finished_.wait_for(lock, std::chrono::milliseconds(maxMs),
                                [&] { return !done_.empty() || !donePreload_.empty(); })) {
            return false;
        }
    }
    return take(out);
}

bool ImageDecoder::take(DecodeResult& out)
{
    bool resume = false;
    {
        std::lock_guard lock(mutex_);
        // What was asked for while drawing before the map's preload (it's on screen).
        auto& from = !done_.empty() ? done_ : donePreload_;
        if (from.empty()) {
            return false;
        }
        out = std::move(from.front());
        from.pop_front();
        const std::size_t bytes = resultBytes(out);
        resume = doneBytes_ >= kMaxWaitingBytes && doneBytes_ - std::min(doneBytes_, bytes) < kMaxWaitingBytes;
        doneBytes_ -= std::min(doneBytes_, bytes);
        busy_.erase(out.job.key());
    }
    if (resume) {
        wake_.notify_all(); // (the preload list may go on)
    }
    return true;
}

void ImageDecoder::run()
{
    // Below the drawing and the game: a decode can wait, a frame can't. But while a frame waits
    // for its floor (takeWait) the decodes are what it waits on: then normal, set as each job is
    // taken (a low one would sit behind the host's and the bots' threads with the frame stopped).
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    bool raised = false;
    const auto prioritise = [&] {
        const bool urgent = waiting_ > 0;
        if (urgent != raised) {
            raised = urgent;
            SDL_SetCurrentThreadPriority(urgent ? SDL_THREAD_PRIORITY_NORMAL : SDL_THREAD_PRIORITY_LOW);
        }
    };
    // Its own reader, opened now and, should that fail, again at its next pack image. Meanwhile it
    // still takes its share of the jobs: a composite needs no pack, and a pack image comes back
    // failed (as one the pack couldn't read), its SD one drawn. (A worker gone instead would leave
    // what's asked for unanswered: with none left, the floor would wait its whole deadline for
    // its tiles each frame.)
    std::unique_ptr<nox::video::ImagePackReader> reader;
    bool openFailed = false;
    const auto haveReader = [&] {
        if (!reader) {
            try {
                reader = std::make_unique<nox::video::ImagePackReader>(path_);
            } catch (const std::exception& ex) {
                if (!std::exchange(openFailed, true)) {
                    SDL_Log("image decoder: %s: %s", path_.string().c_str(), ex.what());
                }
            }
        }
        return reader != nullptr;
    };
    haveReader();
    for (;;) {
        DecodeJob job;
        bool mapImage = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] {
                return stop_ || !order_.empty() || (!preload_.empty() && doneBytes_ < kMaxWaitingBytes);
            });
            if (stop_) {
                return;
            }
            if (!order_.empty()) {
                // The last asked for first.
                const auto last = std::prev(order_.end());
                const auto it = wanted_.find(last->second);
                job = std::move(it->second.job);
                mapImage = it->second.mapImage;
                wanted_.erase(it);
                order_.erase(last);
            } else {
                job = std::move(preload_.front());
                preload_.pop_front();
                // Skipped if it came another way meanwhile (cancelled, asked for, or being decoded).
                if (!preloadKeys_.erase(job.key()) || busy_.contains(job.key())) {
                    continue;
                }
                mapImage = true;
            }
            busy_.insert(job.key());
        }
        prioritise();
        const auto start = std::chrono::steady_clock::now();
        DecodeResult r;
        try {
            if (job.kind == DecodeKind::Composite) {
                // The caller's pixels, built here (its builder let go once it has run: what it held
                // goes with it).
                auto build = std::move(job.build);
                job.build = nullptr;
                r.job = job;
                std::vector<std::uint8_t> pixels;
                try {
                    pixels = build ? build() : std::vector<std::uint8_t>{};
                } catch (const std::exception& ex) {
                    SDL_Log("image decoder: building %s: %s", job.cacheKey.c_str(), ex.what());
                }
                build = nullptr;
                r.ok = job.width > 0 && job.height > 0 &&
                       pixels.size() == static_cast<std::size_t>(job.width) * static_cast<std::size_t>(job.height) * 4;
                if (r.ok) {
                    r.width = static_cast<float>(job.width);
                    r.height = static_cast<float>(job.height);
                    r.levels.push_back(rgbaLevel(std::move(pixels), job.width, job.height));
                }
            } else {
                std::optional<nox::video::PackImage> image;
                try {
                    if (job.index >= 0 && haveReader() && static_cast<std::uint32_t>(job.index) < reader->size()) {
                        image = reader->read(static_cast<std::size_t>(job.index), false, job.blocks);
                    }
                } catch (const std::exception& ex) {
                    SDL_Log("image pack slot %d: %s", job.index, ex.what());
                }
                r = buildDecodeResult(job, std::move(image));
                r.job.build = nullptr;
            }
        } catch (...) {
            // Anything else (out of memory on a big image's levels, a builder's own throw): a
            // failed result all the same, so its key leaves busy_ (else it's never asked for again
            // and a floor waiting for it waits each frame) and the drawing draws the SD one. Out
            // of the thread it would end the program.
            SDL_Log("image decoder: slot %d (%s) failed", job.index, job.cacheKey.c_str());
            r = DecodeResult{};
            r.job.kind = job.kind;
            r.job.index = job.index;
            r.job.colour = job.colour;
            r.job.colours = job.colours;
            r.job.cacheKey = std::move(job.cacheKey);
            r.job.pin = job.pin;
        }
        r.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        {
            std::lock_guard lock(mutex_);
            doneBytes_ += resultBytes(r);
            (mapImage ? donePreload_ : done_).push_back(std::move(r));
        }
        finished_.notify_all();
    }
}

} // namespace noxsdl
