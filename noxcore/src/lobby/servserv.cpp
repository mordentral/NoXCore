// Asking servserv for the XWIS chat server, and the connection to it; see servserv.hpp.
#include "noxcore/lobby/servserv.hpp"

#include "noxcore/lobby/xwis.hpp" // (kXwisHost, kXwisPort)
#include "noxcore/strings/l10n.hpp"

#include "irc.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <utility>

namespace nox::lobby {

namespace {

constexpr auto word = detail::ircWord;

// "verchk 9472 65540" and the like.
std::string numbers(std::string_view command, std::uint32_t a, std::uint32_t b)
{
    return std::string(command) + " " + std::to_string(a) + " " + std::to_string(b);
}

// The decimal number at the front of `s` (atoi's reading: digits up to the first other character).
std::optional<unsigned> leadingNumber(std::string_view s)
{
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end == s.data()) {
        return std::nullopt;
    }
    return value;
}

// A language code as the game reads it (atoi): leading white space skipped, a sign, the digits up to
// the first other character; 0 when there are none.
long codeNumber(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) {
        ++i;
    }
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    long value = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9' && value < 1000000; ++i) {
        value = value * 10 + (s[i] - '0');
    }
    return negative ? -value : value;
}

std::string hostAndPort(std::string_view host, std::uint16_t port)
{
    return std::string(host) + ":" + std::to_string(port);
}

} // namespace

std::vector<std::string> servservRequest(std::string_view nick, std::string_view password)
{
    // The serial (the CD key) last: none.
    return {
        numbers("verchk", kWolapiSku, kWolapiVersion),
        numbers("verchk", kWolSku, kWolGameVersion),
        "lobcount " + std::to_string(kWolSku),
        "whereto " + std::string(nick) + " " + std::string(password) + " " + std::to_string(kWolSku) + " " +
            std::to_string(kWolGameVersion) + " ",
        "QUIT",
    };
}

std::optional<ChatServer> parseServservChatServer(std::string_view line)
{
    std::string_view rest = line;
    if (!rest.empty() && rest.front() == ':') {
        word(rest); // (the server's prefix, empty from XWIS: ": 605 ...")
    }
    if (word(rest) != "605") {
        return std::nullopt;
    }
    // After the target ("u") and its ':': "<host> <port> '<name>' <time zone> <latitude> <longitude>".
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    rest.remove_prefix(colon + 1);
    ChatServer server;
    server.host = std::string(word(rest));
    const std::string_view portText = word(rest);
    const auto port = leadingNumber(portText);
    if (server.host.empty() || !port || *port == 0 || *port > 0xFFFF) {
        return std::nullopt;
    }
    server.port = static_cast<std::uint16_t>(*port);
    // The name: between the quotes.
    const auto open = rest.find('\'');
    if (open != std::string_view::npos) {
        const auto close = rest.find('\'', open + 1);
        server.name = std::string(rest.substr(open + 1, close == std::string_view::npos ? std::string_view::npos : close - open - 1));
    }
    return server;
}

