// LiveSession (live_session.hpp): the lobby and game sockets, the extended query, and the headless
// client pumped from them each frame.
#include "live_session.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/net/transport.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace noxsdl {

namespace fs = std::filesystem;

namespace {

std::uint32_t nowMs()
{
    return static_cast<std::uint32_t>(SDL_GetTicks());
}

// "host[:port]" to an endpoint (the default server port without one); throws if it doesn't resolve.
nox::net::Endpoint resolve(const std::string& hostAndPort)
{
    std::string host = hostAndPort;
    std::uint16_t port = nox::net::kDefaultServerPort;
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        port = static_cast<std::uint16_t>(std::stoi(host.substr(colon + 1)));
        host.resize(colon);
    }
    const auto server = nox::net::Endpoint::resolve(host, port);
    if (!server) {
        throw std::runtime_error("can't resolve " + hostAndPort);
    }
    return *server;
}

const char* kindName(nox::client::ClientEvent::Kind kind)
{
    using K = nox::client::ClientEvent::Kind;
    switch (kind) {
    case K::Log:
        return "log";
    case K::InGame:
        return "in game";
    case K::Chat:
        return "chat";
    case K::PlayerJoined:
        return "joined";
    case K::PlayerLeft:
        return "left";
    case K::Motd:
        return "motd";
    case K::Notice:
        return "notice";
    case K::Disconnected:
        return "disconnected";
    case K::Kill:
        return "kill";
    }
    return "?";
}

// The lobby's and the handshake's opcodes by name, for the join's log lines.
const char* joinOpName(std::uint8_t op)
{
    switch (op) {
    case 0x00: return "CONNECT";
    case 0x01: return "ACCEPT";
    case 0x02: case 0x03: case 0x04: return "CONNECT REFUSED";
    case 0x0C: return "DISCOVER";
    case 0x0D: return "SERVER_INFO";
    case 0x0E: return "TRY_JOIN";
    case 0x0F: return "PASSWORD_REQUIRED";
    case 0x10: return "SERVER_PING";
    case 0x11: return "PASSWORD";
    case 0x12: return "SERVER_PONG";
    case 0x13: return "SERVER_ERROR";
    case 0x14: return "JOIN_OK";
    case 0x15: return "JOIN_FAIL";
    case nox::lobby::kExtendedOp: return "NoX Core query/answer";
    default: return "";
    }
}

} // namespace

void LiveSession::joinLog(const char* fmt, ...)
{
    if (!joinLogging_ || joinLogLines_ >= kJoinLogLines) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    ++joinLogLines_;
    SDL_Log("join: %s%s", line, joinLogLines_ == kJoinLogLines ? " (no more join lines)" : "");
}

void LiveSession::logJoinDatagram(const char* socket, const nox::net::Endpoint& from, std::span<const std::uint8_t> d,
                                  const char* verdict)
{
    if (!joinLogging_ || joinLogLines_ >= kJoinLogLines) {
        return;
    }
    char hex[3 * 12 + 1] = {};
    std::size_t at = 0;
    for (std::size_t i = 0; i < d.size() && i < 12; ++i) {
        at += static_cast<std::size_t>(std::snprintf(hex + at, sizeof(hex) - at, i == 0 ? "%02X" : " %02X", d[i]));
    }
    // (Named on the lobby socket, and the NoX Core answer on the game one; the game socket's
    // others are the handshake's and the game's, XORed once connected: shown as they came.)
    const bool lobby = socket[0] == 'l';
    const char* op = d.size() >= 3 && (lobby || d[2] == nox::lobby::kExtendedOp) ? joinOpName(d[2]) : "";
    joinLog("+%u ms got %zu bytes on the %s socket from %s: %s%s%s%s (%s)", nowMs() - joinStartedAt_, d.size(), socket,
            from.toString().c_str(), hex, d.size() > 12 ? " ..." : "", *op ? " = " : "", op, verdict);
}

