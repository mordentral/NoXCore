// A hosted mod's own pictures by name (mod_images.hpp).
#include "mod_images.hpp"

#include "sprite_cache.hpp"

#include <SDL3/SDL.h>

namespace noxsdl {

namespace ms = nox::modes::script;

int modImageIndex(const ms::ModAssets* set, std::string_view name)
{
    if (!set || !ms::validAssetName(name)) {
        return -1;
    }
    return set->find(name, ms::ModAsset::Kind::Image);
}

ModImages::ModImages(SDL_Renderer* renderer) : renderer_(renderer) {}

ModImages::~ModImages()
{
    clear();
}

void ModImages::clear()
{
    for (auto& [index, s] : sprites_) {
        if (s && s->texture) {
            SDL_DestroyTexture(s->texture);
        }
    }
    sprites_.clear();
    of_.reset();
}

const Sprite* ModImages::get(const std::shared_ptr<const ms::ModAssets>& set, std::string_view name)
{
    if (set != of_) {
        clear();
        of_ = set;
    }
    const int index = modImageIndex(set.get(), name);
    if (index < 0 || !renderer_) {
        return nullptr;
    }
    auto it = sprites_.find(index);
    if (it == sprites_.end()) {
        const ms::ModAsset& a = set->list[static_cast<std::size_t>(index)];
        std::unique_ptr<Sprite> s;
        if (SDL_Texture* t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, a.width, a.height)) {
            SDL_UpdateTexture(t, nullptr, a.rgba.data(), a.width * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            s = std::make_unique<Sprite>();
            s->texture = t;
            s->width = static_cast<float>(a.width);
            s->height = static_cast<float>(a.height);
            s->rect = SDL_FRect{0, 0, static_cast<float>(a.width), static_cast<float>(a.height)};
        }
        it = sprites_.emplace(index, std::move(s)).first; // (a failed one kept as none: not tried each frame)
    }
    return it->second.get();
}

} // namespace noxsdl
