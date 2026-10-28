// The weather's particles, light and sound (weather.hpp).
#include "weather.hpp"

#include "audio.hpp"

#include "noxcore/audio/sound_names.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace noxsdl {

namespace {

namespace wx = nox::modes::script::weather;

constexpr float kPi = 3.14159265f;
/// Particles at full strength over 1024 x 768 units of the world, and the most kept.
constexpr float kRainPerView = 2400, kSnowPerView = 2600, kMotesPerView = 600, kGustsPerSecond = 60;
constexpr std::size_t kMaxRain = 5000, kMaxSnow = 4000, kMaxMotes = 900, kMaxGusts = 90, kMaxSplashes = 800;
/// Units a second: a raindrop's fall, and a wind of 1's push on rain, snow and dust.
constexpr float kRainFall = 920, kRainWind = 430, kSnowWind = 230, kMoteWind = 360;
/// A streak's length: the way its drop goes in this long.
constexpr float kExposure = 0.045f;
/// The wind's loop's key among the audio's loops (no thing's: things' keys are below 2^33).
constexpr std::uint64_t kWindLoop = 0x57'49'4E'44'00'00'00'01ull;

float smooth(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

float lerp(float a, float b, float k)
{
    return a + (b - a) * k;
}

SDL_FColor colour(float r, float g, float b, float a)
{
    return {std::min(r, 1.0f), std::min(g, 1.0f), std::min(b, 1.0f), std::clamp(a, 0.0f, 1.0f)};
}
} // namespace

WeatherFx::WeatherFx(SDL_GPURenderState* state, SDL_Texture* white) : state_(state), white_(white) {}

WeatherFx::Mix WeatherFx::mixOf(const wx::State& s)
{
    Mix m;
    const float k = static_cast<float>(s.strength) / 255.0f;
    switch (s.kind) {
    case wx::Kind::Rain: m.rain = k; break;
    case wx::Kind::Snow: m.snow = k; break;
    case wx::Kind::Storm:
        m.rain = k;
        m.storm = k;
        break;
    case wx::Kind::None: break;
    }
    // (Clockwise on screen from straight up: up is -y on the screen and on the ground.)
    const float a = static_cast<float>(s.windAngle) / 65536.0f * 2 * kPi;
    const float v = static_cast<float>(s.windSpeed) / 255.0f;
    m.windX = std::sin(a) * v;
    m.windY = -std::cos(a) * v;
    return m;
}

float WeatherFx::random()
{
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return static_cast<float>(seed_ & 0xFFFFFFu) / 16777216.0f;
}

bool WeatherFx::active() const
{
    return mix_.rain > 0 || mix_.snow > 0 || mix_.storm > 0 || mix_.windX != 0 || mix_.windY != 0 || !rain_.empty() || !snow_.empty() ||
           !splashes_.empty() || !motes_.empty() || !gusts_.empty() || flash_ > 0;
}

void WeatherFx::update(const wx::Told& told, std::uint64_t nowNs, const Camera& camera, Level level)
{
    const double now = static_cast<double>(nowNs) * 1e-9;
    const float dt = lastNs_ > 0 ? static_cast<float>(std::clamp(now - lastNs_, 0.0, 0.1)) : 0.0f;
    lastNs_ = now;
    if (!started_ || told.serial != serial_) {
        // A new weather: blended to from where it is now over its fade (logged, but not the none
        // every game starts with).
        const bool logIt = started_ || told.state != nox::modes::script::weather::State{};
        started_ = true;
        serial_ = told.serial;
        from_ = mix_;
        target_ = mixOf(told.state);
        fadeLength_ = static_cast<float>(told.fade) / 30.0f;
        fadeAt_ = 0;
        if (logIt) {
            SDL_Log("weather: kind %d, strength %d, wind %d/65536 of a turn at %d, over %d frames", static_cast<int>(told.state.kind),
                    told.state.strength, told.state.windAngle, told.state.windSpeed, told.fade);
        }
        if (fadeLength_ <= 0) {
            mix_ = target_;
        }
    }
    if (fadeAt_ < fadeLength_) {
        fadeAt_ += dt;
        const float k = smooth(fadeAt_ / fadeLength_);
        mix_ = {lerp(from_.rain, target_.rain, k), lerp(from_.snow, target_.snow, k), lerp(from_.storm, target_.storm, k),
                lerp(from_.windX, target_.windX, k), lerp(from_.windY, target_.windY, k)};
    }
    if (level != level_) {
        level_ = level;
        if (level_ == Level::Off) {
            rain_.clear();
            snow_.clear();
            splashes_.clear();
            motes_.clear();
            gusts_.clear();
        }
    }
    if (level_ == Level::Off || !active()) {
        flash_ = 0;
        thunderPending_ = false;
        return;
    }
    clock_ += dt;
    // The wind comes in waves.
    const auto t = static_cast<float>(std::fmod(clock_, 10000.0));
    gust_ = 0.9f + 0.22f * std::sin(t * 0.83f + 1.7f * std::sin(t * 0.29f)) + 0.13f * std::sin(t * 2.3f + 0.6f * std::sin(t * 0.71f));
    lightning(dt);
    simulate(dt, camera);
}

void WeatherFx::lightning(float dt)
{
    if (thunderPending_) {
        thunderIn_ -= dt;
        if (thunderIn_ <= 0) {
            thunderPending_ = false;
            thunderDue_ = true;
        }
    }
    const float storm = mix_.storm;
    if (storm < 0.05f && flashAge_ < 0) {
        flash_ = 0;
        nextFlash_ = between(2.0f, 5.0f);
        return;
    }
    if (flashAge_ < 0) {
        nextFlash_ -= dt;
        if (nextFlash_ <= 0) {
            flashAge_ = 0;
            flashPower_ = between(0.65f, 1.0f) * (0.45f + 0.55f * std::min(storm, 1.0f));
            doubleFlash_ = random() < 0.6f;
            thunderIn_ = between(0.35f, 2.4f);
            thunderNear_ = thunderIn_ < 1.1f;
            thunderPending_ = true;
            thunderVolume_ = static_cast<int>(std::lround((thunderNear_ ? 100.0f : 70.0f) * (0.5f + 0.5f * std::min(storm, 1.0f))));
            nextFlash_ = between(4.0f, 11.0f) / (0.5f + 0.7f * std::min(storm, 1.0f));
        }
    }
    if (flashAge_ >= 0) {
        flashAge_ += dt;
        const float a = flashAge_;
        float e = 0;
        if (level_ == Level::Reduced) {
            // (Reduced: one soft swell, no flicker.)
            e = a < 0.15f ? a / 0.15f : std::exp(-(a - 0.15f) * 4.5f);
            e *= 0.45f;
        } else if (a < 0.06f) {
            e = 1;
        } else if (doubleFlash_ && a < 0.13f) {
            e = 0.2f;
        } else if (doubleFlash_ && a < 0.2f) {
            e = 0.9f;
        } else {
            e = std::exp(-(a - (doubleFlash_ ? 0.2f : 0.06f)) * 6.5f);
        }
        flash_ = e * flashPower_;
        if (a > 1.3f) {
            flashAge_ = -1;
            flash_ = 0;
        }
    }
}

WeatherFx::Region WeatherFx::region(const Camera& camera, float top) const
{
    constexpr float m = 60;
    return {camera.left() - m, camera.top() - m, camera.right() + m, camera.bottom() + top + m};
}

void WeatherFx::wrap(float& x, float& y, const Region& r)
{
    const float w = r.x1 - r.x0;
    const float h = r.y1 - r.y0;
    if (x < r.x0 || x > r.x1) {
        x = r.x0 + std::fmod(std::fmod(x - r.x0, w) + w, w);
    }
    if (y < r.y0 || y > r.y1) {
        y = r.y0 + std::fmod(std::fmod(y - r.y0, h) + h, h);
    }
}

void WeatherFx::simulate(float dt, const Camera& camera)
{
    const float viewW = static_cast<float>(camera.viewWidth) / camera.zoom;
    const float viewH = static_cast<float>(camera.viewHeight) / camera.zoom;
    const float density = std::clamp(viewW * viewH / (1024.0f * 768.0f), 0.25f, 3.0f) * (level_ == Level::Reduced ? 0.45f : 1.0f);
    const float wx = mix_.windX * gust_;
    const float wy = mix_.windY * gust_;
    const float windSpeed = std::hypot(mix_.windX, mix_.windY);
    const bool fadingIn = fadeAt_ < fadeLength_; // (new drops come in from above, not out of the air)
    const bool instant = fadeLength_ <= 0;
    const float viewL = camera.left(), viewT = camera.top(), viewR = camera.right(), viewB = camera.bottom();

    // Rain: falling from a height that brings it in from above the view.
    {
        const float top = std::max(300.0f, viewH * 0.8f);
        const Region r = region(camera, top);
        const float amount = std::clamp(mix_.rain + 0.6f * mix_.storm, 0.0f, 1.6f);
        const auto want = std::min(kMaxRain, static_cast<std::size_t>(amount * kRainPerView * density));
        const float storm = std::min(mix_.storm, 1.0f);
        const auto made = [&](bool anyHeight) {
            Drop d;
            d.size = between(0.55f, 1.0f);
            d.fall = kRainFall * between(0.85f, 1.15f) * (0.85f + 0.15f * d.size) * (1 + 0.2f * storm);
            d.x = between(r.x0, r.x1);
            d.y = between(r.y0, r.y1);
            d.z = anyHeight ? between(0.0f, top) : top * between(0.9f, 1.1f);
            return d;
        };
        while (rain_.size() < want) {
            rain_.push_back(made(!fadingIn));
        }
        if (instant && rain_.size() > want) {
            rain_.resize(want);
        }
        const float push = kRainWind * (1 + 0.35f * storm);
        for (std::size_t i = 0; i < rain_.size();) {
            Drop& d = rain_[i];
            const float p = push * (0.75f + 0.25f * d.size);
            d.x += wx * p * dt;
            d.y += wy * p * dt;
            d.z -= d.fall * dt;
            if (d.z <= 0) {
                // Landed: a splash where it's seen; then up again (or gone, past what's wanted).
                if (d.x >= viewL && d.x <= viewR && d.y >= viewT && d.y <= viewB && splashes_.size() < kMaxSplashes && random() < 0.8f) {
                    splashes_.push_back({d.x, d.y, 0, between(0.22f, 0.36f), between(3.5f, 7.0f) * (0.7f + 0.3f * d.size) * (1 + 0.3f * storm)});
                }
                if (rain_.size() > want) {
                    d = rain_.back();
                    rain_.pop_back();
                    continue;
                }
                const float under = d.z;
                d = made(false);
                d.z += under;
            }
            if (d.x < r.x0 || d.x > r.x1 || d.y < r.y0 || d.y > r.y1) {
                wrap(d.x, d.y, r);
            }
            ++i;
        }
    }
    for (std::size_t i = 0; i < splashes_.size();) {
        Splash& s = splashes_[i];
        s.age += dt;
        if (s.age >= s.life) {
            s = splashes_.back();
            splashes_.pop_back();
            continue;
        }
        ++i;
    }

    // Snow: slow, swaying, carried by the wind; a flake lies a moment where it lands.
    {
        const float top = std::max(260.0f, viewH * 0.6f);
        const Region r = region(camera, top);
        const auto want = std::min(kMaxSnow, static_cast<std::size_t>(std::clamp(mix_.snow, 0.0f, 1.0f) * kSnowPerView * density));
        const auto made = [&](bool anyHeight) {
            Drop d;
            const float k = random();
            d.size = 1.1f + 2.7f * k * k; // (more small flakes than big)
            d.fall = (34.0f + 20.0f * d.size) * between(0.8f, 1.2f);
            d.phase = between(0.0f, 2 * kPi);
            d.x = between(r.x0, r.x1);
            d.y = between(r.y0, r.y1);
            d.z = anyHeight ? between(0.0f, top) : top * between(0.85f, 1.05f);
            return d;
        };
        while (snow_.size() < want) {
            snow_.push_back(made(!fadingIn));
        }
        if (instant && snow_.size() > want) {
            snow_.resize(want);
        }
        const auto c = static_cast<float>(clock_);
        for (std::size_t i = 0; i < snow_.size();) {
            Drop& d = snow_[i];
            if (d.landed >= 0) {
                d.landed -= dt;
                if (d.landed < 0) {
                    if (snow_.size() > want) {
                        d = snow_.back();
                        snow_.pop_back();
                        continue;
                    }
                    d = made(false);
                }
            } else {
                // The sway: a slow side to side and a little bob, each flake in its own time.
                const float f = 1.1f + 0.25f * d.size;
                const float swayX = std::cos(c * f + d.phase) * (9.0f + 3.0f * d.size) * f;
                const float swayY = std::sin(c * f * 0.7f + d.phase * 1.3f) * 4.0f;
                const float carry = kSnowWind * (1.3f - 0.12f * d.size);
                d.x += (wx * carry + swayX) * dt;
                d.y += (wy * carry + swayY) * dt;
                d.z -= d.fall * dt;
                if (d.z <= 0) {
                    d.z = 0;
                    d.landed = between(0.5f, 1.3f);
                }
            }
            if (d.x < r.x0 || d.x > r.x1 || d.y < r.y0 || d.y > r.y1) {
                wrap(d.x, d.y, r);
            }
            ++i;
        }
    }

    // Wind: motes of dust low over the ground (fewer when it's wet), and gusts of streaks.
    {
        const Region r = region(camera, 40);
        const float wet = std::min(1.0f, mix_.rain + mix_.snow);
        const auto want = std::min(kMaxMotes, static_cast<std::size_t>(std::pow(windSpeed, 1.3f) * kMotesPerView * density * (1 - 0.75f * wet)));
        while (motes_.size() < want) {
            Mote m;
            m.x = between(r.x0, r.x1);
            m.y = between(r.y0, r.y1);
            m.z = between(0.0f, 26.0f);
            m.life = between(1.4f, 3.2f);
            m.age = between(0.0f, m.life); // (in step from the start: not all fading in at once)
            m.size = between(1.2f, 2.6f);
            m.speed = between(0.7f, 1.15f);
            motes_.push_back(m);
        }
        const auto c = static_cast<float>(clock_);
        for (std::size_t i = 0; i < motes_.size();) {
            Mote& m = motes_[i];
            m.age += dt;
            if (m.age >= m.life) {
                if (motes_.size() > want) {
                    m = motes_.back();
                    motes_.pop_back();
                    continue;
                }
                m.x = between(r.x0, r.x1);
                m.y = between(r.y0, r.y1);
                m.z = between(0.0f, 26.0f);
                m.age = 0;
            }
            // Carried, with eddies that swirl it about.
            const float ex = std::sin(m.y * 0.031f + c * 2.3f) * 30.0f;
            const float ey = std::cos(m.x * 0.027f + c * 1.9f) * 18.0f;
            m.x += (wx * kMoteWind * m.speed + ex) * dt;
            m.y += (wy * kMoteWind * m.speed + ey) * dt;
            m.z = std::clamp(m.z + std::sin(c * 4.0f + m.x * 0.05f) * 14.0f * dt, 0.0f, 40.0f);
            if (m.x < r.x0 || m.x > r.x1 || m.y < r.y0 || m.y > r.y1) {
                wrap(m.x, m.y, r);
            }
            ++i;
        }
        // Gusts: more as the wind rises in its waves.
        if (windSpeed > 0.02f) {
            const Region g = region(camera, 90);
            gustDue_ += std::pow(windSpeed, 1.5f) * kGustsPerSecond * density * gust_ * gust_ * dt;
            while (gustDue_ >= 1 && gusts_.size() < kMaxGusts) {
                gustDue_ -= 1;
                Mote m;
                m.x = between(g.x0, g.x1);
                m.y = between(g.y0, g.y1);
                m.z = between(8.0f, 90.0f);
                m.life = between(0.5f, 1.1f);
                m.size = between(90.0f, 240.0f); // (its length)
                m.speed = between(0.9f, 1.25f);
                gusts_.push_back(m);
            }
            gustDue_ = std::min(gustDue_, 1.0f);
        }
        for (std::size_t i = 0; i < gusts_.size();) {
            Mote& m = gusts_[i];
            m.age += dt;
            if (m.age >= m.life) {
                m = gusts_.back();
                gusts_.pop_back();
                continue;
            }
            m.x += wx * kMoteWind * 2.0f * m.speed * dt;
            m.y += wy * kMoteWind * 2.0f * m.speed * dt;
            ++i;
        }
    }
}

Rgb WeatherFx::ambient(Rgb base)
{
    if (level_ == Level::Off || !active()) {
        brightness_ = static_cast<float>(std::max({base[0], base[1], base[2]})) / 255.0f;
        return base;
    }
    // Rain and storm darker and cooler, snow a touch cooler; lightning's flash toward white-blue.
    const float wet = std::clamp(mix_.rain, 0.0f, 1.0f);
    const float storm = std::clamp(mix_.storm, 0.0f, 1.0f);
    const float snow = std::clamp(mix_.snow, 0.0f, 1.0f);
    const float dark = 1 - 0.45f * storm - 0.1f * std::max(0.0f, wet - storm);
    float r = base[0] * dark * (1 - 0.06f * wet) * (1 - 0.04f * snow);
    float g = base[1] * dark * (1 - 0.03f * wet) * (1 - 0.015f * snow);
    float b = base[2] * dark;
    const float f = std::clamp(flash_, 0.0f, 1.0f);
    r += (255 - r) * f * 0.8f;
    g += (255 - g) * f * 0.86f;
    b += (255 - b) * f * 0.95f;
    brightness_ = std::max({r, g, b}) / 255.0f;
    const auto byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    return {byte(r), byte(g), byte(b)};
}

void WeatherFx::quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_FColor color)
{
    const int base = static_cast<int>(vertices_.size());
    vertices_.push_back({a, color, {0, 0}});
    vertices_.push_back({b, color, {1, 0}});
    vertices_.push_back({c, color, {1, 1}});
    vertices_.push_back({d, color, {0, 1}});
    indices_.insert(indices_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void WeatherFx::segment(SDL_FPoint tail, SDL_FPoint head, float halfWidth, SDL_FColor color)
{
    float dx = head.x - tail.x;
    float dy = head.y - tail.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) {
        dx = 0;
        dy = 1;
    } else {
        dx /= len;
        dy /= len;
    }
    // (The head reaches a half width past the point: its round end inside the quad.)
    const SDL_FPoint h{head.x + dx * halfWidth, head.y + dy * halfWidth};
    const SDL_FPoint side{-dy * halfWidth, dx * halfWidth};
    quad({tail.x - side.x, tail.y - side.y}, {tail.x + side.x, tail.y + side.y}, {h.x + side.x, h.y + side.y}, {h.x - side.x, h.y - side.y}, color);
}

void WeatherFx::box(SDL_FPoint centre, float rx, float ry, SDL_FColor color)
{
    quad({centre.x - rx, centre.y - ry}, {centre.x + rx, centre.y - ry}, {centre.x + rx, centre.y + ry}, {centre.x - rx, centre.y + ry}, color);
}

void WeatherFx::flush(SDL_Renderer* renderer, int shape, float z, float w)
{
    if (vertices_.empty()) {
        return;
    }
    const float uniforms[4] = {static_cast<float>(shape), static_cast<float>(std::fmod(clock_, 3600.0)), z, w};
    SDL_SetGPURenderStateFragmentUniforms(state_, 0, uniforms, sizeof(uniforms));
    SDL_SetTextureBlendMode(white_, SDL_BLENDMODE_BLEND);
    SDL_SetGPURenderState(renderer, state_);
    SDL_RenderGeometry(renderer, white_, vertices_.data(), static_cast<int>(vertices_.size()), indices_.data(), static_cast<int>(indices_.size()));
    SDL_SetGPURenderState(renderer, nullptr);
    vertices_.clear();
    indices_.clear();
}

void WeatherFx::draw(SDL_Renderer* renderer, const Camera& camera, bool lit, const SightMap* sight)
{
    if (level_ == Level::Off || !state_ || !white_ || !active()) {
        return;
    }
    const float zoom = camera.zoom;
    const float left = camera.left();
    const float top = camera.top();
    const auto vw = static_cast<float>(camera.viewWidth);
    const auto vh = static_cast<float>(camera.viewHeight);
    const auto screen = [&](float x, float y, float h) { return SDL_FPoint{(x - left) * zoom, (y - h - top) * zoom}; };
    const auto seen = [&](SDL_FPoint p, float pad) { return p.x >= -pad && p.y >= -pad && p.x <= vw + pad && p.y <= vh + pad; };
    const float storm = std::clamp(mix_.storm, 0.0f, 1.0f);
    const float wet = std::clamp(mix_.rain, 0.0f, 1.0f);
    // How lit the weather is: the ambient it falls through (all of it with the lighting off), and
    // the lightning.
    const float light = std::clamp((lit ? brightness_ : 1.0f) + flash_ * 0.5f, 0.0f, 1.3f);
    const float rainLight = 0.65f + 0.4f * std::min(light, 1.2f); // (rain catches what light there is)
    const float dustLight = 0.62f + 0.4f * std::min(light, 1.2f);

    SDL_BlendMode oldBlend = SDL_BLENDMODE_BLEND;
    SDL_GetRenderDrawBlendMode(renderer, &oldBlend);
    if (!lit && (storm > 0 || wet > 0)) {
        // No light pass to take the storm's dark: laid over the view.
        const float k = 1 - 0.4f * storm - 0.08f * std::max(0.0f, wet - storm);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_MOD);
        SDL_SetRenderDrawColorFloat(renderer, k * 0.96f, k * 0.98f, k, 1);
        const SDL_FRect all{0, 0, vw, vh};
        SDL_RenderFillRect(renderer, &all);
    }

    // Only over the ground that's seen (the line of sight's dark has none).
    const auto seenAt = [&](float x, float y) { return !sight || sight->visible(x, y); };

    // Sheets of distant rain drifting over the ground: a grid over the view whose corners fade
    // out where the ground isn't seen, so they follow the line of sight softly. The wind's own: dust
    // blown along it in the same way, in its waves (fewer when it's wet).
    const auto sheet = [&](SDL_FColor c, float dirX, float dirY) {
        constexpr float kCell = 40; // (world units)
        const int nx = static_cast<int>(std::ceil(vw / zoom / kCell)) + 1;
        const int ny = static_cast<int>(std::ceil(vh / zoom / kCell)) + 1;
        const int base = static_cast<int>(vertices_.size());
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const float sx = std::min(static_cast<float>(i) * kCell * zoom, vw);
                const float sy = std::min(static_cast<float>(j) * kCell * zoom, vh);
                const float wx = left + sx / zoom;
                const float wy = top + sy / zoom;
                SDL_FColor v = c;
                v.a = seenAt(wx, wy) ? c.a : 0;
                vertices_.push_back({{sx, sy}, v, {wx / 256.0f, wy / 256.0f}});
            }
        }
        for (int j = 0; j + 1 < ny; ++j) {
            for (int i = 0; i + 1 < nx; ++i) {
                const int a = base + j * nx + i;
                indices_.insert(indices_.end(), {a, a + 1, a + nx + 1, a, a + nx + 1, a + nx});
            }
        }
        const float len = std::max(std::hypot(dirX, dirY), 1e-4f);
        flush(renderer, 4, dirX / len, dirY / len);
    };
    const float windSpeed = std::hypot(mix_.windX, mix_.windY);
    const float dustAmount = std::clamp(windSpeed * gust_, 0.0f, 1.2f) * (1 - 0.8f * std::min(1.0f, wet + mix_.snow));
    if (dustAmount > 0.02f) {
        sheet(colour(0.9f * dustLight, 0.82f * dustLight, 0.66f * dustLight, dustAmount * 0.08f), mix_.windX, mix_.windY);
    }
    const float sheetAmount = std::clamp(wet * 0.6f + storm * 0.5f, 0.0f, 1.0f);
    if (sheetAmount > 0.01f) {
        sheet(colour(0.80f * light, 0.84f * light, 0.92f * light, sheetAmount * 0.1f), mix_.windX * gust_ * kRainWind,
              mix_.windY * gust_ * kRainWind + kRainFall);
    }

    // Splashes: rings spreading on the ground, and a fleck at their start.
    if (!splashes_.empty()) {
        for (const Splash& s : splashes_) {
            const float k = s.age / s.life;
            const SDL_FPoint p = screen(s.x, s.y, 0);
            const float rx = s.size * (0.3f + 0.7f * k) * zoom;
            if (!seen(p, rx) || !seenAt(s.x, s.y)) {
                continue;
            }
            const float a = std::pow(1 - k, 1.2f) * 0.85f;
            box(p, rx, rx * 0.42f, colour(0.85f * rainLight, 0.9f * rainLight, 1.0f * rainLight, a));
        }
        flush(renderer, 2, 0.28f);
        for (const Splash& s : splashes_) {
            const float k = s.age / s.life;
            if (k > 0.45f) {
                continue;
            }
            // (The fleck thrown up: a dot rising and falling back.)
            const float h = std::sin(k / 0.45f * kPi) * s.size * 0.9f;
            const SDL_FPoint p = screen(s.x, s.y, h);
            if (seen(p, 4)) {
                const float r = std::max(1.0f, 0.75f * zoom);
                box(p, r, r, colour(0.85f * light, 0.9f * light, 1.0f * light, (1 - k / 0.45f) * 0.6f));
            }
        }
        flush(renderer, 1);
    }

    // Dust in the wind.
    if (!motes_.empty()) {
        for (const Mote& m : motes_) {
            const SDL_FPoint p = screen(m.x, m.y, m.z);
            const float r = std::max(1.1f, m.size * zoom) * 1.5f;
            if (!seen(p, r) || !seenAt(m.x, m.y)) {
                continue;
            }
            const float a = std::sin(kPi * m.age / m.life) * 0.8f;
            box(p, r, r, colour(0.95f * dustLight, 0.88f * dustLight, 0.72f * dustLight, a));
        }
        flush(renderer, 1);
    }

    // Snow.
    if (!snow_.empty()) {
        const float shine = std::clamp(0.72f + 0.4f * light, 0.0f, 1.2f); // (snow shows in the dark too)
        for (const Drop& d : snow_) {
            const SDL_FPoint p = screen(d.x, d.y, d.z);
            const float r = std::max(1.2f, d.size * zoom) * 1.6f;
            if (!seen(p, r) || !seenAt(d.x, d.y)) {
                continue;
            }
            float a = 0.5f + 0.13f * d.size;
            if (d.landed >= 0) {
                a *= std::min(1.0f, d.landed / 0.5f) * 0.75f;
            }
            box(p, r, r, colour(0.94f * shine, 0.96f * shine, 1.0f * shine, a));
        }
        flush(renderer, 1);
    }

    // Rain: each drop's streak along its way on the screen this instant.
    if (!rain_.empty()) {
        const float push = kRainWind * (1 + 0.35f * storm);
        for (const Drop& d : rain_) {
            const SDL_FPoint head = screen(d.x, d.y, d.z);
            const float p = push * (0.75f + 0.25f * d.size);
            const float vx = mix_.windX * gust_ * p;
            const float vy = mix_.windY * gust_ * p + d.fall;
            const SDL_FPoint tail{head.x - vx * kExposure * zoom, head.y - vy * kExposure * zoom};
            if ((!seen(head, 4) && !seen(tail, 4)) || !seenAt(d.x, d.y)) {
                continue;
            }
            const float half = std::max(1.1f, (0.6f + 0.5f * d.size) * zoom);
            const float a = (0.36f + 0.42f * d.size) * (0.85f + 0.2f * storm);
            segment(tail, head, half, colour(0.84f * rainLight, 0.9f * rainLight, 1.0f * rainLight, a));
        }
        flush(renderer, 0);
    }

    // Gusts' streaks.
    if (!gusts_.empty()) {
        const float speed = std::hypot(mix_.windX, mix_.windY);
        if (speed > 0.001f) {
            const float dx = mix_.windX / speed;
            const float dy = mix_.windY / speed;
            for (const Mote& m : gusts_) {
                const SDL_FPoint head = screen(m.x, m.y, m.z);
                const SDL_FPoint tail{head.x - dx * m.size * zoom, head.y - dy * m.size * zoom};
                if ((!seen(head, 4) && !seen(tail, 4)) || !seenAt(m.x, m.y)) {
                    continue;
                }
                const float a = 0.5f * std::min(gust_, 1.2f) * std::sin(kPi * m.age / m.life);
                segment(tail, head, std::max(1.3f, 0.9f * zoom), colour(0.92f * dustLight, 0.94f * dustLight, 0.98f * dustLight, a));
            }
            flush(renderer, 3);
        }
    }

    if (flash_ > 0.01f) {
        // The flash over everything (the light pass has the most of it when it's on).
        const float f = flash_ * (lit ? 0.18f : 0.38f);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColorFloat(renderer, f * 0.82f, f * 0.88f, f, 1);
        const SDL_FRect all{0, 0, vw, vh};
        SDL_RenderFillRect(renderer, &all);
    }
    SDL_SetRenderDrawBlendMode(renderer, oldBlend);
}

void WeatherFx::sound(Audio& audio)
{
    static const std::optional<std::uint32_t> wind = nox::audio::soundId("LoopAmbWindWL1");
    static const std::optional<std::uint32_t> closeClap = nox::audio::soundId("EarthRumbleMajor");
    static const std::optional<std::uint32_t> distantRoll = nox::audio::soundId("EarthRumbleMinor");
    if (level_ == Level::Off) {
        thunderDue_ = false;
        return; // (the wind's loop, not named, stops)
    }
    const float speed = std::hypot(mix_.windX, mix_.windY);
    const int volume = static_cast<int>(std::lround(std::clamp(speed * gust_ * 80.0f + std::min(mix_.storm, 1.0f) * 25.0f, 0.0f, 100.0f)));
    if (wind && volume > 0) {
        audio.loopAt(kWindLoop, *wind, volume);
    }
    if (thunderDue_) {
        thunderDue_ = false;
        if (const auto& id = thunderNear_ ? closeClap : distantRoll) {
            audio.play(*id, thunderVolume_);
        }
    }
}
} // namespace noxsdl
