// MonsterDraw's (so MaidenDraw's) dot over a creature on our friends list: radius 3, in our
// team's colour, twice its height over its feet. It needs us on a team the client was told of
// and the creature on our friends list (MSG_OBJECT_FRIEND_ADD), but not a quest's (game flag
// 0x1000) creature of SUBCLASS SHOPKEEPER (8).
//
// Not in a campaign game (game flag 0x800). Solo, the game draws none: soloTeam puts the host in
// team 1 ("COOP") without telling its client, and the join's team list skips the host (slot 0x1F),
// so the host's own drawable never has a team and team play (gameplay flag 4) is off. Ours learns
// team 1 from that list (our host's client joins as any other), and the campaign's friends (NPCs,
// fish, the summons) would all get the dot. Co-op's guests are told team 1 to share the party's
// health; the party's game draws as the solo one does: no dot.
#pragma once

#include "noxcore/client/world.hpp"
#include "noxcore/things/object_class.hpp"

#include <cstdint>

namespace noxsdl {

/// Whether the dot is drawn over a creature (CLASS `classes`, SUBCLASS `subclasses`, on our
/// friends list or not), in a game of flags `gameFlags`, our own unit in team `ownTeam` (0: none)
/// that the client knows of (`teamKnown`).
inline bool friendDot(std::uint32_t gameFlags, std::uint32_t ownTeam, bool teamKnown, bool friendListed,
                      std::uint32_t classes, std::uint32_t subclasses)
{
    constexpr std::uint32_t kSolo = 0x800;
    constexpr std::uint32_t kQuest = 0x1000;
    constexpr std::uint32_t kShopkeeper = 0x8; // (monster SUBCLASS)
    if ((gameFlags & kSolo) || ownTeam == 0 || !teamKnown || !friendListed) {
        return false;
    }
    return !((gameFlags & kQuest) && (classes & nox::things::ObjectClass::Monster) && (subclasses & kShopkeeper));
}

/// The team whose colour the dot over the creature `netCode` takes (ours), or 0 for none.
inline std::uint32_t friendDotTeam(const nox::client::World& world, std::uint16_t netCode, std::uint32_t classes,
                                   std::uint32_t subclasses, std::uint32_t ownTeam)
{
    const bool known = ownTeam != 0 && world.teams().contains(ownTeam);
    return friendDot(world.game().gameFlags, ownTeam, known, world.isFriend(netCode), classes, subclasses) ? ownTeam : 0;
}

} // namespace noxsdl
