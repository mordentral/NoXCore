// The server browser: XWIS, LAN broadcast and listed servers merged into one list and pinged;
// see browser.hpp.
#include "noxcore/lobby/browser.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"

#include <algorithm>
#include <charconv>
#include <exception>

namespace nox::lobby {

namespace {

constexpr std::uint32_t kXwisTimeoutMs = 15'000;
constexpr std::uint32_t kPingRetryMs = 1'000;
constexpr int kPingTries = 3;
constexpr std::uint32_t kBroadcastWaitMs = 1'500;
constexpr std::uint32_t kBroadcast = 0xFFFFFFFF; // 255.255.255.255

/// "host" or "host:port" as an endpoint (the game port by default).
std::optional<net::Endpoint> parseServer(std::string_view text)
{
    std::uint16_t port = kDefaultGamePort;
    if (const auto colon = text.rfind(':'); colon != std::string_view::npos) {
        const std::string_view p = text.substr(colon + 1);
        if (std::from_chars(p.data(), p.data() + p.size(), port).ec != std::errc{}) {
            return std::nullopt;
        }
        text = text.substr(0, colon);
    }
    return net::Endpoint::resolve(text, port);
}

} // namespace

ServerBrowser::ServerBrowser(Options options)
    : options_(std::move(options)), rng_(std::random_device{}())
{
}

void ServerBrowser::setXwisListing(bool on)
{
    xwisListing_ = on;
    if (!on) {
        closeXwis({});
    }
}

bool ServerBrowser::searching() const
{
    return xwis_.has_value() || !pings_.empty() || !queued_.empty();
}

// The entry for this address and port, made if it isn't there yet.
ServerEntry& ServerBrowser::upsert(std::uint32_t address, std::uint16_t port)
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const ServerEntry& e) { return e.game.address == address && e.game.port == port; });
    if (it != entries_.end()) {
        return *it;
    }
    ServerEntry& e = entries_.emplace_back();
    e.game.address = address;
    e.game.port = port;
    return e;
}

void ServerBrowser::ping(const net::Endpoint& to, std::uint32_t nowMs, bool broadcast)
{
    if (!udp_) {
        return;
    }
    // MSG_SERVER_DISCOVER on the lobby channel (header 00 00); the server echoes the token.
    std::uint32_t token = 0;
    // A fresh nonzero token not already waiting for a reply.
    do {
        token = static_cast<std::uint32_t>(rng_());
    } while (token == 0 || pings_.contains(token));
    net::msg::ServerDiscover discover;
    discover.token = token;
    ByteWriter w;
    w.u8(0);
    w.u8(0);
    discover.write(w);
    try {
        udp_->sendTo(to, w.data());
        pings_[token] = {to, nowMs, 1, broadcast};
        // Beside it, in its own datagram, the query only a noxcpp server answers (a stock one
        // drops it: extended.hpp).
        udp_->sendTo(to, encodeExtendedQuery(token));
        queried_.insert(token);
    } catch (const std::exception& ex) {
        log_.push_back("ping " + to.toString() + ": " + ex.what());
    }
}

// Forgets the last search, opens the UDP socket and queues the pings; XWIS is either taken as
// given or logged into (pollXwis carries the login on).
void ServerBrowser::refresh(std::uint32_t nowMs, const std::vector<GameListing>* xwisGames)
{
    entries_.clear();
    pings_.clear();
    queried_.clear();
    closeXwis({});
    try {
        if (!udp_) {
            udp_ = net::UdpSocket::open(0);
            udp_->enableBroadcast();
        }
    } catch (const std::exception& ex) {
        log_.push_back(std::string("UDP: ") + ex.what());
        udp_.reset();
    }

    // Pings go out from poll(), so name lookups here don't count toward them.
    queued_.clear();
    if (options_.lan) {
        queued_.push_back({options_.lanQueryTo.value_or(net::Endpoint{kBroadcast, kDefaultGamePort}), true});
    }
    for (const auto& s : options_.servers) {
        if (const auto to = parseServer(s)) {
            ServerEntry& e = upsert(to->address, to->port);
            e.sources |= kFromList;
            if (e.game.name.empty()) {
                e.game.name = s;
            }
            queued_.push_back({*to, false});
        } else {
            log_.push_back("can't resolve " + s);
        }
    }
    // Network (LAN play) lists no XWIS: neither a login nor the games given.
    const bool xwis = listsXwis();
    if (xwis && xwisGames) {
        // Listed already (by a player's login, which XWIS won't let us repeat for a few seconds).
        log_.push_back("XWIS: " + std::to_string(xwisGames->size()) + " game(s)");
        for (const auto& g : *xwisGames) {
            ServerEntry& e = upsert(g.address, g.port);
            e.game = g;
            e.sources |= kFromXwis;
            queued_.push_back({{g.address, g.port}, false});
        }
    } else if (xwis) {
        const auto host = options_.xwisServer ? options_.xwisServer : net::Endpoint::resolve(kXwisHost, kXwisPort);
        if (!host) {
            log_.push_back("XWIS: can't resolve " + std::string(kXwisHost));
        } else {
            try {
                xwisSocket_ = net::TcpSocket::connect(*host);
                const std::string nick =
                    options_.xwisNick.empty() ? xwisNick(static_cast<std::uint32_t>(rng_())) : options_.xwisNick;
                xwis_.emplace(nick, options_.xwisPassword);
                xwisStartedAt_ = nowMs;
                log_.push_back("XWIS: logging in as " + nick);
            } catch (const std::exception& ex) {
                log_.push_back(std::string("XWIS: ") + ex.what());
            }
        }
    }
}

void ServerBrowser::closeXwis(const std::string& why)
{
    if (!why.empty()) {
        log_.push_back(why);
    }
    xwisSocket_.reset();
    xwis_.reset();
    xwisSend_.clear();
}

