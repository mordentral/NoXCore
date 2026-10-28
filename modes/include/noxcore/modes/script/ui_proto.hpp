// MSG_NOXCPP_MODEUI (0xF8): a script mode's UI between its server and a NoX Core client that asked
// for it (its extended query named lobby::ClientFeature::ModeUi; nobody else is ever sent one).
// Framed as every NoX Core mode message: {u8 0xF8, u8 sub, u16 the rest's length, the rest}.
//
// Server -> client:
//   kBegin  u8 api, u32 the source's size, str the mode's id, str its name   (a UI coming)
//   kChunk  u32 offset, the source's bytes there                             (in order)
//   kData   a value (value codec) for the UI's onMessage
//   kView   a view as a value ({kind = "say" | "choice" | "table", ...}) for the UI's onView
//   kDrop   (nothing)                                                        (the UI gone: run no more)
// and, only to a client that also named ClientFeature::ModeUi2, for a UI of API 2 (mode.json's
// "uiApi"), its mod's assets (mod_assets.hpp) before its kBegin, each asset's chunks in order:
//   kAssets     u16 count, u32 images' bytes, u32 sounds' bytes   (a new set: the last one gone)
//   kAsset      u16 index (0 on, in order), u8 kind (1 image, 2 sound), str name,
//               image: u16 width, u16 height; sound: u32 rate, u8 channels, u32 frames;
//               then u8 coding (0 stored, 1 NXZ), u32 the coded bytes   (an asset coming)
//   kAssetChunk u16 index, u32 offset, the coded bytes there                   (in order)
// (decoded: an image RGBA8, rows top first; a sound PCM16, interleaved). A reload's UI that
// keeps the same set sends none; a kDrop leaves the set where it is.
// and, only to a client at client level 1 or above that named ModeUi (any script mode, UI or none:
// nox.weather; weather.hpp), the weather it draws:
//   kWeather    u8 kind (0 none, 1 rain, 2 snow, 3 storm; another: none), u8 strength, u16 the
//               wind's angle (1/65536 of a turn, clockwise on screen, 0 up), u8 its speed,
//               u16 fade (server frames)                                    (7 bytes)
// Client -> server:
//   kReady  u8 1 when the UI draws views itself (it has onView), else 0     (it runs)
//   kSend   a value from the UI, for the server script's onUiMessage
//   kAnswer u8 an open choice's answer (1-9)
//   kFailed str why (too big, a compile or run error)                        (views stay text)
// A str is u16 its length, then its UTF-8 bytes.
#pragma once

#include "noxcore/modes/mode_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::modes::script::uimsg {

inline constexpr std::uint8_t kOp = 0xF8;

enum Sub : std::uint8_t {
    kBegin = 1,
    kChunk = 2,
    kData = 3,
    kView = 4,
    kDrop = 5,
    kAssets = 6,
    kAsset = 7,
    kAssetChunk = 8,
    kWeather = 9,
    kReady = 16,
    kSend = 17,
    kAnswer = 18,
    kFailed = 19,
};

/// The most UI source a server sends, and a client takes (section 10.3's cap).
inline constexpr std::size_t kMaxSource = 256 * 1024;
/// A chunk's bytes at most (each its own reliable message, as each value is).
inline constexpr std::size_t kChunkBytes = 900;
/// A value's encoding at most (data, views, sends): each is one reliable message, and a reliable
/// packet holds about 1 KB. Bigger: nox.send errors, a view goes as its text form instead.
inline constexpr std::size_t kMaxValue = 900;

/// One message: `sub` and its payload framed (mode_frame.hpp).
inline std::vector<std::uint8_t> message(Sub sub, std::span<const std::uint8_t> payload = {})
{
    return modes::framed(kOp, sub, payload);
}

/// A message's sub and payload; none: not one (or cut short).
inline std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m)
{
    return modes::unframed(kOp, m);
}

/// Little-endian writing and reading for the payloads.
struct Writer {
    std::vector<std::uint8_t> out;
    void u8(std::uint8_t v) { out.push_back(v); }
    void u16(std::uint16_t v)
    {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void str(std::string_view s)
    {
        const std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
        out.push_back(static_cast<std::uint8_t>(n));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }
};

struct Reader {
    std::span<const std::uint8_t> in;
    std::size_t at = 0;
    bool ok = true;
    std::uint8_t u8()
    {
        if (at + 1 > in.size()) {
            ok = false;
            return 0;
        }
        return in[at++];
    }
    std::uint16_t u16()
    {
        if (at + 2 > in.size()) {
            ok = false;
            return 0;
        }
        const auto v = static_cast<std::uint16_t>(in[at] | in[at + 1] << 8);
        at += 2;
        return v;
    }
    std::uint32_t u32()
    {
        if (at + 4 > in.size()) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = in[at] | in[at + 1] << 8 | in[at + 2] << 16 | static_cast<std::uint32_t>(in[at + 3]) << 24;
        at += 4;
        return v;
    }
    std::string str()
    {
        if (at + 2 > in.size()) {
            ok = false;
            return {};
        }
        const std::size_t n = in[at] | static_cast<std::size_t>(in[at + 1]) << 8;
        at += 2;
        if (at + n > in.size()) {
            ok = false;
            return {};
        }
        std::string s(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return s;
    }
    std::span<const std::uint8_t> rest() const { return in.subspan(std::min(at, in.size())); }
};

} // namespace nox::modes::script::uimsg
