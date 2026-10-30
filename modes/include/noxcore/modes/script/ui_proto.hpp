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
// A UI of API 3 (kBegin's api byte 3) and everything of it above go only to a client at client
// level 3 or above (kUiApi3Level) that named ModeUi and ModeUi2; the bytes are API 2's.
// and, only to a client at client level 1 or above that named ModeUi (any script mode, UI or none:
// nox.weather; weather.hpp), the weather it draws:
//   kWeather    u8 kind (0 none, 1 rain, 2 snow, 3 storm; another: none), u8 strength, u16 the
//               wind's angle (1/65536 of a turn, clockwise on screen, 0 up), u8 its speed,
//               u16 fade (server frames)                                    (7 bytes)
// and, only to a client at client level 2 or above (kBookLevel) that named ModeUi, from a script
// mode whose configure set bookIsTheMod (the host's modeOwnsBook), at each client's ready:
//   kBook       u8 1: the book is the mode's (the client's book lists only what it was granted,
//               not a network game's every spell, ability and creature); 0: the stock book
//               (bytes past the first are left for later versions)
// and, only to a client at client level 5 or above (kDrawScaleLevel) that named ModeUi, from a
// script mode that called nox.setDrawScale: a thing's draw scale, when it's set (to the clients
// that know the thing then) and when a client is first sent the thing (its record new to it):
//   kDrawScale  u16 the thing's net code, u16 its type (its net type id), u16 the scale in 1/256
//               (128-768: 0.5-3; 256: as its type's, the scale let go)    (6 bytes)
// A UI of API 4 (kBegin's api byte 4) and everything of it go only to a client at client level 5
// or above (kUiApi4Level) that named ModeUi and ModeUi2; the bytes are API 2's.
// and, only to a client at client level 6 or above (kBigDataLevel), a value too big for one kData
// (nox.send over kMaxValue bytes, to kMaxBigValue), in order with the kData before and after it:
//   kDataBegin  u32 the value's bytes, u8 coding (0 stored, 1 NXZ), u32 the coded bytes
//   kDataChunk  u32 offset, the coded bytes there                            (in order)
// (decoded: a value for the UI's onMessage, read with the kBigValue limits below).
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
    kBook = 10,
    kDrawScale = 11,
    kDataBegin = 12,
    kDataChunk = 13,
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

/// The lowest client level (lobby::kClientLevel's list) a server sends kBook to.
inline constexpr std::uint16_t kBookLevel = 2;
/// The lowest client level a server sends a UI of API 3 to (with ModeUi and ModeUi2 named): every
/// sub of it (its assets, kBegin, chunks, data, views). Below it the client gets the views as text.
inline constexpr std::uint16_t kUiApi3Level = 3;
/// The lowest client level a server sends a UI of API 4 to (as kUiApi3Level's for API 3).
inline constexpr std::uint16_t kUiApi4Level = 5;
/// The lowest client level a server sends a UI of API 5 to (nox.hudReserve: as kUiApi3Level's).
inline constexpr std::uint16_t kUiApi5Level = 6;
/// The lowest client level a server sends kDrawScale to (with ModeUi named).
inline constexpr std::uint16_t kDrawScaleLevel = 5;
/// The lowest client level a server sends kDataBegin and kDataChunk to (a big value).
inline constexpr std::uint16_t kBigDataLevel = 6;
/// A big value's bytes at most (encoded, before packing), and the depth and pairs a table it's
/// read with (the mod store's: value_codec.hpp's CodecLimits).
inline constexpr std::size_t kMaxBigValue = 256 * 1024;
inline constexpr int kBigValueDepth = 16;
inline constexpr int kBigValuePairs = 8192;

/// kBook's message: `modes` 1, the book is the mode's; 0, the stock book.
inline std::vector<std::uint8_t> bookMessage(bool modes)
{
    const std::uint8_t payload[1] = {static_cast<std::uint8_t>(modes ? 1 : 0)};
    return message(kBook, payload);
}

/// The draw scales a mode may give (nox.setDrawScale), and kDrawScale's unit (1/256).
inline constexpr float kMinDrawScale = 0.5f;
inline constexpr float kMaxDrawScale = 3.0f;
inline constexpr int kDrawScaleUnit = 256;

/// kDrawScale's payload.
struct DrawScale {
    std::uint16_t object = 0; ///< its net code
    std::uint16_t type = 0;   ///< its net type id
    float scale = 1;          ///< (1: let go)
    bool operator==(const DrawScale&) const = default;
};

/// kDrawScale's message: `scale` kept to 0.5-3 (1: as its type's), in 1/256.
inline std::vector<std::uint8_t> drawScaleMessage(std::uint16_t object, std::uint16_t type, float scale)
{
    const float s = scale == scale ? std::clamp(scale, kMinDrawScale, kMaxDrawScale) : 1.0f;
    const auto units = static_cast<std::uint16_t>(static_cast<int>(s * kDrawScaleUnit + 0.5f));
    const std::uint8_t payload[6] = {static_cast<std::uint8_t>(object), static_cast<std::uint8_t>(object >> 8),
                                     static_cast<std::uint8_t>(type),   static_cast<std::uint8_t>(type >> 8),
                                     static_cast<std::uint8_t>(units),  static_cast<std::uint8_t>(units >> 8)};
    return message(kDrawScale, payload);
}

/// A kDrawScale payload read (bytes past the 6 left for later versions); none: cut short, or no
/// object. A scale out of 0.5-3 is kept to it (a server's numbers are untrusted).
inline std::optional<DrawScale> decodeDrawScale(std::span<const std::uint8_t> payload)
{
    if (payload.size() < 6) {
        return std::nullopt;
    }
    DrawScale d;
    d.object = static_cast<std::uint16_t>(payload[0] | payload[1] << 8);
    d.type = static_cast<std::uint16_t>(payload[2] | payload[3] << 8);
    const int units = payload[4] | payload[5] << 8;
    d.scale = std::clamp(static_cast<float>(units) / kDrawScaleUnit, kMinDrawScale, kMaxDrawScale);
    if (d.object == 0) {
        return std::nullopt;
    }
    return d;
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
