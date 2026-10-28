// Copies of textures for the editor's floating panels (texture_mirror.hpp).
#include "editor/texture_mirror.hpp"

#include <algorithm>
#include <cstring>

namespace noxsdl::editor {

TexelRect TexelRect::unite(const TexelRect& o) const
{
    if (empty()) return o;
    if (o.empty()) return *this;
    const int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    const int x1 = std::max(x + w, o.x + o.w), y1 = std::max(y + h, o.y + o.h);
    return TexelRect{x0, y0, x1 - x0, y1 - y0};
}

TexelRect TexelRect::clip(int width, int height) const
{
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min(x + w, width), y1 = std::min(y + h, height);
    if (x1 <= x0 || y1 <= y0) return TexelRect{};
    return TexelRect{x0, y0, x1 - x0, y1 - y0};
}

TextureMirror::Copy* TextureMirror::findCopy(Source& s, Renderer r)
{
    for (auto& [renderer, copy] : s.copies) {
        if (renderer == r) return &copy;
    }
    return nullptr;
}

void TextureMirror::freeCopies(Source& s)
{
    for (auto& [renderer, copy] : s.copies) {
        if (copy.handle && uploader_) uploader_->destroy(renderer, copy.handle);
    }
    s.copies.clear();
}

void TextureMirror::setPixels(Key key, const TexturePixels& pixels)
{
    if (!key) return;
    Source& s = sources_[key];
    if (!pixels.data || pixels.width <= 0 || pixels.height <= 0 || (pixels.bytesPerPixel != 1 && pixels.bytesPerPixel != 4)) {
        s.pixels.clear(); // (nothing usable: its copies draw as placeholders)
        s.width = s.height = 0;
    } else {
        const std::size_t row = static_cast<std::size_t>(pixels.width) * static_cast<std::size_t>(pixels.bytesPerPixel);
        s.pixels.resize(row * static_cast<std::size_t>(pixels.height));
        for (int y = 0; y < pixels.height; ++y) {
            std::memcpy(s.pixels.data() + row * static_cast<std::size_t>(y), pixels.at(0, y), row);
        }
        s.width = pixels.width;
        s.height = pixels.height;
    }
    s.external = false;
    s.bytesPerPixel = pixels.bytesPerPixel;
    s.format = pixels.format;
    s.generation = nextGeneration_++;
}

void TextureMirror::updatePixels(Key key, const TexelRect& rect, const std::uint8_t* data, int pitch)
{
    const auto it = sources_.find(key);
    if (it == sources_.end() || it->second.external || it->second.pixels.empty() || !data) return;
    Source& s = it->second;
    const TexelRect r = rect.clip(s.width, s.height);
    if (r.empty()) return;
    const std::size_t row = static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.bytesPerPixel);
    const std::size_t span = static_cast<std::size_t>(r.w) * static_cast<std::size_t>(s.bytesPerPixel);
    // (data is the rectangle's own pixels, as SDL_UpdateTexture takes them: its first row first.)
    const int dx = r.x - rect.x, dy = r.y - rect.y;
    for (int y = 0; y < r.h; ++y) {
        const std::uint8_t* from = data + static_cast<std::ptrdiff_t>(y + dy) * pitch + static_cast<std::ptrdiff_t>(dx) * s.bytesPerPixel;
        std::memcpy(s.pixels.data() + row * static_cast<std::size_t>(r.y + y) + static_cast<std::size_t>(r.x) * static_cast<std::size_t>(s.bytesPerPixel), from, span);
    }
    markDirty(key, r);
}

void TextureMirror::setExternal(Key key, int width, int height)
{
    if (!key) return;
    auto [it, made] = sources_.try_emplace(key);
    Source& s = it->second;
    if (made || !s.external || s.width != width || s.height != height) {
        s.external = true;
        s.pixels.clear();
        s.width = width;
        s.height = height;
        s.generation = nextGeneration_++;
    }
}

void TextureMirror::invalidate(Key key)
{
    const auto it = sources_.find(key);
    if (it != sources_.end()) it->second.generation = nextGeneration_++;
}

void TextureMirror::markDirty(Key key, const TexelRect& rect)
{
    const auto it = sources_.find(key);
    if (it == sources_.end()) return;
    const TexelRect r = rect.clip(it->second.width, it->second.height);
    if (r.empty()) return;
    for (auto& [renderer, copy] : it->second.copies) copy.dirty = copy.dirty.unite(r);
}

void TextureMirror::remove(Key key)
{
    const auto it = sources_.find(key);
    if (it == sources_.end()) return;
    freeCopies(it->second);
    sources_.erase(it);
}

TextureMirror::Handle TextureMirror::resolve(Renderer renderer, Key key, const TexturePixels* external)
{
    const auto it = sources_.find(key);
    if (it == sources_.end() || !renderer) return 0;
    Source& s = it->second;
    TexturePixels px;
    if (s.external) {
        if (!external || !external->data || external->width != s.width || external->height != s.height) return 0;
        px = *external;
    } else {
        if (s.pixels.empty()) return 0;
        px.data = s.pixels.data();
        px.width = s.width;
        px.height = s.height;
        px.bytesPerPixel = s.bytesPerPixel;
        px.pitch = s.width * s.bytesPerPixel;
        px.format = s.format;
    }
    Copy* c = findCopy(s, renderer);
    if (!c) {
        s.copies.emplace_back(renderer, Copy{});
        c = &s.copies.back().second;
    }
    if (c->handle && c->generation != s.generation) {
        if (uploader_) uploader_->destroy(renderer, c->handle);
        c->handle = 0;
    }
    if (!c->handle) {
        if (c->failedGeneration == s.generation || !uploader_) return 0; // (a copy that failed: a placeholder till the source changes)
        c->handle = uploader_->create(renderer, px);
        c->dirty = TexelRect{};
        if (!c->handle) {
            c->failedGeneration = s.generation;
            return 0;
        }
        c->generation = s.generation;
        return c->handle;
    }
    if (!c->dirty.empty() && uploader_) {
        uploader_->update(renderer, c->handle, px, c->dirty);
        c->dirty = TexelRect{};
    }
    return c->handle;
}

void TextureMirror::dropRenderer(Renderer renderer)
{
    for (auto& [key, s] : sources_) {
        for (auto c = s.copies.begin(); c != s.copies.end(); ++c) {
            if (c->first != renderer) continue;
            if (c->second.handle && uploader_) uploader_->destroy(renderer, c->second.handle);
            s.copies.erase(c);
            break;
        }
    }
}

void TextureMirror::clear()
{
    for (auto& [key, s] : sources_) freeCopies(s);
    sources_.clear();
}

std::size_t TextureMirror::copies(Renderer renderer) const
{
    std::size_t n = 0;
    for (const auto& [key, s] : sources_) {
        for (const auto& [r, c] : s.copies) {
            if (r == renderer && c.handle) ++n;
        }
    }
    return n;
}

} // namespace noxsdl::editor