void ServerBrowser::poll(std::uint32_t nowMs)
{
    for (const auto& [to, broadcast] : std::exchange(queued_, {})) {
        ping(to, nowMs, broadcast);
    }
    pollXwis(nowMs);
    pollUdp(nowMs);
}

// Drives the XWIS listing login: connect, send its lines, read replies; when it's done, its
// games are added and pinged.
void ServerBrowser::pollXwis(std::uint32_t nowMs)
{
    if (!xwis_ || !xwisSocket_) {
        return;
    }
    const auto state = xwisSocket_->state();
    if (state == net::TcpSocket::State::Failed) {
        closeXwis("XWIS: can't connect to " + std::string(kXwisHost));
        return;
    }
    if (nowMs - xwisStartedAt_ > kXwisTimeoutMs) {
        closeXwis("XWIS: timed out");
        return;
    }
    if (state == net::TcpSocket::State::Connecting) {
        return;
    }
    // Connected: send what the session queued (the rest waits for the next poll), then feed it every reply.
    for (const auto& line : xwis_->takeOutgoing()) {
        xwisSend_ += line;
        xwisSend_ += "\r\n";
    }
    if (!xwisSend_.empty()) {
        const auto sent = xwisSocket_->send(
            std::span(reinterpret_cast<const std::uint8_t*>(xwisSend_.data()), xwisSend_.size()));
        xwisSend_.erase(0, sent);
    }
    std::uint8_t buffer[4096];
    while (const auto got = xwisSocket_->receive(buffer)) {
        xwis_->receive(std::span(buffer, *got));
    }
    const auto session = xwis_->state();
    if (session == XwisSession::State::Failed) {
        closeXwis("XWIS: " + xwis_->error());
        return;
    }
    if (session == XwisSession::State::Done) {
        // Send the QUIT it queued, then list and ping the games.
        for (const auto& line : xwis_->takeOutgoing()) {
            xwisSend_ += line + "\r\n";
        }
        xwisSocket_->send(std::span(reinterpret_cast<const std::uint8_t*>(xwisSend_.data()), xwisSend_.size()));
        const auto games = xwis_->games();
        closeXwis("XWIS: " + std::to_string(games.size()) + " game(s)");
        for (const auto& g : games) {
            ServerEntry& e = upsert(g.address, g.port);
            if (!e.answered) {
                e.game = g;
            }
            e.sources |= kFromXwis;
            ping({g.address, g.port}, nowMs);
        }
        return;
    }
    if (xwisSocket_->state() != net::TcpSocket::State::Connected) {
        closeXwis("XWIS: the connection closed early");
    }
}

// Reads every datagram waiting: extended replies mark noxcpp servers, SERVER_INFO replies to our
// pings update their entries; then unanswered pings are retried or dropped.
void ServerBrowser::pollUdp(std::uint32_t nowMs)
{
    if (!udp_) {
        return;
    }
    std::uint8_t buffer[1024];
    for (;;) {
        std::optional<net::UdpSocket::Received> got;
        try {
            got = udp_->receive(buffer);
        } catch (const std::exception& ex) {
            log_.push_back(std::string("UDP: ") + ex.what());
            break;
        }
        if (!got) {
            break;
        }
        const std::span<const std::uint8_t> d(buffer, got->size);
        // A noxcpp server's extended reply, matched to one of our queries by its token.
        if (const auto reply = decodeExtendedReply(d); reply && queried_.contains(reply->token)) {
            upsert(got->from.address, got->from.port).extended = reply->server;
            continue;
        }
        // SERVER_INFO on the lobby channel: header 00 00, then the opcode.
        if (d.size() < 3 || d[0] != 0 || d[1] != 0 || d[2] != static_cast<std::uint8_t>(net::Op::ServerInfo)) {
            continue;
        }
        net::msg::ServerInfo info;
        try {
            ByteReader r(d.subspan(3), "SERVER_INFO");
            info = net::msg::ServerInfo::read(r);
        } catch (const std::exception&) {
            continue;
        }
        // Our ping's token, echoed back in SERVER_INFO.
        const auto p = pings_.find(info.token);
        if (p == pings_.end()) {
            continue; // not ours, or already answered
        }
        ServerEntry& e = upsert(got->from.address, got->from.port);
        const unsigned sources = e.sources | (p->second.broadcast ? kFromLan : 0u);
        std::string channel = std::move(e.game.channel); // (only the game list knows them)
        std::string host = std::move(e.game.host);
        e.game = listingFromServerInfo(info, got->from.address, got->from.port); // (extended kept)
        e.game.channel = std::move(channel);
        e.game.host = std::move(host);
        e.sources = sources;
        e.pingMs = nowMs - p->second.sentAt;
        e.answered = true;
        if (!p->second.broadcast) {
            pings_.erase(p); // a broadcast collects every reply until it times out
        }
    }

    // Try again the ones that haven't answered; give up after a few tries.
    std::vector<Ping> retry;
    for (auto it = pings_.begin(); it != pings_.end();) {
        const Ping& p = it->second;
        const std::uint32_t wait = p.broadcast ? kBroadcastWaitMs : kPingRetryMs;
        if (nowMs - p.sentAt < wait) {
            ++it;
            continue;
        }
        if (!p.broadcast && p.tries < kPingTries) {
            retry.push_back(p);
        }
        it = pings_.erase(it);
    }
    for (const Ping& old : retry) {
        ping(old.to, nowMs);
        // The retry gets a new token; carry the count over.
        for (auto& [token, p] : pings_) {
            if (p.to == old.to && p.sentAt == nowMs) {
                p.tries = old.tries + 1;
            }
        }
    }
}

} // namespace nox::lobby
