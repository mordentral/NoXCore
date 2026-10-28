// Images as SDL textures for the GUI, made on first use within a per-frame budget (so scrolling
// stays smooth), from a loader: video.bag images (edits included) or image pack images.
#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace videotools::gui {

/// What a loader hands over: an image at its stored size, with where and how big it draws.
struct LoadedImage {
    int pixelWidth = 0; // stored size (the texture's)
    int pixelHeight = 0;
    int width = 0; // logical size, original pixels
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;
    int type = 0;
    int headerByte = 0;
    std::vector<std::uint8_t> rgba;     // pixelWidth * pixelHeight * 4
    std::vector<std::uint8_t> material; // empty, or a code a pixel (0 plain, n = slot n - 1)
    bool edgeMask = false;
    std::string extra; // a line of facts for the info table (codec, ...)
};

struct Tex {
    SDL_Texture* texture = nullptr;
    SDL_Texture* tinted = nullptr; // material slots shaded with sample colours
    int pixelWidth = 0;
    int pixelHeight = 0;
    int width = 0;
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;
    int type = 0;
    int headerByte = 0;
    std::vector<int> materialSlots;
    bool edgeMask = false;
    bool empty = false;   // a 0 x 0 image that draws nothing
    bool missing = false; // the loader has no image here (an empty pack slot)
    std::string extra;
    std::string error;
    std::uint64_t lastUsed = 0;

    SDL_Texture* show(bool tint) const { return tint && tinted ? tinted : texture; }
};

ImTextureRef texRef(SDL_Texture* t);

/// A texture of RGBA pixels that floating panels can draw too (destroyTexture it). Throws.
SDL_Texture* makeTexture(SDL_Renderer* renderer, const std::vector<std::uint8_t>& rgba, int w, int h);
void destroyTexture(SDL_Texture* t);

/// Each slot's grey multiplied by a fixed sample colour (the game takes the colours from the
/// object; this only shows where the slots are).
std::vector<std::uint8_t> tintMaterials(const std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& material);

class TextureCache {
public:
    /// nullopt: no image there. Throws for a broken one (shown as the error).
    using Loader = std::function<std::optional<LoadedImage>(std::int32_t)>;

    explicit TextureCache(SDL_Renderer* renderer, int loadsPerFrame = 48) : renderer_(renderer), loadsPerFrame_(loadsPerFrame) {}
    ~TextureCache() { clear(); }
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    void setLoader(Loader loader)
    {
        clear();
        loader_ = std::move(loader);
    }
    bool hasLoader() const { return static_cast<bool>(loader_); }

    void newFrame();
    /// The image, or nullptr while it isn't loaded yet (it will be on a later frame).
    const Tex* get(std::int32_t index);
    /// Loads now, whatever the budget.
    const Tex* getNow(std::int32_t index);
    /// Drops an image (it changed).
    void forget(std::int32_t index);
    void clear();

private:
    static void destroy(Tex& t);
    void evict();
    const Tex* load(std::int32_t index);

    SDL_Renderer* renderer_;
    int loadsPerFrame_;
    Loader loader_;
    std::unordered_map<std::int32_t, Tex> map_; // node-based: pointers stay valid until evicted
    std::uint64_t frame_ = 0;
    int budget_ = 0;
};

/// Draws an image scaled to fit a box, centred; whole-number scales when enlarging.
void drawFit(ImDrawList* dl, const Tex* t, ImVec2 p0, ImVec2 size, bool tint = false);

/// Fills a box with one of the backgrounds (0 checker, 1 black, 2 grey, 3 white, 4 magenta).
void drawBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int background, float scale);

inline constexpr const char* kBackgrounds[] = {"Checker", "Black", "Grey", "White", "Magenta"};

/// A human name for a video.bag image type.
const char* imageTypeName(int type);

} // namespace videotools::gui
