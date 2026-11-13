// The join's dialog, as the 1.2b client shows it over the game list while it waits for the host
// (noxworld.c's join states; no SDL here: the tests use it).
//
//   Join (any listed or typed game but our own): "Attempting to connect" (Noxworld.c:AttemptingConn,
//   no title) with Cancel, over the game list, from the TRY_JOIN on. The host's ping test turns
//   its text to Noxworld.c:TestCon; JOIN_OK to Noxworld.c:Connected, no buttons, till the
//   connection's up (the join data's wait). Then the game, as before.
//   No answer in the lobby's 20 s: Noxworld.c:ErrTimeout, no title, OK.
//   SERVER_ERROR (0x13 n): the lobby's error table's text n, titled Noxworld.c:ConnError (but for
//   8-10, the client's own), OK.
//   The connect refused after JOIN_OK (the transport's 0x02-0x04) or unanswered: the game's
//   connection error box (netclint.c:ConnectionError and the code's text), OK.
//   OK, and Cancel while waiting, go back to the game list (fetched again).
#pragma once

#include "noxcore/client/headless_client.hpp"
#include "noxcore/net/transport.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace noxsdl::join_wait {

/// A dialog's words as string-table ids ("" for no title) and its buttons (Gui_Dialog_Create's
/// flags: 1 OK, 2 Cancel, 0 none).
struct Words {
    std::string title;
    std::string text;
    std::uint32_t buttons = 0;
    bool operator==(const Words&) const = default;
};

/// The lobby's refusal texts by SERVER_ERROR's code (noxworld.c's error table, ErrLowPing on).
inline constexpr std::array<std::string_view, 14> kLobbyErrors = {
    "Noxworld.c:ErrLowPing",     "Noxworld.c:ErrHighPing",  "Noxworld.c:ErrLowLevel",     "Noxworld.c:ErrHighLevel",
    "Noxworld.c:ErrClosed",      "Noxworld.c:ErrBanned",    "Noxworld.c:ErrWrongPassword", "Noxworld.c:ErrIllegalClass",
    "Noxworld.c:ErrTimeout",     "Noxworld.c:ErrFindFailed", "Noxworld.c:ErrNeedRefresh", "Noxworld.c:ErrFull",
    "Noxworld.c:ErrDupSerial",   "Noxworld.c:ErrWrongVer",
};
/// The lobby's own failure: no answer within its wait (the client's code 8).
inline constexpr int kLobbyTimedOut = 8;

/// The lobby's failure `code`'s dialog: titled ConnError but for 8-10 (the client's own: timed
/// out, the game not found, the list to refresh), its text from the table, OK. (A code past the
/// table, which no 1.2b host sends, reads past the exe's table; here the network code's unknown
/// error stands in.)
inline Words lobbyFailure(int code)
{
    Words w;
    w.title = code == 8 || code == 9 || code == 10 ? "" : "Noxworld.c:ConnError";
    w.text = code >= 0 && code < static_cast<int>(kLobbyErrors.size()) ? std::string(kLobbyErrors[static_cast<std::size_t>(code)])
                                                                        : std::string("netclint.c:UnknownConnError");
    w.buttons = 1;
    return w;
}

/// The connect's failure after JOIN_OK (the game's connection error box): the transport's refusal
/// 0x02 too many users, 0x03 banned, 0x04 not allowed; no SERVER_ACCEPT in time, the ack's
/// timeout.
inline Words connectFailure(int code)
{
    Words w;
    w.title = "netclint.c:ConnectionError";
    switch (code) {
    case 2: w.text = "netclint.c:JoinConnTooManyUsers"; break;
    case 3: w.text = "netclint.c:JoinConnUserBanned"; break;
    case 4: w.text = "netclint.c:JoinConnUserNotAllowed"; break;
    default: w.text = "netclint.c:ConnectAckTimeout"; break;
    }
    w.buttons = 1;
    return w;
}

/// While the join waits: the dialog's words for where it stands.
inline Words waiting(const nox::client::HeadlessClient& client)
{
    using CS = nox::net::ClientConnection::State;
    const CS state = client.connection().state();
    if (state == CS::JoinOk || state == CS::Connecting || state == CS::Connected) {
        return {"", "Noxworld.c:Connected", 0}; // (JOIN_OK: no buttons till the game)
    }
    if (client.connection().lobbyPings() != 0) {
        return {"", "Noxworld.c:TestCon", 2}; // (the host's ping test)
    }
    return {"", "Noxworld.c:AttemptingConn", 2};
}

/// Whether the join is still waiting for the host (the dialog stays up): before the join starts
/// (a NoX Core host's query answered first) and through the lobby and the connect.
inline bool stillWaiting(const nox::client::HeadlessClient& client)
{
    using Stage = nox::client::HeadlessClient::Stage;
    return client.stage() == Stage::Idle || client.stage() == Stage::Joining;
}

/// A join that failed while waiting: its dialog, or none when it's shown otherwise (the password
/// asked for, a refusal for a wrong password: the password dialog again; or a failure that isn't
/// the lobby's or the connect's).
inline std::optional<Words> failure(const nox::client::HeadlessClient& client)
{
    using CS = nox::net::ClientConnection::State;
    if (client.stage() != nox::client::HeadlessClient::Stage::Failed || client.passwordAsked()) {
        return std::nullopt;
    }
    switch (client.connection().state()) {
    case CS::Joining:
    case CS::PasswordRequired:
    case CS::JoinAgain:
        return lobbyFailure(kLobbyTimedOut); // (no answer in the lobby's wait)
    case CS::JoinRefused:
        if (client.joinError() == 6) {
            return std::nullopt; // (ErrWrongPassword: asked again, the password dialog's)
        }
        return lobbyFailure(client.joinError());
    case CS::ConnectRefused:
        return connectFailure(client.joinError());
    default:
        return std::nullopt;
    }
}

} // namespace noxsdl::join_wait
