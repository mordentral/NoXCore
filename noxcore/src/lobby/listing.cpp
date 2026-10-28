// Game listings: the 69-byte WOL server record (both ways), MSG_SERVER_INFO replies, mode names;
// see listing.hpp for the record's layout.
#include "noxcore/lobby/listing.hpp"

#include "noxcore/util/bytes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace nox::lobby {

namespace {

using util::u16At;

/// NUL-terminated (or full-width) ASCII.
std::string textAt(std::span<const std::uint8_t> b, std::size_t off, std::size_t len)
{
    std::string s;
    for (std::size_t i = off; i < off + len && i < b.size() && b[i] != 0; ++i) {
        s.push_back(static_cast<char>(b[i]));
    }
    return s;
}

} // namespace

std::array<std::uint8_t, kWolRecordSize> makeWolRecord(const GameListing& game, std::uint8_t videoMode)
{
    std::array<std::uint8_t, kWolRecordSize> r{};
    const auto put16 = [&](std::size_t at, int v) {
        r[at] = static_cast<std::uint8_t>(v);
        r[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    // Byte 0: access (0 open, 1 closed, 2 password) in the high nibble, banned classes in the low.
    const int access = game.access == GameAccess::Closed ? 1 : game.access == GameAccess::Password ? 2 : 0;
    r[0] = static_cast<std::uint8_t>(access << 4 | (game.bannedClasses & 0x0F));
    r[1] = 0xFF;
    r[2] = videoMode;
    r[3] = static_cast<std::uint8_t>(game.players);
    r[4] = static_cast<std::uint8_t>(game.maxPlayers);
    put16(5, game.pingMin.value_or(0xFFFF));
    put16(7, game.pingMax.value_or(0xFFFF));
    put16(9, game.port);
    for (std::size_t i = 0; i < 8 && i < game.map.size(); ++i) { // (strcpy: 8 letters and its 0)
        r[11 + i] = static_cast<std::uint8_t>(game.map[i]);
    }
    for (std::size_t i = 0; i < 15 && i < game.name.size(); ++i) { // (strncpy 15: no 0 when full)
        r[20 + i] = static_cast<std::uint8_t>(game.name[i]);
    }
    std::fill(r.begin() + 35, r.begin() + 63, std::uint8_t{0xFF});
    put16(63, static_cast<int>(game.gameFlags));
    if (game.gameFlags & 0x1000) {
        put16(65, game.questStage);
    }
    return r;
}

std::string hostOfChannel(std::string_view channel)
{
    constexpr std::string_view kSuffix = "'s_game";
    if (!channel.starts_with('#') || !channel.ends_with(kSuffix) || channel.size() <= 1 + kSuffix.size()) {
        return {};
    }
    return std::string(channel.substr(1, channel.size() - 1 - kSuffix.size()));
}

std::string_view gameModeName(std::uint32_t flags)
{
    // The first matching bit wins, in this order.
    static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 8> kModes{{
        {0x0010, "kotr"},
        {0x0020, "ctf"},
        {0x0040, "flagball"},
        {0x0080, "chat"},
        {0x0100, "arena"},
        {0x0400, "elimination"},
        {0x1000, "quest"},
        {0x0800, "coop"},
    }};
    for (const auto& [bit, name] : kModes) {
        if (flags & bit) {
            return name;
        }
    }
    return "custom";
}

std::optional<GameListing> parseWolRecord(std::span<const std::uint8_t> r)
{
    if (r.size() < 67) {
        return std::nullopt;
    }
    GameListing g;
    const int access = r[0] >> 4;
    g.access = access == 1 ? GameAccess::Closed : access == 2 ? GameAccess::Password : GameAccess::Open;
    g.bannedClasses = r[0] & 0x0F;
    g.players = r[3];
    g.maxPlayers = r[4];
    if (const auto v = u16At(r, 5); v != 0xFFFF) {
        g.pingMin = v;
    }
    if (const auto v = u16At(r, 7); v != 0xFFFF) {
        g.pingMax = v;
    }
    g.port = u16At(r, 9);
    // Port 0 means the default game port.
    if (g.port == 0) {
        g.port = kDefaultGamePort;
    }
    g.map = textAt(r, 11, 9);
    g.name = textAt(r, 20, 15);
    g.gameFlags = u16At(r, 63);
    if (g.gameFlags & 0x1000) {
        g.questStage = u16At(r, 65);
    }
    // (The exe copies the record into its list entry, where the description box finds the
    // resolution, the spells, the weapons and the armour.)
    g.resolution = r[2];
    std::copy(r.begin() + 35, r.begin() + 55, g.spells.begin());
    g.weapons = static_cast<std::uint32_t>(r[55] | r[56] << 8 | r[57] << 16) | static_cast<std::uint32_t>(r[58]) << 24;
    g.armor = static_cast<std::uint32_t>(r[59] | r[60] << 8 | r[61] << 16) | static_cast<std::uint32_t>(r[62]) << 24;
    return g;
}

GameListing listingFromServerInfo(const net::msg::ServerInfo& info, std::uint32_t address, std::uint16_t port)
{
    GameListing g;
    g.address = address;
    g.port = port;
    g.players = info.players;
    g.maxPlayers = info.maxPlayers;
    g.map = textAt(info.mapName, 0, info.mapName.size());
    g.name = textAt(info.serverName, 0, info.serverName.size());
    g.gameFlags = info.gameFlags;
    // serverFlags: banned classes in the low 3 bits, 0x10 closed, 0x20 password.
    g.bannedClasses = info.serverFlags & 0x07;
    g.access = (info.serverFlags & 0x10) ? GameAccess::Closed
               : (info.serverFlags & 0x20) ? GameAccess::Password
                                           : GameAccess::Open;
    if (info.pingMin != 0xFFFF && info.pingMin != 0) {
        g.pingMin = info.pingMin;
    }
    if (info.pingMax != 0xFFFF && info.pingMax != 0) {
        g.pingMax = info.pingMax;
    }
    if (g.gameFlags & 0x1000) {
        g.questStage = info.questStage;
    }
    // The list entry made from a reply: the resolution the reply's video byte, the spells its
    // bitmap, the armour its gamedata+48, and the weapons the four bytes from its
    // +5: gamedata+44's low three and the map name's first letter (the reply carries only three).
    g.resolution = info.videoFlags;
    g.spells = info.allowedSpells;
    g.weapons = static_cast<std::uint32_t>(info.gameData44[0] | info.gameData44[1] << 8 | info.gameData44[2] << 16) |
                static_cast<std::uint32_t>(info.mapName[0]) << 24;
    g.armor = info.gameData48;
    return g;
}

std::vector<std::string> splitServerList(std::span<const std::uint8_t> bytes)
{
    std::vector<std::string> out;
    std::string line;
    for (std::size_t i = 0; i <= bytes.size(); ++i) {
        const char c = i < bytes.size() ? static_cast<char>(bytes[i]) : '\n';
        if (c != '\n') {
            line.push_back(c);
            continue;
        }
        line = line.substr(0, line.find('#'));
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        const auto start = line.find_first_not_of(" \t");
        if (start != std::string::npos) {
            out.push_back(line.substr(start));
        }
        line.clear();
    }
    return out;
}

} // namespace nox::lobby
