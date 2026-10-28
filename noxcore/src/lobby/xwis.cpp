// XWIS listing: the seven-bit packing of game topics, the 326 line parser, the password encoding
// and the list-or-login session; see xwis.hpp.
#include "noxcore/lobby/xwis.hpp"

#include "irc.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <utility>

namespace nox::lobby {

namespace {

/// The WOL game record's size in bytes (0x228 bits in the record writer's call to the packer).
constexpr std::size_t kRecordSize = 69;

/// Splits off the first space-separated token of `rest` (irc.hpp).
constexpr auto token = detail::ircWord;

} // namespace

std::vector<std::uint8_t> packSevenBit(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> out;
    int bit = 7; // position in out.back(); 7 = start a new byte
    // Bit i of the input goes to bit (i % 7) of output byte i / 7; each finished byte gets 0x80.
    for (std::size_t i = 0; i < data.size() * 8; ++i) {
        if (bit == 7) {
            if (!out.empty()) {
                out.back() |= 0x80;
            }
            out.push_back(0);
            bit = 0;
        }
        if (data[i / 8] & (1u << (i % 8))) {
            out.back() |= static_cast<std::uint8_t>(1u << bit);
        }
        ++bit;
    }
    // The marker only goes in before the next group starts, so the last byte never has it.
    return out;
}

std::vector<std::uint8_t> unpackSevenBit(std::span<const std::uint8_t> packed, std::size_t size)
{
    std::vector<std::uint8_t> out(size, 0);
    std::size_t bit = 0;
    // Seven data bits from each packed byte (the top bit is only the marker).
    for (const std::uint8_t b : packed) {
        for (int i = 0; i < 7 && bit < size * 8; ++i, ++bit) {
            if (b & (1u << i)) {
                out[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
            }
        }
    }
    return out;
}

std::optional<GameListing> parseXwisTopic(std::string_view topic)
{
    // The tag: 'G' in a game, 'g' not yet (a host still on a chat map, live: "g1P3" for a game
    // anyone could join); the rest is the same either way.
    auto at = topic.find("G1P3");
    if (at == std::string_view::npos) {
        at = topic.find("g1P3");
    }
    // (A topic that ends inside the tag's version bytes is no game's: anyone's channel topic
    // reaches every lister.)
    if (at == std::string_view::npos || topic.size() < at + 4 + 3) {
        return std::nullopt;
    }
    const std::string_view packed = topic.substr(at + 4 + 3); // tag, then the version's three bytes
    // The topic is a C string, so a last packed byte of 0 (the record's final bits, with no top
    // bit) ends it early: 78 of the 79 bytes arrive. The fields read end at byte 66.
    constexpr std::size_t kNeeded = 67;
    if (packed.size() * 7 < kNeeded * 8) {
        return std::nullopt;
    }
    const auto record = unpackSevenBit(
        std::span(reinterpret_cast<const std::uint8_t*>(packed.data()), packed.size()), kRecordSize);
    return parseWolRecord(record);
}

std::optional<GameListing> parseXwisGameLine(std::string_view line)
{
    std::string_view rest = line;
    if (!rest.empty() && rest.front() == ':') {
        token(rest); // the server prefix
    }
    if (token(rest) != "326") {
        return std::nullopt;
    }
    // nick (ours), channel, users, 0, 37 (Nox), 1, flags, then the host's IPv4 as one number.
    token(rest);
    const std::string_view channel = token(rest);
    for (int i = 0; i < 5; ++i) {
        token(rest);
    }
    const std::string_view ip = token(rest);
    std::uint32_t address = 0;
    if (std::from_chars(ip.data(), ip.data() + ip.size(), address).ec != std::errc{}) {
        return std::nullopt;
    }
    // The topic is the rest of the line: its packed bytes may include spaces, so it isn't split.
    auto game = parseXwisTopic(rest);
    if (game) {
        game->address = address;
        game->channel = std::string(channel);
        game->host = hostOfChannel(channel);
    }
    return game;
}

std::string xwisNick(std::uint32_t random)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "NoXList%02x", static_cast<unsigned>(random & 0xFF));
    return buf;
}

std::string apgar(std::string_view password)
{
    static constexpr std::string_view kLookup = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    const std::size_t n = std::min<std::size_t>(password.size(), 8);
    const auto at = [&](std::size_t i) { return i < n ? static_cast<std::uint8_t>(password[i]) : std::uint8_t{0}; };
    std::uint8_t mixed[8] = {};
    // The pairing and mixing apgar describes in xwis.hpp; bytes past n stay 0.
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = at(i);
        const std::uint8_t pair = at(n - i); // (at(n): the terminator)
        mixed[i] = (c & 1) ? static_cast<std::uint8_t>((c << 1) & pair) : static_cast<std::uint8_t>(c ^ pair);
    }
    std::string out;
    for (const std::uint8_t m : mixed) {
        out += kLookup[m & 0x3F];
    }
    return out;
}

XwisSession::XwisSession(std::string nick, std::string apgarPassword, Purpose purpose)
    : nick_(std::move(nick)), purpose_(purpose)
{
    outgoing_ = detail::xwisLoginLines(nick_, apgarPassword.empty() ? nick_ : apgarPassword);
}

std::vector<std::string> XwisSession::takeOutgoing()
{
    return std::exchange(outgoing_, {});
}

// Splits the bytes into lines (CR LF or LF); a partial last line waits for more.
void XwisSession::receive(std::span<const std::uint8_t> bytes)
{
    detail::splitLines(partial_, bytes, [this](std::string_view l) { line(l); });
}

// One server line: answers PING, and moves login -> listing -> done on 376, 326s and 323.
void XwisSession::line(std::string_view text)
{
    if (state_ == State::Done || state_ == State::Failed || text.empty()) {
        return;
    }
    std::string_view rest = text;
    if (rest.front() == ':') {
        token(rest);
    }
    const std::string_view command = token(rest);
    if (command == "PING") {
        outgoing_.push_back("PONG " + std::string(rest));
    } else if (command == "ERROR") {
        state_ = State::Failed;
        error_ = std::string(text);
    } else if (command == "372" && state_ == State::LoggingIn && text.find("Invalid Password") != std::string_view::npos) {
        // A wrong password for a registered nick: XWIS goes on to welcome a guest who sees no
        // games, its only word this line of the welcome (":- Invalid Password! Use the XWIS
        // password shown at xwis.net/ga").
        state_ = State::Failed;
        const auto words = text.find(":- ");
        error_ = std::string(words != std::string_view::npos ? text.substr(words + 3) : text);
    } else if (command == "376" && state_ == State::LoggingIn) {
        // End of the welcome: logged in.
        if (purpose_ == Purpose::Login) {
            outgoing_.push_back("QUIT");
            state_ = State::Done;
            return;
        }
        outgoing_.push_back("LIST -1 37");
        state_ = State::Listing;
    } else if (command == "326") {
        if (auto game = parseXwisGameLine(text)) {
            games_.push_back(std::move(*game));
        }
    } else if (command == "323" && state_ == State::Listing) {
        outgoing_.push_back("QUIT");
        state_ = State::Done;
    } else if (command.size() == 3 && (command[0] == '4' || command[0] == '5')) {
        // An IRC error reply (nick taken, bad password, ...).
        state_ = State::Failed;
        error_ = std::string(text);
    }
}

} // namespace nox::lobby
