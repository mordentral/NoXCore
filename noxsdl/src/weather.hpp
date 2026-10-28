// Weather in a game: what a script mode's nox.weather tells NoX
// Core clients (ScriptUiClient::weather), drawn by the client with its own shader
// (shaders/weather.frag.hlsl) over the world and under the HUD. Not in the game; the server's
// game is unchanged by it.
//
//   rain   streaks falling and slanting with the wind, small rings where they land, faint sheets
//          of distant rain drifting over the ground
//   snow   flakes drifting down, swaying, carried by the wind, settling a moment where they land
//   storm  heavy rain, the scene darker (the light pass's ambient), lightning flashes now and then
//          that briefly brighten the light pass, thunder after them
//   wind   slants the rain, carries the snow, and with any kind (none too) gusts of streaks and
//          motes of dust along it, in waves; its loop heard
//
// The particles live on the ground's plane (a world x, y and a height above it, drawn height
// units up the screen), so they stay put on the ground as the view moves rather than sliding
// with it. Changes blend over the fade the server gives. Options' Weather Effects: On, Reduced
// (fewer particles, softer flashes) or Off (nothing drawn). Drawn only with the GPU renderer's
// shaders; SDL's usual renderer draws none.
#pragma once

#include "camera.hpp"
#include "lighting.hpp"
#include "sight.hpp"

#include "noxcore/modes/script/weather.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace noxsdl {

class Audio;

class WeatherFx {
public:
    /// Options' Weather Effects.
    enum class Level : std::uint8_t {
        On = 0,
        Reduced = 1,
        Off = 2,
    };

    /// Drawn through `state` (GpuShaders::weather) on `white` (a white pixel).
    WeatherFx(SDL_GPURenderState* state, SDL_Texture* white);

    /// Once a frame before the world's drawn: the weather told (the server's, or the console's
    /// test), the clock, the view and the setting.
    void update(const nox::modes::script::weather::Told& told, std::uint64_t nowNs, const Camera& camera, Level level);
    /// The light pass's ambient under the weather: a storm's darker, lightning's flash brighter.
    Rgb ambient(Rgb base);
    /// Over the world and under the HUD, in `camera`'s space. `lit`: the light pass took
    /// ambient() (else the storm's dark and the flashes are laid over the view here). `sight`
    /// (null: everything seen): weather only over the ground that's seen.
    void draw(SDL_Renderer* renderer, const Camera& camera, bool lit, const SightMap* sight);
    /// Between Audio::beginLoops and endLoops: the wind's loop; thunder when it's due.
    void sound(Audio& audio);
    /// Whether anything shows (weather or wind, or particles still landing).
    bool active() const;

private:
    /// The weather as drawn: how much of each, and the wind (a vector: its speed 0-1 as long).
    struct Mix {
        float rain = 0, snow = 0, storm = 0;
        float windX = 0, windY = 0;
    };
    static Mix mixOf(const nox::modes::script::weather::State& s);

    struct Drop { ///< a raindrop or a flake: on the ground's plane at (x, y), `z` above it
        float x = 0, y = 0, z = 0;
        float fall = 0;  ///< units a second
        float size = 0;  ///< its own: a drop's weight 0.6-1, a flake's radius in units
        float phase = 0; ///< a flake's sway
        float landed = -1; ///< a flake lying on the ground: seconds left (-1: falling)
    };
    struct Splash {
        float x = 0, y = 0;
        float age = 0, life = 0, size = 0;
    };
    struct Mote { ///< a mote of dust, or a gust's streak
        float x = 0, y = 0, z = 0;
        float age = 0, life = 0, size = 0, speed = 0;
    };

    float random();
    float between(float lo, float hi) { return lo + (hi - lo) * random(); }
    void simulate(float dt, const Camera& camera);
    void lightning(float dt);
    /// The region the particles live in (the view's ground, with room round it), and wrapping into it.
    struct Region {
        float x0, y0, x1, y1;
    };
    Region region(const Camera& camera, float top) const;
    static void wrap(float& x, float& y, const Region& r);

    // The batch being built, and its draw.
    void quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_FColor color);
    void segment(SDL_FPoint tail, SDL_FPoint head, float halfWidth, SDL_FColor color);
    void box(SDL_FPoint centre, float rx, float ry, SDL_FColor color);
    void flush(SDL_Renderer* renderer, int shape, float z = 0, float w = 0);

    SDL_GPURenderState* state_ = nullptr;
    SDL_Texture* white_ = nullptr;
    Level level_ = Level::On;
    std::uint32_t serial_ = 0;
    bool started_ = false;
    Mix mix_, from_, target_;
    float fadeAt_ = 0, fadeLength_ = 0;
    double clock_ = 0;   ///< seconds of weather drawn (the shapes' animation)
    double lastNs_ = 0;
    float gust_ = 1;     ///< the wind's waves now (about 0.55-1.25)
    float flash_ = 0;    ///< lightning's light now, 0-1
    float nextFlash_ = 4;
    float flashAge_ = -1;
    float flashPower_ = 1;
    bool doubleFlash_ = false;
    float thunderIn_ = 0;        ///< seconds till the thunder (while pending)
    bool thunderPending_ = false;
    bool thunderDue_ = false;    ///< (sound plays it)
    int thunderVolume_ = 0;
    bool thunderNear_ = false;
    float brightness_ = 1; ///< the ambient's brightness the particles are drawn under
    std::uint32_t seed_ = 0x9E3779B9u;
    std::vector<Drop> rain_, snow_;
    std::vector<Splash> splashes_;
    std::vector<Mote> motes_, gusts_;
    float gustDue_ = 0;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

} // namespace noxsdl
