// Delving Mode host set-up: the GameHost options and hooks for a run (delve_host.hpp).
#include "noxcore/server/delve_host.hpp"

#include "mode_shared.hpp"

#include <utility>

namespace nox::server {

std::filesystem::path setUpDelveHost(DelveDirector& director, GameHost::Options& o, bool conjurers,
                                     std::function<void(const DelveDirector&)> onLevel)
{
    director.sweepOwn();
    const std::filesystem::path lobby = director.writeLobby();
    detail::runHostOptions(o, director.options().mapName); // (level after level)
    if (conjurers) {
        o.forceClass = 2; // (every one a conjurer, whatever its character)
    } else {
        o.multiclass = true; // (noxsdl players: any class's spells, abilities and weapons)
    }
    o.serverName = DelveDirector::listedName(0); // (renamed each map: the depth)
    o.prepareMap = [&director, onLevel = std::move(onLevel)](const std::filesystem::path& next, const World& world) {
        if (director.prepare(next, world) && onLevel) {
            onLevel(director);
        }
    };
    hookDelveHost(director, o);
    return lobby;
}

void hookDelveHost(DelveDirector& director, GameHost::Options& o)
{
    o.monstersAlert = true; // (its monsters look all about them: quick to the party coming in)
    // (A campfire's pick: a stock player's by chat, a noxsdl player's by MSG_NOXCPP_MODE_F4.)
    o.onChat = [&director](GameHost& host, std::uint8_t peer, std::u16string_view text) { return director.onChat(host, peer, text); };
    o.noxcppMessages[static_cast<std::uint8_t>(net::Op::NoxcppModeF4)] = [&director](GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m) {
        director.onMessage(host, peer, m);
    };
    // The multiclass, to a noxsdl player that plays it (at its join and after each map's load,
    // after what the game sends it then): its book, bars and meters for every class.
    o.onClientReady = [](GameHost& host, std::uint8_t peer) {
        if (const ServerObject* unit = host.unitOf(peer); unit && unit->player && unit->player->multiclass) {
            host.sendToIdentified(peer, delvemap::multiclassMessage(true));
        }
    };
}

} // namespace nox::server
