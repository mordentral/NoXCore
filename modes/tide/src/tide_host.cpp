// Undead Tide host set-up: the GameHost options and hooks for a run (tide_host.hpp).
#include "noxcore/server/tide_host.hpp"

#include "mode_shared.hpp"

#include <utility>

namespace nox::server {

std::filesystem::path setUpTideHost(TideDirector& director, GameHost::Options& o, std::function<void(const TideDirector&)> onArena)
{
    director.sweepOwn();
    const std::filesystem::path lobby = director.writeLobby();
    detail::runHostOptions(o, director.options().mapName); // (straight into the arena)
    o.serverName = TideDirector::listedName(std::nullopt);
    o.monstersAlert = true; // (the tide looks all about it: quick to the players)
    o.prepareMap = [&director, onArena = std::move(onArena)](const std::filesystem::path& next, const World& world) {
        if (director.prepare(next, world) && onArena) {
            onArena(director);
        }
    };
    o.onChat = [&director](GameHost& host, std::uint8_t peer, std::u16string_view text) { return director.onChat(host, peer, text); };
    o.carryLinks = [&director](GameHost&, const KeyCarry& carry) { director.carryLinks(carry); };
    o.noxcppMessages[static_cast<std::uint8_t>(net::Op::NoxcppModeF5)] = [&director](GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m) {
        director.onMessage(host, peer, m);
    };
    return lobby;
}

} // namespace nox::server
