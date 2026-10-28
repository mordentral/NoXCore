// Posting a hosted game to XWIS: the login, the game channel and its topic; see xwis_host.hpp.
#include "noxcore/lobby/xwis_host.hpp"

#include "noxcore/net/udp_socket.hpp" // (net::Endpoint)

#include "irc.hpp"

#include <cctype>
#include <cstdio>
#include <exception>
#include <utility>

namespace nox::lobby {

namespace {

// How long a failed connection waits before trying again.
constexpr std::uint32_t kRetryMs = 30000;

// (irc.hpp)
constexpr auto word = detail::ircWord;
constexpr auto sameName = detail::ircSameName;

} // namespace

std::string xwisGameTopic(std::span<const std::uint8_t> record)
{
    // WOLAPI's prefix for GAME.EXE's channel: in a game, 1 to 32 users, three bytes
    // of extra info (the version 0x1039A, low byte first).
    std::string topic = "G1P3";
    topic += static_cast<char>(0x9A);
    topic += static_cast<char>(0x03);
    topic += static_cast<char>(0x01);
    for (const std::uint8_t b : packSevenBit(record)) {
        if (b == 0) {
            break; // (a C string: a last group of nothing but zeros ends it)
        }
        topic += static_cast<char>(b);
    }
    return topic;
}

XwisHostSession::XwisHostSession(std::string nick, std::string apgarPassword, std::uint32_t place)
    : nick_(std::move(nick))
    , channel_("#" + nick_ + "'s_game")
    , place_(place)
    , outgoing_(detail::xwisLoginLines(nick_, apgarPassword))
{
}

void XwisHostSession::setRecord(std::span<const std::uint8_t> record)
{
    topic_ = xwisGameTopic(record);
    sendTopic();
}

// Only once in the channel, and only when the topic differs from the last one sent.
void XwisHostSession::sendTopic()
{
    if (state_ == State::Hosting && !topic_.empty() && topic_ != topicSent_) {
        outgoing_.push_back("TOPIC " + channel_ + " :" + topic_);
        topicSent_ = topic_;
    }
}

void XwisHostSession::quit()
{
    if (state_ == State::Hosting || state_ == State::Joining) {
        outgoing_.push_back("PART " + channel_);
    }
    outgoing_.push_back("QUIT");
    state_ = State::Failed;
    error_ = "left";
}

std::vector<std::string> XwisHostSession::takeOutgoing()
{
    return std::exchange(outgoing_, {});
}

// Splits the bytes into lines (CR LF or LF); a partial last line waits for more.
void XwisHostSession::receive(std::span<const std::uint8_t> bytes)
{
    detail::splitLines(partial_, bytes, [this](std::string_view l) { line(l); });
}

// One server line: LoggingIn -> (376) Joining -> (our JOIN echoed) Hosting; an error reply
// before Hosting fails the session.
void XwisHostSession::line(std::string_view text)
{
    if (state_ == State::Failed || text.empty()) {
        return;
    }
    std::string_view rest = text;
    std::string_view from;
    if (rest.front() == ':') {
        const std::string_view prefix = word(rest).substr(1);
        from = prefix.substr(0, prefix.find('!'));
    }
    const std::string_view command = word(rest);
    if (command == "PING") {
        outgoing_.push_back("PONG " + std::string(rest));
    } else if (command == "ERROR") {
        state_ = State::Failed;
        error_ = std::string(text);
    } else if (command == "372" && state_ == State::LoggingIn && text.find("Invalid Password") != std::string_view::npos) {
        state_ = State::Failed;
        error_ = std::string(text);
    } else if (command == "376" && state_ == State::LoggingIn) {
        // Logged in: the game's channel (WOLAPI's JOINGAME; the key, none, after the reserved word).
        state_ = State::Joining;
        char buf[160];
        std::snprintf(buf, sizeof buf, "JOINGAME %s %d %d %d %d %d %d %u ", channel_.c_str(), 1, 32, 37, 3, 1, 1, place_);
        outgoing_.emplace_back(buf);
    } else if ((command == "JOIN" || command == "JOINGAME") && state_ == State::Joining && sameName(from, nick_)) {
        // Our own join echoed back: in the channel, so the record goes up as its topic.
        state_ = State::Hosting;
        sendTopic();
    } else if (command.size() == 3 && (command[0] == '4' || command[0] == '5') && state_ != State::Hosting) {
        // An error reply while logging in or making the channel (a nick in use, the channel taken).
        state_ = State::Failed;
        error_ = std::string(text);
    }
}

XwisHost::XwisHost(std::string nick, std::string apgarPassword, std::uint32_t nowMs, std::uint32_t place)
    : nick_(std::move(nick))
    , password_(std::move(apgarPassword))
    , place_(place)
{
    connect(nowMs);
}

XwisHost::~XwisHost()
{
    stop();
}

// Starts over: a new socket and session (the record re-posted once in the channel); retries in 30 s.
void XwisHost::connect(std::uint32_t nowMs)
{
    socket_.reset();
    session_.reset();
    send_.clear();
    retryAt_ = nowMs + kRetryMs;
    socket_ = detail::connectXwis(error_);
    if (!socket_) {
        return;
    }
    session_.emplace(nick_, password_, place_);
    if (!record_.empty()) {
        session_->setRecord(record_);
    }
}

void XwisHost::setRecord(std::span<const std::uint8_t> record)
{
    record_.assign(record.begin(), record.end());
    if (session_) {
        session_->setRecord(record_);
    }
}

void XwisHost::flush()
{
    for (const auto& line : session_->takeOutgoing()) {
        send_ += line + "\r\n";
    }
    if (!send_.empty() && socket_ && socket_->state() == net::TcpSocket::State::Connected) {
        send_.erase(0, socket_->send(std::span(reinterpret_cast<const std::uint8_t*>(send_.data()), send_.size())));
    }
}

// A broken connection or failed session waits for retryAt_ and reconnects; otherwise sends and
// receives lines.
void XwisHost::poll(std::uint32_t nowMs)
{
    if (stopped_) {
        return;
    }
    const bool broken = !socket_ || !session_ || socket_->state() == net::TcpSocket::State::Failed ||
                        socket_->state() == net::TcpSocket::State::Closed || session_->state() == XwisHostSession::State::Failed;
    if (broken) {
        if (session_ && session_->state() == XwisHostSession::State::Failed && error_ != session_->error()) {
            error_ = session_->error();
        }
        // Signed difference, so the clock wrapping doesn't matter.
        if (static_cast<std::int32_t>(nowMs - retryAt_) >= 0) {
            connect(nowMs);
        }
        return;
    }
    if (socket_->state() == net::TcpSocket::State::Connecting) {
        return;
    }
    flush();
    std::uint8_t buffer[4096];
    while (const auto got = socket_->receive(buffer)) {
        session_->receive(std::span(buffer, *got));
    }
    flush();
    if (session_->state() == XwisHostSession::State::Failed) {
        retryAt_ = nowMs + kRetryMs;
    }
}

void XwisHost::stop()
{
    if (stopped_) {
        return;
    }
    stopped_ = true;
    if (socket_ && session_ && socket_->state() == net::TcpSocket::State::Connected) {
        session_->quit();
        flush();
    }
    socket_.reset();
    session_.reset();
}

} // namespace nox::lobby
