// A Nox game as a server browser shows it: name, map, mode, players, where to join.
//
// Hosts describe their game in the same server-info block wherever they advertise it: the
// record GAME.EXE posts to Westwood Online (XWIS serves it back, see xwis.hpp) and
// the MSG_SERVER_INFO reply to a MSG_SERVER_DISCOVER ping (server_makeServerInfoPacket).
#pragma once

#include "noxcore/net/messages.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby {

inline constexpr std::uint16_t kDefaultGamePort = 18590;

enum class GameAccess : std::uint8_t { Open, Closed, Password };

struct GameListing {
    std::string name;
    std::string map;
    std::uint32_t address = 0; ///< IPv4, host byte order
    std::uint16_t port = kDefaultGamePort;
    int players = 0;
    int maxPlayers = 0;
    std::uint32_t gameFlags = 0; ///< game mode bits (see gameModeName)
    GameAccess access = GameAccess::Open;
    std::uint8_t bannedClasses = 0; ///< 1 warrior, 2 wizard, 4 conjurer
    std::optional<int> pingMin;     ///< the server's ping restriction, if any
    std::optional<int> pingMax;
    int questStage = 0; ///< quest games
    /// XWIS: the game's channel, "#<host>'s_game"; "" for games found otherwise (LAN,
    /// servers.txt).
    std::string channel;
    /// XWIS: the nick (the XWIS login) of whoever hosts it, from its channel; "" if unknown.
    std::string host;
    /// What the game list's description box shows of it (client_gui_serverInfoBlock,
    /// from the client's list entry): the resolution (& 0x7F: 0
    /// 640x480, 1 800x600, 2 1024x768), the spells allowed (bit n spell n), the weapons and the
    /// armour allowed (by bit). A WOL record has them all 0xFF.
    std::uint8_t resolution = 0;
    std::array<std::uint8_t, 20> spells = [] {
        std::array<std::uint8_t, 20> all{};
        all.fill(0xFF);
        return all;
    }();
    std::uint32_t weapons = 0xFFFFFFFFu;
    std::uint32_t armor = 0xFFFFFFFFu;
};

/// The host's nick from an XWIS game channel ("#tak7's_game": "tak7"); "" if it isn't one.
std::string hostOfChannel(std::string_view channel);

/// The game mode for game flags ("ctf", "arena", ...), as the mode bits GAME.EXE uses:
/// 0x10 KOTR, 0x20 CTF, 0x40 flag ball, 0x80 chat, 0x100 arena, 0x400 elimination,
/// 0x800 co-op (solo campaign), 0x1000 quest. "custom" if none is set.
std::string_view gameModeName(std::uint32_t gameFlags);

/// The 69-byte record GAME.EXE posts to Westwood Online (its server-info block):
///   0 access << 4 | banned classes, 2 resolution, 3 players, 4 max players, 5-6 / 7-8 ping
///   min / max (0xFFFF none), 9-10 game port, 11-19 map, 20-34 name (15 characters),
///   63-64 game flags, 65-66 quest stage.
std::optional<GameListing> parseWolRecord(std::span<const std::uint8_t> record);

inline constexpr std::size_t kWolRecordSize = 69;

/// The record as GAME.EXE's fills it (seen the same in live hosts' topics): byte 1
/// 0xFF, 5-8 0xFF (no ping limits), 35-62 0xFF, 67-68 0; `videoMode` byte 2.
std::array<std::uint8_t, kWolRecordSize> makeWolRecord(const GameListing& game, std::uint8_t videoMode = 0);

/// A MSG_SERVER_INFO reply from `address`:`port` as a listing.
GameListing listingFromServerInfo(const net::msg::ServerInfo& info, std::uint32_t address, std::uint16_t port);

/// A servers.txt's lines, the servers to ping ("host" or "host:port"): split at '\n' (the end of
/// the file ends the last line), '#' starting a comment, blanks trimmed, empty lines skipped.
std::vector<std::string> splitServerList(std::span<const std::uint8_t> bytes);

} // namespace nox::lobby
