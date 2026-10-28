// The WOL chat session (the IRC-like conversation with XWIS), the chat screen's lobby and user
// lists, and the polled socket that carries the session; see wol_chat.hpp.
#include "noxcore/lobby/wol_chat.hpp"

#include "noxcore/net/udp_socket.hpp" // (net::Endpoint)
#include "noxcore/util/text.hpp"

#include "irc.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <utility>

namespace nox::lobby {

namespace {

constexpr std::uint32_t kLoginTimeoutMs = 20000;
constexpr std::uint32_t kCloseWaitMs = 3000;
/// The lobbies' names: the exe counts 53 of them.
constexpr int kLobbyNames = 53;

/// The next space-separated word of `rest` (consumed; irc.hpp).
constexpr auto word = detail::ircWord;

/// _stricmp: ASCII letters folded to lower case.
using util::compareNoCase;

bool sameName(std::string_view a, std::string_view b)
{
    return compareNoCase(a, b) == 0;
}

int toInt(std::string_view s)
{
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

/// The sender's nick from a prefix (":nick!user@host").
std::string_view senderOf(std::string_view prefix)
{
    return prefix.substr(0, prefix.find('!'));
}

/// The nicks as a PRIVMSG's target list: "a,b,c".
std::string joinNicks(const std::vector<std::string>& nicks)
{
    std::string to;
    for (const auto& n : nicks) {
        to += (to.empty() ? "" : ",") + n;
    }
    return to;
}

/// A channel's name without its leading '#'.
std::string_view dropHash(std::string_view name)
{
    if (!name.empty() && name.front() == '#') {
        name.remove_prefix(1);
    }
    return name;
}

} // namespace

std::optional<LobbyName> wolLobbyName(std::string_view channel)
{
    if (!channel.empty() && channel.front() == '#') {
        channel.remove_prefix(1);
    }
    // The lobby list's add: strncmp with the prefix ("%s%d_": "Lob_" and the game code), then the
    // number after it (atoi) to the lobby name lookup.
    const std::string prefix = "Lob_" + std::to_string(kWolGameCode) + "_";
    if (!channel.starts_with(prefix)) {
        return std::nullopt;
    }
    const int n = std::max(0, toInt(channel.substr(prefix.size())));
    // Lobby n is name (n % 53) + 1; past the 53rd, a round number (n / 53 + 1) follows the name.
    return LobbyName{n % kLobbyNames + 1, n / kLobbyNames + 1};
}

std::string wolEncodeText(std::u16string_view text)
{
    // cliCanTalkMB: every character's high byte zero, the text as bytes ("%S");
    // else 0xF0 and the UTF-16 text with its terminator packed ((n + 1) * 16 bits),
    // copied as a C string (a last byte of 0 isn't).
    const bool narrow = std::all_of(text.begin(), text.end(), [](char16_t c) { return c < 0x100; });
    std::string out;
    if (narrow) {
        for (const char16_t c : text) {
            out.push_back(static_cast<char>(c));
        }
        return out;
    }
    // UTF-16 little-endian, then the terminator, then seven bits a byte so no byte is 0 until the end.
    std::vector<std::uint8_t> raw;
    for (const char16_t c : text) {
        raw.push_back(static_cast<std::uint8_t>(c & 0xFF));
        raw.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    raw.push_back(0);
    raw.push_back(0);
    const auto packed = packSevenBit(raw);
    out.push_back(static_cast<char>(0xF0));
    for (const std::uint8_t b : packed) {
        if (b == 0) {
            break;
        }
        out.push_back(static_cast<char>(b));
    }
    return out;
}

std::u16string wolDecodeText(std::string_view wire)
{
    std::u16string out;
    if (!wire.empty() && static_cast<std::uint8_t>(wire.front()) == 0xF0) {
        // Unpacked until a terminator.
        wire.remove_prefix(1);
        const auto raw = unpackSevenBit(std::span(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()),
                                        wire.size() * 7 / 8);
        return util::utf16Until(raw);
    }
    // "%S": the bytes as the ANSI code page's characters (Latin-1 here).
    for (const char c : wire) {
        out.push_back(static_cast<char16_t>(static_cast<std::uint8_t>(c)));
    }
    return out;
}

// Every line that ends with a line break is kept (an empty one as " "); a last unended line only
// if it isn't empty.
std::vector<std::string> wolMotdLines(std::string_view motd)
{
    // The MOTD as the exe reads it, with its line splitting.
    std::vector<std::string> lines;
    std::string current;
    std::size_t i = 0;
    while (i < motd.size()) {
        current.clear();
        bool ended = false;
        while (i < motd.size()) {
            const char c = motd[i];
            if (c == '\n') {
                ++i;
                ended = true;
                break;
            }
            if (c == '\r') {
                i += i + 1 < motd.size() && motd[i + 1] == '\n' ? 2 : 1;
                ended = true;
                break;
            }
            current.push_back(c);
            ++i;
        }
        if (!ended) {
            break; // (the last line: kept below if not empty)
        }
        lines.push_back(current.empty() ? " " : current);
        current.clear();
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

// ----------------------------------------------------------------------------- WolChatSession

// Queues the login lines, as XwisSession sends them (the PASS word is the same for every login).
WolChatSession::WolChatSession(std::string nick, std::string apgarPassword)
    : nick_(std::move(nick))
{
    outgoing_ = detail::xwisLoginLines(nick_, apgarPassword);
}

std::vector<std::string> WolChatSession::takeOutgoing()
{
    return std::exchange(outgoing_, {});
}

std::vector<WolChatSession::Event> WolChatSession::takeEvents()
{
    return std::exchange(events_, {});
}

// Splits the bytes into lines (CR LF or LF); a partial last line waits for more.
void WolChatSession::receive(std::span<const std::uint8_t> bytes)
{
    detail::splitLines(partial_, bytes, [this](std::string_view l) { line(l); });
}

void WolChatSession::fail(std::string why, bool refused)
{
    state_ = State::Failed;
    error_ = std::move(why);
    refused_ = refused;
}

WolChatSession::User* WolChatSession::findUser(std::string_view name)
{
    for (auto& u : users_) {
        if (sameName(u.name, name)) {
            return &u;
        }
    }
    return nullptr;
}

void WolChatSession::removeUser(std::string_view name)
{
    std::erase_if(users_, [&](const User& u) { return sameName(u.name, name); });
}

bool WolChatSession::requestChannelList(int type)
{
    if (state_ != State::Ready) {
        return false;
    }
    // RequestChannelList, as WOLAPI words it.
    listType_ = type;
    listChannels_.clear();
    listGames_.clear();
    outgoing_.push_back(type == kWolAllChannels ? "LIST -1 " + std::to_string(kWolGameCode)
                                                : "LIST " + std::to_string(type) + " " + std::to_string(kWolGameCode));
    return true;
}

bool WolChatSession::requestChannelJoin(const std::string& name)
{
    // RequestChannelJoin: not while a join is under way (0x800401F7) nor while in a channel
    // (0x800401FC).
    if (state_ != State::Ready || joining_ || !channel_.empty() || name.empty()) {
        return false;
    }
    outgoing_.push_back("JOIN #" + name + " " + std::string(kWolLobbyKey));
    joining_ = true;
    return true;
}

bool WolChatSession::requestChannelLeave()
{
    // RequestChannelLeave: none to leave, 0x800401FD; else PART, and the channel forgotten now.
    if (state_ != State::Ready || channel_.empty()) {
        return false;
    }
    outgoing_.push_back("PART #" + channel_);
    channel_.clear();
    users_.clear();
    naming_.clear();
    return true;
}

bool WolChatSession::requestUserList()
{
    if (state_ != State::Ready || channel_.empty()) {
        return false;
    }
    outgoing_.push_back("NAMES #" + channel_);
    return true;
}

bool WolChatSession::requestPublicMessage(std::string_view wireText)
{
    if (state_ != State::Ready || channel_.empty()) {
        return false;
    }
    outgoing_.push_back("PRIVMSG #" + channel_ + " :" + std::string(wireText));
    return true;
}

bool WolChatSession::requestPublicAction(std::string_view wireText)
{
    if (state_ != State::Ready || channel_.empty()) {
        return false;
    }
    outgoing_.push_back("PRIVMSG #" + channel_ + " :\x01" "ACTION" + std::string(wireText) + "\x01");
    return true;
}

bool WolChatSession::requestPrivateMessage(const std::vector<std::string>& nicks, std::string_view wireText)
{
    if (state_ != State::Ready || nicks.empty()) {
        return false;
    }
    outgoing_.push_back("PRIVMSG " + joinNicks(nicks) + " :" + std::string(wireText));
    return true;
}

bool WolChatSession::requestPrivateAction(const std::vector<std::string>& nicks, std::string_view wireText)
{
    if (state_ != State::Ready || nicks.empty()) {
        return false;
    }
    outgoing_.push_back("PRIVMSG " + joinNicks(nicks) + " :\x01" "ACTION" + std::string(wireText) + "\x01");
    return true;
}

void WolChatSession::requestLogout()
{
    outgoing_.push_back("QUIT");
}

// One server line: ":prefix COMMAND params". Before the welcome (376) only the login's replies
// count; after it, the channel list, joins, leaves, user lists and messages become events.
void WolChatSession::line(std::string_view text)
{
    if (state_ == State::Failed || text.empty()) {
        return;
    }
    // Split off the optional ":prefix" (kept as the sender's nick) and the command word.
    std::string_view rest = text;
    std::string_view from;
    if (rest.front() == ':') {
        from = senderOf(word(rest).substr(1));
    }
    const std::string_view command = word(rest);
    // The rest after " :" (a trailing parameter), or empty.
    const auto trailing = [&](std::string_view r) {
        const auto colon = r.find(':');
        return colon == std::string_view::npos ? std::string_view{} : r.substr(colon + 1);
    };
    // The channel we're in, by its name without the '#'.
    const auto ours = [&](std::string_view name) { return !channel_.empty() && sameName(name, channel_); };
    // Answered in every state, the login included.
    if (command == "PING") {
        outgoing_.push_back("PONG " + std::string(rest));
        return;
    }
    if (command == "ERROR") {
        fail(std::string(text), state_ == State::LoggingIn);
        return;
    }
    if (state_ == State::LoggingIn) {
        if (command == "372" && text.find("Invalid Password") != std::string_view::npos) {
            // A wrong password for a registered nick (XWIS goes on to welcome a guest).
            const auto words = text.find(":- ");
            fail(std::string(words != std::string_view::npos ? text.substr(words + 3) : text), true);
            return;
        }
        if (command == "375" || command == "372" || command == "377") {
            // The message of the day: the text after ":- ", each line ended.
            if (const auto at = text.find(":- "); at != std::string_view::npos) {
                motd_ += std::string(text.substr(at + 3)) + "\r\n";
            }
            return;
        }
        if (command == "376") {
            state_ = State::Ready;
            events_.push_back({Event::Kind::Motd, {}, {}, {}, std::exchange(motd_, {})});
            return;
        }
        // Any other 4xx / 5xx numeric during the login fails it as refused.
        if (command.size() == 3 && (command[0] == '4' || command[0] == '5')) {
            fail(std::string(text), true); // (nick taken, bad password...)
        }
        return;
    }
    // 321 starts a channel list: nothing to do until its lines.
    if (command == "321") {
        return;
    }
    if (command == "403" || command == "471" || command == "473" || command == "474" || command == "475") {
        // A JOIN refused (no such channel, full, invite only, banned, a bad key).
        joining_ = false;
        Event e{Event::Kind::JoinFailed};
        e.text = std::string(command);
        events_.push_back(std::move(e));
        return;
    }
    if (command == "327" && listType_) {
        // A channel's line: "<us> #name users official flags[ :topic]", a chat channel (type 0).
        // Skips our own nick (the first parameter).
        word(rest);
        const std::string_view name = word(rest);
        if (name.size() < 2 || (name.front() != '#' && name.front() != '&')) {
            return;
        }
        Channel c;
        c.name = std::string(name.substr(1));
        c.users = toInt(word(rest));
        c.official = static_cast<std::uint32_t>(toInt(word(rest)));
        const std::string_view flags = word(rest);
        c.flags = static_cast<std::uint32_t>(toInt(flags.substr(0, flags.find(':'))));
        c.topic = std::string(trailing(rest));
        if (*listType_ == kWolAllChannels || *listType_ == 0) {
            listChannels_.push_back(std::move(c));
        }
        return;
    }
    if (command == "326" && listType_) {
        if (auto game = parseXwisGameLine(text)) {
            listGames_.push_back(std::move(*game));
        }
        return;
    }
    if (command == "323" && listType_) {
        // The list's end: OnChannelList.
        Event e{Event::Kind::ChannelList};
        e.channels = std::exchange(listChannels_, {});
        e.games = std::exchange(listGames_, {});
        listType_.reset();
        events_.push_back(std::move(e));
        return;
    }
    if (command == "JOIN") {
        // ":nick!u@h JOIN :0,0 #Lob_37_0"
        const auto space = rest.rfind(' ');
        std::string_view name = rest.substr(space == std::string_view::npos ? 0 : space + 1);
        if (!name.empty() && name.front() == ':') {
            name.remove_prefix(1);
        }
        name = dropHash(name);
        User u{std::string(from), 0};
        if (sameName(from, nick_)) {
            channel_ = std::string(name);
            joining_ = false;
            users_.clear();
            naming_.clear();
            u.flags |= User::kMe;
        } else if (!ours(name)) {
            return;
        } else {
            removeUser(from);
            users_.push_back(u);
        }
        events_.push_back({Event::Kind::ChannelJoin, std::string(name), u});
        return;
    }
    // A PART names its channel; a QUIT is someone leaving ours.
    if (command == "PART" || command == "QUIT") {
        // (A QUIT is someone leaving ours.)
        const std::string name(dropHash(command == "PART" ? word(rest) : std::string_view(channel_)));
        User u{std::string(from), 0};
        if (sameName(from, nick_)) {
            // (A PART we asked for: the channel was forgotten then.)
            u.flags |= User::kMe;
            if (ours(name)) {
                channel_.clear();
                users_.clear();
            }
        } else {
            const User* known = findUser(from);
            if (!ours(name) || !known) {
                return;
            }
            u = *known;
            removeUser(from);
        }
        events_.push_back({Event::Kind::ChannelLeave, name, u});
        return;
    }
    if (command == "KICK") {
        // ":kicker!u@h KICK #channel nick :why"
        const std::string_view name = dropHash(word(rest));
        const std::string_view kicked = word(rest);
        if (!ours(name)) {
            return;
        }
        User u{std::string(kicked), 0};
        if (sameName(kicked, nick_)) {
            u.flags |= User::kMe;
            channel_.clear();
            users_.clear();
        } else {
            removeUser(kicked);
        }
        Event e{Event::Kind::UserKick, std::string(name), u};
        e.other = std::string(from);
        events_.push_back(std::move(e));
        return;
    }
    if (command == "TOPIC") {
        const std::string_view name = dropHash(word(rest));
        if (ours(name)) {
            Event e{Event::Kind::ChannelTopic, std::string(name)};
            e.text = std::string(trailing(rest));
            events_.push_back(std::move(e));
        }
        return;
    }
    if (command == "353") {
        // The user list's lines: ": 353 us * #channel :nick,0,0 @op,0,0" ("@" an op, "+" a voice; ours 0x8000).
        const auto colon = rest.find(':');
        if (colon == std::string_view::npos) {
            return;
        }
        std::string_view head = rest.substr(0, colon);
        // Skips our nick and the "*" / "=" before the channel.
        word(head);
        word(head);
        if (!ours(dropHash(word(head)))) {
            return;
        }
        std::string_view names = rest.substr(colon + 1);
        // Each name is "nick,a,b"; only the nick and its op / voice prefix are kept.
        for (std::string_view n = word(names); !n.empty(); n = word(names)) {
            User u;
            if (n.front() == '@') {
                u.flags = User::kOp;
                n.remove_prefix(1);
            } else if (n.front() == '+') {
                u.flags = User::kVoice;
                n.remove_prefix(1);
            }
            u.name = std::string(n.substr(0, n.find(',')));
            if (sameName(u.name, nick_)) {
                u.flags |= User::kMe;
            }
            naming_.push_back(std::move(u));
        }
        return;
    }
    // 366 ends the names: the gathered 353s become the user list.
    if (command == "366") {
        word(rest);
        if (!ours(dropHash(word(rest)))) {
            return;
        }
        users_ = std::exchange(naming_, {});
        Event e{Event::Kind::UserList, channel_};
        e.users = users_;
        events_.push_back(std::move(e));
        return;
    }
    // A message or "/me" action, to our channel or privately to us.
    if (command == "PRIVMSG") {
        const std::string_view to = word(rest);
        std::string_view said = rest;
        if (!said.empty() && said.front() == ':') {
            said.remove_prefix(1);
        }
        // "\1ACTION" and the text to the closing "\1", as WOLAPI reads it.
        const bool action = said.starts_with("\x01" "ACTION");
        if (action) {
            said.remove_prefix(7);
            if (!said.empty() && said.back() == '\x01') {
                said.remove_suffix(1);
            }
        }
        if (!to.empty() && to.front() == '#') {
            // A channel's: ours, from someone in its list.
            const User* known = findUser(from);
            if (!ours(to.substr(1)) || !known) {
                return;
            }
            Event e{action ? Event::Kind::PublicAction : Event::Kind::PublicMessage, channel_, *known};
            e.text = std::string(said);
            events_.push_back(std::move(e));
            return;
        }
        Event e{action ? Event::Kind::PrivateAction : Event::Kind::PrivateMessage};
        e.user = User{std::string(from), 0};
        if (const User* known = findUser(from)) {
            e.user = *known;
        }
        e.text = std::string(said);
        events_.push_back(std::move(e));
        return;
    }
}

// ------------------------------------------------------------------------------ WolLobbyLists

void WolLobbyLists::clearLobbies()
{
    lobbies_.clear();
    current_ = -1;
}

int WolLobbyLists::addLobby(const WolChatSession::Channel& channel)
{
    // The lobby list's add in the chat (state 7): type 0, a lobby's name; before the first it
    // doesn't sort after (the lobbies' compare(new, old) < 1).
    if (channel.type != 0) {
        return -1;
    }
    const auto name = wolLobbyName(channel.name);
    if (!name) {
        return -1;
    }
    Lobby l{channel.name, *name, channel.users, channel.official};
    // Official lobbies first, then by channel name.
    const auto order = [](const Lobby& a, const Lobby& b) {
        if (b.official < a.official) {
            return -1;
        }
        if (a.official < b.official) {
            return 1;
        }
        return compareNoCase(a.channel, b.channel);
    };
    std::size_t at = 0;
    // Inserted before the first lobby it doesn't sort after; the current index moves with it.
    while (at < lobbies_.size() && order(l, lobbies_[at]) >= 1) {
        ++at;
    }
    lobbies_.insert(lobbies_.begin() + static_cast<std::ptrdiff_t>(at), std::move(l));
    if (current_ >= static_cast<int>(at)) {
        ++current_;
    }
    return static_cast<int>(at);
}

int WolLobbyLists::setCurrent(std::string_view channel)
{
    current_ = -1;
    for (std::size_t i = 0; i < lobbies_.size() && !channel.empty(); ++i) {
        if (sameName(lobbies_[i].channel, channel)) {
            current_ = static_cast<int>(i);
            break;
        }
    }
    return current_;
}

int WolLobbyLists::changeCurrentUsers(int delta, bool& removed)
{
    removed = false;
    if (current_ < 0) {
        return -1;
    }
    const int at = current_;
    Lobby& l = lobbies_[static_cast<std::size_t>(at)];
    l.users += delta;
    if (l.users == 0) {
        // Gone from the list, and no longer current.
        lobbies_.erase(lobbies_.begin() + at);
        current_ = -1;
        removed = true;
    }
    return at;
}

// The first lobby with fewer than 25 users; when every one is full, the caller's random index.
int WolLobbyLists::pickLobby(int random) const
{
    for (std::size_t i = 0; i < lobbies_.size(); ++i) {
        if (static_cast<unsigned>(lobbies_[i].users) < 25) {
            return static_cast<int>(i);
        }
    }
    return random >= 0 && random < static_cast<int>(lobbies_.size()) ? random : -1;
}

int WolLobbyLists::addUser(const User& user)
{
    // Ops first, then by name; inserted before the first user it doesn't sort after.
    const auto order = [](const User& a, const User& b) {
        if ((a.flags & User::kOp) && !(b.flags & User::kOp)) {
            return -1;
        }
        if (!(a.flags & User::kOp) && (b.flags & User::kOp)) {
            return 1;
        }
        return compareNoCase(a.name, b.name);
    };
    std::size_t at = 0;
    while (at < users_.size() && order(user, users_[at]) >= 1) {
        ++at;
    }
    users_.insert(users_.begin() + static_cast<std::ptrdiff_t>(at), user);
    return static_cast<int>(at);
}

int WolLobbyLists::removeUser(std::string_view name)
{
    // Found by name (_stricmp).
    for (std::size_t i = 0; i < users_.size(); ++i) {
        if (sameName(users_[i].name, name)) {
            users_.erase(users_.begin() + static_cast<std::ptrdiff_t>(i));
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ------------------------------------------------------------------------------------ WolChat

WolChat::WolChat(std::string nick, std::string apgarPassword, std::uint32_t nowMs)
    : startedAt_(nowMs)
{
    session_.emplace(std::move(nick), std::move(apgarPassword));
    std::string why;
    socket_ = detail::connectXwis(why);
    if (!socket_) {
        fail(std::move(why), false);
    }
}

void WolChat::fail(std::string why, bool refused)
{
    state_ = State::Failed;
    error_ = std::move(why);
    refused_ = refused;
    socket_.reset();
}

// Queues the session's lines and sends what the socket takes; the rest waits for the next poll.
void WolChat::flush()
{
    for (const auto& line : session_->takeOutgoing()) {
        send_ += line + "\r\n";
    }
    if (!send_.empty() && socket_ && socket_->state() == net::TcpSocket::State::Connected) {
        send_.erase(0, socket_->send(std::span(reinterpret_cast<const std::uint8_t*>(send_.data()), send_.size())));
    }
}

void WolChat::logOff(std::uint32_t nowMs)
{
    if (state_ == State::Failed || state_ == State::Closed || quitAt_ != 0) {
        return;
    }
    if (!socket_ || socket_->state() != net::TcpSocket::State::Connected) {
        state_ = State::Closed;
        socket_.reset();
        return;
    }
    session_->requestLogout();
    flush();
    quitAt_ = std::max<std::uint32_t>(nowMs, 1);  // 0 means not logging off
}

// Connect, flush the login, read lines into the session, flush replies; the session's state
// becomes ours (Ready, or Failed with its reason).
void WolChat::poll(std::uint32_t nowMs)
{
    if (state_ == State::Failed || state_ == State::Closed || !socket_) {
        return;
    }
    if (quitAt_ != 0) {
        // Logged off: the rest is drained until the server closes (or 3 seconds pass).
        flush();
        std::uint8_t drain[1024];
        while (socket_->receive(drain)) {
        }
        if (socket_->state() != net::TcpSocket::State::Connected || nowMs - quitAt_ > kCloseWaitMs) {
            state_ = State::Closed;
            socket_.reset();
        }
        return;
    }
    const auto socketState = socket_->state();
    if (socketState == net::TcpSocket::State::Failed) {
        fail("can't connect to " + std::string(kXwisHost), false);
        return;
    }
    if (state_ == State::Connecting && nowMs - startedAt_ > kLoginTimeoutMs) {
        fail("timed out", false);
        return;
    }
    if (socketState == net::TcpSocket::State::Connecting) {
        return;
    }
    flush();
    std::uint8_t buffer[4096];
    while (const auto got = socket_->receive(buffer)) {
        session_->receive(std::span(buffer, *got));
    }
    flush(); // (a PONG, at once)
    switch (session_->state()) {
    case WolChatSession::State::Failed:
        fail(session_->error(), session_->refused());
        return;
    case WolChatSession::State::Ready:
        state_ = State::Ready;
        break;
    default:
        break;
    }
    if (socket_->state() != net::TcpSocket::State::Connected) {
        fail(state_ == State::Ready ? "the connection closed" : "the connection closed early", false);
    }
}

} // namespace nox::lobby