bool servesLanguage(const ChatServer& server, unsigned language)
{
    const std::string_view name = server.name;
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    // The codes: up to the ':', split at the commas (an empty one skipped); each read as atoi
    // would, so " 2" is 2 and "x" is 0.
    std::string_view codes = name.substr(0, name.find(':'));
    while (!codes.empty()) {
        const auto comma = codes.find(',');
        const std::string_view code = codes.substr(0, comma);
        if (!code.empty() && codeNumber(code) == static_cast<long>(language)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        codes.remove_prefix(comma + 1);
    }
    return false;
}

void ServservSession::receive(std::span<const std::uint8_t> bytes)
{
    detail::splitLines(partial_, bytes, [this](std::string_view l) { line(l); });
}

void ServservSession::line(std::string_view text)
{
    if (state_ == State::Done) {
        return;
    }
    if (auto server = parseServservChatServer(text)) {
        chatServers_.push_back(std::move(*server));
        return;
    }
    std::string_view rest = text;
    if (detail::ircHead(rest).command == "607") {
        state_ = State::Done;
    }
}

const ChatServer* ServservSession::pick(unsigned language) const
{
    if (state_ != State::Done) {
        return nullptr;
    }
    for (const auto& s : chatServers_) {
        if (servesLanguage(s, language)) {
            return &s;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------ XwisConnection

XwisConnection::XwisConnection(std::string_view nick, std::string_view password, std::uint32_t nowMs, XwisRoute route)
    : route_(std::move(route))
    , nick_(nick)
    , password_(password.empty() ? nick : password)
{
    if (route_.direct) {
        useFallback({}, nowMs);
        return;
    }
    if (route_.servservs.empty()) {
        // By decision, 2026-11-11: the community client's server server, then (after xwis.net:4000
        // itself) WOLAPI's own.
        servservs_.push_back({hostAndPort(kServservCommunityHost, kServservPort), std::nullopt});
        servservs_.push_back({hostAndPort(kServservHost, kServservPort), std::nullopt});
    } else {
        for (const auto& at : route_.servservs) {
            servservs_.push_back({at.toString(), at});
        }
    }
    askNext(nowMs);
}

std::string XwisConnection::failures() const
{
    std::string why;
    for (const auto& f : failures_) {
        why += (why.empty() ? "" : "; ") + f;
    }
    return why;
}

bool XwisConnection::moreToTry() const
{
    return !fallbackTried_ || next_ < servservs_.size();
}

void XwisConnection::moveOn(const std::string& why, std::uint32_t nowMs)
{
    servserv_.reset();
    socket_.reset();
    failures_.push_back(why);
    if (!fallbackTried_) {
        useFallback(failures(), nowMs);
    } else {
        askNext(nowMs);
    }
}

void XwisConnection::askNext(std::uint32_t nowMs)
{
    // The first server server before the fallback, the rest after it.
    const std::size_t end = fallbackTried_ ? servservs_.size() : std::min<std::size_t>(1, servservs_.size());
    while (next_ < end) {
        const Servserv& s = servservs_[next_++];
        std::optional<net::Endpoint> to = s.at;
        if (!to) {
            const auto colon = s.name.rfind(':');
            to = net::Endpoint::resolve(std::string_view(s.name).substr(0, colon), kServservPort);
        }
        if (!to) {
            failures_.push_back(l10n::format(NOX_TR("lobby.xwis.cant_resolve", "can't resolve {name}"), {{"name", s.name.substr(0, s.name.rfind(':'))}}));
            continue;
        }
        // (Asking the same address again would get the same answer, or none.)
        const bool again = std::any_of(tried_.begin(), tried_.end(), [&](const net::Endpoint& e) {
            return e.address == to->address && e.port == to->port;
        });
        if (again) {
            log_.push_back(l10n::format(NOX_TR("lobby.xwis.skipped", "XWIS: {name} skipped (the address just asked)"), {{"name", s.name}}));
            continue;
        }
        tried_.push_back(*to);
        try {
            servserv_ = net::TcpSocket::connect(*to);
        } catch (const std::exception& ex) {
            failures_.push_back(l10n::format(NOX_TR("lobby.xwis.cant_connect_why", "can't connect to {name}: {error}"), {{"name", s.name}, {"error", ex.what()}}));
            continue;
        }
        servservName_ = s.name;
        startedAt_ = nowMs;
        session_ = {};
        request_.clear();
        for (const auto& line : servservRequest(nick_, password_)) {
            request_ += line + "\r\n";
        }
        if (!failures_.empty()) {
            log_.push_back(l10n::format(NOX_TR("lobby.xwis.failed_asking", "XWIS: failed ({why}), asking servserv {name}"), {{"why", failures()}, {"name", s.name}}));
        }
        return;
    }
    if (!fallbackTried_) {
        useFallback(failures(), nowMs);
        return;
    }
    // Every way tried.
    failed_ = true;
    error_ = l10n::format(NOX_TR("lobby.xwis.cant_reach", "can't reach XWIS ({why})"), {{"why", failures()}});
    log_.push_back(l10n::format(NOX_TR("lobby.xwis.failed_all", "XWIS: failed ({why}), nothing left to try"), {{"why", failures()}}));
}

bool XwisConnection::poll(std::uint32_t nowMs)
{
    if (!servserv_) {
        // A chat connection that can't be opened (refused, or not open in the game's 30 s) moves on
        // while there's another way to try. (One lost after it was open is the login's or the
        // session's to see: its lines went on it, so no other server is tried in its place.)
        if (!socket_ || !moreToTry()) {
            return false;
        }
        const auto chat = chatState();
        if (chatOpened_) {
            return false;
        }
        if (chat == net::TcpSocket::State::Failed) {
            moveOn(l10n::format(NOX_TR("lobby.xwis.cant_connect", "can't connect to {name}"), {{"name", chatServer_}}), nowMs);
            return !asking();
        }
        if (chat == net::TcpSocket::State::Connecting && nowMs - chatStartedAt_ > kXwisChatTimeoutMs) {
            moveOn(l10n::format(NOX_TR("lobby.xwis.not_open", "{name}: not open in {n} s"), {{"name", chatServer_}, {"n", kXwisChatTimeoutMs / 1000}}), nowMs);
            return !asking();
        }
        return false;
    }
    const auto state = servserv_->state();
    if (state == net::TcpSocket::State::Failed) {
        moveOn(l10n::format(NOX_TR("lobby.xwis.cant_connect", "can't connect to {name}"), {{"name", servservName_}}), nowMs);
        return !asking();
    }
    if (nowMs - startedAt_ > route_.servservTimeoutMs) {
        const std::uint32_t ms = route_.servservTimeoutMs;
        moveOn(ms % 1000 == 0 ? l10n::format(NOX_TR("lobby.xwis.no_answer_s", "{name}: no answer in {n} s"), {{"name", servservName_}, {"n", ms / 1000}})
                              : l10n::format(NOX_TR("lobby.xwis.no_answer_ms", "{name}: no answer in {n} ms"), {{"name", servservName_}, {"n", ms}}),
               nowMs);
        return !asking();
    }
    if (state == net::TcpSocket::State::Connecting) {
        return false;
    }
    if (!request_.empty() && state == net::TcpSocket::State::Connected) {
        request_.erase(0, servserv_->send(std::span(reinterpret_cast<const std::uint8_t*>(request_.data()), request_.size())));
    }
    std::uint8_t buffer[2048];
    while (const auto got = servserv_->receive(buffer)) {
        session_.receive(std::span(buffer, *got));
    }
    if (session_.state() == ServservSession::State::Done) {
        servserv_.reset();
        if (const ChatServer* chat = session_.pick()) {
            useServserv(*chat, nowMs);
        } else {
            moveOn(l10n::format(session_.chatServers().empty()
                                    ? NOX_TR("lobby.xwis.no_chat_server", "{name}: it listed no chat server")
                                    : NOX_TR("lobby.xwis.no_chat_server_language", "{name}: it listed no chat server for the game's language"),
                                {{"name", servservName_}}),
                   nowMs);
        }
        return !asking();
    }
    if (servserv_->state() != net::TcpSocket::State::Connected) {
        moveOn(l10n::format(NOX_TR("lobby.xwis.closed_before_list_end", "{name}: it closed the connection before the end of its list"), {{"name", servservName_}}), nowMs);
        return !asking();
    }
    return false;
}

void XwisConnection::useServserv(const ChatServer& server, std::uint32_t nowMs)
{
    const std::string name = hostAndPort(server.host, server.port);
    const auto to = net::Endpoint::resolve(server.host, server.port);
    if (!to) {
        moveOn(l10n::format(NOX_TR("lobby.xwis.chat_unresolved", "{name}: it gave chat server {chat}, which can't be resolved"), {{"name", servservName_}, {"chat", name}}), nowMs);
        return;
    }
    log_.push_back(l10n::format(NOX_TR("lobby.xwis.gave_chat_server", "XWIS: {name} gave chat server {chat}"), {{"name", servservName_}, {"chat", name}}));
    connectChat(*to, name, nowMs);
}

void XwisConnection::useFallback(const std::string& why, std::uint32_t nowMs)
{
    fallbackTried_ = true;
    const std::string name = route_.fallback ? route_.fallback->toString() : hostAndPort(kXwisHost, kXwisPort);
    if (why.empty()) {
        log_.push_back(l10n::format(NOX_TR("lobby.xwis.connecting_straight", "XWIS: connecting straight to {name}"), {{"name", name}}));
    } else {
        log_.push_back(l10n::format(NOX_TR("lobby.xwis.servserv_failed", "XWIS: servserv failed ({why}), using {name}"), {{"why", why}, {"name", name}}));
    }
    const auto to = route_.fallback ? route_.fallback : net::Endpoint::resolve(kXwisHost, kXwisPort);
    if (!to) {
        chatServer_ = name;
        failures_.push_back(l10n::format(NOX_TR("lobby.xwis.cant_resolve", "can't resolve {name}"), {{"name", kXwisHost}}));
        askNext(nowMs);
        if (failed_ && failures_.size() == 1) {
            error_ = failures_.back(); // (only it was tried: "can't resolve xwis.net")
        }
        return;
    }
    connectChat(*to, name, nowMs);
}

void XwisConnection::connectChat(const net::Endpoint& to, const std::string& name, std::uint32_t nowMs)
{
    chatServer_ = name;
    chatStartedAt_ = nowMs;
    chatOpened_ = false;
    try {
        socket_ = net::TcpSocket::connect(to);
    } catch (const std::exception& ex) {
        socket_.reset();
        if (moreToTry()) {
            moveOn(l10n::format(NOX_TR("lobby.xwis.cant_connect_why", "can't connect to {name}: {error}"), {{"name", name}, {"error", ex.what()}}), nowMs);
            return;
        }
        failed_ = true;
        error_ = ex.what();
    }
}

net::TcpSocket::State XwisConnection::state()
{
    if (socket_) {
        const auto s = chatState();
        // (A refused connection with another way left is moved on from by the next poll; one lost
        // after it was open is told as it is.)
        return s == net::TcpSocket::State::Failed && !chatOpened_ && moreToTry() ? net::TcpSocket::State::Connecting : s;
    }
    return failed_ ? net::TcpSocket::State::Failed : net::TcpSocket::State::Connecting;
}

net::TcpSocket::State XwisConnection::chatState()
{
    const auto s = socket_->state();
    if (s == net::TcpSocket::State::Connected) {
        chatOpened_ = true;
    }
    return s;
}

std::size_t XwisConnection::send(std::span<const std::uint8_t> data)
{
    if (!socket_) {
        return 0;
    }
    chatState(); // (seen open before a send can lose it)
    return socket_->send(data);
}

std::optional<std::size_t> XwisConnection::receive(std::span<std::uint8_t> buffer)
{
    if (!socket_) {
        return std::nullopt;
    }
    chatState(); // (seen open before a receive can lose it)
    return socket_->receive(buffer);
}

std::string XwisConnection::error()
{
    if (!error_.empty()) {
        return error_;
    }
    if (state() == net::TcpSocket::State::Failed) {
        return l10n::format(chatOpened_ ? NOX_TR("lobby.xwis.lost_connection", "lost the connection to {name}")
                                        : NOX_TR("lobby.xwis.cant_connect", "can't connect to {name}"),
                            {{"name", chatServer_}});
    }
    return {};
}

} // namespace nox::lobby
