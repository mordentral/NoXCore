// The IRC-like plumbing every XWIS session shares (xwis.cpp, xwis_host.cpp, xwis_login.cpp,
// wol_chat.cpp, servserv.cpp): the login lines, splitting received bytes into lines, and words
// and names. (Internal to noxcore's lobby sources; the connection is servserv.hpp's.)
#pragma once

#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby::detail {

/// The login lines a session starts with: the client version, the PASS word (the same for every
/// login), the nick, the apgar-encoded password and the USER line, whose server field WOLAPI
/// always fills with irc.westwood.com, whichever chat server it is on.
inline std::vector<std::string> xwisLoginLines(const std::string& nick, std::string_view apgarPassword)
{
    return {
        "CVERS 11015 9472",
        "PASS supersecret",
        "NICK " + nick,
        "apgar " + std::string(apgarPassword) + " 0",
        "USER UserName HostName irc.westwood.com :RealName",
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

} // namespace nox::lobby::detail
