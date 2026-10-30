// A live game: the headless client with its sockets, pumped from the SDL loop.
#pragma once

#include "identify_wait.hpp"

#include "noxcore/client/headless_client.hpp"
#include "noxcore/lobby/client_keys.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/net/udp_socket.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace noxsdl {

class LiveSession {
public:
    /// Throws if the host can't be resolved or the sockets can't be opened. `identifyFirst`: the
    /// join waits for the host's answer to our extended query (a NoX Core-only game's, or one with
    /// a lowest client level, as listed: joinWaitsForIdentity), as it does when our content packs
    /// add object types; our level alone never makes it wait (joinWaits). `knownStock`:
    /// the game list saw the host answer its ping but not the extended query (a GAME.EXE host):
    /// the join starts at once, never waiting for an answer that won't come.
    /// `realm`: the host's client key realm, when known before (the game list's answer, or our own
    /// host's): the first query carries our secret for it (client_keys.hpp); else a keyed query
    /// follows as soon as the host's answer names one.
    LiveSession(const std::string& hostAndPort, nox::client::PlayerProfile profile, const nox::things::ThingDb& things,
                const std::filesystem::path& gameDir, const std::filesystem::path& mapCache, bool identifyFirst = false,
                bool knownStock = false, std::uint64_t realm = 0);

    /// Receives, runs the client and sends. Call every frame.
    void update();
    /// Starts the vanilla leave sequence; keep calling update() until done().
    void quit();
    bool done() const;
    /// Whether quit() was called (our leave under way, or done).
    bool quitting() const noexcept { return quitting_; }

    nox::client::HeadlessClient& client() { return client_; }
    const nox::client::HeadlessClient& client() const { return client_; }
    /// The game socket's port (the server sees us at it).
    std::uint16_t gamePort() const { return game_.localPort(); }

    /// The .map file the client loaded for the server's map (the game's copy, a download, or a
    /// solo game's saved copy); none while it's still coming.
    std::optional<std::filesystem::path> mapPath() const;

    /// Called with each of the client's events (chat, notices, joins...) as well as logging it.
    std::function<void(const nox::client::ClientEvent&)> onEvent;
    /// Called with each game datagram from the server before the client takes it, and before the
    /// client's own datagrams go out (the game we host: LocalHost's order and client view).
    std::function<void(std::span<const std::uint8_t>)> beforeDatagram;
    /// Called once every game datagram waiting has been taken (each update).
    std::function<void()> afterDatagrams;
    std::function<void()> beforeSend;
    /// The server's message of the day, once it has sent it.
    const std::string& motd() const { return motd_; }
    /// One came that isn't shown yet; taking it clears it.
    bool takeNewMotd() { return std::exchange(motdNew_, false); }
    /// A noxcpp server's answer to our extended query (none from a stock one).
    const std::optional<nox::lobby::ExtendedServer>& extended() const { return extended_; }
    /// The host's answer said its lowest client level is above ours: no join made (done()).
    bool tooOld() const noexcept { return tooOld_; }

private:
    std::optional<nox::lobby::ExtendedServer> extended_;
    std::string motd_;
    bool motdNew_ = false;
    nox::net::Endpoint server_;
    nox::client::HeadlessClient client_;
    nox::net::UdpSocket lobby_;
    nox::net::UdpSocket game_;
    std::filesystem::path mapsDir_;
    std::filesystem::path mapCache_;
    std::uint32_t quitAt_ = 0;
    bool quitting_ = false;
    // Our content packs' added object types, or a NoX Core-only game: the join waits for the
    // host's answer to the query (identify_wait.hpp), as a host with added types lets in only a
    // game whose query it has, and a NoX Core-only one holds out a client it doesn't know.
    bool started_ = false;
    bool tooOld_ = false; ///< (tooOld)
    std::vector<std::uint8_t> typesQuery_; ///< (empty: no wait, the join starts at once)
    IdentifyWait wait_;
    /// Our secrets for hosts' client key realms (noxcore_keys.txt in the game's folder), and the
    /// realm our last query carried a secret for (0: none).
    nox::lobby::ClientKeyring keyring_;
    std::uint64_t keyedRealm_ = 0;
    /// Our extended query, with our secret for `realm` when it's not 0.
    std::vector<std::uint8_t> query(std::uint64_t realm);
};

} // namespace noxsdl
