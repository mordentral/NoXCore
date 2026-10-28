// LiveSession (live_session.hpp): the lobby and game sockets, the extended query, and the headless
// client pumped from them each frame.
#include "live_session.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/net/transport.hpp"

#include <SDL3/SDL.h>

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

} // namespace

LiveSession::LiveSession(const std::string& hostAndPort, nox::client::PlayerProfile profile,
                         const nox::things::ThingDb& things, const fs::path& gameDir, const fs::path& mapCache, bool identifyFirst,
                         bool knownStock)
    : server_(resolve(hostAndPort))
    , client_(std::move(profile), &things, gameDir / "maps", mapCache)
    , lobby_(nox::net::UdpSocket::open())
    , game_(nox::net::UdpSocket::open())
    , mapsDir_(gameDir / "maps")
    , mapCache_(mapCache)
{
    SDL_Log("joining %s", server_.toString().c_str());
    // noxcpp's: the extended query from the socket the game will connect from, before it does (a
    // stock server drops it: lobby/extended.hpp), twice in case one's lost. A noxcpp server then
    // sends us its own extras (MSG_NOXCPP_SUBSTEP: the important objects between frames).
    // (Tests: with NOX_STOCK_STREAM set it isn't sent, so a noxcpp server sends us a stock client's
    // 30 Hz stream, as a GAME.EXE host does; a host whose content packs add object types then
    // refuses us as it does a stock client.)
    if (!SDL_getenv("NOX_STOCK_STREAM")) {
        // (We take a script mode's UI, of API 2 with its assets too: a server that knows
        // ClientFeature reads it, an older one the token alone. The object types our content
        // packs add, if any: a host with added types lets in only a game with the same ones. Our
        // client level: a host with a lowest one lets in only a game at it or above, and says it
        // in its answer.)
        const std::uint64_t types = nox::content::newThingTypes().hash;
        const auto query = nox::lobby::encodeExtendedQuery(
            nowMs(), nox::lobby::ClientFeature::ModeUi | nox::lobby::ClientFeature::ModeUi2, types, nox::lobby::kClientLevel);
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

void LiveSession::update()
{
    const std::uint32_t now = nowMs();
    std::uint8_t buffer[2048];
    // Lobby packets, then game packets, only from our server; a noxcpp server's extended reply is taken here.
    while (const auto got = lobby_.receive(buffer)) {
        if (got->from == server_) {
            client_.receiveLobby(std::span(buffer, got->size));
        }
    }
    while (const auto got = game_.receive(buffer)) {
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
        (d.lobby ? lobby_ : game_).sendTo(server_, d.bytes);
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
