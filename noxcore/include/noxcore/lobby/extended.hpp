// Finding noxcpp servers in the game list: a query noxsdl's browser sends beside each
// MSG_SERVER_DISCOVER ping, which only a noxcpp server answers.
//
//   query  00 00 F0 "NXSD" u16 version, u32 token          (browser -> server)
//          [version 2: u32 the client's feature bits]
//          [version 3: then u64 its added object types' hash (content::newThingTypesHash)]
//          [version 4: then u16 its client level (kClientLevel); the hash 0 when none]
//   reply  00 00 F0 "NXSV" u16 version, u32 token,         (noxcpp server -> browser)
//          u8 max substeps a frame (1-4: 30-120 Hz), u32 feature bits
//          [version 3: then u64 the host's added object types' hash, u8 n, n bytes: the ids
//           of the packs that add them, joined with ", "]
//          [version 4: then u16 the lowest client level it lets in, u16 its own level; the
//           hash and n 0 when it adds no types]
//
// A query with no client features is version 1's, byte for byte, and one from a game whose
// content packs add no object types is version 2's at most: version 3's tail is only there when
// they do (and a reply's when the host's do), version 4's only from a client above level 0 (and
// a reply's from a host above level 0 or asking for a level). A server from before version 2
// reads the token alone (it never looked at the version or past the token), and one from before
// version 3 the features alone, so a newer tail costs nothing there; a client from before a
// version asks without it, and reads a reply up to its feature bits (or its packs).
//
// A reply too short for its version's tail is no reply (decodeExtendedReply: nullopt); a query
// too short for it names no types or level (extendedQueryTypesHash, extendedQueryClientLevel:
// nullopt), its token still read.
//
// Safe both ways (GAME.EXE's lobby receive): a stock server answers only 0x0C
// (discover) and hands 0x0E-0x14 to the join handshake; anything else must come from a
// connection it already has (matches the slot, address and port) or is dropped, and a
// browser's socket is never one. A stock client never sees a reply: only the query asks for one.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby {

inline constexpr std::uint8_t kExtendedOp = 0xF0;
inline constexpr std::uint16_t kExtendedVersion = 1;

namespace ServerFeature {
inline constexpr std::uint32_t HighRate = 1;    ///< characters and spells at 60-120 Hz to noxsdl
inline constexpr std::uint32_t Compression = 2; ///< compact updates to noxsdl
inline constexpr std::uint32_t Bots = 4;        ///< in-server bots
inline constexpr std::uint32_t EditSession = 8; ///< a collaborative map editor session (the browser's pencil)
inline constexpr std::uint32_t CoopCampaign = 16; ///< a co-op campaign (a NoX Core mode's: the browser shows it so)
inline constexpr std::uint32_t NoxCoreOnly = 32;  ///< stock clients are turned away (GameHost::Options::stockRefusal)
} // namespace ServerFeature

/// What a client says it takes (a version 2 query's): a server sends what needs a feature only to
/// a client that named it.
namespace ClientFeature {
inline constexpr std::uint32_t ModeUi = 1; ///< a script mode's UI (MSG_NOXCPP_MODEUI 0xF8: modes/script)
/// UI API 2 and a mod's assets on MSG_NOXCPP_MODEUI (a client that names it names ModeUi too)
inline constexpr std::uint32_t ModeUi2 = 2;
} // namespace ClientFeature

/// The version whose tail carries the added object types' hash.
inline constexpr std::uint16_t kExtendedTypesVersion = 3;
/// The version whose tail carries the client levels.
inline constexpr std::uint16_t kExtendedLevelVersion = 4;

