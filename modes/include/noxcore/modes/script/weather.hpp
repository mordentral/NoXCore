// The weather a script mode turns on for NoX Core players (nox.weather), as MSG_NOXCPP_MODEUI's
// kWeather carries it (ui_proto.hpp): a fixed 7 bytes,
//   u8 kind (0 none, 1 rain, 2 snow, 3 storm), u8 strength (0-255), u16 the wind's angle
//   (1/65536 of a turn, clockwise on screen, 0: blowing toward the top), u8 the wind's speed
//   (0-255), u16 fade (server frames, from the weather before),
// little-endian. A kind the client doesn't know is none. The server only says which weather; the
// client draws it with its own shaders (noxsdl's weather.cpp), and nothing in the game changes.
#pragma once

#include "noxcore/modes/script/ui_proto.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nox::modes::script::weather {

enum class Kind : std::uint8_t {
    None = 0,
    Rain = 1,
    Snow = 2,
    Storm = 3,
};

/// One weather as the wire carries it (a fade is how it's reached, not part of it).
struct State {
    Kind kind = Kind::None;
    std::uint8_t strength = 0;   ///< 0-255 (255: 1)
    std::uint16_t windAngle = 0; ///< 1/65536 of a turn, clockwise on screen, 0: blowing toward the top
    std::uint8_t windSpeed = 0;  ///< 0-255 (255: 1)
    bool operator==(const State&) const = default;
};

/// kWeather's payload's size.
inline constexpr std::size_t kPayloadBytes = 7;

/// A state from nox.weather's numbers: strength and speed 0-1 (clamped), the angle in degrees
/// (clockwise on screen, 0 up; any number, taken round the turn).
inline State make(Kind kind, double strength, double angleDegrees, double speed)
{
    // (Halves rounded up whatever the thread's rounding: a server's runs toward zero with exact
    // maths on, where 0.7 * 255 comes out a hair under 178.5.)
    const auto nearest = [](double v) { return std::floor(v + 0.5 + 1e-7); };
    const auto unit = [&](double v) { return static_cast<std::uint8_t>(nearest(std::fmin(std::fmax(v, 0.0), 1.0) * 255.0)); };
    double turn = std::fmod(angleDegrees, 360.0) / 360.0;
    if (turn < 0) {
        turn += 1.0;
    }
    State s;
    s.kind = kind;
    s.strength = kind == Kind::None ? 0 : unit(strength); // (none has no strength)
    s.windAngle = static_cast<std::uint16_t>(static_cast<std::uint32_t>(nearest(turn * 65536.0)) & 0xFFFFu);
    s.windSpeed = unit(speed);
    return s;
}

/// kWeather's payload.
inline std::vector<std::uint8_t> encode(const State& s, std::uint16_t fade)
{
    uimsg::Writer w;
    w.u8(static_cast<std::uint8_t>(s.kind));
    w.u8(s.strength);
    w.u16(s.windAngle);
    w.u8(s.windSpeed);
    w.u16(fade);
    return std::move(w.out);
}

/// The whole message (framed).
inline std::vector<std::uint8_t> message(const State& s, std::uint16_t fade)
{
    const std::vector<std::uint8_t> payload = encode(s, fade);
    return uimsg::message(uimsg::kWeather, payload);
}

struct Decoded {
    State state;
    std::uint16_t fade = 0;
};

/// A kWeather payload read (bytes past the 7 are left for later versions); none: cut short.
inline std::optional<Decoded> decode(std::span<const std::uint8_t> payload)
{
    if (payload.size() < kPayloadBytes) {
        return std::nullopt;
    }
    uimsg::Reader r{payload};
    Decoded d;
    const std::uint8_t kind = r.u8();
    d.state.kind = kind <= static_cast<std::uint8_t>(Kind::Storm) ? static_cast<Kind>(kind) : Kind::None;
    d.state.strength = r.u8();
    d.state.windAngle = r.u16();
    d.state.windSpeed = r.u8();
    d.fade = r.u16();
    if (d.state.kind == Kind::None) {
        d.state.strength = 0; // (none has no strength: one none is like another)
    }
    return d;
}

/// What a client keeps of the last one it was sent: the state, its fade, and a count that moves
/// each time it's told (or cleared), so the drawing knows a new one came.
struct Told {
    State state;
    std::uint16_t fade = 0;
    std::uint32_t serial = 0;
};

} // namespace nox::modes::script::weather
