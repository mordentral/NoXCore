// Westwood Online's chat as GAME.EXE has WOLAPI.dll talk it, to XWIS: the one connection a
// player's WOL login keeps from the login screen through the chat lobby (wolchat.c) and the game
// list, until a game is joined. Its requests are IChat's (GAME.EXE's wrappers),
// its news IChatEvent's (GAME.EXE's sink, vtable), as
// WOLAPI.dll words and reads them:
//
//   RequestChannelList(t)   "LIST -1 37" for t = -99999 (every channel: the lobbies'
//                           327 lines and the games' 326), else "LIST t 37"; its 321 starts and
//                           323 ends it (OnChannelList with them all for -99999, else
//                           only those of type t).
//   RequestChannelJoin      "JOIN #name key" (a listed channel's own key: XWIS takes
//                           any for a lobby); refused while in one (0x800401FC).
//   RequestChannelLeave     "PART #name", the channel forgotten at once.
//   RequestUserList         "NAMES #name"; the 353 lines ("@" an op, "+"
//                           a voice, ours flagged 0x8000) and 366 (OnUserList).
//   RequestPublicMessage    "PRIVMSG #name :text"; RequestPublicAction "PRIVMSG #name :\1ACTION
//                           text\1" (no space after ACTION: WOLAPI's own format).
//   RequestPrivateMessage   "PRIVMSG nick,nick :text"; the action likewise.
//   RequestLogout           "QUIT".
//
// The text in a message is GAME.EXE's: plain bytes when every character fits in
// one, else 0xF0 and the UTF-16 text (its terminator too) packed seven bits a byte;
// read back the same way. A channel's message from someone not in its user list is
// dropped, as WOLAPI drops it. The welcome's lines (375, 372, 377: the text after
// ":- ") are the message of the day.
//
// WolChatSession is the conversation without sockets (tests feed it lines); WolChat runs it over a
// TcpSocket to xwis.net.
#pragma once

#include "noxcore/lobby/listing.hpp"
#include "noxcore/lobby/xwis.hpp"
#include "noxcore/net/tcp_socket.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby {

/// The SKU's game code (the registry's SKU >> 8): Nox's 37.
inline constexpr int kWolGameCode = 37;
/// RequestChannelList's "every channel" (GAME.EXE's 0xFFFE7961).
inline constexpr int kWolAllChannels = -99999;
/// The lobbies' key WOLAPI joins them with (XWIS takes any).
inline constexpr std::string_view kWolLobbyKey = "zotclot9";

/// A lobby's name in the chat's list: "#Lob_37_<n>" is WolChat.c:Nox_Lobby_<n % 53
/// + 1>, with " <n / 53 + 1>" after it past the 53rd; nullopt for a channel that isn't a Nox lobby
/// (its name doesn't start "Lob_37_", the prefix the exe makes).
struct LobbyName {
    int nameIndex = 0; ///< 1-53: WolChat.c:Nox_Lobby_<nameIndex>
    int round = 1;     ///< over 1: " %d" after the name
};
std::optional<LobbyName> wolLobbyName(std::string_view channel);

/// Chat text as it goes on the wire.
std::string wolEncodeText(std::u16string_view text);
/// The wire's text back.
std::u16string wolDecodeText(std::string_view wire);
/// The lines of the message of the day: split at "\n" or "\r\n" (or a lone "\r"), an
/// empty line as " ", the last line kept only if not empty.
std::vector<std::string> wolMotdLines(std::string_view motd);

class WolChatSession {
public:
    enum class State : std::uint8_t { LoggingIn, Ready, Failed };

    /// WOLAPI's User (0x6C bytes): its name and flags.
    struct User {
        static constexpr std::uint32_t kOp = 1, kVoice = 2, kSquelched = 4, kMe = 0x8000;
        std::string name;
        std::uint32_t flags = 0;
    };
    /// WOLAPI's Channel: what a 327 line gives.
    struct Channel {
        std::string name; ///< without the '#'
        std::uint32_t type = 0; ///< 0 a chat channel; a game's, its game code
        int users = 0;          ///< how many are in it
        std::uint32_t official = 0; ///< nonzero: an official lobby (listed first)
        std::uint32_t flags = 0;    ///< the line's flags (before any ':')
        std::string topic;
    };
    /// One of IChatEvent's.
    struct Event {
        enum class Kind : std::uint8_t {
            Motd,           ///< text: the welcome's (once, at 376)
            ChannelList,    ///< channels (the chat channels; games in `games`)
            ChannelJoin,    ///< channel, user (flags kMe: ours)
            ChannelLeave,   ///< channel, user
            UserList,       ///< channel, users
            PublicMessage,  ///< channel, user, text (wire bytes)
            PublicAction,
            PrivateMessage, ///< user, text
            PrivateAction,
            UserKick,       ///< channel, user (kicked), other (who kicked)
            ChannelTopic,   ///< channel, text
            JoinFailed,     ///< text: the server's numeric (403 none such, 471 full, 474 banned,
                            ///< 475 a bad key...): OnChannelJoin's error
        };
        Kind kind = Kind::PublicMessage;
        std::string channel;
        User user;
        std::string other;
        std::string text;
        std::vector<Channel> channels;
        std::vector<GameListing> games;
        std::vector<User> users;
    };