LiveSession::LiveSession(const std::string& hostAndPort, nox::client::PlayerProfile profile,
                         const nox::things::ThingDb& things, const fs::path& gameDir, const fs::path& mapCache, bool identifyFirst,
                         bool knownStock, std::uint64_t realm, bool stockShaped)
    : server_(resolve(hostAndPort))
    , client_(std::move(profile), &things, gameDir / "maps", mapCache)
    , lobby_(nox::net::UdpSocket::open())
    , game_(nox::net::UdpSocket::open())
    , mapsDir_(gameDir / "maps")
    , mapCache_(mapCache)
    , keyring_(gameDir / "noxcore_keys.txt")
{
    SDL_Log("joining %s", server_.toString().c_str());
    joinStartedAt_ = nowMs();
    const bool asks = !SDL_getenv("NOX_STOCK_STREAM") && !stockShaped;
    joinLog("to %s from our lobby socket's port %u (the join) and game socket's port %u (the game); %s", server_.toString().c_str(),
            lobby_.localPort(), game_.localPort(),
            asks ? "the NoX Core query twice from the game socket first"
                 : stockShaped ? "nothing before the join (an XWIS game that answered no ping: a GAME.EXE host's join)"
                               : "nothing before the join (NOX_STOCK_STREAM)");
    // noxcpp's: the extended query from the socket the game will connect from, before it does (a
    // stock server drops it: lobby/extended.hpp), twice in case one's lost. A noxcpp server then
    // sends us its own extras (MSG_NOXCPP_SUBSTEP: the important objects between frames).
    // (Tests: with NOX_STOCK_STREAM set it isn't sent, so a noxcpp server sends us a stock client's
    // 30 Hz stream, as a GAME.EXE host does; a host whose content packs add object types then
    // refuses us as it does a stock client.) Never to a host the game list knows only from XWIS
    // and never heard from (`stockShaped`): there the join is the 1.2b client's alone.
    if (asks) {
        // (We take a script mode's UI, of API 2 with its assets too: a server that knows
        // ClientFeature reads it, an older one the token alone. The object types our content
        // packs add, if any: a host with added types lets in only a game with the same ones. Our
        // client level: a host with a lowest one lets in only a game at it or above, and says it
        // in its answer. Our secret for the host's client key realm, when we know it.)
        const std::uint64_t types = nox::content::newThingTypes().hash;
        const auto query = this->query(realm);
        game_.sendTo(server_, query);
        game_.sendTo(server_, query);
        // (A host the game list saw as stock, or any host when only our level is new: the join
        // at once, never slower than before: joinWaits.)
        if (joinWaits(types != 0, identifyFirst, knownStock)) {
            typesQuery_ = query; // (the join waits for its answer: update)
            wait_.begin(nowMs());
            return;
        }
    }
    client_.start(nowMs());
    started_ = true;
}

std::vector<std::uint8_t> LiveSession::query(std::uint64_t realm)
{
    // Our script mode UI's features (API 2 with its assets too), our content packs' added object
    // types, our client level, and our secret for the host's client key realm (made the first time
    // we meet it: client_keys.hpp).
    const auto secret = keyring_.secretFor(realm);
    if (secret) {
        keyedRealm_ = realm;
    }
    return nox::lobby::encodeExtendedQuery(nowMs(), nox::lobby::ClientFeature::ModeUi | nox::lobby::ClientFeature::ModeUi2,
                                           nox::content::newThingTypes().hash, nox::lobby::kClientLevel, secret);
}

