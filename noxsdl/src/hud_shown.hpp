// Whether the game's own HUD pieces are drawn and take the mouse (the spell bar, the health and mana
// meters and the potion slots, the weapon panel, the buffs' row, the summon cage, a mod's ability
// strip): not while we observe, as the game turns its GUI off while our player's client status has
// the observing bit (and on again, as the player had it, once it's clear); nor while a mod's UI
// covers them (UI API 6: a window opened with cover = true). F11 (App::renderGui) stays the
// player's own switch over all of it.
#pragma once

#include "noxcore/client/world.hpp"

#include <cstdint>

namespace noxsdl {

/// The HUD pieces shown to us (net code `us`) in `world`: not observing (status bit 1; a player not
/// listed yet counts as playing), and not covered by a mod's window (`modeCovers`).
inline bool hudShownTo(const nox::client::World& world, std::uint16_t us, bool modeCovers)
{
    if (modeCovers) {
        return false;
    }
    const auto me = world.players().find(us);
    return me == world.players().end() || (me->second.status & 1) == 0; // status 1: observing
}

} // namespace noxsdl