    /// Queues the login (XwisSession's: CVERS, PASS, NICK, apgar, USER); `apgarPassword` is already
    /// encoded.
    WolChatSession(std::string nick, std::string apgarPassword);

    std::vector<std::string> takeOutgoing();
    void receive(std::span<const std::uint8_t> bytes);
    /// One line from the server, without its line end.
    void line(std::string_view text);
    std::vector<Event> takeEvents();

    // IChat's requests. Each is refused (false, nothing sent) where WOLAPI refuses it.
    bool requestChannelList(int type);
    bool requestChannelJoin(const std::string& name);
    bool requestChannelLeave();
    bool requestUserList();
    bool requestPublicMessage(std::string_view wireText);
    bool requestPublicAction(std::string_view wireText);
    bool requestPrivateMessage(const std::vector<std::string>& nicks, std::string_view wireText);
    bool requestPrivateAction(const std::vector<std::string>& nicks, std::string_view wireText);
    void requestLogout();

    State state() const noexcept { return state_; }
    const std::string& error() const noexcept { return error_; }
    /// Failed because the server said no (a wrong password, a nick in use), not the connection.
    bool refused() const noexcept { return refused_; }
    const std::string& nick() const noexcept { return nick_; }
    /// The channel we're in (without '#'), or empty.
    const std::string& channel() const noexcept { return channel_; }
    /// Its users as the last 353s and the joins and leaves since left them (WOLAPI's list).
    const std::vector<User>& users() const noexcept { return users_; }
    bool listing() const noexcept { return listType_.has_value(); }

private:
    void fail(std::string why, bool refused);
    User* findUser(std::string_view name);
    void removeUser(std::string_view name);

    std::string nick_;
    State state_ = State::LoggingIn;
    std::string error_;
    bool refused_ = false;
    std::string partial_;
    std::vector<std::string> outgoing_;
    std::vector<Event> events_;
    std::string motd_;
    std::string channel_;
    bool joining_ = false; ///< a JOIN sent, not yet answered
    std::vector<User> users_;
    std::vector<User> naming_; ///< the 353s so far
    std::optional<int> listType_;
    std::vector<Channel> listChannels_;
    std::vector<GameListing> listGames_;
};

/// GAME.EXE's own lists for the chat (wol.c): the lobbies listed ("Chat Channels", the group of
/// channel type 0: keeps those named "Lob_37_..." in order), the one we're in,
/// and its users (addUser's list). The screen mirrors each change
/// in its list boxes at the index it returns.
class WolLobbyLists {
public:
    struct Lobby {
        std::string channel; ///< without '#'
        LobbyName name;
        int users = 0;
        std::uint32_t official = 0;
    };
    using User = WolChatSession::User;

    /// no lobbies, none current.
    void clearLobbies();
    /// a listed chat channel, if it's a lobby, put in order (official
    /// ones first, then by name, _stricmp); its index, or -1 if it isn't one.
    int addLobby(const WolChatSession::Channel& channel);
    const std::vector<Lobby>& lobbies() const noexcept { return lobbies_; }
    /// The lobby we're in, or -1.
    int current() const noexcept { return current_; }
    /// the current lobby by channel name (empty: none); its index or -1.
    int setCurrent(std::string_view channel);
    /// The current lobby's user count changed by `delta`. Returns its index, or -1;
    /// `removed` set if its count reached 0 and it went from the list, and with it
    /// the current one.
    int changeCurrentUsers(int delta, bool& removed);
    /// the first lobby with fewer than 25 users, else the one `random` (0 to the
    /// count - 1, randInt) picks; -1 with none.
    int pickLobby(int random) const;

    /// no users.
    void clearUsers() { users_.clear(); }
    /// a user put in order (ops first, then by name); its index.
    int addUser(const User& user);
    /// a user gone; the index it had, or -1.
    int removeUser(std::string_view name);
    const std::vector<User>& users() const noexcept { return users_; }

private:
    std::vector<Lobby> lobbies_;
    int current_ = -1;
    std::vector<User> users_;
};

/// A WolChatSession over a socket to xwis.net: polled, never blocking.
class WolChat {
public:
    enum class State : std::uint8_t { Connecting, Ready, Failed, Closed };

    /// Connects and logs in; `apgarPassword` is already encoded.
    WolChat(std::string nick, std::string apgarPassword, std::uint32_t nowMs);

    /// Sends, receives; the login times out after 20 seconds. After logOff, waits (3 seconds at
    /// most) for the server to close the connection, since XWIS refuses another login from this
    /// address while it lasts.
    void poll(std::uint32_t nowMs);
    /// RequestLogout, then Closed once the server has closed the connection.
    void logOff(std::uint32_t nowMs);

    State state() const noexcept { return state_; }
    /// Logged off, waiting for the server to close.
    bool loggingOff() const noexcept { return quitAt_ != 0 && state_ != State::Closed; }
    const std::string& error() const noexcept { return error_; }
    bool refused() const noexcept { return refused_; }
    WolChatSession& session() { return *session_; }
    const WolChatSession& session() const { return *session_; }

private:
    void fail(std::string why, bool refused);
    void flush();

    std::optional<net::TcpSocket> socket_;
    std::optional<WolChatSession> session_;
    std::string send_;
    std::uint32_t startedAt_ = 0;
    std::uint32_t quitAt_ = 0;
    State state_ = State::Connecting;
    std::string error_;
    bool refused_ = false;
};

} // namespace nox::lobby
