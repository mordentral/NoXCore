// MonsterDraw's (so MaidenDraw's) dot over a creature on our friends list: radius 3, in our
// team's colour, twice its height over its feet. It needs us on a team the client was told of
// and the creature on our friends list (MSG_OBJECT_FRIEND_ADD), but not a quest's (game flag
// 0x1000) creature of SUBCLASS SHOPKEEPER (8).
//
// Not in a campaign game (game flag 0x800). Solo, the game draws none: soloTeam puts the host in
// team 1 ("COOP") without telling its client, and the join's team list skips the host (slot 0x1F),
// so the host's own drawable never has a team and team play (gameplay flag 4) is off. Ours learns
// team 1 from that list (our host's client joins as any other), and the campaign's friends (NPCs,
// fish, the summons) would all get the dot.
//
// noxcpp (VD-39): the co-op campaign (its mode's client side greeted us: App::partyDots) draws it
// over the party's creatures, as a team game draws it over a team's: our own summons and charmed
// creatures (the summon panel's: MSG_REPORT_ACQUIRE_CREATURE) and every other member's (the
// session's list, MSG_NOXCPP_COOP kCreatures: the client isn't told whose a creature is otherwise).
// The campaign's NPCs, maidens and its other friends (its allied creatures, fish) still get none.
#pragma once

#include "noxcore/client/world.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace noxsdl {

/// Whether the dot is drawn over a creature (CLASS `classes`, SUBCLASS `subclasses`, on our
/// friends list or not), in a game of flags `gameFlags`, our own unit in team `ownTeam` (0: none)
/// that the client knows of (`teamKnown`). In a campaign game (0x800) only over the co-op party's
/// creature (`partyCreature`: partyCreature below; never in the solo game).
inline bool friendDot(std::uint32_t gameFlags, std::uint32_t ownTeam, bool teamKnown, bool friendListed,
                      std::uint32_t classes, std::uint32_t subclasses, bool partyCreature = false)
{
    constexpr std::uint32_t kSolo = 0x800;
    constexpr std::uint32_t kQuest = 0x1000;
    constexpr std::uint32_t kShopkeeper = 0x8; // (monster SUBCLASS)
    if (ownTeam == 0 || !teamKnown || !friendListed) {
        return false;
    }
    if (gameFlags & kSolo) {
        return partyCreature;
    }
    return !((gameFlags & kQuest) && (classes & nox::things::ObjectClass::Monster) && (subclasses & kShopkeeper));
}

/// noxcpp, the co-op campaign: whether the creature `netCode` is the party's: ours, in the summon
/// panel (our summons and charmed creatures), or on the session's list of the party's creatures
/// (`party`, sorted: CoopClient::partyCreatures).
inline bool partyCreature(const nox::client::World& world, std::uint16_t netCode, std::span<const std::uint16_t> party = {})
{
    for (const auto& s : world.self().summons) {
        if (s && s->netCode == netCode) {
            return true;
        }
    }
    return std::ranges::binary_search(party, netCode);
}

/// The team whose colour the dot over the creature `netCode` takes (ours), or 0 for none; in the
/// co-op campaign (`coopCampaign`) over the party's creatures (`party`: partyCreature's).
inline std::uint32_t friendDotTeam(const nox::client::World& world, std::uint16_t netCode, std::uint32_t classes,
                                   std::uint32_t subclasses, std::uint32_t ownTeam, bool coopCampaign = false,
                                   std::span<const std::uint16_t> party = {})
{
    const bool known = ownTeam != 0 && world.teams().contains(ownTeam);
    const bool inParty = coopCampaign && partyCreature(world, netCode, party);
    return friendDot(world.game().gameFlags, ownTeam, known, world.isFriend(netCode), classes, subclasses, inParty) ? ownTeam : 0;
}

} // namespace noxsdl
