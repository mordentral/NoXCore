// Checking a player's XWIS login (and listing the games on the same connection); see xwis_login.hpp.
#include "noxcore/lobby/xwis_login.hpp"

#include "irc.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace nox::lobby {

namespace {

constexpr std::uint32_t kTimeoutMs = 20000;

} // namespace

XwisLogin::XwisLogin(std::string nick, std::string apgarPassword, std::uint32_t nowMs)
    : startedAt_(nowMs)
{
    std::string why;
    socket_ = detail::connectXwis(why);
    if (!socket_) {
        fail(std::move(why), false);
        return;
    }
    session_.emplace(std::move(nick), std::move(apgarPassword), XwisSession::Purpose::List);
}

void XwisLogin::fail(std::string why, bool refused)
{
    state_ = State::Failed;
    error_ = std::move(why);
    refused_ = refused;
    socket_.reset();
    session_.reset();
}

// Connect, run the XwisSession to its end, then wait for the server to close the connection
// (at most 3 s) before reporting success.
void XwisLogin::poll(std::uint32_t nowMs)
{
    if (state_ != State::Pending || !socket_ || !session_) {
        return;
    }
    if (quitAt_ != 0) {
        std::uint8_t drain[1024];
        while (socket_->receive(drain)) {
        }
        if (socket_->state() != net::TcpSocket::State::Connected || nowMs - quitAt_ > 3000) {
            state_ = State::Succeeded;
            socket_.reset();
            session_.reset();
        }
        return;
    }
    const auto state = socket_->state();
    if (state == net::TcpSocket::State::Failed) {
        fail("can't connect to " + std::string(kXwisHost), false);
        return;
    }
    if (nowMs - startedAt_ > kTimeoutMs) {
        fail("timed out", false);
        return;
    }
    if (state == net::TcpSocket::State::Connecting) {
        return;
    }
    const auto flush = [&] {
        for (const auto& line : session_->takeOutgoing()) {
            send_ += line + "\r\n";
        }
        if (!send_.empty()) {
            send_.erase(0, socket_->send(std::span(reinterpret_cast<const std::uint8_t*>(send_.data()), send_.size())));
        }
    };
    flush();
    std::uint8_t buffer[4096];
    while (const auto got = socket_->receive(buffer)) {
        session_->receive(std::span(buffer, *got));
    }
    switch (session_->state()) {
    case XwisSession::State::Failed:
        // A failure the server reported counts as refused (a wrong password, a nick in use).
        fail(session_->error(), true);
        return;
    case XwisSession::State::Done:
        games_ = session_->games();
        flush(); // (its QUIT; then the server's close is waited for)
        quitAt_ = std::max<std::uint32_t>(nowMs, 1);
        return;
    default:
        break;
    }
    if (socket_->state() != net::TcpSocket::State::Connected) {
        fail("the connection closed early", false);
    }
}

} // namespace nox::lobby
