// Bloom for the bright effects and light sources (GPU renderer; --no-bloom turns it off, --bloom N
// sets how much, 0-1, 0.2 by default). Into a quarter-size target go the bright parts of the light
// sources' sprites (things of CLASS LIGHT: flames, fireballs, explosions, torches; through the
// bloom_bright shader, as they were drawn) and the effects layer (the
// sparks, particles, glows, rays and orbs Effects::drawParticles draws) is drawn a second time
// into a quarter-size target, blurred twice each way with the bloom_blur shader (a nine-tap
// Gaussian), and screened over the scene, so bright effects throw a soft light round themselves.
// The effects themselves are drawn as before; bloom only adds the halo. What's gathered clamps
// at white, so a crowd of lights (a cleansing flame's ring) blooms no brighter than one, and the
// screen blend leaves what's already bright (the fire itself) nearly as it was.
//
// A light hidden behind something drawn in front of it doesn't bloom there: the scene's sprites
// are replayed in their drawing order (MapView::bloomSprites), the lights adding their bright
// parts and everything drawn after them cutting its silhouette out (hide), so only what's seen of
// a light blooms. The halo then spreads round what's seen, as a camera's bloom does.
//
//   bloom.begin(renderer, w, h);      // draws now go into the bloom target, scaled
//   bloom.addSprite(...); bloom.hide(...);   // the scene's lights and what's in front of them
//   effects.drawParticles(...);
//   bloom.end(renderer);               // blur, then add over the screen
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

namespace noxsdl {

class Bloom {
public:
    /// `blur` and `bright` are GpuShaders::blur() / bright(). `strength` scales what's added (0-1).
    Bloom(SDL_GPURenderState* blur, SDL_GPURenderState* bright, float strength = 0.2f)
        : blur_(blur), bright_(bright), strength_(strength)
    {
    }
    ~Bloom();
    Bloom(const Bloom&) = delete;
    Bloom& operator=(const Bloom&) = delete;

    /// Points drawing at the bloom target (cleared) for a w x h screen. False if it can't
    /// (then don't call end).
    bool begin(SDL_Renderer* renderer, int w, int h);
    /// Between begin and end: a light source's sprite as it was drawn (texture rectangle, screen
    /// rectangle, light tint, alpha); its bright parts go into the bloom (bloom_bright).
    void addSprite(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect& src, const SDL_FRect& dst,
                   const std::array<std::uint8_t, 3>& light, std::uint8_t alpha);
    /// Between begin and end, in drawing order after the sources it may hide: something drawn in
    /// front of a light (a wall, an object) cuts its silhouette out of what's gathered so far.
    void hide(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect& src, const SDL_FRect& dst,
              std::uint8_t alpha);
    /// Blurs what was drawn and adds it over the screen.
    void end(SDL_Renderer* renderer);

private:
    void pass(SDL_Renderer* renderer, SDL_Texture* from, SDL_Texture* to, float dx, float dy);

    SDL_GPURenderState* blur_;
    SDL_GPURenderState* bright_;
    float strength_;
    SDL_Texture* gather_ = nullptr; ///< what's drawn between begin and end, clamped at white
    SDL_Texture* a_ = nullptr;      ///< the blur's ping-pong targets
    SDL_Texture* b_ = nullptr;
    int w_ = 0, h_ = 0; ///< the targets' size
    SDL_Texture* previousTarget_ = nullptr;
    float previousScaleX_ = 1, previousScaleY_ = 1;
};

} // namespace noxsdl
