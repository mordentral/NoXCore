// The IRC-like plumbing every XWIS session shares (xwis.cpp, xwis_host.cpp, xwis_login.cpp,
// wol_chat.cpp, servserv.cpp): the login lines, splitting received bytes into lines, and words
// and names. (Internal to noxcore's lobby sources; the connection is servserv.hpp's.)
#pragma once

#include "noxcore/lobby/servserv.hpp" // (kWolSku, kWolapiSku, kWolapiVersion)

#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby::detail {

/// WOLAPI's chat login version: its file version (major << 16 | minor) plus 0xA0000, so 1.20 is
/// 11.20, sent as CVERS's "11020" and as the chat server's verchk number (720916).
inline constexpr std::uint32_t kWolapiChatVersion = kWolapiVersion + 0xA0000;

/// The login lines a session starts with, WOLAPI's block in its order: the client version (CVERS
/// "<major><minor, three digits> <SKU>"), the PASS word (the same for every login), the nick, the
/// apgar-encoded password, the USER line (whose server field WOLAPI always fills with
/// irc.westwood.com, whichever chat server it is on), the patch check (WOLAPI's product code for
/// the game's language and its chat version), and SETOPT 17,33 (others may find and page us).
/// WOLAPI's SERIAL line (the CD key) between apgar and USER is left out: NoX Core reads no CD key.
/// Nothing waits for the answers to verchk or SETOPT (see isStockLoginExtraReply).
inline std::vector<std::string> xwisLoginLines(const std::string& nick, std::string_view apgarPassword)
{
    std::string minor = std::to_string(kWolapiChatVersion & 0xFFFF);
    minor.insert(0, minor.size() < 3 ? 3 - minor.size() : 0, '0');
    return {
        "CVERS " + std::to_string(kWolapiChatVersion >> 16) + minor + " " + std::to_string(kWolSku),
        "PASS supersecret",
        "NICK " + nick,
        "apgar " + std::string(apgarPassword) + " 0",
        "USER UserName HostName irc.westwood.com :RealName",
        "verchk " + std::to_string(kWolapiSku + (kWolSku & 0xFF)) + " " + std::to_string(kWolapiChatVersion),
        "SETOPT 17,33",
    };
}

/// Appends `bytes` to `partial` and hands each whole line in it (CR LF or LF) to `line`; a partial
/// last line waits in `partial` for more.
template <class Line>
void splitLines(std::string& partial, std::span<const std::uint8_t> bytes, Line&& line)
{
    partial.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t start = 0;
    for (auto end = partial.find('\n'); end != std::string::npos; end = partial.find('\n', start)) {
        std::string_view l(partial.data() + start, end - start);
        if (!l.empty() && l.back() == '\r') {
            l.remove_suffix(1);
        }
        line(l);
        start = end + 1;
    }
    partial.erase(0, start);
}

/// The next space-separated word of `rest` (consumed).
inline std::string_view ircWord(std::string_view& rest)
{
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    const auto space = rest.find(' ');
    const std::string_view w = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    return w;
}

/// Case-insensitive ASCII equality (IRC nicks and channels).
inline bool ircSameName(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Whether `text` is an error numeric (4xx / 5xx) answering the login's verchk or SETOPT (": 421
/// us VERCHK :Unknown command" and the like). The sessions pass these over, so a server that
/// doesn't know those lines can't fail the login; their other answers (an update list, 3xx and
/// 6xx numerics) match nothing a session acts on.
inline bool isStockLoginExtraReply(std::string_view text)
{
    std::string_view rest = text;
    if (!rest.empty() && rest.front() == ':') {
        ircWord(rest); // (the server's prefix)
    }
    const std::string_view command = ircWord(rest);
    if (command.size() != 3 || (command[0] != '4' && command[0] != '5')) {
        return false;
    }
    // The parameters before the trailing one.
    for (std::string_view w = ircWord(rest); !w.empty() && w.front() != ':'; w = ircWord(rest)) {
        if (ircSameName(w, "verchk") || ircSameName(w, "SETOPT")) {
            return true;
        }
    }
    return false;
}

} // namespace nox::lobby::detail
