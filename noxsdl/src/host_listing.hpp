// Where a game hosted from the menus is listed (no SDL: the tests include it). A listed game
// answers the game list's LAN query on its port (LAN lists ask on 18590) and is posted to XWIS
// (the online game list) only with the host logged in to XWIS: XWIS lists a game under its
// host's login ("#<nick>'s_game"), so a host who chose Skip Login or Network posts nothing there.
// What the host is told: the session dialog's line before Host, the log's line once hosted.
#pragma once

#include <cstdint>
#include <string>

namespace noxsdl {

struct HostListing {
    enum class Where : std::uint8_t {
        Xwis,     ///< on XWIS (as <nick>'s_game) and the LAN
        LanOnly,  ///< answers the LAN query, not on XWIS (`why`)
        Unlisted, ///< a local test's (--host-edit, a play-test): neither asked for
    };
    Where where = Where::Unlisted;
    std::string nick; ///< the XWIS login it's posted under (Xwis)
    std::string why;  ///< why it isn't on XWIS (LanOnly)
};

/// `listed`: the menus' network game (not a test's); `online`: the game list's XWIS on (not
/// --no-xwis); `skippedLogin`: the login screen's Skip Login; `nick`: the XWIS login, empty when
/// not logged in (Skip Login, or Network from the Online screen).
inline HostListing hostListing(bool listed, bool online, bool skippedLogin, const std::string& nick)
{
    HostListing l;
    if (!listed) {
        return l;
    }
    if (online && !nick.empty()) {
        l.where = HostListing::Where::Xwis;
        l.nick = nick;
        return l;
    }
    l.where = HostListing::Where::LanOnly;
    l.why = !online       ? "XWIS is off here (--no-xwis)"
            : skippedLogin ? "you chose Skip Login, and XWIS lists a game under its host's login"
                           : "you aren't logged in to XWIS (Online > Network), and XWIS lists a game under its host's login";
    return l;
}

/// The session dialog's line about where Host lists the session; `warning`: shown as one.
inline std::string hostListingNote(const HostListing& l, bool* warning = nullptr)
{
    if (warning) {
        *warning = l.where != HostListing::Where::Xwis;
    }
    switch (l.where) {
    case HostListing::Where::Xwis:
        return "Listed online (XWIS) as " + l.nick + "'s game, and on your LAN. Players outside your network need UDP port 18590 "
               "forwarded to this computer (UPnP tries to map it).";
    case HostListing::Where::LanOnly:
        return "Only players on your LAN will see this session: it isn't listed online because " + l.why +
               ". To list it online, go back to the main menu, choose Online > Westwood Online and log in, then host again.";
    case HostListing::Where::Unlisted:
        break;
    }
    return "Not listed: a local session (join it by address).";
}

/// The host's log line once hosted on `port` ("edit session" / "hosting" as `what`).
inline std::string hostListingLog(const HostListing& l, const std::string& what, std::uint16_t port)
{
    const std::string where = "UDP port " + std::to_string(port);
    std::string line;
    switch (l.where) {
    case HostListing::Where::Xwis:
        line = what + ": posting to XWIS as " + l.nick + "'s_game, and answering LAN game lists (" + where + ")";
        break;
    case HostListing::Where::LanOnly:
        line = what + ": LAN only (" + where + "), not posted to XWIS: " + l.why;
        break;
    case HostListing::Where::Unlisted:
        return what + ": not listed (a local run, " + where + ")";
    }
    if (port != 18590) {
        line += "; port 18590 was taken, so LAN game lists (which ask on 18590) won't find it: join by address";
    }
    return line;
}

} // namespace noxsdl
