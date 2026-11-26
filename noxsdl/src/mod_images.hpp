// (noxcpp) A hosted mod's own pictures where the game's windows look a picture up by name
// (Painter::image): a name none of thing.bin's named images has is the mod's image asset of exactly
// that name (modes/script mod_assets.hpp: mods\<id>\assets\portraits\knight.png is
// "portraits/knight"), as its UI's set came (ScriptUiClient::assets). The game's own pictures always
// come first, so a mod can't hide one; a stock client simply has no picture of such a name. What a
// mod names this way: an NPC's dialog portrait (StoryPic(npc, "portraits/knight")).
//
// The textures are made at a picture's first draw and let go with their set (a new set, a reload,
// the game's end).
#pragma once

#include "noxcore/modes/script/mod_assets.hpp"

#include <map>
#include <memory>
#include <string_view>

struct SDL_Renderer;

namespace noxsdl {

struct Sprite;

/// The index of `set`'s image asset `name`; -1: none (no set, no asset of that name, a sound, a
/// name no asset can have).
int modImageIndex(const nox::modes::script::ModAssets* set, std::string_view name);

class ModImages {
public:
    explicit ModImages(SDL_Renderer* renderer);
    ~ModImages();
    ModImages(const ModImages&) = delete;
    ModImages& operator=(const ModImages&) = delete;

    /// The picture `name` of `set` as a sprite (its own size, no offset); null: none, or its
    /// texture couldn't be made.
    const Sprite* get(const std::shared_ptr<const nox::modes::script::ModAssets>& set, std::string_view name);
    /// Every texture let go.
    void clear();

private:
    SDL_Renderer* renderer_ = nullptr;
    std::shared_ptr<const nox::modes::script::ModAssets> of_; ///< (held: never another set at its address)
    std::map<int, std::unique_ptr<Sprite>> sprites_;          ///< by asset index (null: couldn't be made)
};

} // namespace noxsdl
