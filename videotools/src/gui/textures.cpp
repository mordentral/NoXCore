#include "gui/textures.hpp"

#include "editor/panel_textures.hpp" // (noxsdl's editor host: floating panels draw copies made from these pixels)

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace videotools::gui {

namespace {

constexpr std::size_t kMaxTextures = 6000;

} // namespace

SDL_Texture* makeTexture(SDL_Renderer* renderer, const std::vector<std::uint8_t>& rgba, int w, int h)
{
    SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    noxsdl::editor::panelTextureSet(t, rgba.data(), w, h, w * 4);
    return t;
}

void destroyTexture(SDL_Texture* t)
{
    if (t) {
        noxsdl::editor::panelTextureForget(t);
        SDL_DestroyTexture(t);
    }
}

ImTextureRef texRef(SDL_Texture* t)
{
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(t)));
}

std::vector<std::uint8_t> tintMaterials(const std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& material)
{
    static constexpr std::uint8_t kColours[][3] = {{230, 80, 60},  {70, 160, 230}, {90, 200, 90},  {230, 200, 60},
                                                   {200, 90, 220}, {60, 210, 200}, {240, 140, 40}, {160, 160, 240}};
    std::vector<std::uint8_t> out = rgba;
    for (std::size_t i = 0; i < material.size() && i * 4 + 3 < out.size(); ++i) {
        if (material[i]) {
            const auto* c = kColours[(material[i] - 1) % 8];
            for (int k = 0; k < 3; ++k) {
                out[i * 4 + std::size_t(k)] = std::uint8_t(out[i * 4 + std::size_t(k)] * c[k] / 255);
            }
        }
    }
    return out;
}

void TextureCache::newFrame()
{
    ++frame_;
    budget_ = loadsPerFrame_;
    evict();
}

const Tex* TextureCache::get(std::int32_t index)
{
    if (index < 0 || !loader_) {
        return nullptr;
    }
    if (const auto it = map_.find(index); it != map_.end()) {
        it->second.lastUsed = frame_;
        return &it->second;
    }
    if (budget_ <= 0) {
        return nullptr;
    }
    --budget_;
    return load(index);
}

const Tex* TextureCache::getNow(std::int32_t index)
{
    if (index < 0 || !loader_) {
        return nullptr;
    }
    if (const auto it = map_.find(index); it != map_.end()) {
        it->second.lastUsed = frame_;
        return &it->second;
    }
    return load(index);
}

const Tex* TextureCache::load(std::int32_t index)
{
    Tex t;
    t.lastUsed = frame_;
    try {
        const auto img = loader_(index);
        if (!img) {
            t.missing = true;
            return &map_.emplace(index, std::move(t)).first->second;
        }
        t.width = img->width;
        t.height = img->height;
        t.offsetX = img->offsetX;
        t.offsetY = img->offsetY;
        t.type = img->type;
        t.headerByte = img->headerByte;
        t.extra = img->extra;
        t.edgeMask = img->edgeMask;
        if (img->pixelWidth <= 0 || img->pixelHeight <= 0) {
            t.empty = true;
            return &map_.emplace(index, std::move(t)).first->second;
        }
        t.pixelWidth = img->pixelWidth;
        t.pixelHeight = img->pixelHeight;
        bool slots[256] = {};
        for (const auto m : img->material) {
            slots[m] = true;
        }
        for (int s = 1; s < 256; ++s) {
            if (slots[s]) {
                t.materialSlots.push_back(s - 1);
            }
        }
        t.texture = makeTexture(renderer_, img->rgba, img->pixelWidth, img->pixelHeight);
        if (!t.materialSlots.empty()) {
            t.tinted = makeTexture(renderer_, tintMaterials(img->rgba, img->material), img->pixelWidth, img->pixelHeight);
        }
    } catch (const std::exception& ex) {
        t.error = ex.what();
    }
    return &map_.emplace(index, std::move(t)).first->second;
}

void TextureCache::forget(std::int32_t index)
{
    if (const auto it = map_.find(index); it != map_.end()) {
        destroy(it->second);
        map_.erase(it);
    }
}

void TextureCache::clear()
{
    for (auto& [i, t] : map_) {
        destroy(t);
    }
    map_.clear();
}

void TextureCache::destroy(Tex& t)
{
    if (t.texture) {
        noxsdl::editor::panelTextureForget(t.texture);
        SDL_DestroyTexture(t.texture);
    }
    if (t.tinted) {
        noxsdl::editor::panelTextureForget(t.tinted);
        SDL_DestroyTexture(t.tinted);
    }
}

// Drops the least recently drawn textures (never ones drawn this or last frame).
void TextureCache::evict()
{
    if (map_.size() <= kMaxTextures) {
        return;
    }
    std::vector<std::pair<std::uint64_t, std::int32_t>> old;
    for (const auto& [i, t] : map_) {
        if (t.lastUsed + 1 < frame_) {
            old.emplace_back(t.lastUsed, i);
        }
    }
    std::sort(old.begin(), old.end());
    for (const auto& [used, i] : old) {
        if (map_.size() <= kMaxTextures * 3 / 4) {
            break;
        }
        destroy(map_.at(i));
        map_.erase(i);
    }
}

void drawFit(ImDrawList* dl, const Tex* t, ImVec2 p0, ImVec2 size, bool tint)
{
    if (!t || !t->texture) {
        return;
    }
    // Fitted by the logical size, so an HD image shows as big as its original.
    const float w = float(std::max(1, t->width));
    const float h = float(std::max(1, t->height));
    float s = std::min(size.x / w, size.y / h);
    if (s >= 1) {
        s = std::floor(std::min(s, 4.0f));
    }
    const ImVec2 wh(w * s, h * s);
    const ImVec2 a(std::floor(p0.x + (size.x - wh.x) / 2), std::floor(p0.y + (size.y - wh.y) / 2));
    dl->AddImage(texRef(t->show(tint)), a, ImVec2(a.x + wh.x, a.y + wh.y));
}

void drawBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int background, float scale)
{
    switch (background) {
    case 0: {
        const float cell = 12 * scale;
        dl->AddRectFilled(p0, p1, IM_COL32(58, 58, 64, 255));
        for (float y = p0.y; y < p1.y; y += cell) {
            for (float x = p0.x + (int((y - p0.y) / cell) % 2 ? cell : 0); x < p1.x; x += cell * 2) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, p1.x), std::min(y + cell, p1.y)),
                                  IM_COL32(74, 74, 82, 255));
            }
        }
        break;
    }
    case 1:
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
        break;
    case 2:
        dl->AddRectFilled(p0, p1, IM_COL32(128, 128, 128, 255));
        break;
    case 3:
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
        break;
    default:
        dl->AddRectFilled(p0, p1, IM_COL32(255, 0, 255, 255));
        break;
    }
}

const char* imageTypeName(int type)
{
    switch (type) {
    case 0:
        return "floor tile";
    case 1:
        return "tile edge";
    case 2:
    case 7:
        return "full image (RGBA5551)";
    case 8:
        return "run-length sprite (RGB565)";
    case 3:
    case 4:
    case 6:
        return "run-length sprite";
    case 5:
        return "run-length sprite (RGBA4444)";
    }
    return "unknown";
}

} // namespace videotools::gui
