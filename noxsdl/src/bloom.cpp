// The bloom pass (bloom.hpp): its quarter-size targets, the gather of bright sprites, the blur
// passes and the screen blend back over the scene.
#include "bloom.hpp"

#include <algorithm>

namespace noxsdl {

namespace {

constexpr int kDivisor = 4; ///< the targets are a quarter of the screen each way

SDL_Texture* makeTarget(SDL_Renderer* renderer, int w, int h, bool clamped)
{
    // The gather target clamps at white, so a crowd of lights (a cleansing flame's 48 flames,
    // a pile of sparks) blooms no brighter than one: in half floats they added up to many times
    // white and the halo washed out everything round them. The blur passes use half floats
    // where the renderer has them, so the soft halo doesn't band.
    SDL_Texture* t = clamped ? nullptr
                             : SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA64_FLOAT, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t) {
        t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
    }
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}

} // namespace

Bloom::~Bloom()
{
    if (gather_) {
        SDL_DestroyTexture(gather_);
    }
    if (a_) {
        SDL_DestroyTexture(a_);
    }
    if (b_) {
        SDL_DestroyTexture(b_);
    }
}

// Makes (or remakes, on a new size) the targets, and points drawing at the cleared gather
// target; false when bloom can't run (no blur shader, or a target failed).
bool Bloom::begin(SDL_Renderer* renderer, int w, int h)
{
    if (!blur_ || w <= 0 || h <= 0) {
        return false;
    }
    const int tw = std::max(1, w / kDivisor);
    const int th = std::max(1, h / kDivisor);
    if (!a_ || tw != w_ || th != h_) {
        for (SDL_Texture* t : {gather_, a_, b_}) {
            if (t) {
                SDL_DestroyTexture(t);
            }
        }
        gather_ = makeTarget(renderer, tw, th, true);
        a_ = makeTarget(renderer, tw, th, false);
        b_ = makeTarget(renderer, tw, th, false);
        w_ = tw;
        h_ = th;
    }
    if (!gather_ || !a_ || !b_) {
        return false;
    }
    previousTarget_ = SDL_GetRenderTarget(renderer);
    SDL_GetRenderScale(renderer, &previousScaleX_, &previousScaleY_);
    SDL_SetRenderTarget(renderer, gather_);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    // Screen coordinates land on the smaller target.
    SDL_SetRenderScale(renderer, static_cast<float>(tw) / static_cast<float>(w),
                       static_cast<float>(th) / static_cast<float>(h));
    return true;
}

namespace {

/// Adds by colour alone (a clear target's alpha stays nothing, so colour x alpha adds nothing).
SDL_BlendMode addColour()
{
    static const SDL_BlendMode mode =
        SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
                                   SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    return mode;
}

} // namespace

void Bloom::addSprite(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect& src, const SDL_FRect& dst,
                      const std::array<std::uint8_t, 3>& light, std::uint8_t alpha)
{
    if (!bright_ || !texture) {
        return;
    }
    SDL_BlendMode was = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(texture, &was);
    SDL_SetTextureBlendMode(texture, addColour());
    SDL_SetTextureColorMod(texture, light[0], light[1], light[2]);
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_SetGPURenderState(renderer, bright_);
    SDL_RenderTexture(renderer, texture, &src, &dst);
    SDL_SetGPURenderState(renderer, nullptr);
    SDL_SetTextureBlendMode(texture, was);
    SDL_SetTextureColorMod(texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(texture, 255);
}

void Bloom::hide(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect& src, const SDL_FRect& dst,
                 std::uint8_t alpha)
{
    if (!texture) {
        return;
    }
    // What's there times (1 - the sprite's alpha): its silhouette cut out.
    static const SDL_BlendMode cutOut =
        SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                   SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    SDL_BlendMode was = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(texture, &was);
    SDL_SetTextureBlendMode(texture, cutOut);
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_RenderTexture(renderer, texture, &src, &dst);
    SDL_SetTextureBlendMode(texture, was);
    SDL_SetTextureAlphaMod(texture, 255);
}

// One blur pass from `from` into `to`, one texel step (dx, dy) apart.
void Bloom::pass(SDL_Renderer* renderer, SDL_Texture* from, SDL_Texture* to, float dx, float dy)
{
    SDL_SetRenderTarget(renderer, to);
    SDL_SetRenderScale(renderer, 1, 1);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    const float params[4] = {dx / static_cast<float>(w_), dy / static_cast<float>(h_), 0, 0};  // the step in 0-1 texture coordinates
    SDL_SetGPURenderStateFragmentUniforms(blur_, 0, params, sizeof(params));
    SDL_SetTextureBlendMode(from, SDL_BLENDMODE_NONE);
    SDL_SetTextureColorMod(from, 255, 255, 255);
    SDL_SetTextureAlphaMod(from, 255);
    SDL_SetGPURenderState(renderer, blur_);
    SDL_RenderTexture(renderer, from, nullptr, nullptr);
    SDL_SetGPURenderState(renderer, nullptr);
}

void Bloom::end(SDL_Renderer* renderer)
{
    // Twice across and down: a wide, soft halo (from the clamped gather into the float targets).
    for (int i = 0; i < 2; ++i) {
        pass(renderer, i == 0 ? gather_ : a_, b_, 1, 0);
        pass(renderer, b_, a_, 0, 1);
    }
    SDL_SetRenderTarget(renderer, previousTarget_);
    SDL_SetRenderScale(renderer, previousScaleX_, previousScaleY_);
    // Screened over the scene, 1 - (1 - scene)(1 - bloom), by colour alone (the targets' alpha
    // stays nothing): dark ground round a light takes nearly all of the halo, while what's
    // already bright, the fire itself, takes little, so flames keep their colour instead of
    // washing out to white under their own halo as they did when it was simply added.
    static const SDL_BlendMode screen =
        SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
                                   SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    SDL_SetTextureBlendMode(a_, screen);
    const auto s = static_cast<Uint8>(std::clamp(strength_, 0.0f, 1.0f) * 255);  // the strength scales the halo's colour
    SDL_SetTextureColorMod(a_, s, s, s);
    SDL_RenderTexture(renderer, a_, nullptr, nullptr);
    SDL_SetTextureColorMod(a_, 255, 255, 255);
}

} // namespace noxsdl