void LiveSession::update()
{
    const std::uint32_t now = nowMs();
    std::uint8_t buffer[2048];
    // Lobby packets from our server's address (from any port, as the 1.2b client's lobby receive
    // takes them: it compares the address alone), then game packets from our server alone; a
    // noxcpp server's extended reply is taken here.
    while (const auto got = lobby_.receive(buffer)) {
        const std::span<const std::uint8_t> d(buffer, got->size);
        const bool host = got->from.address == server_.address;
        if (joinLogging_) {
            ++lobbyHeard_;
            joinDropped_ += host ? 0 : 1;
            logJoinDatagram("lobby", got->from, d, host ? "taken" : "dropped: not the host's address");
        }
        if (host) {
            lobbyFrom_ = got->from;
            client_.receiveLobby(d);
        }
    }
    while (const auto got = game_.receive(buffer)) {
        if (joinLogging_) {
            ++gameHeard_;
            joinDropped_ += got->from == server_ ? 0 : 1;
            logJoinDatagram("game", got->from, std::span(buffer, got->size),
                            got->from == server_ ? "taken" : "dropped: not the host's address and port");
        }
        if (got->from == server_) {
            const std::span<const std::uint8_t> d(buffer, got->size);
            if (beforeDatagram) {
                beforeDatagram(d);
            }
            if (const auto reply = nox::lobby::decodeExtendedReply(d)) {
                if (!extended_) {
                    SDL_Log("noxcpp server: up to %d updates a second", 30 * reply->server.maxMultiplier);
                }
                extended_ = reply->server;
                client_.setFreshInput(true); // (our facing and moves as they change: its next frame has them)
                // A host with a client key realm our query had no secret for: the query again with
                // it, before our connect (at a listed one the join waits for this answer).
                if (reply->server.realm != 0 && reply->server.realm != keyedRealm_ && !SDL_getenv("NOX_STOCK_STREAM")) {
                    const auto keyed = query(reply->server.realm);
                    if (keyedRealm_ == reply->server.realm) {
                        game_.sendTo(server_, keyed);
                        game_.sendTo(server_, keyed);
                        if (!typesQuery_.empty()) {
                            typesQuery_ = keyed;
                        }
                    }
                }
                continue;
            }
            client_.receive(d, now);
        }
    }
    if (afterDatagrams) {
        afterDatagrams();
    }
    if (!started_) {
        // The query (naming our added object types, or to a NoX Core-only game or one with a lowest level),
        // again till the host answers it (or a stock one never does); then the join.
        if (quitting_ || tooOld_) {
            return;
        }
        if (extended_ && extended_->minClientLevel > nox::lobby::kClientLevel) {
            tooOld_ = true; // (the host refuses us: no join, the menus' notice says why)
            SDL_Log("noxcpp server: lets in client level %d and above; ours is %d", extended_->minClientLevel, nox::lobby::kClientLevel);
            return;
        }
        switch (wait_.step(now, extended_.has_value())) {
        case IdentifyWait::Step::Resend: game_.sendTo(server_, typesQuery_); return;
        case IdentifyWait::Step::Wait: return;
        case IdentifyWait::Step::Join: break;
        }
        client_.start(now);
        started_ = true;
    }
    client_.tick(now);
    const auto outgoing = client_.takeOutgoing();
    if (!outgoing.empty() && beforeSend) {
        beforeSend();
    }
    for (const auto& d : outgoing) {
        // (The ping test's pong goes back where the ping came from, as the 1.2b client's.)
        const bool pong = d.lobby && d.bytes.size() >= 3 && d.bytes[2] == static_cast<std::uint8_t>(nox::net::Op::ServerPong);
        const nox::net::Endpoint to = pong && lobbyFrom_ ? *lobbyFrom_ : server_;
        if (joinLogging_ && d.bytes.size() >= 3 &&
            (d.lobby || client_.stage() == nox::client::HeadlessClient::Stage::Joining)) {
            // (each lobby send; on the game socket, the handshake's: the connect and our ACCEPT)
            joinLog("+%u ms sent %zu bytes (%s%s%02X %02X %02X) from the %s socket's port %u to %s", now - joinStartedAt_,
                    d.bytes.size(), d.lobby ? joinOpName(d.bytes[2]) : "", d.lobby ? ": " : "", d.bytes[0], d.bytes[1], d.bytes[2],
                    d.lobby ? "lobby" : "game", (d.lobby ? lobby_ : game_).localPort(), to.toString().c_str());
        }
        (d.lobby ? lobby_ : game_).sendTo(to, d.bytes);
    }
    // (The join's log lines end once we're in the game, or with a summary when the join failed.)
    if (joinLogging_ && started_) {
        using Stage = nox::client::HeadlessClient::Stage;
        const auto stage = client_.stage();
        if (stage == Stage::Failed) {
            joinLogLines_ = std::min(joinLogLines_, kJoinLogLines - 1); // (the summary always)
            joinLog("ended after %u ms: heard %u datagram(s) on the lobby socket and %u on the game socket (%u of them not "
                    "from the host), %u of our sends bounced (port unreachable)",
                    now - joinStartedAt_, lobbyHeard_, gameHeard_, joinDropped_, lobby_.bounced() + game_.bounced());
        }
        if (stage == Stage::InGame || stage == Stage::Failed || stage == Stage::Closed) {
            joinLogging_ = false;
        }
    }
    for (const auto& e : client_.takeEvents()) {
        if (e.name.empty()) {
            SDL_Log("[%s] %s", kindName(e.kind), e.text.c_str());
        } else {
            SDL_Log("[%s] %s: %s", kindName(e.kind), e.name.c_str(), e.text.c_str());
        }
        if (e.kind == nox::client::ClientEvent::Kind::Motd) {
            motd_ = e.text;
            motdNew_ = true;
        }
        if (onEvent) {
            onEvent(e);
        }
    }
}

void LiveSession::quit()
{
    if (!quitting_) {
        quitting_ = true;
        quitAt_ = nowMs();
        if (started_) {
            client_.quit();
        }
    }
}

bool LiveSession::done() const
{
    if (!started_ && (quitting_ || tooOld_)) {
        return true; // (left before the join began, or the host's lowest client level above ours)
    }
    using Stage = nox::client::HeadlessClient::Stage;
    const auto stage = client_.stage();
    if (stage == Stage::Closed || stage == Stage::Failed) {
        return true;
    }
    // The leave normally takes a round trip; don't hang if the server stopped answering.
    return quitting_ && nowMs() - quitAt_ > 25'000;
}

std::optional<fs::path> LiveSession::mapPath() const
{
    // The file the client loaded for the server's map: the maps folder's or the download cache's
    // of USE_MAP's CRC, or a solo game's saved copy (HeadlessClient::setSoloMap).
    if (client_.mapPath().empty()) {
        return std::nullopt;
    }
    return client_.mapPath();
}

} // namespace noxsdl