/// What this build's client takes, as one number: raised by 1 in each release that adds something
/// a server may send only to a client that takes it (a feature keyed "level N or above", where a
/// ClientFeature bit would one day run out). A level means the release understands all of it
/// (every level at or below its own). The releases before levels count as level 0.
///   1  a script mode's weather (MSG_NOXCPP_MODEUI's kWeather, nox.weather: modes/script's
///      weather.hpp), sent only to a client of level 1 or above that also named ClientFeature::ModeUi
///   2  a script mode's book (MSG_NOXCPP_MODEUI's kBook, configure's bookIsTheMod), sent only to a
///      client of level 2 or above that named ModeUi
///   3  UI API 3 (mode.json's "uiApi": 3: lines, tooltips, world labels, HUD windows, drags, the
///      wheel; modes/script's script_ui.hpp), a UI of it sent only to a client of level 3 or above
///      that named ModeUi and ModeUi2 (ui_proto.hpp's kUiApi3Level)
///   4  the fast map send (kFastMapSendLevel: server/map_send.cpp): MSG_MAP_SEND_PACKET blocks of
///      up to 1018 bytes, several a frame, every frame, to a client of level 4 or above, for its
///      first two requests after each map load (a lower one, a stock one and a later request get
///      the stock stream: 512 bytes a block, one each even frame)
///   5  a script mode's draw scales (MSG_NOXCPP_MODEUI's kDrawScale, nox.setDrawScale: a thing's
///      sprite and its burst drawn larger or smaller), sent only to a client of level 5 or above
///      that named ClientFeature::ModeUi; and UI API 4 (mode.json's "uiApi": 4: nox.spellIcon,
///      nox.hitArea, ui.tick, nox.label's keep), a UI of it sent only to a client of level 5 or
///      above that named ModeUi and ModeUi2 (ui_proto.hpp's kDrawScaleLevel and kUiApi4Level)
inline constexpr std::uint16_t kClientLevel = 5;
/// The client level whose map downloads take the fast send (kClientLevel's list, 4).
inline constexpr std::uint16_t kFastMapSendLevel = 4;
/// What a client below a server's lowest level is told (the browser's prompt too), word for word.
inline constexpr std::string_view kClientTooOldNotice = "Your NoX Core is too out of date for this game.";

/// What a noxcpp server says of itself.
struct ExtendedServer {
    std::uint16_t version = kExtendedVersion;
    std::uint8_t maxMultiplier = 1;
    std::uint32_t features = 0;
    std::uint64_t typesHash = 0; ///< its content packs' added object types (0: none; version 3)
    std::string typesPacks;      ///< the packs that add them, ", "-joined (sent up to 255 bytes, cut after a whole one)
    std::uint16_t minClientLevel = 0; ///< the lowest client level it lets in (version 4; 0: any)
    std::uint16_t clientLevel = 0;    ///< its own build's kClientLevel (version 4; 0 in an older one's)
    bool operator==(const ExtendedServer&) const = default;
};

/// The query; with client features, version 2's (none: version 1's, as before); with an added
/// object types' hash, version 3's; with a client level above 0, version 4's.
std::vector<std::uint8_t> encodeExtendedQuery(std::uint32_t token, std::uint32_t clientFeatures = 0, std::uint64_t typesHash = 0,
                                              std::uint16_t clientLevel = 0);
/// The query's token, if `datagram` is one.
std::optional<std::uint32_t> decodeExtendedQuery(std::span<const std::uint8_t> datagram);
/// The query's version, if `datagram` is one (1: a browser's bare one; 2 on: a game's).
std::optional<std::uint16_t> extendedQueryVersion(std::span<const std::uint8_t> datagram);
/// The client's feature bits a query names (version 2's; 0 in an older one's), if `datagram` is one.
std::optional<std::uint32_t> extendedQueryClientFeatures(std::span<const std::uint8_t> datagram);
/// The added object types' hash a query names (version 3's; 0 in an older one's), if `datagram`
/// is one: nullopt too for a version 3 one too short for it.
std::optional<std::uint64_t> extendedQueryTypesHash(std::span<const std::uint8_t> datagram);
/// The client level a query names (version 4's; 0 in an older one's), if `datagram` is one:
/// nullopt too for a version 4 one too short for it.
std::optional<std::uint16_t> extendedQueryClientLevel(std::span<const std::uint8_t> datagram);

std::vector<std::uint8_t> encodeExtendedReply(std::uint32_t token, const ExtendedServer& server);
struct ExtendedReply {
    std::uint32_t token = 0;
    ExtendedServer server;
};
std::optional<ExtendedReply> decodeExtendedReply(std::span<const std::uint8_t> datagram);

} // namespace nox::lobby
